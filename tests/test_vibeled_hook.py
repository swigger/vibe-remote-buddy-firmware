"""Agent interface regression tests; no hardware or extra Python dependencies."""
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("hook", ROOT / "tools/vibeled_agent_hook.py")
hook = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hook)


class HookTests(unittest.TestCase):
    def test_syntax_and_event_precedence(self):
        self.assertEqual(hook._event_from(["agent_hook.py", "--agent", "codex", "Stop"], {}), ("codex", "Stop"))
        self.assertEqual(hook._event_from(["agent_hook.py", "--agent=claude", "Stop"], {"hook_event_name": "PermissionRequest"}), ("claude", "PermissionRequest"))
        self.assertEqual(hook._command_for_event("SessionEnd"), ("off",))
        self.assertIn("yellow", hook._command_for_event("PermissionRequest"))
        self.assertIn("red", hook._command_for_event("PostToolUseFailure"))
        self.assertIsNone(hook._command_for_event("Unknown"))
        self.assertEqual(hook._command_for_event("preToolUse"), hook._command_for_event("PreToolUse"))

    def test_dedup_and_changed_state(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, {"VIBELED_HOOK_STATE_DIR": directory}), patch.object(hook, "_launch") as launch:
            app = Path(directory) / "vibeled.exe"
            work = hook._command_for_event("PreToolUse")
            hook._launch_coalesced(app, work)
            hook._launch_coalesced(app, work)
            self.assertEqual(launch.call_count, 1)
            hook._launch_coalesced(app, ("off",))
            self.assertEqual(launch.call_count, 2)
            state = Path(directory) / "agent-hook-state.json"
            state.write_text("[]")
            hook._launch_coalesced(app, work)
            self.assertEqual(launch.call_count, 3)

    def test_failed_launch_does_not_suppress_retry(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, {"VIBELED_HOOK_STATE_DIR": directory}), patch.object(hook, "_launch", side_effect=OSError):
            with self.assertRaises(OSError):
                hook._launch_coalesced(Path(directory) / "vibeled", ("off",))
            self.assertFalse((Path(directory) / "agent-hook-state.json").exists())

    def test_discovery(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / "vibeled.app"
            binary = app / "Contents/MacOS/vibeled"
            binary.parent.mkdir(parents=True)
            binary.touch()
            with patch.dict(os.environ, {"VIBELED_TOOL": str(app)}):
                self.assertEqual(hook._find_app(), binary)

    def test_silent_failure_all_entry_points(self):
        env = dict(os.environ, VIBELED_TOOL="__missing_vibeled__")
        for script in ("agent_hook.py", "logled_agent_hook.py", "vibeled_agent_hook.py"):
            for payload in (b"not json", b'{"hook_event_name":"Stop"}', b"{}"):
                result = subprocess.run([sys.executable, str(ROOT / "tools" / script), "--agent", "codex", "Stop"],
                                        input=payload, capture_output=True, env=env, timeout=5)
                self.assertEqual((result.returncode,result.stdout,result.stderr), (0,b"",b""))

    def test_relocated_package_discovery(self):
        with tempfile.TemporaryDirectory(prefix="vibeled portable ") as directory:
            package = Path(directory)
            for script, binary in ((package / "hooks/vibeled_agent_hook.py", package / "vibeled.exe"),
                                   (package / "vibeled.app/Contents/Resources/hooks/vibeled_agent_hook.py",
                                    package / "vibeled.app/Contents/MacOS/vibeled")):
                binary.parent.mkdir(parents=True, exist_ok=True)
                binary.touch()
                with patch.object(hook, "__file__", str(script)), patch.dict(os.environ, {"USERPROFILE": directory, "HOME": directory}, clear=True), patch.object(hook.shutil, "which", return_value=None):
                    self.assertEqual(hook._find_app(), binary)


if __name__ == "__main__":
    unittest.main()
