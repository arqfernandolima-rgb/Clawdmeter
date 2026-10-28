"""Add (or remove) the Clawdmeter hooks in Claude Code's user settings.

usage:
  python install_hooks.py <pythonw.exe> <daemon dir>     # add / update
  python install_hooks.py --remove                       # remove all Clawdmeter hooks

Two hooks:
  cc_status_hook.py   - live status on the board (prompt/tool/stop events)
  cc_approval_hook.py - PermissionRequest: answer simple prompts on the board

Backs up ~/.claude/settings.json first, keeps every existing setting and hook,
and is idempotent (skips events that already run the script).
"""
import json
import shutil
import sys
import time
from pathlib import Path

SETTINGS = Path.home() / ".claude" / "settings.json"
TOOL_EVENTS = {"PreToolUse", "PostToolUse", "PermissionRequest"}
HOOKS = [
    # script, events, timeout (s)
    ("cc_status_hook.py",
     ["SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "Notification", "Stop", "SessionEnd"],
     5),
    # Waits for a tap for approval_timeout_s (default 20 s, max 120) before
    # stepping aside; the hook timeout sits above that ceiling.
    ("cc_approval_hook.py", ["PermissionRequest"], 130),
]
MARKS = [h[0] for h in HOOKS]


def load() -> dict:
    if not SETTINGS.exists():
        return {}
    text = SETTINGS.read_text(encoding="utf-8-sig").strip()
    return json.loads(text) if text else {}


def save(data: dict) -> None:
    SETTINGS.parent.mkdir(parents=True, exist_ok=True)
    if SETTINGS.exists():
        backup = SETTINGS.with_name(f"settings.json.clawdmeter-backup-{time.strftime('%Y%m%d-%H%M%S')}")
        shutil.copy2(SETTINGS, backup)
        print(f"backup: {backup}")
    SETTINGS.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def has(groups: list, mark: str) -> bool:
    return any(mark in str(h.get("command", "")) for g in groups for h in g.get("hooks", []))


def add(pythonw: str, daemon_dir: str) -> None:
    data = load()
    hooks = data.setdefault("hooks", {})
    added = []
    for script, events, timeout in HOOKS:
        cmd = f'"{Path(pythonw).as_posix()}" "{(Path(daemon_dir) / script).as_posix()}"'
        for ev in events:
            groups = hooks.setdefault(ev, [])
            if has(groups, script):
                continue
            group = {"hooks": [{"type": "command", "command": cmd, "timeout": timeout}]}
            if ev in TOOL_EVENTS:
                group = {"matcher": "*", **group}
            groups.append(group)
            added.append(f"{ev}->{script}")
    if added:
        save(data)
        print("added: " + ", ".join(added))
    else:
        print("hooks already present, nothing to do")


def remove() -> None:
    data = load()
    hooks = data.get("hooks", {})
    changed = False
    for ev in list(hooks):
        kept = []
        for g in hooks[ev]:
            inner = [h for h in g.get("hooks", []) if not any(m in str(h.get("command", "")) for m in MARKS)]
            if len(inner) != len(g.get("hooks", [])):
                changed = True
            if inner:
                kept.append({**g, "hooks": inner})
        if kept:
            hooks[ev] = kept
        else:
            del hooks[ev]
    if not hooks:
        data.pop("hooks", None)
    if changed:
        save(data)
        print("removed Clawdmeter hooks")
    else:
        print("no Clawdmeter hooks found")


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "--remove":
        remove()
    elif len(sys.argv) == 3:
        add(sys.argv[1], sys.argv[2])
    else:
        print(__doc__)
        sys.exit(2)
