#!/usr/bin/env python3
"""Explicit hardware smoke test. Changes lights and leaves a named upgrade-test preset."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tool", type=Path)
    parser.add_argument("--device", default="XGAI_LED")
    parser.add_argument("--check-persisted", action="store_true", help="verify/delete the preset left before firmware restart")
    args = parser.parse_args()
    executable = str(args.tool.resolve())

    def call(*words, code=0):
        result = subprocess.run([executable,"--device",args.device,*words], capture_output=True, text=True, timeout=20)
        text = result.stdout.strip() or result.stderr.strip()
        assert result.returncode == code, (words,result.returncode,text)
        return text

    if args.check_persisted:
        assert "vibe-test" in call("list")
        assert "color=#003311" in call("play","vibe-test","--level","2")
        assert "altcolor=#110033" in call("status")
        call("delete","vibe-test")
        call("off")
        print("PASS: saved preset survived firmware update/restart")
        return

    call("off")
    assert call("status") == "ok off"
    for color in ("red","green","blue"):
        call("set",color,"--duration-ms","1000")
        time.sleep(1)
    call("set","red","--altcolor","blue","--on-ms","250","--off-ms","250","--duration-ms","2000")
    assert "altcolor=#0000ff" in call("status")
    time.sleep(2.1)
    assert call("status") == "ok off"
    call("set","green","--level","1")
    call("set","red","--level","5","--duration-ms","600")
    assert "level=5" in call("status")
    time.sleep(.65)
    assert "level=1" in call("status")
    call("set","black","--level","1")
    assert call("status") == "ok off"
    assert "error" in call("raw","set red --level 6",code=2)
    assert call("raw","getip") == "no ip"
    call("raw","XGAI_LED: heartbeat --level 4")
    assert "name=heartbeat" in call("status")
    call("off")
    with ThreadPoolExecutor(max_workers=8) as pool:
        replies = list(pool.map(lambda i: call("set","blue","--level",str(i%5+1)), range(24)))
    assert all(text.startswith("ok ") for text in replies)
    call("off")
    with tempfile.TemporaryDirectory() as directory:
        env = dict(os.environ, VIBELED_TOOL=executable, VIBELED_DEVICE=args.device,
                   VIBELED_HOOK_STATE_DIR=directory, VIBELED_HOOK_DEDUP_SECONDS="0")
        def hook(event, expected):
            result = subprocess.run([sys.executable,str(ROOT / "tools/agent_hook.py"),"--agent","codex",event],
                input=b"{}", capture_output=True, env=env, timeout=5)
            assert (result.returncode,result.stdout,result.stderr) == (0,b"",b"")
            until = time.monotonic()+6
            while time.monotonic() < until:
                if expected in call("status"):
                    return
                time.sleep(.1)
            raise AssertionError((event,expected))
        hook("PreToolUse","color=#050500")
        hook("PermissionRequest","on=350 off=350")
        hook("Stop","duration=60000")
        hook("SessionEnd","ok off")
    assert "ok saved" in call("save","vibe-test","#003311","--altcolor","#110033","--on-ms","250","--off-ms","750","--duration-ms","1000","--level","2")
    assert "vibe-test" in call("list")
    call("off")
    print("PASS: colors, dual-color timing, priority/expiry, raw errors, 24 concurrent CLI calls, Agent event chain")
    print("Preset vibe-test retained for --check-persisted after restart/update")


if __name__ == "__main__":
    main()
