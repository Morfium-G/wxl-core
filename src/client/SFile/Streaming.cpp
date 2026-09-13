// Async streaming detour: reentrant-drain serialization.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "common/Log.hpp"
#include "config.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"

#include "offsets/engine/Gx.hpp"
#include "offsets/game/World.hpp"

#include <windows.h>
#include <intrin.h>

#include <cstdint>
#include <cstring>

namespace
{
    namespace wld   = wxl::offsets::game::world;
    namespace gxoff = wxl::offsets::engine::gx;

    wld::AsyncServiceQueuesFn g_origAsyncDrain = nullptr;

    // Per-thread async-drain recursion depth. A texture build force-waits nested reads, which re-enter the
    // completion drain; a nested pump running unrelated completions frees / rewrites a buffer the outer
    // build still uploads from (the 0x40cb6a use-after-free).
    thread_local int g_drainDepth = 0;

    // Serialize the reentrant drain. The completed-read queue is an intrusive doubly-linked list: each node
    // holds its link at node+0x28 {next, tagged-prev}; the head (a node base) is at the completed-head global.
    // Addresses + arithmetic verified against the drain's own head-unlink. The lock is a recursive
    // critical section taken by ECX.
    namespace adrain
    {
        constexpr uint32_t kLockEnter     = 0x00774640; // critical-section enter, ecx = lock
        constexpr uint32_t kLockLeave     = 0x00774650; // critical-section leave, ecx = lock
        constexpr uint32_t kAsyncLock     = 0x00B4A240; // the recursive queue lock
        constexpr uint32_t kCompletedHead = 0x00AC3474; // first completed node (sentinel/empty if &1 or 0)
        constexpr uint32_t kAwaitedObj    = 0x00B4A204; // node a force-wait blocks on (0 = none)
        constexpr uint32_t kPendingCount  = 0x00B4A1F8; // outstanding-completion counter
        constexpr uint32_t kLinkOffset    = 0x28;       // node -> link byte offset
        constexpr uint32_t kWaitGuard     = 0x00B4A26C; // the client's async-wait reentrancy counter

        inline uint32_t Rd(uint32_t a)             { return *reinterpret_cast<uint32_t*>(a); }
        inline void     Wr(uint32_t a, uint32_t v) { *reinterpret_cast<uint32_t*>(a) = v; }
        inline uint8_t  RdB(uint32_t a)            { return *reinterpret_cast<uint8_t*>(a); }
        inline void     WrB(uint32_t a, uint8_t v) { *reinterpret_cast<uint8_t*>(a) = v; }
        inline void Lock()   { reinterpret_cast<void(__thiscall*)(uint32_t)>(kLockEnter)(kAsyncLock); }
        inline void Unlock() { reinterpret_cast<void(__thiscall*)(uint32_t)>(kLockLeave)(kAsyncLock); }

        // Diagnostic-only companion to Unlink(): every intermediate value the unlink arithmetic
        // computes, captured so a corruption report can show not just "which node" but "which exact
        // write this unlink performed" -- lets an actual discrepancy in the arithmetic (vs. a
        // discrepancy caused by a stale/already-freed neighbour node) be told apart from real data
        // instead of guessed at.
        struct UnlinkTrace
        {
            bool     tookNextNonZeroBranch = false;
            uint32_t next   = 0; // Rd(node+0x28) at entry
            uint32_t prev   = 0; // Rd(node+0x2c) at entry
            uint32_t nextsPrevField = 0; // only valid if tookNextNonZeroBranch: Rd(next+4), the value the prevSlot formula depends on
            uint32_t prevSlot = 0; // the address this unlink wrote `next` into
        };

        // Detach one node from the completed list (the engine's own head-unlink arithmetic, generalised).
        UnlinkTrace Unlink(uint32_t node)
        {
            UnlinkTrace trace;
            const uint32_t linkNext = node + 0x28;
            const uint32_t next = Rd(linkNext);
            trace.next = next;
            if (next != 0)
            {
                trace.tookNextNonZeroBranch = true;
                const uint32_t prev = Rd(node + 0x2c);
                trace.prev = prev;
                trace.nextsPrevField = Rd(next + 4);
                const uint32_t prevSlot = ((prev & 1u) == 0u && prev != 0u)
                                              ? linkNext + (prev - trace.nextsPrevField)
                                              : (prev & 0xFFFFFFFEu);
                trace.prevSlot = prevSlot;
                Wr(prevSlot, next);
                Wr(next + 4, Rd(node + 0x2c));
                Wr(linkNext, 0);
                Wr(node + 0x2c, 0);
            }
            else
            {
                const uint32_t prev = Rd(node + 0x2c);
                trace.prev = prev;
                if ((prev & 1u) != 0u || prev == 0u)
                {
                    trace.prevSlot = prev & 0xFFFFFFFEu;
                    Wr(trace.prevSlot, 0);
                }
                Wr(node + 0x2c, 0);
            }
            return trace;
        }

