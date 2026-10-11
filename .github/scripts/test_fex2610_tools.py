#!/usr/bin/env python3
"""Tests for build attestation and FF7 frame-time analysis (stdlib only)."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

flags = load("fex_flags", ".github/scripts/verify_fex2610_compile_flags.py")
frames = load("fex_frames", "Scripts/GameNative/benchmark_fex2610_frametimes.py")

class CompileFlagsTests(unittest.TestCase):
    def write_entries(self, temp, extra=""):
        path = Path(temp) / "compile_commands.json"
        command = ("arm64ec-w64-mingw32-clang++ -mtune=cortex-a77 -march=armv8-a+crc "
                   "-O3 " + extra + " -c test.cpp")
        entries = [{"file": f"/src/FEXCore/Source/Translator{n}.cpp", "command": command}
                   for n in range(12)]
        path.write_text(json.dumps(entries), encoding="utf-8")
        return path

    def test_effective_last_march_wins(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self.write_entries(tmp, "-march=armv8.2-a+crc")
            self.assertEqual(flags.verify(path, "armv8.2-a+crc", "cortex-a77", "arm64ec")["status"], "pass")

    def test_reject_overridden_march(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self.write_entries(tmp, "-march=armv8.2-a+crc -march=armv8-a+crc")
            with self.assertRaisesRegex(ValueError, "effective march"):
                flags.verify(path, "armv8.2-a+crc", "cortex-a77", "arm64ec")

    def test_reject_wrong_target(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self.write_entries(tmp, "-march=armv8.2-a+crc")
            with self.assertRaisesRegex(ValueError, "target compiler"):
                flags.verify(path, "armv8.2-a+crc", "cortex-a77", "aarch64")

class FrameTimeTests(unittest.TestCase):
    def test_frametimes_and_trim(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "frames.csv"
            path.write_text("frame_time_ms\n" + ("20\n" * 250), encoding="utf-8")
            measured = frames.trim_warmup(frames.read_intervals(path), 1.0)
            self.assertEqual(len(measured), 200)
            stats = frames.metrics(measured, 120)
            self.assertEqual(stats["mean_fps"], 50.0)
            self.assertEqual(stats["p99_frame_ms"], 20.0)

    def test_present_timestamps_and_ordering(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "frames.csv"
            path.write_text("present_time_ns\n0\n16000000\n32000000\n", encoding="utf-8")
            self.assertEqual(frames.read_intervals(path), [16, 16])
            path.write_text("present_time_ns\n0\n16000000\n16000000\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "strictly increase"):
                frames.read_intervals(path)

    def test_comparison_and_insufficient_samples(self):
        with self.assertRaisesRegex(ValueError, "need at least"):
            frames.metrics([10] * 5, 120)
        base = frames.metrics([20] * 200, 120)
        candidate = frames.metrics([16] * 200, 120)
        self.assertEqual(frames.compare(base, candidate)["mean_fps_percent_change"], 25.0)
        self.assertEqual(frames.compare(base, candidate)["p99_frame_time_percent_reduction"], 20.0)

if __name__ == "__main__":
    unittest.main()
