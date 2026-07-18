#include "TestAllocationCounter.h"

#include <cstdlib>
#include <new>

namespace
{
    thread_local bool allocationTracking = false;
    thread_local std::size_t allocationCount = 0;
    thread_local std::size_t allocatedBytes = 0;

    void recordAllocation (std::size_t bytes) noexcept
    {
        if (! allocationTracking)
            return;
        ++allocationCount;
        allocatedBytes += bytes;
    }

    void* allocate (std::size_t bytes)
    {
        const auto requested = bytes == 0 ? (std::size_t) 1 : bytes;
        if (void* memory = std::malloc (requested))
        {
            recordAllocation (requested);
            return memory;
        }
        throw std::bad_alloc();
    }

    void* allocateNoThrow (std::size_t bytes) noexcept
    {
        try
        {
            return allocate (bytes);
        }
        catch (const std::bad_alloc&)
        {
            return nullptr;
        }
    }
}

void* operator new (std::size_t bytes) { return allocate (bytes); }
void* operator new[] (std::size_t bytes) { return allocate (bytes); }
void* operator new (std::size_t bytes, const std::nothrow_t&) noexcept { return allocateNoThrow (bytes); }
void* operator new[] (std::size_t bytes, const std::nothrow_t&) noexcept { return allocateNoThrow (bytes); }
void operator delete (void* memory) noexcept { std::free (memory); }
void operator delete[] (void* memory) noexcept { std::free (memory); }
void operator delete (void* memory, std::size_t) noexcept { std::free (memory); }
void operator delete[] (void* memory, std::size_t) noexcept { std::free (memory); }
void operator delete (void* memory, const std::nothrow_t&) noexcept { std::free (memory); }
void operator delete[] (void* memory, const std::nothrow_t&) noexcept { std::free (memory); }

ScopedTestAllocationCounter::ScopedTestAllocationCounter() noexcept
    : wasTracking (allocationTracking)
{
    allocationCount = 0;
    allocatedBytes = 0;
    allocationTracking = true;
}

ScopedTestAllocationCounter::~ScopedTestAllocationCounter()
{
    allocationTracking = wasTracking;
}

TestAllocationSnapshot ScopedTestAllocationCounter::snapshot() const noexcept
{
    return { allocationCount, allocatedBytes };
}
