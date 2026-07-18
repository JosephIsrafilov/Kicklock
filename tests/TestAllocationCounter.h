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

private:
    bool wasTracking = false;
};