        // True if target is currently enqueued in the completed list.
        //
        // A confirmed real repro hit a genuine infinite spin in this exact walk (caught live: a
        // thread pegged at 100% CPU sitting in this loop's own "next == 0" check, cross-checked
        // against a disassembly of the built binary). An earlier blind step-cap (return "not found"
        // after a fixed step count once the walk got unusually long) stopped the spin but then
        // appeared to cause a fatal crash on retest instead -- root-caused afterward as a separate,
        // genuine same-thread reentrancy bug in a caller elsewhere (a caller re-entering the wait
        // entry point below from within its own force-wait pump; fixed independently on that
        // caller's side), which the cap's timing change happened to trigger rather than something
        // the cap itself did wrong. A later diagnostic-only build (logs once the walk passes a fixed
        // step count, no behavior change) then confirmed via a real repro that this walk can
        // genuinely never terminate on its own: exactly one "still going" line was logged, then the
        // process hung indefinitely with no further log output -- proof of a true cycle, not just an
        // unusually long list.
        //
        // Fixed properly with Floyd's tortoise-and-hare cycle detection instead of an arbitrary step
        // count. A "hare" pointer advances twice as fast as the "tortoise"; if they ever meet, the
        // list is provably cyclic, and target is definitively not in it (a real, finite chain can
        // never make a 2x-speed pointer lap a 1x-speed one before either falls off the end). This
        // can't produce a false "not found" the way a blind cap theoretically could for a
        // legitimately very long but non-cyclic list -- termination is mathematically guaranteed in
        // O(prefix + cycle length) steps either way.
        bool Enqueued(uint32_t target)
        {
            uint32_t node = Rd(kCompletedHead);
            if ((node & 1u) != 0u || node == 0u) return false;

            uint32_t tortoise = node;
            uint32_t hare      = node;
            for (;;)
            {
                if (tortoise == target) return true;
                const uint32_t tNext = Rd(tortoise + 0x28);
                if (tNext == 0) return false;
                tortoise = tNext - kLinkOffset;

                for (int hop = 0; hop < 2; ++hop)
                {
                    if (hare == target) return true;
                    const uint32_t hNext = Rd(hare + 0x28);
                    if (hNext == 0) return false;
                    hare = hNext - kLinkOffset;
                }

                if (hare == tortoise)
                {
                    WLOG_ERROR("AsyncDrain: Enqueued() detected a genuine cycle in the completed-list "
                               "(target=0x%08x, meeting node=0x%08x) -- target is definitively not "
                               "reachable; treating as not-found instead of hanging.",
                               target, tortoise);
                    return false;
                }
            }
        }

        // Diagnostic only, no effect on the list itself: re-walks the list with the same
        // tortoise/hare technique as Enqueued(), right after a real unlink, to check whether the
        // list is already cyclic at that exact moment. The point is timing: Enqueued() only ever
        // discovers a cycle whenever some later, unrelated lookup happens to walk into the damaged
        // region, which could be long after (and by a completely different caller than) whatever
        // actually introduced it. Running the same check immediately after every successful unlink
        // narrows that down to the specific unlink call that produced the corruption, which is far
        // more useful for finding the actual bug than a downstream symptom. Logs the just-unlinked
        // node's own pre-unlink link fields too, since those are the most likely place a bug in this
        // reimplementation of the engine's own unlink arithmetic would first show up. Bounded by the
        // same mathematical guarantee as Enqueued() (terminates in O(list length) either way), so
        // this is safe to run unconditionally rather than sampled -- intended to be removed again
        // once the actual corruption source is found, not meant to ship long-term.
        bool g_reportedListCorruption = false;
        void VerifyListAcyclicAfterUnlink(uint32_t justUnlinked, const UnlinkTrace& trace)
        {
            if (g_reportedListCorruption) return; // already found and reported once; no new signal in repeats

            uint32_t node = Rd(kCompletedHead);
            if ((node & 1u) != 0u || node == 0u) return; // empty list, trivially fine

            uint32_t tortoise = node;
            uint32_t hare      = node;
            for (;;)
            {
                const uint32_t tNext = Rd(tortoise + 0x28);
                if (tNext == 0) return; // reached a real end -- list is fine
                tortoise = tNext - kLinkOffset;

                for (int hop = 0; hop < 2; ++hop)
                {
                    const uint32_t hNext = Rd(hare + 0x28);
                    if (hNext == 0) return; // reached a real end -- list is fine
                    hare = hNext - kLinkOffset;
                }

                if (hare == tortoise)
                {
                    g_reportedListCorruption = true;
                    WLOG_ERROR("AsyncDrain: completed-list is already cyclic immediately after "
                               "unlinking node=0x%08x (pre-unlink: next=0x%08x prev=0x%08x, branch=%s, "
                               "next's-own-prev-field=0x%08x, computed write target=0x%08x; cycle "
                               "meets at node=0x%08x) -- this unlink is the first one observed after "
                               "which the list is broken.",
                               justUnlinked, trace.next, trace.prev,
                               trace.tookNextNonZeroBranch ? "next!=0" : "next==0",
                               trace.nextsPrevField, trace.prevSlot, tortoise);
                    return;
                }
            }
        }

