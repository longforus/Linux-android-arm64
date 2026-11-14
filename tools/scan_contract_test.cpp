// Host: g++ -std=c++20 -pthread tools/scan_contract_test.cpp -o /tmp/scan-contract-test
#include <iostream>
#define LS_LOGE_TAG_FMT(...) ((void)0)
#include "../android/jni/include/memory/mem_scanner.h"
#include <cassert>
#include <chrono>

struct Memory {
    uintptr_t base = 0x10000;
    std::vector<uint8_t> data = std::vector<uint8_t>(12288, 0);
    uintptr_t failedPage = 0;
    int GetGlobalPid() { return 42; }
    auto GetScanRegions(bool) { return std::vector<std::pair<uintptr_t, uintptr_t>>{{base, base + data.size()}}; }
    int Read(uintptr_t address, void *out, size_t size) {
        if (address < base || address - base > data.size() || size > data.size() - (address - base)) return -1;
        if (failedPage && address < failedPage + 4096 && address + size > failedPage) return -1;
        std::memcpy(out, data.data() + address - base, size);
        return static_cast<int>(size);
    }
    template<class T> void put(size_t offset, T value) { std::memcpy(data.data() + offset, &value, sizeof(value)); }
};
int main() {
    using namespace MemUtils;
    using Types::DataType; using Types::FuzzyMode;
    assert(!ParseScanValue<int8_t>("128"));
    assert(!ParseScanValue<float>("nan"));
    assert(!ParseScanValue<float>("1e100"));
    auto range = ParseFloatScanRange<float>("1.2");
    assert(range && Compare(1.24f, 1.2f, FuzzyMode::Equal, 0.f, 0.f, *range));
    assert(!Compare(1.26f, 1.2f, FuzzyMode::Equal, 0.f, 0.f, *range));
    auto exact = ParseFloatScanRange<float>("1.2", FloatRounding::Exact);
    assert(exact && !Compare(1.24f, 1.2f, FuzzyMode::Equal, 0.f, 0.f, *exact));
    Config::ThreadPools pools; Config::InstallThreadPools(pools);
    Memory memory; MemScanner scanner(memory);
    auto wait = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (scanner.isScanning() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        assert(!scanner.isScanning());
    };
    constexpr int32_t needle = 0x12345678;
    memory.put(1, needle); memory.put(4094, needle); memory.put(8200, needle);
    assert(scanner.startAsync<int32_t>(42, needle, DataType::I32, FuzzyMode::Equal, true, 0, {}, FloatRounding::Exact, 1)); wait();
    auto results = scanner.getPage(0, 10);
    assert((results == std::vector<uintptr_t>{memory.base + 1, memory.base + 4094, memory.base + 8200}));
    // Refinement retains alignment and rejects a change to it.
    assert(!scanner.startAsync<int32_t>(42, needle, DataType::I32, FuzzyMode::Equal, false, 0, {}, FloatRounding::Exact, 4));
    memory.put(1, int32_t{0});
    assert(scanner.startAsync<int32_t>(42, needle, DataType::I32, FuzzyMode::Equal, false)); wait();
    assert(scanner.count() == 2);
    scanner.clear(); memory.failedPage = memory.base + 4096;
    assert(scanner.startAsync<int32_t>(42, needle, DataType::I32, FuzzyMode::Equal, true, 0, {}, FloatRounding::Exact, 1)); wait();
    assert((scanner.getPage(0, 10) == std::vector<uintptr_t>{memory.base + 8200}));
    scanner.clear(); memory.failedPage = 0;
    assert(scanner.startAsync<int32_t>(42, needle, DataType::I32, FuzzyMode::Unknown, true, 0, {}, FloatRounding::Exact, 1)); wait();
    assert(scanner.count() == memory.data.size() - sizeof(int32_t) + 1);
    assert(scanner.startAsync<int32_t>(42, needle, DataType::I32, FuzzyMode::Equal, false)); wait();
    assert(scanner.count() == 2);
    scanner.clear(); std::fill(memory.data.begin(), memory.data.end(), 0);
    memory.put(1, 1.24f); memory.put(4094, 1.26f); memory.put(8200, 1.2f);
    assert(scanner.startAsync<float>(42, 1.2f, DataType::Float, FuzzyMode::Equal, true, 0.f, "1.2", FloatRounding::Rounded, 1)); wait();
    assert((scanner.getPage(0, 10) == std::vector<uintptr_t>{memory.base + 1, memory.base + 8200}));
    assert(scanner.startAsync<float>(42, 1.2f, DataType::Float, FuzzyMode::Equal, false, 0.f, "1.2", FloatRounding::Exact)); wait();
    assert((scanner.getPage(0, 10) == std::vector<uintptr_t>{memory.base + 8200}));
    Config::UninstallThreadPools(pools); pools.shutdown();
    std::cout << "Scan float, alignment, cross-page, read failure and refinement contracts passed\n";
}
