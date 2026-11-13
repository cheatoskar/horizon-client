"""Unit test of tools/bench_report.py with synthetic series data: python tools/test_bench_report.py"""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench_report  # noqa: E402


def run(set_label, n, fps, stutters=0, seconds=20.0, warmup=False, low1=None, note=""):
    return {
        "order": 0, "set": set_label, "run": 0 if warmup else n, "warmup": warmup, "note": note,
        "seconds": seconds, "frames": int(fps * seconds), "avgFps": fps, "medianMs": 1000.0 / fps,
        "low1Fps": low1 if low1 is not None else fps * 0.8, "low01Fps": fps * 0.6, "stutters": stutters,
    }


def series():
    runs = [run("baseline", 0, 50.0, warmup=True), run("fast", 0, 90.0, warmup=True)]  # warm-ups must not count
    # baseline: 100, 102, 98 -> mean 100, spread 4
    for i, f in enumerate([100.0, 102.0, 98.0], 1):
        runs.append(run("baseline", i, f, stutters=2))
    # fast: 110, 111, 109 -> +10 %, well above the spread
    for i, f in enumerate([110.0, 111.0, 109.0], 1):
        runs.append(run("fast", i, f, stutters=1, note="no hybrid CPU found: all cores" if i == 1 else ""))
    # noise: 101, 99.5, 100.5 -> mean 100.33, difference 0.33 < spread 4
    for i, f in enumerate([101.0, 99.5, 100.5], 1):
        runs.append(run("noise", i, f, stutters=2))
    return {
        "version": 1, "kind": "series", "stamp": "20261112-100000", "completed": True, "game": "tmnf",
        "gpu": "Test GPU", "cpu": "Test CPU", "resolution": "1920x1080", "tmvs": False, "mode": "replay",
        "replay": "C:\\r\\a.Replay.Gbx", "map": "", "seconds": 20, "settle": 2, "runsPerSet": 3, "warmupPerSet": 1,
        "sets": [
            {"label": "baseline", "options": {}, "status": "ok", "skipReason": ""},
            {"label": "fast", "options": {}, "status": "ok", "skipReason": ""},
            {"label": "noise", "options": {}, "status": "ok", "skipReason": ""},
            {"label": "vs", "options": {}, "status": "skipped", "skipReason": "needs restart: vsync"},
        ],
        "runs": runs,
    }


class BenchReportTest(unittest.TestCase):
    def test_numbers(self):
        res = bench_report.analyze(series())
        self.assertEqual(res["baseline"], "baseline")
        sets = {s["label"]: s for s in res["sets"]}
        self.assertEqual(list(sets), ["baseline", "fast", "noise"])  # skipped set has no row
        self.assertEqual(sets["baseline"]["runs"], 3)                 # warm-up left out
        self.assertAlmostEqual(sets["baseline"]["metrics"]["avgFps"]["mean"], 100.0)
        self.assertAlmostEqual(sets["fast"]["metrics"]["avgFps"]["mean"], 110.0)
        self.assertAlmostEqual(sets["fast"]["metrics"]["avgFps"]["percent"], 10.0)
        # 2 stutters in 20 s = 6 per minute; 1 in 20 s = 3 per minute (-50 %)
        self.assertAlmostEqual(sets["baseline"]["metrics"]["stuttersPerMin"]["mean"], 6.0)
        self.assertAlmostEqual(sets["fast"]["metrics"]["stuttersPerMin"]["percent"], -50.0)
        # median ms: 1000 / fps, lower for the faster set
        self.assertLess(sets["fast"]["metrics"]["medianMs"]["percent"], 0)
        self.assertEqual(res["skipped"], [{"label": "vs", "reason": "needs restart: vsync"}])

    def test_significance_hint(self):
        sets = {s["label"]: s for s in bench_report.analyze(series())["sets"]}
        self.assertEqual(sets["noise"]["hint"], bench_report.NO_DIFF)
        self.assertTrue(sets["fast"]["hint"].startswith("über der Streuung"))
        self.assertIn("FPS (Mittel)", sets["fast"]["hint"])
        self.assertTrue(sets["baseline"]["hint"].startswith("Bezug"))

    def test_single_baseline_run_has_no_spread(self):
        data = series()
        data["runs"] = [r for r in data["runs"] if not (r["set"] == "baseline" and r["run"] in (2, 3))]
        sets = {s["label"]: s for s in bench_report.analyze(data)["sets"]}
        self.assertIn("Streuung unbekannt", sets["fast"]["hint"])

    def test_render_and_cli(self):
        data = series()
        text = bench_report.render(data, bench_report.analyze(data))
        self.assertIn("| Set | Läufe | FPS (Mittel) | Median ms | 1 % Low | 0,1 % Low | Ruckler/min | Hinweis |", text)
        self.assertIn("110.0 (+10.0 %)", text)
        self.assertIn(bench_report.NO_DIFF, text)
        self.assertIn("vs: needs restart: vsync", text)
        self.assertIn("no hybrid CPU found", text)
        self.assertIn("Test GPU", text)
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "20261112-100000_series.json")
            with open(src, "w", encoding="utf-8") as f:
                json.dump(data, f)
            self.assertEqual(bench_report.main([src]), 0)
            md = os.path.join(d, "20261112-100000_series.md")
            self.assertTrue(os.path.exists(md))
            with open(md, encoding="utf-8") as f:
                self.assertIn("| fast | 3 |", f.read())

    def test_rejects_other_json(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.json")
            with open(p, "w") as f:
                json.dump({"version": 1}, f)
            with self.assertRaises(ValueError):
                bench_report.load(p)


if __name__ == "__main__":
    unittest.main()
