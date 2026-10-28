"""Claude Code PermissionRequest hook: let the Clawdmeter answer simple prompts.

Flow: Claude Code runs this when a tool call needs permission. If the board is
connected (daemon heartbeat is fresh) and the request fits on the small screen,
it is written to %LOCALAPPDATA%\\Clawdmeter\\approval_req.json. The daemon shows
it on the board; a tap on Allow/Deny comes back as approval_resp.json and this
hook returns the matching decision.

Fail-safe by design: on ANY problem (board offline, request too long or of an
unsupported kind, another request already pending, timeout, bad input) the
hook prints nothing and exits 0, so Claude Code's normal permission prompt
handles it exactly as before. It never allows anything on its own.

Config (%LOCALAPPDATA%\\Clawdmeter\\config):
  approval = on|off            (default on)
  approval_timeout_s = 20      (how long to wait for a tap)
"""
import json
import os
import sys
import time
from pathlib import Path

ROOT = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local")) / "Clawdmeter"
REQ = ROOT / "approval_req.json"
RESP = ROOT / "approval_resp.json"
ALIVE = ROOT / "daemon_alive"
CONFIG = ROOT / "config"

MAX_DETAIL = 200       # one-line details (paths, URLs, queries); longer -> keyboard
MAX_CMD = 600          # shell commands, multi-line OK: the board scrolls, and Allow
                       # unlocks only after scrolling to the end. Longer -> keyboard
MAX_HEAD = 90          # Claude's one-line description of the command (the headline)
MAX_TITLE = 16         # characters that fit one line of the big title
ALIVE_MAX_AGE = 6.0    # daemon heartbeat is written every ~2 s while connected


def config_value(key: str, default: str) -> str:
    try:
        for line in CONFIG.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if "=" in line:
                k, v = line.split("=", 1)
                if k.strip().lower() == key:
                    return v.strip()
    except OSError:
        pass
    return default


def short_path(p: str) -> str:
    p = str(p).replace("\\", "/").rstrip("/")
    parts = [x for x in p.split("/") if x]
    out = "/".join(parts[-3:]) if len(parts) > 3 else p
    return out if len(out) <= MAX_DETAIL else "..." + out[-(MAX_DETAIL - 3):]


def one_line(s: str) -> str | None:
    s = str(s).strip()
    if not s or "\n" in s or "\r" in s or len(s) > MAX_DETAIL:
        return None          # can't show all of it -> answer at the keyboard
    return s


def command_text(s: str) -> str | None:
    """A shell command as the board shows it (all of it), or None if too long."""
    s = str(s).replace("\r\n", "\n").replace("\r", "\n").replace("\t", "  ").strip()
    if not s or len(s) > MAX_CMD:
        return None
    return s


def headline(s: str) -> str:
    s = " ".join(str(s or "").split())
    return s if len(s) <= MAX_HEAD else s[:MAX_HEAD - 3].rstrip() + "..."


def summarize(tool: str, ti: dict) -> tuple[str, str, str] | None:
    """(title, headline, detail) for the board, or None when it shouldn't go there."""
    if tool in ("Bash", "PowerShell"):
        d = command_text(ti.get("command", ""))
        return ("Run a command", headline(ti.get("description", "")), d) if d else None
    s = summarize_other(tool, ti)
    return (s[0], "", s[1]) if s else None


def summarize_other(tool: str, ti: dict) -> tuple[str, str] | None:
    """(title, detail) for everything that isn't a shell command."""
    if tool in ("Edit", "MultiEdit", "NotebookEdit"):
        p = ti.get("file_path") or ti.get("notebook_path")
        return ("Edit a file", short_path(p)) if p else None
    if tool == "Write":
        p = ti.get("file_path")
        return ("Write a file", short_path(p)) if p else None
    if tool == "Read":
        p = ti.get("file_path")
        return ("Read a file", short_path(p)) if p else None
    if tool in ("Glob", "Grep"):
        d = one_line(" in ".join(x for x in (ti.get("pattern", ""), ti.get("path", "")) if x))
        return ("Search files", d) if d else None
    if tool == "WebFetch":
        url = str(ti.get("url", "")).split("://", 1)[-1]
        d = one_line(url)
        return ("Fetch a web page", d) if d else None
    if tool == "WebSearch":
        d = one_line(ti.get("query", ""))
        return ("Search the web", d) if d else None
    if tool.startswith("mcp__"):
        parts = tool.split("__")
        server = parts[1] if len(parts) > 1 else "a tool"
        name = parts[-1]
        args = json.dumps(ti, separators=(",", ":")) if ti else ""
        d = one_line(f"{name} {args}".strip())
        return (("Use " + server)[:MAX_TITLE], d) if d else None
    if tool in ("ExitPlanMode", "AskUserQuestion"):
        return None          # plans and questions need the full terminal UI
    args = json.dumps(ti, separators=(",", ":")) if ti else ""
    d = one_line(args)
    return (tool[:MAX_TITLE] or "Use a tool", d) if d else None


