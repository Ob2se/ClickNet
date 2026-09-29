#include "MotionChangeLog.h"
#include <cassert>

int main()
{
    MotionChangeLog log;
    std::uint64_t fastRecipient = 0, deferredRecipient = 0;
    const auto turn = log.Record(10);
    fastRecipient = turn;
    log.RetireThrough((std::min)(fastRecipient, deferredRecipient));
    // Deferring one recipient cannot discard the turn delivered to another.
    assert(log.FirstAfter(deferredRecipient)->playerId == 10);
    const auto landing = log.Record(20);
    auto pending = log.FirstAfter(deferredRecipient);
    assert(pending++->sequence == turn);
    assert(pending->sequence == landing);
    // A failed send leaves the cursor unchanged and the same events retryable.
    assert(log.FirstAfter(deferredRecipient)->sequence == turn);
    deferredRecipient = landing;
    fastRecipient = landing;
    log.RetireThrough((std::min)(fastRecipient, deferredRecipient));
    assert(log.FirstAfter(deferredRecipient) == log.End());
    MotionChangeLog bounded(2);
    bounded.Record(1); bounded.Record(2); bounded.Record(3);
    bounded.RetireThrough(0);
    assert(bounded.NeedsRescan(0));
    assert(!bounded.NeedsRescan(2));
    assert(bounded.FirstAfter(2)->playerId == 3);
}
