# Parallel replication

The server now uses three persistent replication worker threads by default (or
fewer on machines with fewer than four logical processors). The main thread also
processes recipient jobs. Below 32 connections, the batch runs on the main thread
to avoid synchronization overhead.

## Changes and boundaries

- `ReplicationWorkers.h`: persistent pool with a joined batch each server tick.
  Threads are started once, rather than created for every tick. Each recipient
  job runs exactly once. The main thread waits until the workers finish before
  changing simulation state or servicing connection callbacks.
- `ClicknetServer.cpp`: per-recipient visibility scans, dead reckoning checks,
  appearance decisions, cache updates and packet encoding run in parallel. Each
  job owns one recipient's replication caches. Player/body snapshots, spatial
  grids are read-only during the batch.
- Physics, input application/replay, owner acknowledgements, bandwidth sampling,
  file output and player creation/removal remain on the main thread. Box3D receives
  no concurrent simulation or query calls from these replication workers.
- Each worker owns one recipient's traffic counters. Each recipient submits its
  pending messages in one `SendMessages` call. Only accepted messages update the
  replication caches; failed/skipped messages remain eligible for retry, including
  appearances and despawns. Per-recipient reliable messages keep their ordering.
  GNS locks each connection internally, so different recipients can submit
  concurrently. Worker trace writes are protected separately and flushed after
  join. Owner sends and queued player creation/removal run outside the worker batch.
- The existing 14 ms replication deadline and recipient rotation remain. After
  joining, rotation resumes at the first skipped recipient. Already-started jobs
  can finish past the deadline; the deadline is a scheduling budget, not a hard
  real-time guarantee. Body sampling remains at the existing rate.
- JSON statistics add `replicationWorkers` (additional worker count) and
  `remoteRecipients` (remote recipients allowed in the last replication pass).
  CPU/loop CSV records still include process CPU and main-thread CPU, allowing
  work moved to other threads to be distinguished from reduced wall time.

## Comparing at 1,000 bots

Keep the map, wander/turn/jump settings, bot count and focused client consistent.
Run the server yourself with the same existing options and one of:

```text
--replication-workers 0
--replication-workers 3
```

Zero provides the sequential reference. Valid counts are 0 through 16, and count
means additional threads beyond the main thread. Compare loop wall p95/p99,
replication work, late ticks, observer delivery age and outgoing send queues.
Total CPU can increase even if loop time improves. If movement/physics or the
serialized send path dominates, this split may provide limited improvement.
The load test is required to establish any effect on rubberbanding.

## Follow-up after the 1,000-bot capture

The first parallel build still had roughly 10 recipients serviced per pass in
the capture, and client logs showed multi-second gaps for individual players.
This update reduces repeated work without capping visibility:

- Incremental replication now traverses nearby spatial cells and compares each
  source's latest motion revision with the recipient cursor. The server no longer
  builds a duplicate set or walks a global history of movement events for every
  recipient. Periodic full scans still handle baselines, arrivals and departures.
- A per-recipient outbox groups packet submission into one network API call and
  no application-wide send lock. Per-message results control which cache changes are
  committed. State is never marked delivered merely because it was queued locally.
- State serializers reserve their final byte capacity to avoid repeated vector
  reallocations. Packet contents and protocol version are unchanged.
- JSON adds `replicationBuildWorkerMs` and `replicationSendWorkerMs`. These are
  sums of elapsed durations across recipient jobs and can exceed wall time when
  workers overlap. Send time includes the network API call and cache commits.
  `maxRemoteServiceGapTicks` measures the largest interval between checks for
  recipients serviced this pass; it is not a per-packet delivery guarantee.

`ReplicationOutboxTest.cpp` verifies packet decoding, deferred cache updates,
partial success, skipped messages, retries, and failed despawn retention.

The follow-up removes the application-wide send mutex after confirming that the
installed GNS implementation of `SendMessages` locks connections separately.
Per-recipient job ownership still protects cache updates and traffic counters.
CSV events `phase_network`, `phase_mover`, `phase_world`, `phase_replication`, and
`phase_stats` now give phase timings for each completed loop, capturing rare
multi-second stalls that JSON sampling may miss.

## Connection admission thread

The next capture isolated the largest stalls to the network phase during joining:
one 570 ms loop spent 538 ms there, while replication used 3.5 ms. After the join
ramp finished, a 20-second window had network phase p95 below 1 ms. The installed
GNS source takes its global lock in `AcceptConnection`; long trace gaps occurred
before input processing. This points to admission as a distinct ramp bottleneck.

`NetworkControlWorker.h` now pumps GNS callbacks and performs accept, poll-group
assignment and connection closure on a dedicated thread. It queues copies of
connection events to the simulation thread, which alone creates/removes players
and Box3D bodies. The control thread joins before networking/world shutdown.
The simulation can continue while connection lifecycle calls wait on GNS.

The CSV now separates `phase_callbacks` (simulation event handling),
`phase_receive` and `phase_bandwidth_queries`. `connection_control` reports the
maximum completed control-callback duration since the prior simulation sample.
It is background work, not part of simulation-loop wall time. Remaining mover,
stats, transport and replication costs still need live verification at 1,000.

`NetworkControlWorkerTest.cpp` verifies that a blocked control callback does not
block the caller, queued work runs on the control thread in order, and shutdown
and restart complete. It does not open sockets or start a server.

No protocol change, client rebuild or bot rebuild is needed for this change.
No server or load-test process is launched by the build or installation.

## Verification

`ReplicationWorkersTest.cpp` checks actual parallel execution, complete joins,
200 successive 1,000-recipient batches without missing or duplicate jobs,
exception propagation, reuse, sequential fallback and pool restart. Server
Release and Debug builds validate integration. These checks do not substitute
for a live 1,000-bot comparison.
