#pragma once

#include <cstdint>
#include <mutex>

namespace tsl {

// Tizen 5.5's ARM WebAssembly compiler crashes on i64.atomic instructions, even
// before main() runs. Keep the full 64-bit statistics behind a mutex, whose
// Emscripten implementation uses supported 32-bit atomics. Never call platform
// APIs or proxy to the browser thread while holding this lock.
class Counter64 {
public:
    explicit Counter64(uint64_t value = 0) : value_(value) {}

    uint64_t load() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return value_;
    }

    uint64_t operator++() { return Add(1) + 1; }
    uint64_t operator++(int) { return Add(1); }
    uint64_t operator+=(uint64_t amount) { return Add(amount) + amount; }

private:
    uint64_t Add(uint64_t amount) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t previous = value_;
        value_ += amount;
        return previous;
    }

    mutable std::mutex mutex_;
    uint64_t value_;
};

}  // namespace tsl