def file_age(p: Path) -> float:
    try:
        return time.time() - p.stat().st_mtime
    except OSError:
        return float("inf")


def write_json(p: Path, data: dict) -> None:
    tmp = p.with_suffix(".tmp")
    tmp.write_text(json.dumps(data), encoding="utf-8")
    os.replace(tmp, p)


def decision(behavior: str, rules: list | None = None) -> str:
    d = {"behavior": behavior}
    if behavior == "deny":
        d["message"] = "Denied on the Clawdmeter"
    if behavior == "allow" and rules:
        d["updatedPermissions"] = rules      # "Always": the same rule the terminal would save
    return json.dumps({"hookSpecificOutput": {"hookEventName": "PermissionRequest", "decision": d}})


def always_rules(evt: dict) -> tuple[list, str]:
    """Claude Code's own "don't ask again" suggestions, limited to allow rules.

    Returns (updatedPermissions entries, rule text for the board). Empty when
    Claude Code offers none (then the board shows no Always button).
    """
    keep, names = [], []
    for s in evt.get("permission_suggestions") or []:
        if not isinstance(s, dict) or s.get("type") != "addRules" or s.get("behavior") != "allow":
            continue
        rules = [r for r in s.get("rules") or [] if isinstance(r, dict) and r.get("toolName")]
        if not rules:
            continue
        keep.append({**s, "rules": rules})
        for r in rules:
            rc = r.get("ruleContent")
            names.append(f"{r['toolName']}({rc})" if rc else str(r["toolName"]))
    return keep, ", ".join(names)


LOG = ROOT / "approval-hook.log"


def note(msg: str) -> None:
    """One line per prompt (what happened, never the command text), for diagnosis."""
    try:
        if LOG.exists() and LOG.stat().st_size > 200_000:
            LOG.write_text("", encoding="utf-8")
        with LOG.open("a", encoding="utf-8") as f:
            f.write(time.strftime("%Y-%m-%d %H:%M:%S ") + msg + "\n")
    except OSError:
        pass


def main() -> None:
    if config_value("approval", "on").lower() == "off":
        return
    if os.environ.get("CLAWDMETER_KEEPALIVE"):
        return                                   # the daemon's own token keep-alive
    evt = json.loads(sys.stdin.read() or "{}")
    tool = str(evt.get("tool_name", ""))
    try:
        timeout = max(3.0, min(120.0, float(config_value("approval_timeout_s", "20"))))
    except ValueError:
        timeout = 20.0
    if file_age(ALIVE) > ALIVE_MAX_AGE:
        note(f"{tool}: board not connected -> keyboard")
        return
    try:
        if (ROOT / "dnd").read_text(encoding="utf-8").strip() == "1":
            note(f"{tool}: board face-down -> keyboard")
            return
    except OSError:
        pass
    summary = summarize(tool, evt.get("tool_input") or {})
    if not summary:
        n = len(str((evt.get("tool_input") or {}).get("command", "")))
        note(f"{tool}: doesn't fit the board ({n} chars) -> keyboard")
        return
    if file_age(REQ) < timeout + 10:
        note(f"{tool}: another prompt is already on the board -> keyboard")
        return
    title, head, detail = summary
    rules, rule_text = always_rules(evt)
    req_id = int(time.time() * 1000) % 1_000_000_000 or 1
    try:
        RESP.unlink()
    except OSError:
        pass
    write_json(REQ, {"id": req_id, "title": title, "head": head, "detail": detail,
                     "always": rule_text if rules else "", "ts": time.time()})
    answer, always = None, False
    try:
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                r = json.loads(RESP.read_text(encoding="utf-8"))
                if int(r.get("id", -1)) == req_id:
                    answer = bool(r.get("allow"))
                    always = answer and bool(r.get("always")) and bool(rules)
                    break
            except (OSError, ValueError):
                pass
            time.sleep(0.15)
    finally:
        try:
            if json.loads(REQ.read_text(encoding="utf-8")).get("id") == req_id:
                REQ.unlink()
        except (OSError, ValueError):
            pass
    if answer is None:
        note(f"{tool}: shown on the board, no tap in {timeout:.0f} s -> keyboard")
        return
    note(f"{tool}: {'always allowed' if always else 'allowed' if answer else 'denied'} on the board")
    sys.stdout.write(decision("allow" if answer else "deny", rules if always else None))
    sys.stdout.flush()


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass                                     # fail safe: normal prompt takes over
    sys.exit(0)
