#pragma once

#include <cstdint>

// Adaptive kick-envelope region used by Layer A and persisted by Layer B.
// The value is intentionally small and trivially copyable for fixed map storage.
enum class ConflictRegion : uint8_t
{
    none = 0,
    attack,
    body,
    tail
};
