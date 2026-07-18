#pragma once

#include <cstddef>

struct TestAllocationSnapshot
{
    std::size_t count = 0;
    std::size_t bytes = 0;
};

class ScopedTestAllocationCounter
{
public:
    ScopedTestAllocationCounter() noexcept;
    ~ScopedTestAllocationCounter();

    ScopedTestAllocationCounter (const ScopedTestAllocationCounter&) = delete;
    ScopedTestAllocationCounter& operator= (const ScopedTestAllocationCounter&) = delete;

    TestAllocationSnapshot snapshot() const noexcept;
    static bool isTracking() noexcept;

private:
    bool wasTracking = false;
};

// Defined in a separate translation unit so an optimizing compiler cannot
// prove the pointer never escapes and eliminate the new/delete pair that
// produced it (observed on macOS Release/Clang for tight scopes that only
// allocate-then-immediately-delete with no other use of the pointer).
void preventAllocationElision (const volatile void* pointer) noexcept;
