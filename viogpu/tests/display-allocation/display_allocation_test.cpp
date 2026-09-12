#include "../../shared/viogpu_display_allocation.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static unsigned checks;
static void require(bool condition, const char *name)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL %s\n", name); std::exit(1); }
}

static VIOGPU_DVSA_DISCOVERY parse(const void *bytes, size_t size, uint64_t pci)
{
    VIOGPU_DVSA_DISCOVERY output = {};
    require(VioGpuParseDisplayAllocationDiscovery(bytes, size, pci, &output) != 0, "valid host fixture");
    return output;
}

static void rejected(const void *bytes, size_t size, uint64_t pci, const char *name)
{
    struct Guarded {
        uint64_t before;
        VIOGPU_DVSA_DISCOVERY output;
        uint64_t after;
    } value;
    std::memset(&value, 0xa5, sizeof(value));
    const Guarded original = value;
    require(!VioGpuParseDisplayAllocationDiscovery(bytes, size, pci, &value.output), name);
    require(std::memcmp(&value, &original, sizeof(value)) == 0, "rejection never publishes or overruns output");
}

static void reject(const VIOGPU_DVSA_DISCOVERY &value, uint64_t pci, const char *name)
{
    rejected(&value, sizeof(value), pci, name);
}

int main(int argc, char **argv)
{
    require(argc == 1 || argc == 2, "optional fixture directory only");
    const std::string directory = argc == 2 ? argv[1] : "fixtures";
    struct Expected {
        const char *file;
        uint64_t pci, bar, prefix, capacity, alignment, generation, reasons, triple;
    };
    const Expected expected[] = {
        {"absent.bin", 8388608, 0, 0, 0, 0, 0, 0x1f9, 0},
        {"bar8m_reserved8m_align16k.bin", 8388608, 8388608, 8388608, 0, 16384, 17, 0x1f6, 69500928},
        {"bar128m_reserved8m_align4k.bin", 134217728, 134217728, 8388608, 125829120, 4096, 17, 0x1f0, 69464064},
        {"bar128m_reserved8m_align16k.bin", 134217728, 134217728, 8388608, 125829120, 16384, 17, 0x1f0, 69500928},
    };
    VIOGPU_DVSA_HEADER query;
    std::memset(&query, 0xff, sizeof(query));
    VioGpuInitializeDisplayAllocationQuery(&query);
    VIOGPU_DVSA_HEADER expectedQuery = {};
    expectedQuery.hdr.type = DVSA_CMD_DISCOVER;
    expectedQuery.magic = DVSA_MAGIC; expectedQuery.version = 1; expectedQuery.size = 64;
    require(std::memcmp(&query, &expectedQuery, sizeof(query)) == 0, "query initializes exact unfenced64byte identity");
    VioGpuInitializeDisplayAllocationQuery(nullptr);

    for (const auto &e : expected) {
        std::ifstream file(directory + "/" + e.file, std::ios::binary);
        require(file.good(), "actual host fixture opened");
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        require(bytes.size() == 128, "immutable actual host fixture is128bytes");
        const auto value = parse(bytes.data(), bytes.size(), e.pci);
        require(value.bar_size == e.bar && value.reserved_prefix == e.prefix && value.dynamic_capacity == e.capacity &&
                value.mapping_alignment == e.alignment && value.query.surface_generation == e.generation &&
                value.unavailable_reasons == e.reasons && value.triple_bytes_lower_bound == e.triple,
                "actual host fixture fields match recorded producer inputs");
        require(value.feature_bits == 0 && value.min_allocations == 3 && value.max_allocations == 0,
                "frozen host fixture grants no allocation or fullSDR admission");
        require(std::memcmp(&value, bytes.data(), sizeof(value)) == 0, "exact decoded host bytes preserved");

        // Actual bounded buffers make speculative reads on short input visible
        // to ASan. Every prefix and a trailing-byte response must be rejected.
        for (size_t length = 0; length < bytes.size(); ++length) {
            std::vector<uint8_t> shortBytes(bytes.begin(), bytes.begin() + length);
            rejected(shortBytes.data(), length, e.pci, "truncated response");
        }
        bytes.push_back(0);
        rejected(bytes.data(), bytes.size(), e.pci, "trailing response bytes");
        rejected(bytes.data(), SIZE_MAX, e.pci, "overflowed used length");
        rejected(nullptr, 128, e.pci, "null input");
        require(!VioGpuParseDisplayAllocationDiscovery(bytes.data(), 128, e.pci, nullptr), "null output");
        // Transport buffers need not be aligned to a native u64 boundary.
        std::vector<uint8_t> unaligned(129);
        std::memcpy(unaligned.data() + 1, &value, sizeof(value));
        const auto unalignedValue = parse(unaligned.data() + 1, 128, e.pci);
        require(std::memcmp(&unalignedValue, &value, sizeof(value)) == 0, "unaligned input uses byte parser");

        // Every bit of fixed header/identity bytes is validated. Surface
        // generation40..47 is semantic data and has separate coherence checks.
        for (size_t offset = 0; offset < sizeof(VIOGPU_DVSA_HEADER); ++offset) {
            if (offset >= 40 && offset < 48) continue;
            for (unsigned bit = 0; bit < 8; ++bit) {
                auto malformed = value;
                reinterpret_cast<uint8_t *>(&malformed)[offset] ^= static_cast<uint8_t>(1U << bit);
                reject(malformed, e.pci, "mutated fixed header identity");
            }
        }
        for (unsigned bit = 0; bit < 64; ++bit) {
            if (bit == 1) continue;
            auto malformed = value; malformed.feature_bits = UINT64_C(1) << bit;
            reject(malformed, e.pci, "fullSDR or unknown feature bit");
        }
        for (unsigned bit = 9; bit < 64; ++bit) {
            auto malformed = value; malformed.unavailable_reasons |= UINT64_C(1) << bit;
            reject(malformed, e.pci, "unknown reason bit");
        }
        auto bad = value; bad.min_allocations = 0; reject(bad, e.pci, "missing triple contract");
        bad = value; bad.max_allocations = 3; reject(bad, e.pci, "allocation count without feature");
        bad = value; bad.query.surface_generation = e.generation ? 0 : 17;
        reject(bad, e.pci, "surface reason and identity disagree");
        if (e.bar == 0) {
            for (size_t offset = 80; offset < 128; ++offset) {
                if (offset >= 112 && offset < 116) continue;
                bad = value; reinterpret_cast<uint8_t *>(&bad)[offset] ^= 1;
                reject(bad, e.pci, "missing mapper must expose zero mapping data");
            }
        } else {
            reject(value, 0, "missing independently observed PCI region");
            reject(value, e.pci + 4096, "host size differs from PCI capability");
            bad = value; bad.reserved_prefix = UINT64_MAX; reject(bad, e.pci, "reserved prefix overflow");
            bad = value; bad.dynamic_capacity = UINT64_MAX; reject(bad, e.pci, "capacity overflow");
            bad = value; bad.reserved_prefix += 1; bad.dynamic_capacity -= 1;
            reject(bad, e.pci, "unaligned reserved prefix");
            for (uint64_t alignment : {UINT64_C(0), UINT64_C(1), UINT64_C(2048), UINT64_C(12288), UINT64_MAX}) {
                bad = value; bad.mapping_alignment = alignment; reject(bad, e.pci, "invalid host page alignment");
            }
            bad = value; bad.unavailable_reasons ^= DVSA_UNAVAILABLE_NO_SUFFIX;
            reject(bad, e.pci, "suffix reason contradicts actual capacity");
            bad = value; bad.unavailable_reasons ^= DVSA_UNAVAILABLE_TRIPLE_CAPACITY;
            reject(bad, e.pci, "triple reason contradicts actual lower bound");
            bad = value; ++bad.triple_bytes_lower_bound; reject(bad, e.pci, "malformed triple lower bound");
            bad = value; bad.triple_bytes_lower_bound = UINT64_MAX;
            reject(bad, e.pci, "overflowed triple lower bound");
        }

        if (e.capacity != 0) {
            // Derived semantic controls, explicitly not new host-produced
            // runtime fixtures: current source may expose mapping-only bit1.
            auto mapping = value;
            mapping.feature_bits = DVSA_FEATURE_ALLOCATION_MAPPING;
            mapping.max_allocations = 3;
            mapping.unavailable_reasons = DVSA_UNAVAILABLE_PRODUCER_BRIDGE | DVSA_UNAVAILABLE_CONSUMER_BRIDGE;
            const auto accepted = parse(&mapping, sizeof(mapping), e.pci);
            require(accepted.feature_bits == 2 && (accepted.feature_bits & DVSA_FEATURE_SDR_ZERO_COPY) == 0,
                    "mapping-only observation never implies fullSDR");
            for (unsigned bit : {0U, 1U, 2U, 3U, 4U, 5U, 8U}) {
                bad = mapping; bad.unavailable_reasons |= UINT64_C(1) << bit;
                reject(bad, e.pci, "mapping feature contradicts missing prerequisite");
            }
            for (uint32_t count : {0U, 1U, 2U, 4U, UINT32_MAX}) {
                bad = mapping; bad.max_allocations = count; reject(bad, e.pci, "mapping quota outside fixedv1 bound");
            }
            bad = mapping; bad.feature_bits |= DVSA_FEATURE_SDR_ZERO_COPY;
            reject(bad, e.pci, "fullSDR stays unadmitted even with mapping capability");
        }
        std::printf("HOST_DVSA %s reasons=0x%llx bar=%llu capacity=%llu alignment=%llu feature_bits=0 PASS\n",
            e.file, static_cast<unsigned long long>(value.unavailable_reasons),
            static_cast<unsigned long long>(value.bar_size), static_cast<unsigned long long>(value.dynamic_capacity),
            static_cast<unsigned long long>(value.mapping_alignment));
    }
    std::printf("DVSA discovery: %u checks,4 actual host fixtures,strict negative/bounds/output ownership PASS\n", checks);
}
