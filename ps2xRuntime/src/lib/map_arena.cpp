#include "runtime/map_arena.h"
#include <atomic>

namespace
{
    // Unique across runtime instances, including reconstruction at the same address.
    std::atomic<uint64_t> nextGeneration{1};
}

MapArenaLease MapArena::claim(const void *owner)
{
    if (PS2_MAP_ARENA_SIZE == 0u)
        return {};
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_active || !owner || (m_owner && m_owner != owner))
        return {};
    if (!m_owner)
    {
        // Refuse further claims on exhaustion rather than recycle a stale token.
        uint64_t next = nextGeneration.load(std::memory_order_relaxed);
        do
        {
            if (next == UINT64_MAX)
                return {};
        } while (!nextGeneration.compare_exchange_weak(next, next + 1, std::memory_order_relaxed));
        m_generation = next;
        m_owner = owner;
    }
    return {PS2_MAP_ARENA_BASE, PS2_MAP_ARENA_SIZE, m_generation};
}

bool MapArena::release(const void *owner, uint64_t generation)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!owner || m_owner != owner || !generation || m_generation != generation)
        return false;
    m_owner = nullptr;
    m_generation = 0;
    return true;
}

bool MapArena::validate(const void *owner, uint64_t generation) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_active && owner && m_owner == owner && generation && m_generation == generation;
}

void MapArena::reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_owner = nullptr;
    m_generation = 0;
    m_active = true;
}

void MapArena::shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_owner = nullptr;
    m_generation = 0;
    m_active = false;
}
