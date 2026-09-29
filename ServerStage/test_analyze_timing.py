import csv
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("timing", Path(__file__).with_name("analyze-timing.py"))
timing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(timing)


class TimingTest(unittest.TestCase):
    def test_join_and_clock_domains(self):
        fields = "wall_us mono_us event player peer command_tick server_tick ack_tick received_tick value_us lead_ticks".split()
        def row(event, at, peer=0, value=0, tick=50):
            return dict(zip(fields, [at, at, event, 64, peer, 10, tick, 10, 10, value, -2]))
        rows = [row("command_send", 100000), row("command_receive", 120000, value=3000),
                row("command_apply", 125000, value=5000),
                row("snapshot_send", 130000, peer=7, value=5000),
                row("snapshot_receive", 145000, peer=7, value=2000),
                row("snapshot_receive", 146000, peer=8, value=2000),
                row("snapshot_receive", 147000, peer=7, value=2000, tick=51)]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "timing.csv"
            with path.open("w", newline="") as file:
                writer = csv.DictWriter(file, fieldnames=fields)
                writer.writeheader()
                writer.writerows(rows)
            local = timing.analyze([path])
            self.assertNotIn("command_send_to_server_ms", local)
            self.assertEqual(local["command_apply_ms"]["p50"], 5)
            joined = timing.analyze([path], True)
            self.assertEqual(joined["command_send_to_server_ms"]["p50"], 20)
            self.assertEqual(joined["observer_snapshot_delivery_ms"]["p50"], 15)
            self.assertEqual(joined["command_to_first_observer_ms"]["p50"], 45)
            self.assertEqual(joined["matched_snapshot_receives"], 1)
            self.assertEqual(joined["unmatched_snapshot_receives"], 2)
            self.assertEqual(joined["sampled_commands_late_or_current"], 1)


if __name__ == "__main__":
    unittest.main()
