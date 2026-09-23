import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from check_telemetry import check, summarize


def window(thread=1):
    return (f"telemetry_window thread={thread} seq=1 begin_us=100 end_us=200 wall_us=100 accounted_us=100 errors=0 coverage=partial\n"
            f"telemetry_phase thread={thread} seq=1 name=scu kind=work exclusive_wall_us=40 entries=2\n"
            f"telemetry_phase thread={thread} seq=1 name=queue_wait kind=wait exclusive_wall_us=60 entries=1\n"
            f"telemetry_end thread={thread} seq=1\n")


class TelemetryLogTest(unittest.TestCase):
    def test_samples_are_separate_from_partition(self):
        sample = ("telemetry_sample thread=1 seq=1 name=sh2_master entries=2000 roots=2000 "
                  "samples=2 inclusive_wall_us=130 max_wall_us=80 errors=0 open_depth=0 "
                  "probability_denominator=1024 attribution=completion_window additive=0\n")
        text = window().replace("telemetry_end", sample + "telemetry_end")
        owner = summarize(check(text))["owners"][0]
        self.assertEqual(owner["wall_us"], 100)
        self.assertEqual(owner["samples"][0]["inclusive_wall_us"], 130)
        for bad in (text.replace("additive=0", "additive=1"),
                    text.replace("samples=2", "samples=0"),
                    text.replace("max_wall_us=80", "max_wall_us=140"),
                    text.replace("errors=0 open_depth", "errors=1 open_depth")):
            with self.assertRaises(ValueError):
                check(bad)
        # An open sample can complete in a window without any new entries.
        check(text.replace("entries=2000 roots=2000", "entries=0 roots=0"))

    def test_overlapping_threads_are_independent(self):
        first, second = window().splitlines(), window(2).splitlines()
        interleaved = "\n".join(row for pair in zip(first, second) for row in pair)
        result = check(interleaved)
        self.assertEqual(len(result["windows"]), 2)
        self.assertFalse(result["coverage_complete"])

    def test_reject_bad_logs(self):
        for text in ("", window().rsplit("telemetry_end", 1)[0],
                     window().replace("exclusive_wall_us=40", "exclusive_wall_us=41"),
                     window().replace("errors=0", "errors=1"),
                     window().replace("accounted_us=100", "accounted_us=99"),
                     window() + window(), window().replace("entries=2", "entries=-1")):
            with self.subTest(text=text), self.assertRaises(ValueError):
                check(text)

    def test_missing_owner_window(self):
        later = window().replace("seq=1", "seq=3").replace("begin_us=100", "begin_us=200").replace("end_us=200", "end_us=300")
        with self.assertRaises(ValueError):
            check(window() + later)

    def test_summary_does_not_add_overlapping_threads(self):
        result = summarize(check(window() + window(2)))
        self.assertEqual(len(result["owners"]), 2)
        for owner in result["owners"]:
            self.assertEqual(owner["wall_us"], 100)
            self.assertIsNone(owner["cpu_percent_of_one_core"])
            self.assertEqual(owner["phases"][0]["name"], "queue_wait")
            self.assertEqual(owner["phases"][0]["owner_wall_percent"], 60)

    def test_cpu_samples_have_their_own_interval(self):
        text = window().replace("coverage=partial", "coverage=partial cpu_valid=1 cpu_run_us=25 cpu_window_us=50")
        owner = summarize(check(text))["owners"][0]
        self.assertEqual(owner["cpu_percent_of_one_core"], 50)
        with self.assertRaises(ValueError):
            check(text.replace("cpu_window_us=50", "cpu_window_us=0"))

    def test_lost_reports_fail(self):
        for suffix in ("telemetry_report_overflow", "telemetry_registry_full"):
            with self.assertRaises(ValueError):
                check(window() + suffix)
