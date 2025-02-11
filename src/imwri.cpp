/*
* Copyright (c) 2014-2019 Fredrik Mellbin
*
* This file is part of VapourSynth.
*
* VapourSynth is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* VapourSynth is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with VapourSynth; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/


// TODO:
// need to remember working dir on load in case something dicks around with it
// don't write frames twice in the event that a frame really is requested twice since it's a waste of time
// have some way to make sure all frames get written? add a separate function for writing frames that isn't a filter?

#include <Magick++.h>
#include <VapourSynth4.h>
#include <VSHelper4.h>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <memory>
#include <functional>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "vsutf16.h"
#else
#include <unistd.h>
#endif


// Handle both with and without hdri
#if MAGICKCORE_HDRI_ENABLE
#define IMWRI_NAMESPACE "imwri"
#define IMWRI_PLUGIN_NAME "VapourSynth ImageMagick 7 HDRI Writer/Reader"
#define IMWRI_ID "com.vapoursynth.imwri"
#else
#error ImageMagick must be compiled with HDRI enabled
#endif

#if MAGICKCORE_QUANTUM_DEPTH > 32
#error Only up to 32-bit sample size supported
#endif

#if defined(MAGICKCORE_LCMS_DELEGATE)
#define IMWRI_HAS_LCMS2
#endif

// Because proper namespace handling is too hard for ImageMagick shitvelopers
using MagickCore::Quantum;

//////////////////////////////////////////
// Shared

std::once_flag initMagickFlag;
static void initMagick(VSCore *core, const VSAPI *vsapi) {
    std::call_once(initMagickFlag, [=]() {
        std::string path;
#ifdef _WIN32
        const char *pathPtr = vsapi->getPluginPath(vsapi->getPluginByID(IMWRI_ID, core));
        if (pathPtr) {
            path = pathPtr;
            for (auto &c : path)
                if (c == '/')
                    c = '\\';
        }
#endif
        Magick::InitializeMagick(path.c_str());
    });
}

static std::string specialPrintf(const std::string &filename, int number) {
    std::string result;
    size_t copyPos = 0;
    size_t minWidth = 0;
    bool zeroPad = false;
    bool percentSeen = false;
    bool zeroPadSeen = false;
    bool minWidthSeen = false;

    for (size_t pos = 0; pos < filename.length(); pos++) {
        const char c = filename[pos];
        if (c == '%' && !percentSeen) {
            result += filename.substr(copyPos, pos - copyPos);
            copyPos = pos;
            percentSeen = true;
            continue;
        }
        if (percentSeen) {
            if (c == '0' && !zeroPadSeen) {
                zeroPad = true;
                zeroPadSeen = true;
                continue;
            }
            if (c >= '1' && c <= '9' && !minWidthSeen) {
                minWidth = c - '0';
                zeroPadSeen = true;
                minWidthSeen = true;
                continue;
            }
            if (c == 'd') {
                std::string num = std::to_string(number);
                if (minWidthSeen && minWidth > num.length())
                    num = std::string(minWidth - num.length(), zeroPad ? '0' : ' ') + num;
                result += num;
                copyPos = pos + 1;
            }
        }
        minWidth = 0;
        zeroPad = false;
        percentSeen = false;
        zeroPadSeen = false;
        minWidthSeen = false;
    }

    result += filename.substr(copyPos, filename.length() - copyPos);

    return result;
}

static bool isAbsolute(const std::string &path) {
#ifdef _WIN32
    return path.size() > 1 && ((path[0] == '/' && path[1] == '/') || (path[0] == '\\' && path[1] == '\\') || path[1] == ':');
#else
    return path.size() && path[0] == '/';
#endif
}

static bool fileExists(const std::string &filename) {
#ifdef _WIN32
    FILE * f = _wfopen(utf16_from_utf8(filename).c_str(), L"rb");
#else
    FILE * f = fopen(filename.c_str(), "rb");
#endif
    if (f)
        fclose(f);
    return !!f;
}

static void getWorkingDir(std::string &path) {
#ifdef _WIN32
    DWORD size = GetCurrentDirectoryW(0, nullptr);
    std::vector<wchar_t> buffer(size);
    GetCurrentDirectoryW(size, buffer.data());
    path = utf16_to_utf8(buffer.data()) + '\\';
#else
    char *buffer = getcwd(nullptr, 0);

    if (buffer) {
        if (buffer[0] != '(') {
            path = buffer;
            path += '/';
        }
        free(buffer);
    }
#endif
}

//////////////////////////////////////////
// Write

struct WriteData {
    VSNode *videoNode;
    VSNode *alphaNode;
    const VSVideoInfo *vi;
    std::string imgFormat;
    std::string filename;
    std::string workingDir;
    int firstNum;
    int quality;
    MagickCore::CompressionType compressType;
    bool dither;
    bool overwrite;

    WriteData() : videoNode(nullptr), alphaNode(nullptr), vi(nullptr), quality(0), compressType(MagickCore::UndefinedCompression), dither(true) {}
};

#if defined(__SSE2__) || (defined(_MSC_VER) && !defined(__clang__) && ((defined(_M_IX86_FP) && _M_IX86_FP == 2) || defined(_M_X64)))
#include <emmintrin.h>
#define HAVE_SSE2 1

static inline void ssePackPair8(__m128i &outLo, __m128i &outHi, __m128i a, __m128i b) {
    outLo = _mm_packus_epi16(
        _mm_and_si128(a, _mm_set1_epi16(0xff)),
        _mm_and_si128(b, _mm_set1_epi16(0xff))
    );
    outHi = _mm_packus_epi16(
        _mm_srli_epi16(a, 8), _mm_srli_epi16(b, 8)
    );
}
static inline void ssePackPair16(__m128i &outLo, __m128i &outHi, __m128i a, __m128i b) {
    // swap middle two 16-bit words in every 64-bit block
    a = _mm_shufflehi_epi16(_mm_shufflelo_epi16(a, _MM_SHUFFLE(3,1,2,0)), _MM_SHUFFLE(3,1,2,0));
    b = _mm_shufflehi_epi16(_mm_shufflelo_epi16(b, _MM_SHUFFLE(3,1,2,0)), _MM_SHUFFLE(3,1,2,0));

    // pull alternating 32-bit blocks
    outLo = _mm_castps_si128(_mm_shuffle_ps(
        _mm_castsi128_ps(a), _mm_castsi128_ps(b), _MM_SHUFFLE(2,0,2,0)
    ));
    outHi = _mm_castps_si128(_mm_shuffle_ps(
        _mm_castsi128_ps(a), _mm_castsi128_ps(b), _MM_SHUFFLE(3,1,3,1)
    ));
}

template <std::size_t N>
static inline void sseWritePixels8(void *dst, const __m128i (&vecs)[N]) {
#ifdef MAGICKCORE_HDRI_SUPPORT
    // pixels are always 32-bit float
    float *p = reinterpret_cast<float*>(dst);
    for (__m128i vec : vecs) {
        __m128i vec0, vec1;
        if (MAGICKCORE_QUANTUM_DEPTH == 8) {
            vec0 = _mm_unpacklo_epi8(vec, _mm_setzero_si128());
            vec1 = _mm_unpackhi_epi8(vec, _mm_setzero_si128());
        } else {
            vec0 = _mm_unpacklo_epi8(vec, vec);
            vec1 = _mm_unpackhi_epi8(vec, vec);
        }

        __m128 vec00 = _mm_cvtepi32_ps(_mm_unpacklo_epi16(vec0, _mm_setzero_si128()));
        __m128 vec01 = _mm_cvtepi32_ps(_mm_unpackhi_epi16(vec0, _mm_setzero_si128()));
        __m128 vec10 = _mm_cvtepi32_ps(_mm_unpacklo_epi16(vec1, _mm_setzero_si128()));
        __m128 vec11 = _mm_cvtepi32_ps(_mm_unpackhi_epi16(vec1, _mm_setzero_si128()));
        if (MAGICKCORE_QUANTUM_DEPTH == 32) {
            vec00 = _mm_mul_ps(vec00, _mm_set1_ps(65537.0));
            vec01 = _mm_mul_ps(vec01, _mm_set1_ps(65537.0));
            vec10 = _mm_mul_ps(vec10, _mm_set1_ps(65537.0));
            vec11 = _mm_mul_ps(vec11, _mm_set1_ps(65537.0));
        }
        _mm_storeu_ps(p, vec00);
        _mm_storeu_ps(p + 4, vec01);
        _mm_storeu_ps(p + 8, vec10);
        _mm_storeu_ps(p +12, vec11);
        p += 16;
    }
#else
    __m128i *p = reinterpret_cast<__m128i*>(dst);
    if (sizeof(MagickCore::Quantum) == 1) {
        for (__m128i vec : vecs)
            _mm_storeu_si128(p++, vec);
    } else {
        for (__m128i vec : vecs) {
            __m128i vec0, vec1;
            if (MAGICKCORE_QUANTUM_DEPTH == 8) {
                vec0 = _mm_unpacklo_epi8(vec, _mm_setzero_si128());
                vec1 = _mm_unpackhi_epi8(vec, _mm_setzero_si128());
            } else {
                vec0 = _mm_unpacklo_epi8(vec, vec);
                vec1 = _mm_unpackhi_epi8(vec, vec);
            }
            if (sizeof(MagickCore::Quantum) == 4) {
                if (MAGICKCORE_QUANTUM_DEPTH == 32) {
                    _mm_storeu_si128(p++, _mm_unpacklo_epi16(vec0, vec0));
                    _mm_storeu_si128(p++, _mm_unpackhi_epi16(vec0, vec0));
                    _mm_storeu_si128(p++, _mm_unpacklo_epi16(vec1, vec1));
                    _mm_storeu_si128(p++, _mm_unpackhi_epi16(vec1, vec1));
                } else { // 8 or 16
                    _mm_storeu_si128(p++, _mm_unpacklo_epi16(vec0, _mm_setzero_si128()));
                    _mm_storeu_si128(p++, _mm_unpackhi_epi16(vec0, _mm_setzero_si128()));
                    _mm_storeu_si128(p++, _mm_unpacklo_epi16(vec1, _mm_setzero_si128()));
                    _mm_storeu_si128(p++, _mm_unpackhi_epi16(vec1, _mm_setzero_si128()));
                }
            } else { // sizeof(MagickCore::Quantum) == 2
                _mm_storeu_si128(p++, vec0);
                _mm_storeu_si128(p++, vec1);
            }
        }
    }
#endif
}
template <std::size_t N>
static inline void sseWritePixels16(void *dst, const __m128i (&vecs)[N]) {
#ifdef MAGICKCORE_HDRI_SUPPORT
    // pixels are always 32-bit float (64-bit float unsupported here)
    float *p = reinterpret_cast<float*>(dst);
    for (__m128i vec : vecs) {
        if (MAGICKCORE_QUANTUM_DEPTH == 8)
            vec = _mm_srli_epi16(vec, 8);

        __m128 vec0 = _mm_cvtepi32_ps(_mm_unpacklo_epi16(vec, _mm_setzero_si128()));
        __m128 vec1 = _mm_cvtepi32_ps(_mm_unpackhi_epi16(vec, _mm_setzero_si128()));
        if (MAGICKCORE_QUANTUM_DEPTH == 32) {
            vec0 = _mm_mul_ps(vec0, _mm_set1_ps(65537.0));
            vec1 = _mm_mul_ps(vec1, _mm_set1_ps(65537.0));
        }
        _mm_storeu_ps(p, vec0);
        _mm_storeu_ps(p + 4, vec1);
        p += 8;
    }
#else
    __m128i *p = reinterpret_cast<__m128i*>(dst);
    if (sizeof(MagickCore::Quantum) == 1) {
        for (int i = 0; i < N; i += 2) {
            __m128i vec0 = vecs[i], vec1 = vecs[i + 1];
            vec0 = _mm_srli_epi16(vec0, 8); // should rounding be done here?
            vec1 = _mm_srli_epi16(vec1, 8);
            _mm_storeu_si128(p++, _mm_packus_epi16(vec0, vec1));
        }
    } else {
        for (__m128i vec : vecs) {
            if (sizeof(MagickCore::Quantum) == 4) {
                if (MAGICKCORE_QUANTUM_DEPTH == 32) {
                    _mm_storeu_si128(p++, _mm_unpacklo_epi16(vec, vec));
                    _mm_storeu_si128(p++, _mm_unpackhi_epi16(vec, vec));
                } else { // 8 or 16
                    if (MAGICKCORE_QUANTUM_DEPTH == 8)
                        vec = _mm_srli_epi16(vec, 8);
                    _mm_storeu_si128(p++, _mm_unpacklo_epi16(vec, _mm_setzero_si128()));
                    _mm_storeu_si128(p++, _mm_unpackhi_epi16(vec, _mm_setzero_si128()));
                }
            } else { // sizeof(MagickCore::Quantum) == 2
                if (MAGICKCORE_QUANTUM_DEPTH == 8)
                    vec = _mm_srli_epi16(vec, 8);
                _mm_storeu_si128(p++, vec);
            }
        }
    }
#endif
}
#endif

template<typename T>
static void writeImageHelper(const VSFrame *frame, const VSFrame *alphaFrame, bool isGray, Magick::Image &image, int width, int height, int bitsPerSample, const VSAPI *vsapi) {
    unsigned prepeat = (MAGICKCORE_QUANTUM_DEPTH - 1) / bitsPerSample;
    unsigned pleftover = MAGICKCORE_QUANTUM_DEPTH - (bitsPerSample * prepeat);
    unsigned shiftFactor = bitsPerSample - pleftover;
    unsigned scaleFactor = 0;
    for (unsigned i = 0; i < prepeat; i++) {
        scaleFactor <<= bitsPerSample;
        scaleFactor += 1;
    }
    scaleFactor <<= pleftover;

    // basic downsampling support
    if(bitsPerSample > MAGICKCORE_QUANTUM_DEPTH)
        shiftFactor = bitsPerSample - MAGICKCORE_QUANTUM_DEPTH;

    Magick::Pixels pixelCache(image);

    const T * VS_RESTRICT r = reinterpret_cast<const T *>(vsapi->getReadPtr(frame, 0));
    const T * VS_RESTRICT g = reinterpret_cast<const T *>(vsapi->getReadPtr(frame, isGray ? 0 : 1));
    const T * VS_RESTRICT b = reinterpret_cast<const T *>(vsapi->getReadPtr(frame, isGray ? 0 : 2));
    ptrdiff_t strideR = vsapi->getStride(frame, 0);
    ptrdiff_t strideG = vsapi->getStride(frame, isGray ? 0 : 1);
    ptrdiff_t strideB = vsapi->getStride(frame, isGray ? 0 : 2);
    ssize_t rOff = pixelCache.offset(MagickCore::RedPixelChannel);
    ssize_t gOff = pixelCache.offset(MagickCore::GreenPixelChannel);
    ssize_t bOff = pixelCache.offset(MagickCore::BluePixelChannel);
    size_t channels = image.channels();

    if (alphaFrame) {
        ssize_t aOff = pixelCache.offset(MagickCore::AlphaPixelChannel);
        ptrdiff_t strideA = vsapi->getStride(alphaFrame, 0);
        const T * VS_RESTRICT a = reinterpret_cast<const T *>(vsapi->getReadPtr(alphaFrame, 0));

        auto loopImage = [&](const std::function<void(MagickCore::Quantum *, int &)> &loopPixels) {
            for (int y = 0; y < height; y++) {
                MagickCore::Quantum *pixels = pixelCache.get(0, y, width, 1);
                int x = 0;
                loopPixels(pixels, x);
                for (; x < width; x++) {
                    pixels[x * channels + rOff] = r[x] * scaleFactor + (r[x] >> shiftFactor);
                    pixels[x * channels + gOff] = g[x] * scaleFactor + (g[x] >> shiftFactor);
                    pixels[x * channels + bOff] = b[x] * scaleFactor + (b[x] >> shiftFactor);
                    pixels[x * channels + aOff] = a[x] * scaleFactor + (a[x] >> shiftFactor);
                }

                r += strideR / sizeof(T);
                g += strideG / sizeof(T);
                b += strideB / sizeof(T);
                a += strideA / sizeof(T);

                pixelCache.sync();
            }
        };
#ifdef HAVE_SSE2
        if (sizeof(MagickCore::Quantum) <= 4 && MAGICKCORE_QUANTUM_DEPTH <= 32 && channels == 4 && rOff == 0 && gOff == 1 && bOff == 2 && aOff == 3) { // typical ImageMagick config
            if (sizeof(T) == 1 && bitsPerSample == 8) {
                loopImage([&](MagickCore::Quantum *pixels, int &x) {
                    for (; x < width - 15; x += 16) {
                        __m128i r0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r + x));
                        __m128i g0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(g + x));
                        __m128i b0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + x));
                        __m128i a0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + x));

                        // interleave
                        __m128i rg0 = _mm_unpacklo_epi8(r0, g0);
                        __m128i rg1 = _mm_unpackhi_epi8(r0, g0);
                        __m128i ba0 = _mm_unpacklo_epi8(b0, a0);
                        __m128i ba1 = _mm_unpackhi_epi8(b0, a0);

                        __m128i rgba0 = _mm_unpacklo_epi16(rg0, ba0);
                        __m128i rgba1 = _mm_unpackhi_epi16(rg0, ba0);
                        __m128i rgba2 = _mm_unpacklo_epi16(rg1, ba1);
                        __m128i rgba3 = _mm_unpackhi_epi16(rg1, ba1);

                        sseWritePixels8(pixels + x * channels, (const __m128i[4]){
                            rgba0, rgba1, rgba2, rgba3
                        });
                    }
                });
                return;
            } else if (sizeof(T) == 2 && bitsPerSample >= 8 && (MAGICKCORE_QUANTUM_DEPTH <= 16 || bitsPerSample == 8 || bitsPerSample == 16)) { // TODO: consider supporting proper upsampling to 32-bit
                loopImage([&](MagickCore::Quantum *pixels, int &x) {
                    __m128i shl = _mm_set_epi32(0, bitsPerSample * 2 - 16, 0, 16 - bitsPerSample);
                    __m128i shr = _mm_unpackhi_epi64(shl, shl);
                    for (; x < width - 7; x += 8) {
                        __m128i r0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r + x));
                        __m128i g0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(g + x));
                        __m128i b0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + x));
                        __m128i a0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + x));

                        // upsample pixels to 16-bit
                        r0 = _mm_or_si128(_mm_sll_epi16(r0, shl), _mm_srl_epi16(r0, shr));
                        g0 = _mm_or_si128(_mm_sll_epi16(g0, shl), _mm_srl_epi16(g0, shr));
                        b0 = _mm_or_si128(_mm_sll_epi16(b0, shl), _mm_srl_epi16(b0, shr));
                        a0 = _mm_or_si128(_mm_sll_epi16(a0, shl), _mm_srl_epi16(a0, shr));

                        // interleave
                        __m128i rg0 = _mm_unpacklo_epi16(r0, g0);
                        __m128i rg1 = _mm_unpackhi_epi16(r0, g0);
                        __m128i ba0 = _mm_unpacklo_epi16(b0, a0);
                        __m128i ba1 = _mm_unpackhi_epi16(b0, a0);

                        sseWritePixels16(pixels + x * channels, (const __m128i[4]){
                            _mm_unpacklo_epi32(rg0, ba0),
                            _mm_unpackhi_epi32(rg0, ba0),
                            _mm_unpacklo_epi32(rg1, ba1),
                            _mm_unpackhi_epi32(rg1, ba1)
                        });
                    }
                });
                return;
            }
        }
#endif
        loopImage([&](MagickCore::Quantum *pixels, int &x) {});
    } else {
        auto loopImage = [&](const std::function<void(MagickCore::Quantum *, int &)> &loopPixels) {
            for (int y = 0; y < height; y++) {
                MagickCore::Quantum *pixels = pixelCache.get(0, y, width, 1);
                int x = 0;
                loopPixels(pixels, x);
                for (; x < width; x++) {
                    pixels[x * channels + rOff] = r[x] * scaleFactor + (r[x] >> shiftFactor);
                    pixels[x * channels + gOff] = g[x] * scaleFactor + (g[x] >> shiftFactor);
                    pixels[x * channels + bOff] = b[x] * scaleFactor + (b[x] >> shiftFactor);
                }

                r += strideR / sizeof(T);
                g += strideG / sizeof(T);
                b += strideB / sizeof(T);

                pixelCache.sync();
            }
        };

#ifdef HAVE_SSE2
        if (sizeof(MagickCore::Quantum) <= 4 && MAGICKCORE_QUANTUM_DEPTH <= 32 && channels == 3 && rOff == 0 && gOff == 1 && bOff == 2) { // typical ImageMagick config
            if (sizeof(T) == 1 && bitsPerSample == 8) {
                loopImage([&](MagickCore::Quantum *pixels, int &x) {
                    for (; x < width - 31; x += 32) {
                        __m128i r0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r + x));
                        __m128i r1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r + x) + 1);
                        __m128i g0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(g + x));
                        __m128i g1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(g + x) + 1);
                        __m128i b0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + x));
                        __m128i b1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + x) + 1);

                        // interleave pixels via repeated packing
                        __m128i r01a, r01b, g01a, g01b, b01a, b01b;
                        ssePackPair8(r01a, r01b, r0, r1);
                        ssePackPair8(g01a, g01b, g0, g1);
                        ssePackPair8(b01a, b01b, b0, b1);

                        __m128i rg0, rg2, gb1, gb3, br0, br2;
                        ssePackPair8(rg0, rg2, r01a, g01a);
                        ssePackPair8(gb1, gb3, g01b, b01b);
                        ssePackPair8(br0, br2, b01a, r01b);

                        __m128i rgbr0, rgbr1, gbrg0, gbrg1, brgb0, brgb1;
                        ssePackPair8(rgbr0, rgbr1, rg0, br0);
                        ssePackPair8(gbrg0, gbrg1, gb1, rg2);
                        ssePackPair8(brgb0, brgb1, br2, gb3);

                        __m128i r_g0, r_g1, b_r0, b_r1, g_b0, g_b1;
                        ssePackPair8(r_g0, r_g1, rgbr0, gbrg0);
                        ssePackPair8(b_r0, b_r1, brgb0, rgbr1);
                        ssePackPair8(g_b0, g_b1, gbrg1, brgb1);

                        __m128i r_r0, r_r1, g_g0, g_g1, b_b0, b_b1;
                        ssePackPair8(r_r0, r_r1, r_g0, b_r0);
                        ssePackPair8(g_g0, g_g1, g_b0, r_g1);
                        ssePackPair8(b_b0, b_b1, b_r1, g_b1);

                        sseWritePixels8(pixels + x * channels, (const __m128i[6]){
                            r_r0, g_g0, b_b0,
                            r_r1, g_g1, b_b1
                        });
                    }
                });
                return;
            } else if (sizeof(T) == 2 && bitsPerSample >= 8 && (MAGICKCORE_QUANTUM_DEPTH <= 16 || bitsPerSample == 8 || bitsPerSample == 16)) {
                loopImage([&](MagickCore::Quantum *pixels, int &x) {
                    __m128i shl = _mm_set_epi32(0, bitsPerSample * 2 - 16, 0, 16 - bitsPerSample);
                    __m128i shr = _mm_unpackhi_epi64(shl, shl);
                    for (; x < width - 15; x += 16) {
                        __m128i r0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r + x));
                        __m128i r1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r + x) + 1);
                        __m128i g0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(g + x));
                        __m128i g1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(g + x) + 1);
                        __m128i b0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + x));
                        __m128i b1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + x) + 1);

                        // upsample pixels to 16-bit
                        r0 = _mm_or_si128(_mm_sll_epi16(r0, shl), _mm_srl_epi16(r0, shr));
                        r1 = _mm_or_si128(_mm_sll_epi16(r1, shl), _mm_srl_epi16(r1, shr));
                        g0 = _mm_or_si128(_mm_sll_epi16(g0, shl), _mm_srl_epi16(g0, shr));
                        g1 = _mm_or_si128(_mm_sll_epi16(g1, shl), _mm_srl_epi16(g1, shr));
                        b0 = _mm_or_si128(_mm_sll_epi16(b0, shl), _mm_srl_epi16(b0, shr));
                        b1 = _mm_or_si128(_mm_sll_epi16(b1, shl), _mm_srl_epi16(b1, shr));

                        // interleave pixels via repeated packing
                        __m128i r01a, r01b, g01a, g01b, b01a, b01b;
                        ssePackPair16(r01a, r01b, r0, r1);
                        ssePackPair16(g01a, g01b, g0, g1);
                        ssePackPair16(b01a, b01b, b0, b1);

                        __m128i rg0, rg2, gb1, gb3, br0, br2;
                        ssePackPair16(rg0, rg2, r01a, g01a);
                        ssePackPair16(gb1, gb3, g01b, b01b);
                        ssePackPair16(br0, br2, b01a, r01b);

                        __m128i rgbr0, rgbr1, gbrg0, gbrg1, brgb0, brgb1;
                        ssePackPair16(rgbr0, rgbr1, rg0, br0);
                        ssePackPair16(gbrg0, gbrg1, gb1, rg2);
                        ssePackPair16(brgb0, brgb1, br2, gb3);

                        __m128i r_g0, r_g1, b_r0, b_r1, g_b0, g_b1;
                        ssePackPair16(r_g0, r_g1, rgbr0, gbrg0);
                        ssePackPair16(b_r0, b_r1, brgb0, rgbr1);
                        ssePackPair16(g_b0, g_b1, gbrg1, brgb1);

                        sseWritePixels16(pixels + x * channels, (const __m128i[6]){
                            r_g0, b_r0, g_b0,
                            r_g1, b_r1, g_b1
                        });
                    }
                });
                return;
            }
        }
#endif
        loopImage([&](MagickCore::Quantum *pixels, int &x) {});
    }
}

// for the WriteData argument, only `imgFormat`, `compressType`, `dither` and `quality` fields are referenced
static Magick::Image frameToImage(const VSFrame *frame, const VSFrame *alphaFrame, const WriteData *d, const VSAPI *vsapi) {
    const VSVideoFormat *fi = vsapi->getVideoFrameFormat(frame);
    int width = vsapi->getFrameWidth(frame, 0);
    int height = vsapi->getFrameHeight(frame, 0);

    Magick::Image image(Magick::Geometry(width, height), Magick::Color(0, 0, 0, 0));
    image.magick(d->imgFormat);
    image.modulusDepth(fi->bitsPerSample);
    if (d->compressType != MagickCore::UndefinedCompression)
        image.compressType(d->compressType);
    image.quantizeDitherMethod(Magick::FloydSteinbergDitherMethod);
    image.quantizeDither(d->dither);
    image.quality(d->quality);
    image.alphaChannel(alphaFrame ? Magick::ActivateAlphaChannel : Magick::RemoveAlphaChannel);

    bool isGray = fi->colorFamily == cfGray;
    if (isGray)
        image.colorSpace(Magick::GRAYColorspace);

    if (fi->bytesPerSample == 4 && fi->sampleType == stFloat) {
        image.attribute("quantum:format", "floating-point");
        Magick::Pixels pixelCache(image);
        const Quantum scaleFactor = QuantumRange;

        const float * VS_RESTRICT r = reinterpret_cast<const float *>(vsapi->getReadPtr(frame, 0));
        const float * VS_RESTRICT g = reinterpret_cast<const float *>(vsapi->getReadPtr(frame, isGray ? 0 : 1));
        const float * VS_RESTRICT b = reinterpret_cast<const float *>(vsapi->getReadPtr(frame, isGray ? 0 : 2));
       
        ptrdiff_t strideR = vsapi->getStride(frame, 0);
        ptrdiff_t strideG = vsapi->getStride(frame, isGray ? 0 : 1);
        ptrdiff_t strideB = vsapi->getStride(frame, isGray ? 0 : 2);
            
        ssize_t rOff = pixelCache.offset(MagickCore::RedPixelChannel);
        ssize_t gOff = pixelCache.offset(MagickCore::GreenPixelChannel);
        ssize_t bOff = pixelCache.offset(MagickCore::BluePixelChannel);
        size_t channels = image.channels();

        if (alphaFrame) {
            const float * VS_RESTRICT a = reinterpret_cast<const float *>(vsapi->getReadPtr(alphaFrame, 0));
            ptrdiff_t strideA = vsapi->getStride(alphaFrame, 0);
            ssize_t aOff = pixelCache.offset(MagickCore::AlphaPixelChannel);
    
            for (int y = 0; y < height; y++) {
                MagickCore::Quantum* pixels = pixelCache.get(0, y, width, 1);
                for (int x = 0; x < width; x++) {
                    pixels[x * channels + rOff] = r[x] * scaleFactor;
                    pixels[x * channels + gOff] = g[x] * scaleFactor;
                    pixels[x * channels + bOff] = b[x] * scaleFactor;
                    pixels[x * channels + aOff] = a[x] * scaleFactor;
                }

                r += strideR / sizeof(float);
                g += strideG / sizeof(float);
                b += strideB / sizeof(float);
                a += strideA / sizeof(float);

                pixelCache.sync();
            }
        } else {
            const float *r = reinterpret_cast<const float *>(vsapi->getReadPtr(frame, 0));
            const float *g = reinterpret_cast<const float *>(vsapi->getReadPtr(frame, isGray ? 0 : 1));
            const float *b = reinterpret_cast<const float *>(vsapi->getReadPtr(frame, isGray ? 0 : 2));

            for (int y = 0; y < height; y++) {
                MagickCore::Quantum* pixels = pixelCache.get(0, y, width, 1);
                for (int x = 0; x < width; x++) {
                    pixels[x * channels + rOff] = r[x] * scaleFactor;
                    pixels[x * channels + gOff] = g[x] * scaleFactor;
                    pixels[x * channels + bOff] = b[x] * scaleFactor;
                }

                r += strideR / sizeof(float);
                g += strideG / sizeof(float);
                b += strideB / sizeof(float);

                pixelCache.sync();
            }
        }
    } else if (fi->bytesPerSample == 4) {
        writeImageHelper<uint32_t>(frame, alphaFrame, isGray, image, width, height, fi->bitsPerSample, vsapi);
    } else if (fi->bytesPerSample == 2) {
        writeImageHelper<uint16_t>(frame, alphaFrame, isGray, image, width, height, fi->bitsPerSample, vsapi);
    } else if (fi->bytesPerSample == 1) {
        writeImageHelper<uint8_t>(frame, alphaFrame, isGray, image, width, height, fi->bitsPerSample, vsapi);
    }

    return image;
}

static inline bool frameDimsMatch(const VSFrame *a, const VSFrame *b, const VSAPI *vsapi) {
    return vsapi->getFrameWidth(a, 0) == vsapi->getFrameWidth(b, 0) &&
           vsapi->getFrameHeight(b, 0) == vsapi->getFrameHeight(b, 0);
}

static const VSFrame *VS_CC writeGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    WriteData *d = static_cast<WriteData *>(instanceData);

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->videoNode, frameCtx);
        if (d->alphaNode)
            vsapi->requestFrameFilter(n, d->alphaNode, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrame *frame = vsapi->getFrameFilter(n, d->videoNode, frameCtx);
        const VSFrame *alphaFrame = nullptr;

        std::string filename = specialPrintf(d->filename, n + d->firstNum);
        if (!isAbsolute(filename))
            filename = d->workingDir + filename;

        if (!d->overwrite && fileExists(filename))
            return frame;

        if (d->alphaNode) {
            alphaFrame = vsapi->getFrameFilter(n, d->alphaNode, frameCtx);

            if (!frameDimsMatch(frame, alphaFrame, vsapi)) {
                vsapi->setFilterError("Write: Mismatched dimension of the alpha clip", frameCtx);
                vsapi->freeFrame(frame);
                vsapi->freeFrame(alphaFrame);
                return nullptr;
            }
        }

        try {
            auto image = frameToImage(frame, alphaFrame, d, vsapi);
            image.strip();
            image.write(filename);

            vsapi->freeFrame(alphaFrame);
            return frame;
        } catch (Magick::Exception &e) {
            vsapi->setFilterError((std::string("Write: ImageMagick error: ") + e.what()).c_str(), frameCtx);
            vsapi->freeFrame(frame);
            vsapi->freeFrame(alphaFrame);
            return nullptr;
        }
    }

    return nullptr;
}

static void VS_CC writeFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    WriteData *d = static_cast<WriteData *>(instanceData);
    vsapi->freeNode(d->videoNode);
    vsapi->freeNode(d->alphaNode);
    delete d;
}

static const char* fillWriteDataFromMap(const VSMap *in, std::unique_ptr<WriteData> &d, const VSAPI *vsapi) {
    int err = 0;
    d->quality = vsapi->mapGetIntSaturated(in, "quality", 0, &err);
    if (err)
        d->quality = 75;
    if (d->quality < 0 || d->quality > 100)
        return "Quality must be between 0 and 100";

    const char *compressType = vsapi->mapGetData(in, "compression_type", 0, &err);
    if (!err) {
        std::string s = compressType;
        std::transform(s.begin(), s.end(), s.begin(), toupper);
        if (s == "" || s == "UNDEFINED")
            d->compressType = MagickCore::UndefinedCompression;
        else if (s == "NONE")
            d->compressType = MagickCore::NoCompression;
        else if (s == "BZIP")
            d->compressType = MagickCore::BZipCompression;
        else if (s == "DXT1")
            d->compressType = MagickCore::DXT1Compression;
        else if (s == "DXT3")
            d->compressType = MagickCore::DXT3Compression;
        else if (s == "DXT5")
            d->compressType = MagickCore::DXT5Compression;
        else if (s == "FAX")
            d->compressType = MagickCore::FaxCompression;
        else if (s == "GROUP4")
            d->compressType = MagickCore::Group4Compression;
        else if (s == "JPEG")
            d->compressType = MagickCore::JPEGCompression;
        else if (s == "JPEG2000")
            d->compressType = MagickCore::JPEG2000Compression;
        else if (s == "LOSSLESSJPEG")
            d->compressType = MagickCore::LosslessJPEGCompression;
        else if (s == "LZW")
            d->compressType = MagickCore::LZWCompression;
        else if (s == "RLE")
            d->compressType = MagickCore::RLECompression;
        else if (s == "ZIP")
            d->compressType = MagickCore::ZipCompression;
        else if (s == "ZIPS")
            d->compressType = MagickCore::ZipSCompression;
        else if (s == "PIZ")
            d->compressType = MagickCore::PizCompression;
        else if (s == "PXR24")
            d->compressType = MagickCore::Pxr24Compression;
        else if (s == "B44")
            d->compressType = MagickCore::B44Compression;
        else if (s == "B44A")
            d->compressType = MagickCore::B44ACompression;
        else if (s == "LZMA")
            d->compressType = MagickCore::LZMACompression;
        else if (s == "JBIG1")
            d->compressType = MagickCore::JBIG1Compression;
        else if (s == "JBIG2")
            d->compressType = MagickCore::JBIG2Compression;
        else {
            return "Unrecognized compression type";
        }
    }

    d->imgFormat = vsapi->mapGetData(in, "imgformat", 0, nullptr);
    d->dither = !!vsapi->mapGetInt(in, "dither", 0, &err);
    if (err)
        d->dither = true;

    return nullptr;
}

static void VS_CC writeCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<WriteData> d(new WriteData());
    int err = 0;

    initMagick(core, vsapi);

    const char *errMsg = fillWriteDataFromMap(in, d, vsapi);
    if (errMsg) {
        vsapi->mapSetError(out, (std::string("Write: ") + errMsg).c_str());
        return;
    }

    d->firstNum = vsapi->mapGetIntSaturated(in, "firstnum", 0, &err);
    if (d->firstNum < 0) {
        vsapi->mapSetError(out, "Write: Frame number offset can't be negative");
        return;
    }

    d->videoNode = vsapi->mapGetNode(in, "clip", 0, nullptr);
    d->vi = vsapi->getVideoInfo(d->videoNode);
    if ((d->vi->format.colorFamily != cfRGB && d->vi->format.colorFamily != cfGray)
        || (d->vi->format.sampleType == stFloat && d->vi->format.bitsPerSample != 32))
    {
        vsapi->freeNode(d->videoNode);
        vsapi->mapSetError(out, "Write: Only constant format 8-32 bit integer or float RGB and Grayscale input supported");
        return;
    }

    d->alphaNode = vsapi->mapGetNode(in, "alpha", 0, &err);
    d->filename = vsapi->mapGetData(in, "filename", 0, nullptr);
    d->overwrite = !!vsapi->mapGetInt(in, "overwrite", 0, &err);

    if (d->alphaNode) {
        const VSVideoInfo *alphaVi = vsapi->getVideoInfo(d->alphaNode);
        VSVideoFormat alphaFormat;
        vsapi->queryVideoFormat(&alphaFormat, cfGray, d->vi->format.sampleType, d->vi->format.bitsPerSample, 0, 0, core);

        if (d->vi->width != alphaVi->width || d->vi->height != alphaVi->height || alphaVi->format.colorFamily == cfUndefined ||
            !vsh::isSameVideoFormat(&alphaVi->format, &alphaFormat)) {
            vsapi->freeNode(d->videoNode);
            vsapi->freeNode(d->alphaNode);
            vsapi->mapSetError(out, "Write: Alpha clip dimensions and format don't match the main clip");
            return;
        }
        
    }

    if (!d->overwrite && specialPrintf(d->filename, 0) == d->filename) {
        // No valid digit substitution in the filename so error out to warn the user
        vsapi->freeNode(d->videoNode);
        vsapi->freeNode(d->alphaNode);
        vsapi->mapSetError(out, "Write: Filename string doesn't contain a number");
        return;
    }

    getWorkingDir(d->workingDir);

    VSFilterDependency deps[] = {{ d->videoNode, rpStrictSpatial }, { d->alphaNode, rpStrictSpatial }};
    vsapi->createVideoFilter(out, "Write", d->vi, writeGetFrame, writeFree, fmParallelRequests, deps, d->alphaNode ? 2 : 1, d.get(), core);
    d.release();
}

static void VS_CC encodeFrame(const VSMap *in, VSMap *out, void *, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<WriteData> d(new WriteData());
    int err = 0;

    initMagick(core, vsapi);

    const char *errMsg = fillWriteDataFromMap(in, d, vsapi);
    if (errMsg) {
        vsapi->mapSetError(out, (std::string("EncodeFrame: ") + errMsg).c_str());
        return;
    }

    const VSFrame *frame = vsapi->mapGetFrame(in, "frame", 0, nullptr);
    const VSVideoFormat *fi = vsapi->getVideoFrameFormat(frame);

    if ((fi->colorFamily != cfRGB && fi->colorFamily != cfGray)
        || (fi->sampleType == stFloat && fi->bitsPerSample != 32))
    {
        vsapi->freeFrame(frame);
        vsapi->mapSetError(out, "EncodeFrame: Only constant format 8-32 bit integer or float RGB and Grayscale input supported");
        return;
    }

    const VSFrame *alpha = vsapi->mapGetFrame(in, "alpha", 0, &err);

    if (alpha) {
        const VSVideoFormat *alphaFi = vsapi->getVideoFrameFormat(alpha);

        if (!frameDimsMatch(frame, alpha, vsapi) ||
            alphaFi->colorFamily == cfUndefined ||
            !vsh::isSameVideoFormat(fi, alphaFi)) {
            vsapi->freeFrame(frame);
            vsapi->freeFrame(alpha);
            vsapi->mapSetError(out, "EncodeFrame: Alpha frame dimensions and format don't match the main frame");
            return;
        }
    }

    Magick::Blob data;
    try {
        auto image = frameToImage(frame, alpha, d.get(), vsapi);
        image.strip();
        image.write(&data);
    } catch (Magick::Exception &e) {
        vsapi->mapSetError(out, (std::string("EncodeFrame: ImageMagick error: ") + e.what()).c_str());
        vsapi->freeFrame(frame);
        vsapi->freeFrame(alpha);
        return;
    }

    vsapi->freeFrame(frame);
    vsapi->freeFrame(alpha);

    vsapi->mapSetData(out, "bytes", static_cast<const char*>(data.data()), data.length(), dtBinary, maReplace);
}

//////////////////////////////////////////
// Read

struct ReadData {
    VSVideoInfo vi[2];
    std::vector<std::string> filenames;
    std::string workingDir;
    int firstNum;
    bool alpha;
    bool mismatch;
    bool fileListMode;
    bool floatOutput;
    int cachedFrameNum;
    bool cachedAlpha;
    bool embedICC;
    const VSFrame *cachedFrame;

    ReadData() : fileListMode(true) {};
};

template<typename T>
static void readImageHelper(VSFrame *frame, VSFrame *alphaFrame, bool isGray, Magick::Image &image, int width, int height, int bitsPerSample, const VSAPI *vsapi) {
    float outScale = ((1 << bitsPerSample) - 1) / static_cast<float>((1 << MAGICKCORE_QUANTUM_DEPTH) - 1);
    size_t channels = image.channels();
    Magick::Pixels pixelCache(image);

    T *r = reinterpret_cast<T *>(vsapi->getWritePtr(frame, 0));
    T *g = reinterpret_cast<T *>(vsapi->getWritePtr(frame, isGray ? 0 : 1));
    T *b = reinterpret_cast<T *>(vsapi->getWritePtr(frame, isGray ? 0 : 2));

    ptrdiff_t strideR = vsapi->getStride(frame, 0);
    ptrdiff_t strideG = vsapi->getStride(frame, isGray ? 0 : 1);
    ptrdiff_t strideB = vsapi->getStride(frame, isGray ? 0 : 2);

    ssize_t rOff = pixelCache.offset(MagickCore::RedPixelChannel);
    ssize_t gOff = pixelCache.offset(MagickCore::GreenPixelChannel);
    ssize_t bOff = pixelCache.offset(MagickCore::BluePixelChannel);
    ssize_t aOff = pixelCache.offset(MagickCore::AlphaPixelChannel);

    if (alphaFrame && aOff >= 0) {
        T *a = reinterpret_cast<T *>(vsapi->getWritePtr(alphaFrame, 0));
        ptrdiff_t strideA = vsapi->getStride(alphaFrame, 0);

        for (int y = 0; y < height; y++) {
            const Magick::Quantum *pixels = pixelCache.getConst(0, y, width, 1);
            for (int x = 0; x < width; x++) {
                r[x] = (unsigned)(pixels[x * channels + rOff] * outScale + .5f);
                g[x] = (unsigned)(pixels[x * channels + gOff] * outScale + .5f);
                b[x] = (unsigned)(pixels[x * channels + bOff] * outScale + .5f);
                a[x] = (unsigned)(pixels[x * channels + aOff] * outScale + .5f);
            }

            r += strideR / sizeof(T);
            g += strideG / sizeof(T);
            b += strideB / sizeof(T);
            a += strideA / sizeof(T);
        }
    } else {
        for (int y = 0; y < height; y++) {
            const Magick::Quantum *pixels = pixelCache.getConst(0, y, width, 1);
            for (int x = 0; x < width; x++) {
                r[x] = (unsigned)(pixels[x * channels + rOff] * outScale + .5f);
                g[x] = (unsigned)(pixels[x * channels + gOff] * outScale + .5f);
                b[x] = (unsigned)(pixels[x * channels + bOff] * outScale + .5f);
            }

            r += strideR / sizeof(T);
            g += strideG / sizeof(T);
            b += strideB / sizeof(T);
        }

        if (alphaFrame) {
            T *a = reinterpret_cast<T *>(vsapi->getWritePtr(alphaFrame, 0));
            ptrdiff_t strideA = vsapi->getStride(alphaFrame, 0);
            memset(a, 0, strideA  * height);
        }
    }
}

static void readSampleTypeDepth(const ReadData *d, const Magick::Image &image, VSSampleType &st, int &depth) {
        st = stInteger;
        depth = static_cast<int>(image.depth());
        if (depth == 32)
                st = stFloat;

        if (d->floatOutput || image.attribute("quantum:format") == "floating-point") {
                depth = 32;
                st = stFloat;
        }

        // VapourSynth does not support <8-bit integer types.
        if (depth < 8)
                depth = 8;
}

static std::string getVideoFormatName(const VSVideoFormat &f, const VSAPI *vsapi) {
    char name[32];
    if (vsapi->getVideoFormatName(&f, name))
        return name;
    else
        return "";
}

static const VSFrame *VS_CC readGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    ReadData *d = static_cast<ReadData *>(instanceData);

    if (activationReason == arInitial) {
        VSFrame *frame = nullptr;
        VSFrame *alphaFrame = nullptr;
        
        try {
            std::string filename = d->fileListMode ? d->filenames[n] : specialPrintf(d->filenames[0], n + d->firstNum);
            if (!isAbsolute(filename))
                filename = d->workingDir + filename;

            Magick::Image image(filename);
            VSColorFamily cf = cfRGB;
            if (image.colorSpace() == Magick::GRAYColorspace)
                cf = cfGray;

            int width = static_cast<int>(image.columns());
            int height = static_cast<int>(image.rows());
            size_t channels = image.channels();

            VSSampleType st;
            int depth;
            readSampleTypeDepth(d, image, st, depth);

            if (d->vi[0].format.colorFamily != cfUndefined && (cf != d->vi[0].format.colorFamily || depth != d->vi[0].format.bitsPerSample)) {
                VSVideoFormat tmp;
                vsapi->queryVideoFormat(&tmp, cf, st, depth, 0, 0, core);

                std::string err = "Read: Format mismatch for frame " + std::to_string(n) + ", is ";
                err += getVideoFormatName(tmp, vsapi) + std::string(" but should be ") + getVideoFormatName(d->vi[0].format, vsapi);
                vsapi->setFilterError(err.c_str(), frameCtx);
                return nullptr;
            }

            if (d->vi[0].width && (width != d->vi[0].width || height != d->vi[0].height)) {
                std::string err = "Read: Size mismatch for frame " + std::to_string(n) + ", is " + std::to_string(width) + "x" + std::to_string(height) + " but should be " + std::to_string(d->vi[0].width) + "x" + std::to_string(d->vi[0].height);
                vsapi->setFilterError(err.c_str(), frameCtx);
                return nullptr;
            }

            VSVideoFormat fformat;
            vsapi->queryVideoFormat(&fformat, cf, st, depth, 0, 0, core);
            frame = vsapi->newVideoFrame(&fformat, width, height, nullptr, core);

            if (d->alpha) {
                VSVideoFormat aformat;
                vsapi->queryVideoFormat(&aformat, cfGray, st, depth, 0, 0, core);
                alphaFrame = vsapi->newVideoFrame(&aformat, width, height, nullptr, core);
            }

            const VSVideoFormat *fi = vsapi->getVideoFrameFormat(frame);
 
            bool isGray = fi->colorFamily == cfGray;                
     
            if (fi->bytesPerSample == 4 && fi->sampleType == stFloat) {
                const Quantum scaleFactor = QuantumRange;
                Magick::Pixels pixelCache(image);

                float *r = reinterpret_cast<float *>(vsapi->getWritePtr(frame, 0));
                float *g = reinterpret_cast<float *>(vsapi->getWritePtr(frame, isGray ? 0 : 1));
                float *b = reinterpret_cast<float *>(vsapi->getWritePtr(frame, isGray ? 0 : 2));

                ptrdiff_t strideR = vsapi->getStride(frame, 0);
                ptrdiff_t strideG = vsapi->getStride(frame, isGray ? 0 : 1);
                ptrdiff_t strideB = vsapi->getStride(frame, isGray ? 0 : 2);

                ssize_t rOff = pixelCache.offset(MagickCore::RedPixelChannel);
                ssize_t gOff = pixelCache.offset(MagickCore::GreenPixelChannel);
                ssize_t bOff = pixelCache.offset(MagickCore::BluePixelChannel);

                if (alphaFrame) {
                    float *a = reinterpret_cast<float *>(vsapi->getWritePtr(alphaFrame, 0));
                    ptrdiff_t strideA = vsapi->getStride(alphaFrame, 0);
                    ssize_t aOff = pixelCache.offset(MagickCore::AlphaPixelChannel);

                    if (aOff >= 0) {
                        for (int y = 0; y < height; y++) {
                            const MagickCore::Quantum* pixels = pixelCache.getConst(0, y, width, 1);
                            for (int x = 0; x < width; x++) {
                                r[x] = pixels[x * channels + rOff] / scaleFactor;
                                g[x] = pixels[x * channels + gOff] / scaleFactor;
                                b[x] = pixels[x * channels + bOff] / scaleFactor;
                                a[x] = pixels[x * channels + aOff] / scaleFactor;
                            }

                            r += strideR / sizeof(float);
                            g += strideG / sizeof(float);
                            b += strideB / sizeof(float);
                            a += strideA / sizeof(float);
                        }
                    } else {
                        memset(a, 0, strideA  * height);
                    }
                } else {
                    for (int y = 0; y < height; y++) {
                        const MagickCore::Quantum* pixels = pixelCache.getConst(0, y, width, 1);
                        for (int x = 0; x < width; x++) {
                            r[x] = pixels[x * channels + rOff] / scaleFactor;
                            g[x] = pixels[x * channels + gOff] / scaleFactor;
                            b[x] = pixels[x * channels + bOff] / scaleFactor;
                        }

                        r += strideR / sizeof(float);
                        g += strideG / sizeof(float);
                        b += strideB / sizeof(float);
                    }
                }
            } else if (fi->bytesPerSample == 4) {
                readImageHelper<uint32_t>(frame, alphaFrame, isGray, image, width, height, fi->bitsPerSample, vsapi);
            } else if (fi->bytesPerSample == 2) {
                readImageHelper<uint16_t>(frame, alphaFrame, isGray, image, width, height, fi->bitsPerSample, vsapi);
            } else if (fi->bytesPerSample == 1) {
                readImageHelper<uint8_t>(frame, alphaFrame, isGray, image, width, height, fi->bitsPerSample, vsapi);
            }
#if defined(IMWRI_HAS_LCMS2)
            if (d->embedICC) {
                const MagickCore::StringInfo *icc_profile = MagickCore::GetImageProfile(image.constImage(), "icc");
                if (icc_profile) {
                    vsapi->mapSetData(vsapi->getFramePropertiesRW(frame), "ICCProfile", reinterpret_cast<const char *>(icc_profile->datum), icc_profile->length, dtBinary, maReplace);
                }
            }
#endif
        } catch (Magick::Exception &e) {
            vsapi->setFilterError((std::string("Read: ImageMagick error: ") + e.what()).c_str(), frameCtx);
            vsapi->freeFrame(frame);
            vsapi->freeFrame(alphaFrame);
            return nullptr;
        }

        if (alphaFrame)
            vsapi->mapConsumeFrame(vsapi->getFramePropertiesRW(frame), "_Alpha", alphaFrame, maAppend);
        return frame;
    }

    return nullptr;
}

static void VS_CC readFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    ReadData *d = static_cast<ReadData *>(instanceData);
    delete d;
}

static void VS_CC readCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<ReadData> d(new ReadData());
    int err = 0;

    initMagick(core, vsapi);

    d->firstNum = vsapi->mapGetIntSaturated(in, "firstnum", 0, &err);
    if (d->firstNum < 0) {
        vsapi->mapSetError(out, "Read: Frame number offset can't be negative");
        return;
    }

    d->alpha = !!vsapi->mapGetInt(in, "alpha", 0, &err);
    d->mismatch = !!vsapi->mapGetInt(in, "mismatch", 0, &err);
    d->floatOutput = !!vsapi->mapGetInt(in, "float_output", 0, &err);
#if defined(IMWRI_HAS_LCMS2)
    d->embedICC = !!vsapi->mapGetInt(in, "embed_icc", 0, &err);
#else
    d->embedICC = false;
#endif
    int numElem = vsapi->mapNumElements(in, "filename");
    d->filenames.resize(numElem);
    for (int i = 0; i < numElem; i++)
        d->filenames[i] = vsapi->mapGetData(in, "filename", i, nullptr);
    
    d->vi[0] = {{}, 30, 1, 0, 0, static_cast<int>(d->filenames.size())};
    // See if it's a single filename with number substitution and check how many files exist
    if (d->vi[0].numFrames == 1 && specialPrintf(d->filenames[0], 0) != d->filenames[0]) {
        d->fileListMode = false;

        for (int i = d->firstNum; i < INT_MAX; i++) {
            if (!fileExists(specialPrintf(d->filenames[0], i))) {
                d->vi[0].numFrames = i - d->firstNum;
                break;
            }
        }

        if (d->vi[0].numFrames == 0) {
            vsapi->mapSetError(out, "Read: No files matching the given pattern exist");
            return;
        }
    }

    try {
        Magick::Image image(d->fileListMode ? d->filenames[0] : specialPrintf(d->filenames[0], d->firstNum));

        VSSampleType st;
        int depth;
        readSampleTypeDepth(d.get(), image, st, depth);

        if (!d->mismatch || d->vi[0].numFrames == 1) {
            d->vi[0].height = static_cast<int>(image.rows());
            d->vi[0].width = static_cast<int>(image.columns());
            VSColorFamily cf = cfRGB;
            if (image.colorSpace() == Magick::GRAYColorspace)
                cf = cfGray;

            vsapi->queryVideoFormat(&d->vi[0].format, cf, st, depth, 0, 0, core);
        }

        if (d->alpha) {
            d->vi[1] = d->vi[0];
            if (d->vi[0].format.colorFamily != cfUndefined)
                vsapi->queryVideoFormat(&d->vi[1].format, cfGray, st, depth, 0, 0, core);
        }
    } catch (Magick::Exception &e) {
        vsapi->mapSetError(out, (std::string("Read: Failed to read image properties: ") + e.what()).c_str());
        return;
    }

    getWorkingDir(d->workingDir);

    vsapi->createVideoFilter(out, "Read", d->vi, readGetFrame, readFree, fmUnordered, nullptr, 0, d.get(), core);
    d.release();
}


//////////////////////////////////////////
// Init

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
    vspapi->configPlugin(IMWRI_ID, IMWRI_NAMESPACE, IMWRI_PLUGIN_NAME, VS_MAKE_VERSION(2, 0), VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction("Write", "clip:vnode;imgformat:data;filename:data;firstnum:int:opt;quality:int:opt;dither:int:opt;compression_type:data:opt;overwrite:int:opt;alpha:vnode:opt;", "clip:vnode;", writeCreate, nullptr, plugin);
    vspapi->registerFunction("Read", "filename:data[];firstnum:int:opt;mismatch:int:opt;alpha:int:opt;float_output:int:opt;embed_icc:int:opt;", "clip:vnode;", readCreate, nullptr, plugin);
    vspapi->registerFunction("EncodeFrame", "frame:vframe;imgformat:data;quality:int:opt;dither:int:opt;compression_type:data:opt;alpha:vframe:opt;", "bytes:data;", encodeFrame, nullptr, plugin);
}
