#include "TestAllocationCounter.h"

#include <cstdlib>
#include <new>

#if defined (_WIN32)
 #include <malloc.h>
#endif

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

    void* allocateAligned (std::size_t bytes, std::align_val_t alignment)
    {
        const auto requested = bytes == 0 ? (std::size_t) 1 : bytes;
        void* memory = nullptr;

#if defined (_WIN32)
        memory = _aligned_malloc (requested, static_cast<std::size_t> (alignment));
#else
        if (posix_memalign (&memory, static_cast<std::size_t> (alignment), requested) != 0)
            memory = nullptr;
#endif

        if (memory != nullptr)
        {
            recordAllocation (requested);
            return memory;
        }

        throw std::bad_alloc();
    }

    void* allocateAlignedNoThrow (std::size_t bytes, std::align_val_t alignment) noexcept
    {
        try
        {
            return allocateAligned (bytes, alignment);
        }
        catch (const std::bad_alloc&)
        {
            return nullptr;
        }
    }

    void deallocateAligned (void* memory) noexcept
    {
#if defined (_WIN32)
        _aligned_free (memory);
#else
        std::free (memory);
#endif
    }
}

void* operator new (std::size_t bytes) { return allocate (bytes); }
void* operator new[] (std::size_t bytes) { return allocate (bytes); }
void* operator new (std::size_t bytes, const std::nothrow_t&) noexcept { return allocateNoThrow (bytes); }
void* operator new[] (std::size_t bytes, const std::nothrow_t&) noexcept { return allocateNoThrow (bytes); }
void* operator new (std::size_t bytes, std::align_val_t alignment) { return allocateAligned (bytes, alignment); }
void* operator new[] (std::size_t bytes, std::align_val_t alignment) { return allocateAligned (bytes, alignment); }
void* operator new (std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept { return allocateAlignedNoThrow (bytes, alignment); }
void* operator new[] (std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept { return allocateAlignedNoThrow (bytes, alignment); }
void operator delete (void* memory) noexcept { std::free (memory); }
void operator delete[] (void* memory) noexcept { std::free (memory); }
void operator delete (void* memory, std::size_t) noexcept { std::free (memory); }
void operator delete[] (void* memory, std::size_t) noexcept { std::free (memory); }
void operator delete (void* memory, const std::nothrow_t&) noexcept { std::free (memory); }
void operator delete[] (void* memory, const std::nothrow_t&) noexcept { std::free (memory); }
void operator delete (void* memory, std::align_val_t) noexcept { deallocateAligned (memory); }
void operator delete[] (void* memory, std::align_val_t) noexcept { deallocateAligned (memory); }
void operator delete (void* memory, std::size_t, std::align_val_t) noexcept { deallocateAligned (memory); }
void operator delete[] (void* memory, std::size_t, std::align_val_t) noexcept { deallocateAligned (memory); }
void operator delete (void* memory, std::align_val_t, const std::nothrow_t&) noexcept { deallocateAligned (memory); }
void operator delete[] (void* memory, std::align_val_t, const std::nothrow_t&) noexcept { deallocateAligned (memory); }

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

bool ScopedTestAllocationCounter::isTracking() noexcept
{
    return allocationTracking;
}

namespace
{
    // A global, externally-visible sink the pointer is stored into. Combined
    // with living in a separate translation unit from the allocation, a
    // non-LTO compiler cannot prove the pointer is unobserved and so cannot
    // eliminate the new/delete pair that produced it.
    const volatile void* allocationElisionSink = nullptr;
}

void preventAllocationElision (const volatile void* pointer) noexcept
{
    allocationElisionSink = pointer;
}
