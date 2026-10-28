"""Second account (personal Max plan) and Claude Code login keep-alive.

Max screen
  The board's Max screen shows a second Claude account, read through a second
  Claude Code login kept in its own config folder (CLAUDE_CONFIG_DIR). Max
  limits are shared by the Claude apps (desktop, web, mobile) and Claude Code,
  so this is where desktop-app usage shows up. Message to the board:
    {"hm":1, "s":%, "sr":min, "w":%, "wr":min, "e":0}      (+ "m","mr","mn")
  e: 0 ok, 1 not signed in, 2 sign-in expired.

Keep-alive
  Claude Code access tokens last a few hours and are only renewed when Claude
  Code itself runs. When a token is close to expiring, the daemon runs one tiny
  `claude -p` (Haiku, no tools, no MCP, nothing saved) so Claude Code renews it
  the official way. It runs from %LOCALAPPDATA%\\Clawdmeter\\keepalive with
  CLAWDMETER_KEEPALIVE=1, which the status/approval hooks and the spend tracker
  ignore. Tokens never leave the credentials files and are never logged.

Config keys (%LOCALAPPDATA%\\Clawdmeter\\config):
  max_config_dir = auto | off | <folder>   auto: ~/.claude-personal if signed in
  keepalive      = on | off
  claude_path    = <full path to claude>   only if it isn't on PATH
"""
from __future__ import annotations

import asyncio
import json
import os
import re
import shutil
import subprocess
import time
from pathlib import Path

ROOT = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local")) / "Clawdmeter"
KEEPALIVE_DIR = ROOT / "keepalive"
DEFAULT_MAX_DIR = Path.home() / ".claude-personal"

# Claude Code renews a token only in its last few minutes, so run inside that
# window; if a run comes too early (nothing renewed), retry a few minutes later.
REFRESH_AHEAD_S = 4 * 60       # renew when the token has less than this left
MIN_GAP_S = 3 * 60             # min time between runs for one account
FAIL_BACKOFF_S = 2 * 3600      # after 3 non-renewing runs in a row, wait this long
RUN_TIMEOUT_S = 180


# ---------------------------------------------------------------- credentials

def read_creds(path: Path) -> tuple[str | None, float | None]:
    """(access token, expiry epoch seconds) from a Claude Code .credentials.json."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None, None
    oauth = data.get("claudeAiOauth") if isinstance(data, dict) else None
    if not isinstance(oauth, dict):
        return None, None
    tok = oauth.get("accessToken")
    exp = oauth.get("expiresAt")
    try:
        exp_s = float(exp) / 1000.0 if exp is not None else None
    except (TypeError, ValueError):
        exp_s = None
    return (tok if isinstance(tok, str) and tok.strip() else None), exp_s


def max_dir(cfg: dict) -> Path | None:
    """The Max account's Claude Code config folder, or None when not set up."""
    raw = str(cfg.get("max_config_dir") or "auto").strip()
    if raw.lower() in ("off", "no", "none", "0"):
        return None
    if raw.lower() == "auto":
        return DEFAULT_MAX_DIR if (DEFAULT_MAX_DIR / ".credentials.json").exists() else None
    return Path(os.path.expandvars(raw)).expanduser()


# ---------------------------------------------------------------- board message

_MODEL_RE = re.compile(r"^anthropic-ratelimit-unified-7d[_-]([a-z0-9]+)-utilization$")


def model_limit(headers) -> tuple[str, str, str] | None:
    """Optional per-model weekly limit header -> (name, util header, reset header).
    Prefers Sonnet, then Opus, then whatever is there."""
    found = {}
    for k in headers.keys():
        m = _MODEL_RE.match(k.lower())
        if m:
            found[m.group(1)] = k
    if not found:
        return None
    for pick in ("sonnet", "opus"):
        if pick in found:
            break
    else:
        pick = sorted(found)[0]
    util = found[pick]
    return pick.capitalize()[:11], util, util.replace("-utilization", "-reset")


