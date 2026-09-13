// SPDX-License-Identifier: BSD-3-Clause
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifndef _In_
#define _In_
#define _In_reads_(n)
#define _Out_writes_bytes_(n)
#define _In_reads_bytes_(n)
#endif
using BOOLEAN = bool;
using UINT = uint32_t;
using DWORD = uint32_t;
using ULONGLONG = uint64_t;
using LONGLONG = int64_t;
using SIZE_T = size_t;
using PUCHAR = unsigned char *;
using VOID = void;
constexpr bool FALSE = false, TRUE = true;
enum D3DDDIFORMAT { D3DDDIFMT_UNKNOWN = 0, D3DDDIFMT_A8R8G8B8 = 21,
                   D3DDDIFMT_X8R8G8B8 = 22, D3DDDIFMT_A8B8G8R8 = 32 };
constexpr UINT VIOGPU_WDDM_PRESENT_RECTS_PER_PASS = 256;
struct RECT { int32_t left, top, right, bottom; };
struct VIOGPU_WDDM_ALLOCATION {
    D3DDDIFORMAT Format;
    UINT Width, Height, Pitch;
    ULONGLONG BackingSize;
    void *ApertureAddress;
};
struct VIOGPU_WDDM_PRESENT_TRANSACTION {
    VIOGPU_WDDM_ALLOCATION *Source, *Destination;
    RECT SourceRect, DestinationRect;
    RECT *DestinationSubRects;
    UINT RectCount;
};
static void RtlCopyMemory(void *dst, const void *src, SIZE_T size) { std::memcpy(dst, src, size); }

// INSERT_PRODUCTION

static void ExecuteCopy(VIOGPU_WDDM_PRESENT_TRANSACTION *transaction)
{
    auto *source = transaction->Source;
    auto *destination = transaction->Destination;
    auto *sourceBase = static_cast<PUCHAR>(source->ApertureAddress);
    auto *destinationBase = static_cast<PUCHAR>(destination->ApertureAddress);
    // INSERT_COPY
}

static unsigned failures, cases;
static void check(bool ok, const char *name)
{
    ++cases;
    if (!ok) { ++failures; std::printf("FAIL %s\n", name); }
}

static bool color_copy(D3DDDIFORMAT sourceFormat, D3DDDIFORMAT destinationFormat)
{
    // Different pitches, unaligned row starts, nonzero source/destination origins,
    // two disjoint clips, untouched borders and a nonopaque alpha byte.
    std::vector<unsigned char> src(4 * 31, 0xcc), dst(6 * 37, 0x9d);
    for (UINT y = 0; y < 4; ++y) {
        for (UINT x = 0; x < 6; ++x) {
            unsigned char *p = src.data() + y * 31 + x * 4;
            const unsigned char red = static_cast<unsigned char>(0x21 + 7 * x);
            const unsigned char green = static_cast<unsigned char>(0x42 + 3 * y);
            const unsigned char blue = static_cast<unsigned char>(0xc8 - 2 * x - y);
            p[0] = sourceFormat == D3DDDIFMT_A8B8G8R8 ? red : blue;
            p[1] = green;
            p[2] = sourceFormat == D3DDDIFMT_A8B8G8R8 ? blue : red;
            p[3] = static_cast<unsigned char>(0x17 + x + y);
        }
    }
    const auto original = src;
    VIOGPU_WDDM_ALLOCATION s{sourceFormat, 6, 4, 31, src.size(), src.data()};
    VIOGPU_WDDM_ALLOCATION d{destinationFormat, 8, 6, 37, dst.size(), dst.data()};
    RECT clips[]{{2, 2, 4, 3}, {5, 3, 6, 4}};
    VIOGPU_WDDM_PRESENT_TRANSACTION tx{&s, &d, {1, 1, 5, 3}, {2, 2, 6, 4}, clips, 2};
    if (!ValidatePresentGeometry(&s, &d, &tx.SourceRect, &tx.DestinationRect, clips, 2)) return false;
    ExecuteCopy(&tx);
    if (src != original) return false;
    for (UINT y = 0; y < 6; ++y) {
        for (UINT offset = 0; offset < 37; ++offset) {
            UINT x = offset / 4, lane = offset % 4;
            bool copied = (y == 2 && x >= 2 && x < 4) || (y == 3 && x == 5);
            if (!copied) { if (dst[y * 37 + offset] != 0x9d) return false; continue; }
            const UINT sourceX = x - 1, sourceY = y - 1;
            const unsigned char rgba[]{static_cast<unsigned char>(0x21 + 7 * sourceX),
                                       static_cast<unsigned char>(0x42 + 3 * sourceY),
                                       static_cast<unsigned char>(0xc8 - 2 * sourceX - sourceY),
                                       static_cast<unsigned char>(sourceFormat == D3DDDIFMT_X8R8G8B8 ? 0xff :
                                                                  0x17 + sourceX + sourceY)};
            if (lane == 3 && destinationFormat == D3DDDIFMT_X8R8G8B8) continue;
            UINT component = destinationFormat == D3DDDIFMT_A8B8G8R8 || lane == 1 || lane == 3 ? lane : 2 - lane;
            if (dst[y * 37 + offset] != rgba[component]) return false;
        }
    }
    return true;
}

