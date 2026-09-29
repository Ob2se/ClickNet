Timing diagnostic build
=======================

No wire-format or simulation behavior changes are required. Trace all human
commands and players whose server ID is divisible by 64. Each process creates a
unique CSV, buffers writes, flushes about once a second, and caps its file at
64 MiB. Trace files contain IDs and timing counters, not player names.

Locations:

- Server: beside `bandwidth-live.json`, `timing-server-*.csv`.
- Bots: beside their configured CSV output, `timing-bots-*.csv`.
- Unreal: project `Saved/Timing/timing-client-*.csv`.

Run the same load for about 30 seconds with the Unreal viewport focused. Select
the files for ONE server run, including both generator files if using two bot
processes. After the regular one-second flush they can be read while running:

    python analyze-timing.py --same-host <server.csv> <bots1.csv> <bots2.csv> <client.csv>

Use `--same-host` only when every process ran on the same machine. It enables
wall-clock joins between processes. Across machines, use the default local-only
report unless clock synchronization has been independently measured. Local
intervals use monotonic clocks. Wall-clock adjustments can invalidate joins;
negative intervals are counted and excluded from percentiles.

Events and interpretation:

- `command_send`: successful enqueue to GameNetworkingSockets, correlated by
  player ID and command tick. This is not the instant the packet reaches the wire.
- `command_receive`: server application accepted the command; `value_us` is the
  time from GNS message receipt to application polling. `lead_ticks` is command
  tick minus the server's already-simulated input tick, before any rebase.
- `command_apply`: first simulation/replay using that command; `value_us` is
  receive-to-apply duration. `lead_ticks` records a tick rebase for commands older
  than input history. Intermediate commands superseded before application may
  have no apply record. Pending trace tracking is bounded at 64 commands/player.
- `snapshot_send`: successful enqueue, keyed by source, observer, and server tick.
  `command_tick` is the last sampled command applied to that source. `value_us`
  is time since that application; subsequent held-command refreshes are not new
  command responses. The analyzer uses the first matched observer response per
  command/observer. Multiple rapid commands can be coalesced into one snapshot.
- `snapshot_receive`: application polling time; `value_us` is local GNS receive
  queue wait. Sources equal to peers are owner snapshots; others are observers.
- `send_queue`/`server_send_queue`: GNS estimated outgoing queue duration;
  negative values are unknown. `pending_reliable`/`server_pending_reliable` use
  `value_us` to hold BYTES, despite the general column name.
- `loop_wall`, `loop_thread_cpu`, `loop_process_cpu`: server loop work duration,
  actual main-thread CPU, and process-wide CPU, in microseconds. CPU uses Windows
  GetThreadTimes/GetProcessTimes. Compare aggregate CPU with aggregate wall time;
  individual CPU samples are quantized. Process CPU may exceed wall time when
  multiple worker threads run. This is not overall machine CPU percentage.
  `ack_tick` on these events stores the number of physics steps in that loop.
- `wake_delay`: lateness relative to the scheduled start of a server loop.
- `bot_receive_work` and `bot_loop_wall`: generator callback/receive work and
  complete loop work, excluding sleep.

The JSON monitor snapshot adds mainThreadCpuMs/processCpuMs for the last completed
loop and pendingReliableBytes/pendingUnreliableBytes/sendQueueUs per connection.
Connection counters refresh in the existing rotating sampler. Use CSV loop
records for aligned CPU/wall comparisons; existing phase fields in JSON can
refer to different loops at publication time.

Send-to-receive wall time includes outgoing buffering, transport, and incoming
application queue time. Subtract the measured incoming queue time only when the
records share a valid wall clock. An unmatched receive can be a capture boundary
or an unsampled sender, not necessarily packet loss. Tracing adds some CPU and
file I/O; compare against the uninstrumented baseline before attributing small
timing changes to the networking code.
