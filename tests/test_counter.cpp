#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "core/counter.h"

int main() {
    // Cross the 32-bit boundary so replacing statistics with uint32_t cannot
    // silently pass. Writers contend with the same reader used by the UI.
    constexpr uint64_t initial = (uint64_t{1} << 32) - 10;
    constexpr int writers = 4;
    constexpr int increments = 50000;
    tsl::Counter64 counter(initial);
    std::atomic<bool> done{false};
    bool monotonic = true;
    std::thread reader([&] {
        uint64_t previous = initial;
        while (!done) {
            uint64_t current = counter.load();
            if (current < previous) monotonic = false;
            previous = current;
        }
    });
    std::vector<std::thread> threads;
    for (int i = 0; i < writers; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < increments; ++j) counter += 3;
        });
    }
    for (auto &thread : threads) thread.join();
    done = true;
    reader.join();
    uint64_t expected = initial + uint64_t{writers} * increments * 3;
    if (!monotonic || counter.load() != expected || counter++ != expected || ++counter != expected + 2) {
        std::fprintf(stderr, "64-bit counter concurrency/overflow test failed\n");
        return 1;
    }
    std::puts("64-bit counter: concurrent reads/writes and 32-bit boundary passed");
}
