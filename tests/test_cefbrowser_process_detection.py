#!/usr/bin/env python3
"""Regression coverage for the real yaVDR process tree and static options."""

import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "detector", ROOT / "tools/detect_cefbrowser_static_root.py")
detector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(detector)


class DetectionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.proc = Path(self.temp.name)

    def process(self, pid, parent, *args):
        entry = self.proc / str(pid)
        entry.mkdir()
        (entry / "comm").write_text("cefbrowser\n")
        (entry / "status").write_text(f"Name:\tcefbrowser\nPPid:\t{parent}\n")
        (entry / "cmdline").write_bytes(
            ("\0".join(["/opt/hbbtv/cefbrowser/cefbrowser", *args]) + "\0").encode())
        return entry

    def test_real_yavdr_inventory(self):
        self.process(1946, 1, "--config=/etc/hbbtv/sockets.ini",
                     "--profilepath=/var/lib/hbbtv/cefbrowser/profile",
                     "--cachepath=/var/lib/hbbtv/cefbrowser/profile/cache",
                     "--staticpath=/var/lib/hbbtv/cefbrowser",
                     "--browserdb=/var/lib/hbbtv/cefbrowser/database",
                     "--uagent=/var/lib/hbbtv/cefbrowser/database",
                     "--ozone-platform=headless", "--disable-gpu")
        for pid, parent, kind in [(2098, 1946, "zygote"), (2099, 1946, "zygote"),
                                  (2216, 2098, "gpu-process"), (2236, 2099, "utility"),
                                  (2500, 2099, "renderer")]:
            self.process(pid, parent, f"--type={kind}", "--staticpath=/wrong")
        self.assertEqual(detector.detect(self.proc),
                         (1946, Path("/var/lib/hbbtv/cefbrowser")))

    def test_option_spellings_and_spaces(self):
        for option in ["--staticPath", "--staticpath", "-s"]:
            with self.subTest(option=option):
                self.assertEqual(detector.static_argument(["cef", option, "/static path"]),
                                 "/static path")
        for option in ["--staticPath=", "--staticpath=", "-s"]:
            self.assertEqual(detector.static_argument(["cef", option + "/static path"]),
                             "/static path")

    def test_invalid_options_do_not_fall_back(self):
        for args in [["-s"], ["--staticpath="], ["--staticPath", "--disable-gpu"],
                     ["-s", "/one", "--staticpath=/two"]]:
            with self.subTest(args=args), self.assertRaises(ValueError):
                detector.static_argument(["cef", *args])

    def test_pid_one_parent_is_preferred(self):
        self.process(10, 77, "-s/other")
        self.process(1946, 1, "-s/expected")
        self.assertEqual(detector.detect(self.proc)[0], 1946)

    def test_root_without_pid_one_parent(self):
        self.process(10, 77, "-s/expected")
        self.process(11, 10, "-s/other")
        self.assertEqual(detector.detect(self.proc)[0], 10)

    def test_two_independent_roots_fail(self):
        for parent in [1, 77]:
            with tempfile.TemporaryDirectory() as directory:
                old = self.proc
                self.proc = Path(directory)
                self.process(10, parent, "-s/a")
                self.process(11, parent, "-s/b")
                with self.assertRaises(ValueError):
                    detector.detect(self.proc)
                self.proc = old

    def test_children_only_or_no_browser_fail(self):
        with self.assertRaises(ValueError):
            detector.detect(self.proc)
        self.process(10, 1, "--type=renderer")
        self.process(11, 1, "--type", "utility")
        with self.assertRaises(ValueError):
            detector.detect(self.proc)

    def test_unreadable_known_process_fails(self):
        entry = self.process(10, 1, "-s/expected")
        (entry / "cmdline").unlink()
        with self.assertRaises(OSError):
            detector.detect(self.proc)

    @unittest.skipIf(os.name == "nt", "Linux proc symlinks")
    def test_executable_fallback_and_relative_path(self):
        exe = self.proc / "bin/cefbrowser"
        exe.parent.mkdir()
        exe.touch()
        entry = self.process(10, 1)
        (entry / "exe").symlink_to(exe)
        self.assertEqual(detector.detect(self.proc)[1], exe.parent)
        (entry / "cwd").symlink_to(self.proc)
        (entry / "cmdline").write_bytes(b"cefbrowser\0-s\0static content\0")
        self.assertEqual(detector.detect(self.proc)[1], self.proc / "static content")

    @unittest.skipIf(os.name == "nt", "Linux Bash installer")
    def test_installer_detection_and_failure_before_apply(self):
        tools = self.proc / "tools"
        tools.mkdir()
        shutil.copy(ROOT / "tools/install_cefbrowser_zdf_controls_fix.sh", tools)
        patcher = tools / "apply_cefbrowser_zdf_controls_fix.py"
        patcher.write_text("from pathlib import Path\nPath(__file__).with_name('called').touch()\n")
        patcher.chmod(0o755)
        helper = tools / "detect_cefbrowser_static_root.py"
        helper.write_text("raise SystemExit(1)\n")
        command = ["bash", str(tools / "install_cefbrowser_zdf_controls_fix.sh")]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((tools / "called").exists())
        static = self.proc / "static content"
        for relative in ["js/video_quirks.js", "js/keyhandler.js", "css/videoquirks.css"]:
            target = static / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.touch()
        helper.write_text(f"print({str(static)!r})\n")
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"STATIC_ROOT={static}", result.stdout)
        self.assertTrue((tools / "called").exists())


if __name__ == "__main__":
    unittest.main()
