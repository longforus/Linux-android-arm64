#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace lsdebug
{
struct ReadSegment
{
    uint64_t address;
    size_t offset;
    size_t size;
    int status;
    bool valid;
};
struct ReadResult
{
    std::vector<uint8_t> bytes;
    std::vector<ReadSegment> segments;
    size_t validBytes = 0;
    bool complete() const { return validBytes == bytes.size(); }
};

// 4K is a conservative subdivision of Android's 4K/16K/64K pages. A failed
// request never contributes bytes: the old protocol reports a count, not a mask.
template <typename Read> ReadResult ReadWithIntegrity(uint64_t address, size_t size, Read read)
{
    if (!size || size > 1024 * 1024 || address > std::numeric_limits<uint64_t>::max() - (size - 1))
        throw std::invalid_argument("invalid memory range (maximum 1 MiB)");
    ReadResult result;
    result.bytes.resize(size, 0);
    for (size_t offset = 0; offset < size;)
    {
        const uint64_t current = address + offset;
        const size_t count = std::min(size - offset, size_t(4096 - (current & 4095)));
        const int status = read(current, result.bytes.data() + offset, count);
        const bool valid = status == static_cast<int>(count);
        if (valid) result.validBytes += count;
        else std::fill_n(result.bytes.data() + offset, count, uint8_t(0));
        result.segments.push_back({current, offset, count, status, valid});
        offset += count;
    }
    return result;
}
}
