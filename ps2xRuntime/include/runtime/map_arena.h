#pragma once

#include <cstdint>
#include <mutex>
#include "runtime/ps2_memory.h"

// Guest addresses only. Host presentation/RGBA storage must stay outside this arena.
// A lease is a token, not an allocation or a lifetime extension for PS2Runtime.
// Initialize runtime memory before resolving base through memory().getRDRAM().
struct MapArenaLease
{
    uint32_t base = 0;
    uint32_t capacity = 0;
    uint64_t generation = 0;
    explicit operator bool() const { return generation != 0; }
};

// Exclusive ownership of the fixed reservation; owns no separate allocation.
// Callers must stop using arena bytes before release/reset/shutdown/destruction.
// The mutex protects tokens, not concurrent access to the arena bytes.
class MapArena
{
public:
    MapArenaLease claim(const void *owner);
    bool release(const void *owner, uint64_t generation);
    bool validate(const void *owner, uint64_t generation) const;
    void reset(); // Invalidate leases and enable claims, retaining the fixed reservation.
    void shutdown(); // Invalidate leases and disable claims until reset.

private:
    mutable std::mutex m_mutex;
    const void *m_owner = nullptr;
    uint64_t m_generation = 0;
    bool m_active = true;
};
