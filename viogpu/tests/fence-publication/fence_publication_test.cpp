#include "../../common/fence_publication.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

int main()
{
    uint32_t reported = 0;
    // The two adjacent IDs from dump7125, delivered newer before older.
    VioGpuFencePublication state = {};
    assert(state.Prepare(1, 1, 0, 0x41ded, false, reported) && reported == 0x41ded);
    assert(!state.Prepare(1, 1, 0, 0x41dec, false, reported));
    assert(!state.Prepare(1, 1, 0, 0x41ded, false, reported));
    assert(state.Prepare(1, 1, 0, 0x41dec, true, reported) && reported == 0x41ded);

    // Every delivery order preserves an increasing, nonduplicated stream.
    unsigned order[] = { 0, 1, 2, 3 };
    unsigned permutations = 0;
    do
    {
        state = {};
        uint32_t last = 0;
        for (unsigned index : order)
        {
            if (state.Prepare(1, 1, 0, 100 + index, false, reported))
            {
                assert(reported > last);
                last = reported;
            }
        }
        assert(last == 103);
        ++permutations;
    } while (std::next_permutation(std::begin(order), std::end(order)));
    assert(permutations == 24);

    // Interrupt synchronization serializes publication, not the preceding
    // producer work. Force the earlier producer to arrive after the later one.
    state = {};
    std::mutex interruptLock, gateLock;
    std::condition_variable gate;
    bool newerDelivered = false;
    std::vector<uint32_t> delivered;
    std::thread older([&] {
        {
            std::unique_lock<std::mutex> lock(gateLock);
            gate.wait(lock, [&] { return newerDelivered; });
        }
        std::lock_guard<std::mutex> interrupt(interruptLock);
        uint32_t fence;
        if (state.Prepare(1, 1, 0, 100, false, fence))
            delivered.push_back(fence);
    });
    std::thread newer([&] {
        {
            std::lock_guard<std::mutex> interrupt(interruptLock);
            uint32_t fence;
            if (state.Prepare(1, 1, 0, 101, false, fence))
                delivered.push_back(fence);
        }
        {
            std::lock_guard<std::mutex> lock(gateLock);
            newerDelivered = true;
        }
        gate.notify_one();
    });
    older.join();
    newer.join();
    assert(delivered.size() == 1 && delivered[0] == 101);

    // UINT fence wrap is independent of epoch. Zero is never a completion.
    state = {};
    assert(state.Prepare(7, 7, 0, 0xfffffffeU, false, reported));
    assert(state.Prepare(7, 7, 0, 0xffffffffU, false, reported));
    assert(!state.Prepare(7, 7, 0, 0, false, reported));
    assert(state.Prepare(7, 7, 0, 1, false, reported) && reported == 1);
    assert(!state.Prepare(7, 7, 0, 0xffffffffU, false, reported));

    // Reset advances the floor without an ordinary completion interrupt;
    // old completions AND old preemptions are discarded. A new preemption
    // still acknowledges an idle engine with the actual reset floor.
    assert(!state.Prepare(7, 8, 99, 100, false, reported));
    assert(!state.Prepare(7, 8, 99, 100, true, reported));
    assert(!state.Prepare(8, 8, 99, 99, false, reported));
    assert(state.Prepare(8, 8, 99, 99, true, reported) && reported == 99);
    assert(state.Prepare(8, 8, 99, 100, false, reported) && reported == 100);
    assert(!state.Prepare(7, 8, 99, 101, false, reported));
    assert(state.Fence == 100 && state.Epoch == 8);
    assert(state.Prepare(9, 9, 0, 0, true, reported) && reported == 0);
    assert(!state.Prepare(9, 9, 0, 0, false, reported));
    assert(state.Prepare(9, 9, 0, 1, false, reported) && reported == 1);
    puts("fence publication: raced delivery/24 permutations/preempt/reset/UINT wrap PASS");
}
