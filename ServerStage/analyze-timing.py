"""Join timing files from ONE server run. No servers or clients are launched."""
import argparse
import csv
import json
from pathlib import Path


def summarize(values, divisor=1000):
    values = sorted(v / divisor for v in values if v >= 0)
    if not values:
        return {"count": 0}
    def percentile(p):
        return round(values[min(len(values) - 1, int((len(values) - 1) * p))], 3)
    return {"count": len(values), "p50": percentile(.5), "p95": percentile(.95),
            "max": round(values[-1], 3)}


def analyze(paths, same_host=False):
    rows = []
    for path in paths:
        with Path(path).open(newline="", encoding="utf-8") as source:
            for row in csv.DictReader(source):
                rows.append({k: (v if k == "event" else int(v)) for k, v in row.items()})
    events = {}
    for row in sorted(rows, key=lambda r: r["wall_us"]):
        events.setdefault(row["event"], []).append(row)
    def values(name, column="value_us"):
        return [r[column] for r in events.get(name, [])]
    result = {"rows": len(rows), "same_host_wall_clock_join": same_host}
    for name in ("loop_wall", "loop_thread_cpu", "loop_process_cpu", "command_receive",
                 "command_apply", "send_queue", "server_send_queue", "wake_delay",
                 "bot_receive_work", "bot_loop_wall", "phase_network", "phase_mover",
                 "phase_world", "phase_replication", "phase_stats", "phase_callbacks",
                 "phase_receive", "phase_bandwidth_queries", "connection_control"):
        result[name + "_ms"] = summarize(values(name))
    for label, own in (("owner", True), ("observer", False)):
        received = [r for r in events.get("snapshot_receive", []) if (r["player"] == r["peer"]) == own]
        result[label + "_receive_queue_ms"] = summarize([r["value_us"] for r in received])
    wall = sum(values("loop_wall"))
    if wall:
        result["main_thread_cpu_fraction_of_work"] = round(sum(v for v in values("loop_thread_cpu") if v >= 0) / wall, 4)
        result["process_cpu_cores_during_work"] = round(sum(v for v in values("loop_process_cpu") if v >= 0) / wall, 4)
    leads = values("command_receive", "lead_ticks")
    result["sampled_commands_late_or_current"] = sum(v <= 0 for v in leads)
    result["sampled_commands_received"] = len(leads)
    result["command_lateness_ticks"] = summarize([-v for v in leads if v <= 0], divisor=1)
    result["pending_reliable_bytes"] = summarize(values("pending_reliable"), divisor=1)
    result["server_pending_reliable_bytes"] = summarize(values("server_pending_reliable"), divisor=1)
    if not same_host:
        return result
    commands = {(r["player"], r["command_tick"]): r for r in events.get("command_send", [])}
    snapshots = {(r["player"], r["peer"], r["server_tick"]): r for r in events.get("snapshot_send", [])}
    stages = {"command_send_to_server_ms": [], "owner_snapshot_delivery_ms": [],
              "observer_snapshot_delivery_ms": [], "command_to_first_observer_ms": [],
              "apply_to_first_observer_send_ms": []}
    negatives = 0
    matched = 0
    for row in events.get("command_receive", []):
        sent = commands.get((row["player"], row["command_tick"]))
        if sent:
            delta = row["wall_us"] - sent["wall_us"]
            negatives += delta < 0
            stages["command_send_to_server_ms"].append(delta)
    first_observer = set()
    for row in events.get("snapshot_receive", []):
        sent = snapshots.get((row["player"], row["peer"], row["server_tick"]))
        if not sent:
            continue
        matched += 1
        delta = row["wall_us"] - sent["wall_us"]
        negatives += delta < 0
        own = row["player"] == row["peer"]
        stages["owner_snapshot_delivery_ms" if own else "observer_snapshot_delivery_ms"].append(delta)
        key = (row["player"], sent["command_tick"], row["peer"])
        command = commands.get(key[:2])
        if not own and command and key not in first_observer:
            first_observer.add(key)
            stages["command_to_first_observer_ms"].append(row["wall_us"] - command["wall_us"])
            stages["apply_to_first_observer_send_ms"].append(sent["value_us"])
    result.update({name: summarize(v) for name, v in stages.items()})
    result["matched_snapshot_receives"] = matched
    result["unmatched_snapshot_receives"] = len(events.get("snapshot_receive", [])) - matched
    result["negative_cross_process_intervals"] = negatives
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--same-host", action="store_true", help="Enable wall-clock joins only for processes on the same machine")
    args = parser.parse_args()
    print(json.dumps(analyze(args.files, args.same_host), indent=2))
