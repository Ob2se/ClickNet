# ClickNet headless load generator

`ClickNetLoadTest.exe` creates many real GameNetworkingSockets connections to the standalone server. It uses the same version 6 wire protocol as the Unreal client, sends names (`LoadBot-1`, etc.), movement commands, and one second command refreshes. It receives and decodes server player/body states, including batched player states. It does not run Unreal or Box3D locally.

Build from this directory with `python build.py Release` (or `Debug`). The executable and required DLLs are written to `x64/Release` or `x64/Debug`. The project finds `ClickNetWire.h` in the Unreal plugin two directories above this one.

Start the standalone server yourself, then run a small check from this directory:

```powershell
.\x64\Release\ClickNetLoadTest.exe --bots 20 --ramp 5 --duration 30 --pattern spread --map ..\..\Saved\Box3DTest.box3d --csv check.csv
```

For a larger distributed-player test:

```powershell
.\x64\Release\ClickNetLoadTest.exe --bots 1000 --ramp 25 --duration 180 --pattern spread --spread-seconds 12 --map ..\..\Saved\Box3DTest.box3d --csv spread-1000.csv
```

The duration includes the ramp. In the example above, creating 1,000 connections takes about 40 seconds. Use `--host IP --port 27015` when generating load from a separate machine. Run multiple generators on separate machines for high connection counts and give each a distinct CSV path. `--dry-run` checks arguments and the optional map hash without opening a connection. `--help` lists all options. Ctrl+C stops the generator and closes its connections.

For bots that keep moving, turn, and jump, combine `wander` with `--jump-every`:

```powershell
.\x64\Release\ClickNetLoadTest.exe --bots 200 --ramp 10 --duration 90 --pattern wander --turn-seconds 3 --jump-every 2 --csv wander-jump.csv
```

Use `--pattern steady --jump-every 2` for one continuous direction with periodic jumps. Bots request a jump only when their latest own-player state says they are grounded. Their first jumps are staggered, and the console and CSV report a cumulative `jumps_sent` count.

Patterns:

| Pattern | Behavior | Purpose |
| --- | --- | --- |
| `idle` | All bots stay at the server spawn. | Connection and idle traffic baseline; dense interest area. |
| `steady` | All bots run in the same direction. | Sparse straight-line input and player prediction. |
| `spread` | Each bot runs outward for `--spread-seconds`, then stops. | Broad map visibility scaling. |
| `cluster` | Bots reverse direction every `--turn-seconds`. | Dense moving players. |
| `wander` | Bots turn every `--turn-seconds`. | Repeated direction changes. |
| `target` | Bots steer toward `--target X Y` in Box3D meters. | Approximate body contact near a chosen point. |

The map collision may stop bots from reaching their intended positions. `target` has simple steering and no pathfinding; inspect the server's player positions or use a clear map when testing specific box contacts.

Every second the generator prints attempted, connected, welcomed, failed, and stale bot counts, aggregate application payload rates, GameNetworkingSockets transport rates, average ping, and `botLate` ticks. The CSV saves those metrics plus send and protocol error counts. A bot is stale when its own state has not arrived for two seconds. `botLate` growing means the generator itself cannot keep its 60 Hz loop; move load generation to more machines before treating that run as a server limit.

Compare the CSV with the server's `bandwidth-live.json`: check `tickWorkMs`, `peakTickWorkMs`, and `lateTickCount`. A sustained tick above 16.7 ms or growing late ticks means the 60 Hz server is overloaded. Run idle, spread, and cluster tests separately; a thousand spread-out players and a thousand players inside one 40 m interest region have very different costs.

The command clock preserves elapsed ticks when a generator loop is late and re-anchors to owner acknowledgements instead of accumulating drift. Commands allow a measured round trip plus two scheduling ticks (4–30 ticks total lead). This avoids stamping turns in the server's past just because generator frames were skipped. Check the server's `lateInputCommands` and `inputHistoryMisses` alongside `botLate`: excessive late commands cause authoritative movement replay and can make other clients see corrections even when their own frame rate is healthy.

Movement uses sequenced unreliable packets with recent unacknowledged commands, retried every 50 ms. Owner snapshots selectively acknowledge receipt. Duplicate jumps are ignored. Recovery is bounded to 64 outstanding commands; `expiredCommands` in the console and `expired_commands` in the CSV count commands that exceeded that window. Run matching version 6 server, bots, and Unreal client together.
