#include "MiniTest.h"
#include "ps2_runtime.h"
#include "runtime/map_arena.h"

void register_map_arena_tests()
{
    MiniTest::Case("MapArena", [](TestCase &tc) {
        if (PS2_MAP_ARENA_SIZE == 0u) {
            tc.Run("claims are disabled without expanded viewport", [](TestCase &t) {
                PS2Runtime rt;
                int owner = 0;
                t.IsTrue(!rt.claimMapArena(&owner), "disabled arena rejects claims");
            });
            return;
        }
        tc.Run("exclusive idempotent claims and generations", [](TestCase &t) {
            PS2Runtime rt;
            int owner = 0, other = 0;
            t.IsTrue(!rt.claimMapArena(nullptr), "null owner rejected");
            auto lease = rt.claimMapArena(&owner);
            t.IsTrue(bool(lease), "claim succeeds");
            t.Equals(lease.base, PS2_MAP_ARENA_BASE, "fixed guest base");
            t.Equals(lease.capacity, 8u * 1024u * 1024u, "8 MiB capacity");
            t.Equals(rt.claimMapArena(&owner).generation, lease.generation, "repeat is idempotent");
            t.IsTrue(!rt.claimMapArena(&other), "second owner rejected");
            t.IsTrue(!rt.validateMapArena(nullptr, lease.generation), "null validation rejected");
            t.IsTrue(!rt.validateMapArena(&other, lease.generation), "wrong owner rejected");
            t.IsTrue(!rt.releaseMapArena(nullptr, lease.generation), "null release rejected");
            t.IsTrue(!rt.releaseMapArena(&other, lease.generation), "wrong release rejected");
            t.IsTrue(!rt.releaseMapArena(&owner, lease.generation + 1), "stale release rejected");
            t.IsTrue(rt.validateMapArena(&owner, lease.generation), "bad operations preserve owner");
            t.IsTrue(rt.releaseMapArena(&owner, lease.generation), "release succeeds");
            t.IsTrue(!rt.releaseMapArena(&owner, lease.generation), "double release rejected");
            auto next = rt.claimMapArena(&owner);
            t.IsTrue(next.generation != lease.generation, "reclaim advances generation");
            t.IsTrue(!rt.validateMapArena(&owner, lease.generation), "old lease rejected");
            t.IsTrue(rt.releaseMapArena(&owner, next.generation), "reclaimed release succeeds");
            t.IsTrue(bool(rt.claimMapArena(&other)), "ownership transferable after release");
        });
        tc.Run("heap configuration preserves arena and contents", [](TestCase &t) {
            PS2Runtime rt;
            int owner = 0;
            t.IsTrue(rt.memory().initialize(), "memory initialized");
            auto lease = rt.claimMapArena(&owner);
            auto *bytes = rt.memory().getRDRAM() + lease.base;
            bytes[0] = 0x53;
            bytes[lease.capacity - 1] = 0x72;
            rt.configureGuestHeap(0x100000, PS2_RAM_SIZE);
            rt.configureGuestHeap(0x200000, PS2_RAM_SIZE);
            auto same = rt.claimMapArena(&owner);
            t.IsTrue(same.base == lease.base && same.capacity == lease.capacity && same.generation == lease.generation,
                     "heap changes preserve descriptor and lease");
            t.IsTrue(rt.validateMapArena(&owner, lease.generation), "lease remains valid");
            t.IsTrue(bytes[0] == 0x53 && bytes[lease.capacity - 1] == 0x72, "contents preserved");
        });
        tc.Run("core rebind invalidates leases while repeated sync preserves them", [](TestCase &t) {
            PS2Runtime rt;
            int owner = 0;
            auto before = rt.claimMapArena(&owner);
            t.IsTrue(rt.memory().initialize(), "memory initialized");
            t.IsTrue(rt.syncCoreSubsystems(), "core bound");
            t.IsTrue(!rt.validateMapArena(&owner, before.generation), "core binding invalidates lease");
            auto bound = rt.claimMapArena(&owner);
            t.IsTrue(rt.syncCoreSubsystems(), "repeated sync succeeds");
            t.IsTrue(rt.validateMapArena(&owner, bound.generation), "no-op sync preserves lease");
        });
        tc.Run("reset shutdown and runtime lifetime invalidate leases", [](TestCase &t) {
            int owner = 0;
            uint64_t previous = 0;
            {
                PS2Runtime rt;
                auto first = rt.claimMapArena(&owner);
                rt.resetMapArena();
                t.IsTrue(!rt.validateMapArena(&owner, first.generation), "reset invalidates");
                auto next = rt.claimMapArena(&owner);
                t.IsTrue(next.generation != first.generation, "reset changes generation");
                rt.shutdownMapArena();
                rt.shutdownMapArena();
                t.IsTrue(!rt.validateMapArena(&owner, next.generation), "shutdown invalidates");
                t.IsTrue(!rt.claimMapArena(&owner), "shutdown rejects claims");
                rt.resetMapArena();
                auto restarted = rt.claimMapArena(&owner);
                t.IsTrue(restarted && restarted.generation != next.generation, "reset reactivates");
                previous = restarted.generation;
            }
            PS2Runtime replacement;
            auto fresh = replacement.claimMapArena(&owner);
            t.IsTrue(fresh.generation != previous, "runtime replacement gets unique lease");
            t.IsTrue(!replacement.validateMapArena(&owner, previous), "destroyed runtime token rejected");
        });
    });
}