        // The mip-source singleton an upload reads while we run a nested completion. kMipTablePtr is a
        // pointer whose buffer holds the per-mip source pointers (read as ((u32*)ptr)[mip]); kMipTableValid
        // gates that read. The awaited completion is itself a texture build that refills both with its own
        // aliases and frees its IO buffer, so the outer build whose GxTexUpdate force-waited us would resume
        // reading a clobbered, freed table (0x40cb6a). Snapshot the outer view, run the nested build, put it
        // back: the outer copy then reads its own pointers into its own still-live buffer. The real mip count
        // is <= 13; 16 dwords is a safe upper bound, and the table buffer is the 1024-DXT scratch so the
        // fixed-size copy is always in bounds.
        struct MipTableSnapshot
        {
            uint32_t ptr;
            uint32_t valid;
            uint32_t table[16];
            bool     hasTable;
        };

        inline MipTableSnapshot SnapshotMipTable()
        {
            MipTableSnapshot s{};
            s.ptr      = Rd(gxoff::kMipTablePtr);
            s.valid    = Rd(gxoff::kMipTableValid);
            s.hasTable = (s.ptr != 0);
            if (s.hasTable)
                std::memcpy(s.table, reinterpret_cast<const void*>(s.ptr), sizeof(s.table));
            return s;
        }

        inline void RestoreMipTable(const MipTableSnapshot& s)
        {
            Wr(gxoff::kMipTablePtr, s.ptr);
            if (s.hasTable)
                std::memcpy(reinterpret_cast<void*>(s.ptr), s.table, sizeof(s.table));
            Wr(gxoff::kMipTableValid, s.valid);
        }

        // Process ONLY the awaited node; leave every other completion queued for the outer pump.
        int DrainAwaitedOnly()
        {
            Lock();
            const uint32_t target = Rd(kAwaitedObj);
            if (target == 0) { Unlock(); return 1; }
            if (RdB(target + 0x21) != 0) // already serviced this turn
            {
                if (Rd(kAwaitedObj) == target) Wr(kAwaitedObj, 0);
                Unlock();
                return 1;
            }
            if (!Enqueued(target)) { Unlock(); return 1; } // armed but not yet delivered by the worker
            const UnlinkTrace trace = Unlink(target);
            VerifyListAcyclicAfterUnlink(target, trace); // diagnostic only, see its own comment
            if (Rd(kAwaitedObj) == target) Wr(kAwaitedObj, 0);
            WrB(target + 0x21, 1);
            Unlock(); // the engine releases the lock before every completion call

            // Save the outer build's mip-source view across the nested build this completion runs, then
            // restore it so the force-waiting outer GxTexUpdate resumes reading its own live aliases.
            const MipTableSnapshot snap = SnapshotMipTable();
            reinterpret_cast<void(__cdecl*)(uint32_t)>(Rd(target + 0x10))(Rd(target + 0x0c));
            RestoreMipTable(snap);

            Wr(kPendingCount, Rd(kPendingCount) - 1);
            return 1;
        }
    }

    /**
     * @brief Detours the async-queue drain to serialize reentrant pumps.
     *
     * Depth 0 runs the full engine drain. A reentrant pump (a build force-waiting a nested read) processes
     * only the node that wait is blocked on and leaves the rest, so no nested completion frees or rewrites
     * a buffer the outer build is still uploading from.
     */
    int __cdecl hkAsyncDrain(int a, int b)
    {
        if (g_drainDepth > 0)
            return adrain::DrainAwaitedOnly();
        ++g_drainDepth;
        const int r = g_origAsyncDrain(a, b);
        --g_drainDepth;
        return r;
    }

