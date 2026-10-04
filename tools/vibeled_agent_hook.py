#!/usr/bin/env python3
"""Silent cross-platform agent status hook: --agent NAME EVENT, unchanged from logled.
The stdin JSON event takes precedence. All failures remain silent with exit code 0.
Only the Qt vibeled executable opens USB CDC; Python needs no third-party packages.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import time
from typing import Any, Sequence

__version__ = "2.0.0"

# Keep the same semantic states as PromLight's default agent hook.
_EVENT_STATES = {
    # Kiro CLI official trigger names.
    "agentSpawn": "start",
    "userPromptSubmit": "work1",
    "preToolUse": "work",
    "postToolUse": "work1",
    "stop": "idle",
    # Claude/Codex-compatible trigger names.
    "SessionStart": "start",
    "UserPromptSubmit": "work",
    "PreToolUse": "work",
    "PostToolUse": "work",
    "PostToolUseFailure": "error",
    "PermissionRequest": "await",
    "PermissionDenied": "await",
    "Elicitation": "await",
    "SubagentStart": "work",
    "SubagentStop": "work",
    "PreCompact": "work",
    "PostCompact": "work",
    "Stop": "idle",
    "SessionEnd": "end",
    "StopFailure": "error",
}

# Arguments are passed directly to the Qt vibeled executable.
_STATE_COMMANDS: dict[str, tuple[str, ...]] = {
    # Brief green flashes; the firmware turns the effect off after its duration.
    "start": ("set", "green", "--on-ms", "180", "--off-ms", "180", "--duration-ms", "1800", "--level", "3"),
    "work": ("set", "050500", "--level", "3"),
    "work1": ("set", "010100", "--level", "3"),
    "await": ("set", "yellow", "--on-ms", "350", "--off-ms", "350", "--level", "3"),
    "idle": ("set", "green", "--on-ms", "500", "--off-ms", "500", "--duration-ms", "60000", "--level", "3"),
    "error": ("set", "red", "--on-ms", "180", "--off-ms", "180", "--duration-ms", "10000", "--level", "3"),
    "end": ("off",),
}

_DEFAULT_DEDUP_SECONDS = 15.0


def _read_payload() -> dict[str, Any]:
    try:
        if sys.stdin is None or sys.stdin.isatty():
            return {}
        raw = sys.stdin.buffer.read()
        if not raw:
            return {}
        value = json.loads(raw.decode("utf-8-sig", "replace"))
        return value if isinstance(value, dict) else {}
    except Exception:
        return {}


def _parse_args(argv: Sequence[str]) -> tuple[str, str]:
    agent = ""
    positional = ""
    index = 1
    while index < len(argv):
        argument = argv[index]
        if argument == "--agent":
            agent = argv[index + 1] if index + 1 < len(argv) else ""
            index += 2
            continue
        if argument.startswith("--agent="):
            agent = argument[len("--agent=") :]
            index += 1
            continue
        if not positional and not argument.startswith("-"):
            positional = argument
        index += 1
    return agent.strip().lower(), positional


def _event_from(argv: Sequence[str], payload: dict[str, Any]) -> tuple[str, str]:
    agent, positional = _parse_args(argv)
    event = payload.get("hook_event_name") or positional or _env("HOOK_EVENT", "")
    return agent, str(event) if event else ""


def _command_for_event(event: str) -> tuple[str, ...] | None:
    state = _EVENT_STATES.get(event)
    return _STATE_COMMANDS.get(state) if state else None


def _env(suffix: str, default: str = "") -> str:
    return os.environ.get("VIBELED_" + suffix, os.environ.get("LOGLED_" + suffix, default))


def _find_app() -> Path | None:
    configured = os.environ.get("VIBELED_TOOL", _env("APP", "")).strip()
    here = Path(__file__).resolve().parent
    root = here.parent
    if configured:
        candidates = [Path(configured).expanduser()]
    else:
        located = shutil.which("vibeled")
        candidates = [here / "vibeled.exe", root / "vibeled.exe",
                      root.parent / "MacOS" / "vibeled",
                      root / "dist" / "vibeled" / "vibeled.exe",
                      root / "dist" / "vibeled.exe", root / "dist" / "vibeled.app",
                      root / "build" / "vibeled" / "Release" / "vibeled.exe",
                      root / "build" / "vibeled" / "vibeled.exe",
                      Path.home() / "Applications" / "vibeled.app", Path("/Applications/vibeled.app")]
        if located:
            candidates.insert(0, Path(located))
    for candidate in candidates:
        if candidate.suffix.lower() == ".app":
            candidate = candidate / "Contents" / "MacOS" / "vibeled"
        if candidate.is_file():
            return candidate
    return None


def _state_directory() -> Path:
    configured = _env("HOOK_STATE_DIR", "").strip()
    if configured:
        return Path(configured).expanduser()
    if os.name == "nt":
        return Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local")) / "vibeled"
    return Path.home() / "Library" / "Caches" / "vibeled"


def _dedup_seconds() -> float:
    try:
        value = float(_env("HOOK_DEDUP_SECONDS", str(_DEFAULT_DEDUP_SECONDS)))
        return value if value >= 0 and value < float("inf") else _DEFAULT_DEDUP_SECONDS
    except (TypeError, ValueError):
        return _DEFAULT_DEDUP_SECONDS


def _launch(app: Path, command: tuple[str, ...]) -> None:
    device = _env("DEVICE", "").strip()
    args = [str(app), "--timeout", "5"]
    if device:
        args += ["--device", device]
    args += command
    options: dict[str, Any] = {"close_fds": True}
    if os.name == "nt":
        options["creationflags"] = subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        options["start_new_session"] = True
    subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, **options)


def _launch_coalesced(app: Path, command: tuple[str, ...]) -> None:
    """Keep hook locking brief. The Qt executable serializes device commands."""
    directory = _state_directory()
    directory.mkdir(parents=True, exist_ok=True)
    state_path = directory / "agent-hook-state.json"
    with (directory / "agent-hook.lock").open("a+b") as lock:
        if os.name == "nt":
            import msvcrt
            lock.seek(0, 2)
            if lock.tell() == 0:
                lock.write(b"0")
                lock.flush()
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        # Closing the file releases the OS lock on either platform.
        try:
            state = json.loads(state_path.read_text(encoding="utf-8"))
        except Exception:
            state = {}
        key = [str(app.resolve()), _env("DEVICE", ""), *command]
        now = time.time()
        previous = state.get("time") if isinstance(state, dict) else None
        elapsed = now - previous if isinstance(previous, (int, float)) else None
        if isinstance(state, dict) and state.get("command") == key and elapsed is not None and 0 <= elapsed < _dedup_seconds():
            return
        _launch(app, command)
        try:
            temporary = state_path.with_name(f"{state_path.name}.{os.getpid()}.tmp")
            temporary.write_text(json.dumps({"command": key, "time": now}), encoding="utf-8")
            os.replace(temporary, state_path)
        except OSError:
            pass  # Already launched; never submit twice because the cache is unwritable.


def main(argv: Sequence[str]) -> int:
    payload = _read_payload()
    _agent, event = _event_from(argv, payload)
    command = _command_for_event(event)
    if command is None:
        return 0
    app = _find_app()
    if app is not None:
        try:
            _launch_coalesced(app, command)
        except BlockingIOError:
            # A simultaneous event must still reach the serialized Qt queue.
            _launch(app, command)
        except OSError:
            _launch(app, command)
    return 0


def entry() -> None:
    try:
        main(sys.argv)
    except BaseException:
        pass
    raise SystemExit(0)


if __name__ == "__main__":
    entry()