def max_message(payload: dict | None, err: int = 0) -> dict:
    if err or not payload:
        return {"hm": 1, "e": err or 2}
    msg = {"hm": 1, "e": 0}
    for k in ("s", "sr", "w", "wr", "m", "mr", "mn"):
        if k in payload:
            msg[k] = payload[k]
    return msg


# ---------------------------------------------------------------- keep-alive

def find_claude(cfg: dict) -> str | None:
    p = str(cfg.get("claude_path") or "").strip().strip('"')
    if p and Path(p).exists():
        return p
    hit = shutil.which("claude")
    if hit:
        return hit
    home = Path.home()
    appdata = Path(os.environ.get("APPDATA", home / "AppData" / "Roaming"))
    for c in (home / ".local" / "bin" / "claude.exe",
              appdata / "npm" / "claude.cmd",
              home / "AppData" / "Local" / "Programs" / "claude" / "claude.exe"):
        if c.exists():
            return str(c)
    return None


class TokenKeeper:
    """Runs the keep-alive in the background, at most once per account at a time."""

    def __init__(self, log) -> None:
        self._log = log
        self._last: dict[str, float] = {}
        self._fails: dict[str, int] = {}
        self._tasks: dict[str, asyncio.Task] = {}
        self._warned_missing = False

    def failing(self, label: str) -> bool:
        return self._fails.get(label, 0) >= 2

    def kick(self, label: str, cred_path: Path | None, config_dir: Path | None,
             cfg: dict, force: bool = False) -> None:
        """Start a renewal if the token is close to expiring (or `force`, after a 401)."""
        if str(cfg.get("keepalive", "on")).lower() == "off" or cred_path is None:
            return
        t = self._tasks.get(label)
        if t and not t.done():
            return
        _, exp = read_creds(cred_path)
        now = time.time()
        due = force or (exp is not None and exp - now < REFRESH_AHEAD_S)
        if not due:
            return
        gap = FAIL_BACKOFF_S if self._fails.get(label, 0) >= 3 else MIN_GAP_S
        if now - self._last.get(label, 0) < gap:
            return
        exe = find_claude(cfg)
        if not exe:
            if not self._warned_missing:
                self._warned_missing = True
                self._log("Keep-alive: couldn't find claude (set claude_path in the config)")
            return
        self._last[label] = now
        self._tasks[label] = asyncio.ensure_future(
            self._renew(label, exe, cred_path, config_dir, exp))

    async def _renew(self, label: str, exe: str, cred_path: Path,
                     config_dir: Path | None, old_exp: float | None) -> None:
        ok = await asyncio.to_thread(self._run, exe, config_dir)
        _, exp = read_creds(cred_path)
        renewed = exp is not None and (old_exp is None or exp > old_exp + 60) and exp > time.time() + REFRESH_AHEAD_S
        if renewed:
            self._fails[label] = 0
            left = int((exp - time.time()) / 60)
            self._log(f"Keep-alive: {label} sign-in renewed ({left} min left)")
        else:
            self._fails[label] = self._fails.get(label, 0) + 1
            self._log(f"Keep-alive: {label} sign-in not renewed (claude exit ok={ok}); "
                      f"attempt {self._fails[label]}")

    @staticmethod
    def _run(exe: str, config_dir: Path | None) -> bool:
        KEEPALIVE_DIR.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ)
        env["CLAWDMETER_KEEPALIVE"] = "1"
        if config_dir is not None:
            env["CLAUDE_CONFIG_DIR"] = str(config_dir)
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        minimal = [exe, "-p", "ok", "--model", "haiku", "--tools", "",
                   "--strict-mcp-config", "--no-session-persistence",
                   "--system-prompt", "Reply with one word."]
        plain = [exe, "-p", "ok", "--model", "haiku"]
        for args in (minimal, plain):            # older Claude Code may not know a flag
            try:
                r = subprocess.run(args, cwd=str(KEEPALIVE_DIR), env=env,
                                   stdin=subprocess.DEVNULL, capture_output=True,
                                   timeout=RUN_TIMEOUT_S, creationflags=flags)
            except (OSError, subprocess.TimeoutExpired):
                continue
            if r.returncode == 0:
                return True
        return False