    wld::AsyncFileReadWaitFn g_origAsyncFileReadWait = nullptr;

    // A client-close mass-teardown (many game objects being destroyed in a short window) can drive
    // this recovery hundreds of times in well under a second, almost always for the SAME leaked
    // object -- observed once as 522 recoveries in a single burst, all logged at Warn, which flushes
    // to disk immediately (see common/Log.hpp). 522 synchronous flushes was the real cause of a
    // multi-second freeze on close, not a revival of the underlying crash. An attempt to stop the
    // CALLER (a per-frame hook elsewhere that indirectly triggers this wait) from reaching this
    // function during that window was tried and reverted: those very calls turn out to be the only
    // thing still pumping the async completion queue that late in shutdown (the render loop has
    // already stopped calling EndScene/Present by then) -- removing them made the client's own final
    // teardown, which blocks waiting for every outstanding request to signal, hang indefinitely
    // instead of freezing and eventually closing. The call pattern into this function must stay
    // exactly as it is; only the logging cost needs fixing.
    void* g_lastLeakObj     = nullptr;
    uint32_t g_lastLeakRepeats = 0;

    /**
     * @brief Recovers kWaitGuard from a native early-return path that leaks it.
     *
     * AsyncFileReadWait asserts kWaitGuard is 0 on entry (fatal app-terminate otherwise), increments
     * it, then decrements it once before every NORMAL return. One path skips that decrement: if the
     * target object is already marked handled by the time the critical section is taken, the
     * function releases the lock and returns immediately, never reaching the decrement at the
     * bottom. That leaves the guard permanently incremented -- the very next call to this function
     * from ANYWHERE in the client (it has dozens of call sites) hits the entry assert and
     * fatal-terminates the process. Confirmed via direct disassembly of the native function; no
     * caller-side argument avoids this, the early-return is unconditional once that flag is set.
     * Rare under light/occasional calling, much easier to hit for any caller invoking this function
     * at high frequency (found once a caller elsewhere started doing exactly that).
     *
     * Fix: read the guard's own value immediately before and after the real call. A leak shows up
     * as an exact +1 net change (the increment ran, the matching decrement didn't) -- restore it to
     * whatever it was before this call, nothing more. Keying off the counter's own observed delta,
     * rather than trying to predict the leak from the object's state beforehand, means this can
     * never remove more than what THIS call itself left behind, regardless of what any other caller
     * or thread does to the same guard concurrently.
     *
     * Logging is deduped by object, not silenced -- the first recovery for a given object still logs
     * at Warn (immediate flush, matches the original rare-case design so a live repro is still easy
     * to spot), but an immediate repeat of the SAME object logs at Info instead (batched, flushed
     * every 64 lines or on close -- see Log.cpp). Every recovery is still logged and the guard is
     * still restored exactly as before; only the per-line synchronous-flush cost of a long
     * same-object streak is gone.
     */
    void __cdecl hkAsyncFileReadWait(void* obj)
    {
        const uint32_t before = adrain::Rd(adrain::kWaitGuard);
        g_origAsyncFileReadWait(obj);
        const uint32_t after = adrain::Rd(adrain::kWaitGuard);
        if (after == before + 1)
        {
            adrain::Wr(adrain::kWaitGuard, before);
            if (obj == g_lastLeakObj)
            {
                ++g_lastLeakRepeats;
                WLOG_INFO("AsyncFileReadWait: recovered a leaked reentrancy guard (obj=%p, caller=%p) "
                          "-- repeat #%u for this object", obj, _ReturnAddress(), g_lastLeakRepeats);
            }
            else
            {
                g_lastLeakObj     = obj;
                g_lastLeakRepeats = 1;
                WLOG_WARN("AsyncFileReadWait: recovered a leaked reentrancy guard (obj=%p, caller=%p) -- "
                          "native early-return path skipped its own decrement", obj, _ReturnAddress());
            }
        }
    }

    /**
     * @brief Normal-phase install: reentrant-drain serialization.
     */
    bool InstallStreaming()
    {
        wxl::hook::Install("AsyncDrain", wld::kAsyncServiceQueues,
                           &hkAsyncDrain, &g_origAsyncDrain);
        wxl::hook::Install("AsyncFileReadWaitGuard", wld::kAsyncFileReadWait,
                           &hkAsyncFileReadWait, &g_origAsyncFileReadWait);
        return true;
    }
}

WXL_REGISTER_FEATURE("streaming", true, InstallStreaming)
