"""Claude Code hook -> per-session state files for the Clawdmeter daemon.

Registered in ~/.claude/settings.json for SessionStart, UserPromptSubmit,
PreToolUse, PostToolUse, Notification, Stop and SessionEnd. Claude Code passes
the event as JSON on stdin. This script records it and exits 0 with no output,
so it never blocks or alters Claude Code; any error is swallowed.

Writes %LOCALAPPDATA%\\Clawdmeter\\sessions\\<session_id>.json:
  {"sid", "cwd", "state", "tool", "ts", "turn_start", "turn_end",
   "last_turn_secs", "date", "turn_secs_today"}
and, for older daemons, the latest event to cc_state.json.
"""
import json
import os
import sys
import time
from pathlib import Path

ROOT = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local")) / "Clawdmeter"
STATE_FILE = ROOT / "cc_state.json"
SESSIONS = ROOT / "sessions"


def classify(evt: dict) -> tuple[str, str] | None:
    name = evt.get("hook_event_name", "")
    if name == "SessionStart":
        return ("ready", "")
    if name == "UserPromptSubmit":
        return ("prompt", "")
    if name == "PostToolUse":
        return ("thinking", "")
    if name == "PreToolUse":
        return ("tool", str(evt.get("tool_name", "")))
    if name == "Notification":
        kind = str(evt.get("notification_type", "")).lower()
        msg = str(evt.get("message", "")).lower()
        if "permission" in kind or "permission" in msg:
            return ("needs_you", "")
        return ("waiting", "")       # idle prompt: Claude finished and is waiting for you
    if name == "Stop":
        return ("done", "")
    if name == "SessionEnd":
        return ("ended", "")
    return None                      # SubagentStop, PreCompact, ...: no change


def write_json(p: Path, data: dict) -> None:
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_name(p.name + f".{os.getpid()}.tmp")
    tmp.write_text(json.dumps(data), encoding="utf-8")
    os.replace(tmp, p)


def main() -> None:
    try:
        evt = json.loads(sys.stdin.read() or "{}")
        # The daemon's token keep-alive (a tiny `claude -p`) isn't you working.
        if os.environ.get("CLAWDMETER_KEEPALIVE") or \
                str(evt.get("cwd", "")).rstrip("\\/").lower().endswith("clawdmeter\\keepalive"):
            return
        result = classify(evt)
        if result is None:
            return
        state, tool = result
        now = time.time()
        today = time.strftime("%Y-%m-%d")

        sid = "".join(c for c in str(evt.get("session_id", "")) if c.isalnum() or c in "-_")[:80]
        if sid:
            f = SESSIONS / f"{sid}.json"
            try:
                s = json.loads(f.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                s = {}
            if s.get("date") != today:
                s["date"], s["turn_secs_today"] = today, 0
            s["sid"] = sid
            if evt.get("cwd"):
                s["cwd"] = str(evt["cwd"])
            if state == "prompt":
                # demo_backdate_s lets the installer's demo fake a long turn
                s["turn_start"] = now - float(evt.get("demo_backdate_s", 0) or 0)
                state = "thinking"
            elif state == "done":
                start = float(s.get("turn_start") or 0)
                if start:
                    secs = max(0, int(now - start))
                    s["last_turn_secs"] = secs
                    s["turn_secs_today"] = int(s.get("turn_secs_today", 0)) + secs
                s["turn_start"] = 0
                s["turn_end"] = now
            elif state == "ended":
                s["turn_start"] = 0
            s["state"], s["tool"], s["ts"] = state, tool, now
            write_json(f, s)

        legacy = {"prompt": "thinking", "waiting": "done", "ended": "idle"}.get(state, state)
        write_json(STATE_FILE, {"state": legacy, "tool": tool, "ts": now})
    except Exception:
        pass


if __name__ == "__main__":
    main()
    sys.exit(0)