int main()
{
    const D3DDDIFORMAT formats[]{D3DDDIFMT_A8R8G8B8, D3DDDIFMT_X8R8G8B8, D3DDDIFMT_A8B8G8R8};
    const char *names[]{"BGRA", "BGRX", "RGBA"};
    for (unsigned source = 0; source < 3; ++source) {
        for (unsigned destination = 0; destination < 3; ++destination) {
            char name[64];
            std::snprintf(name, sizeof(name), "%s to %s", names[source], names[destination]);
            check(color_copy(formats[source], formats[destination]), name);
        }
    }
    VIOGPU_WDDM_ALLOCATION source{D3DDDIFMT_A8R8G8B8, 4, 4, 16, 64, nullptr};
    auto destination = source;
    RECT area{0, 0, 4, 4}, clipped{1, 1, 3, 3};
    auto valid = [&]() { return ValidatePresentGeometry(&source, &destination, &area, &area, &clipped, 1); };
    source.Format = D3DDDIFMT_UNKNOWN; check(!valid(), "unknown source format");
    source.Format = D3DDDIFMT_A8R8G8B8;
    destination.Format = D3DDDIFMT_UNKNOWN; check(!valid(), "unknown destination format");
    destination.Format = D3DDDIFMT_A8R8G8B8;
    source.Pitch = 15; check(!valid(), "short source row"); source.Pitch = 16;
    destination.Pitch = 15; check(!valid(), "short destination row"); destination.Pitch = 16;
    source.BackingSize = 63; check(!valid(), "short source backing"); source.BackingSize = 64;
    destination.BackingSize = 63; check(!valid(), "short destination backing"); destination.BackingSize = 64;
    source.Width = UINT32_MAX; check(!valid(), "source width arithmetic"); source.Width = 4;
    destination.Width = UINT32_MAX; check(!valid(), "destination width arithmetic"); destination.Width = 4;
    clipped.left = -1; check(!valid(), "negative clip"); clipped.left = 1;
    clipped.right = 5; check(!valid(), "clip past backing"); clipped.right = 3;
    RECT stretch{0, 0, 3, 4};
    check(!ValidatePresentGeometry(&source, &destination, &stretch, &area, &clipped, 1), "unsupported stretch");
    check(!ValidatePresentGeometry(&source, &destination, &area, &area, &clipped, 0), "zero clip count");
    check(!ValidatePresentGeometry(&source, &destination, &area, &area, &clipped, 257), "excess clip count");
    check(!ValidatePresentGeometry(nullptr, &destination, &area, &area, &clipped, 1), "null source");
    std::printf("%u cases, %u failures\n", cases, failures);
    return failures ? 1 : 0;
}
