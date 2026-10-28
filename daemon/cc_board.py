"""Builds the board's extra messages and talks to Home Assistant (Windows daemon).

The board's BLE link carries ~250 bytes per write, so data goes out as four
small message kinds (see firmware main.cpp parse_json):

  usage    - the 60 s poll (numbers, clock, volume, face-down sign)
  status   - {"so":1, ...} Claude Code state, project, timers, attention, sound
  approval - {"so":1, "ai", "at", "ad"} a pending touchscreen approval
  stats    - {"hs":1, ...} today's counters + this month's daily spend

Config keys (%LOCALAPPDATA%\\Clawdmeter\\config), all optional:
  sound = on|off            volume = 0..100           facedown_sign = 1|-1
  done_alert_min_s = 60     celebrate_min_s = 600     remind_after_s = 120
  ha_webhook_url = http://homeassistant.local:8123/api/webhook/<id>
  max_config_dir = auto|off|<folder>   keepalive = on|off   claude_path = <path>
"""
from __future__ import annotations

import asyncio
import json
import os
import time
from pathlib import Path

try:
    from daemon import cc_approval, cc_spend
    from daemon.cc_status import ascii_only
except ImportError:
    import cc_approval
    import cc_spend
    from cc_status import ascii_only

CONFIG_FILE = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local")) / "Clawdmeter" / "config"

_DEFAULTS = {
    "sound": "on", "volume": 70, "facedown_sign": 1,
    "done_alert_min_s": 60, "celebrate_min_s": 600, "remind_after_s": 120,
    "ha_webhook_url": "",
    # second account + keep-alive (see cc_accounts.py)
    "max_config_dir": "auto", "keepalive": "on", "claude_path": "",
}
_cache: tuple[float, dict] = (0.0, dict(_DEFAULTS))


def read_cfg() -> dict:
    """Config with defaults; re-read at most every 10 s."""
    global _cache
    if time.time() - _cache[0] < 10:
        return _cache[1]
    cfg = dict(_DEFAULTS)
    try:
        for line in CONFIG_FILE.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if "=" not in line:
                continue
            k, v = (x.strip() for x in line.split("=", 1))
            k = k.lower()
            if k not in cfg:
                continue
            if isinstance(_DEFAULTS[k], int):
                try:
                    cfg[k] = int(float(v))
                except ValueError:
                    pass
            else:
                cfg[k] = v
    except OSError:
        pass
    cfg["volume"] = max(0, min(100, cfg["volume"]))
    cfg["facedown_sign"] = -1 if cfg["facedown_sign"] < 0 else 1
    _cache = (time.time(), cfg)
    return cfg


def parse_volume(data: bytes) -> int | None:
    """Board notify "V0".."V100" (volume changed with the button) -> int."""
    s = bytes(data)
    if len(s) < 2 or s[:1] != b"V" or not s[1:].isdigit():
        return None
    return max(0, min(100, int(s[1:])))


def set_volume(volume: int) -> None:
    """Save a volume chosen on the board: volume = n, and sound = off at 0."""
    global _cache
    try:
        lines = CONFIG_FILE.read_text().splitlines() if CONFIG_FILE.exists() else []
    except OSError:
        lines = []
    keep = [l for l in lines if l.split("=", 1)[0].strip().lower() not in ("volume", "sound")]
    keep += [f"sound = {'off' if volume == 0 else 'on'}"]
    if volume:
        keep += [f"volume = {volume}"]
    else:
        old = [l for l in lines if l.split("=", 1)[0].strip().lower() == "volume"]
        keep += old[-1:]                    # keep the last level for when sound comes back
    try:
        CONFIG_FILE.parent.mkdir(parents=True, exist_ok=True)
        CONFIG_FILE.write_text("\n".join(keep) + "\n")
    except OSError:
        pass
    _cache = (0.0, dict(_DEFAULTS))         # re-read on the next poll


def add_usage_extras(payload: dict, cfg: dict) -> None:
    payload["vol"] = cfg["volume"] if str(cfg["sound"]).lower() != "off" else 0
    payload["fz"] = cfg["facedown_sign"]


def approval_message(req: dict | None, snd: int, sq: int) -> dict:
    if not req:
        return {"so": 1, "ai": 0}
    title = ascii_only(req["title"], 16)
    detail = ascii_only(req["detail"], 90)
    # keep the JSON-escaped detail short enough for one BLE write
    while len(json.dumps(detail)) > 110 and detail:
        detail = detail[:-1]
    return {"so": 1, "ai": req["id"], "at": title, "ad": detail, "snd": snd, "sq": sq}


def stats_message() -> dict:
    st = cc_spend.stats_figures()
    return {
        "hs": 1,
        "dh": st["hist"],
        "di": st["today_idx"],
        "dm": st["top_usd"],
        "kt": st["tools"],
        "kf": st["files"],
        "kp": st["prompts"],
        "ka": cc_approval.approvals_today(),
        "km": st["minutes"],
        "kr": st["streak"],
    }


class HaNotifier:
    """POSTs to a Home Assistant webhook when the attention state changes.

    Payload: {"state": "needs_you"|"your_turn"|"celebrate"|"busy"|"idle",
              "project": "...", "status": "..."}. The HA side decides what to
    do with it (e.g. an Inovelli LED bar effect). Errors are logged once and
    otherwise ignored, so an unreachable HA never affects the board.
    """

    def __init__(self, log) -> None:
        self._last = None
        self._log = log
        self._warned = False

    @staticmethod
    def category(st: dict) -> str:
        att = st.get("att", 0)
        if att == 1:
            return "needs_you"
        if att == 3:
            return "celebrate"
        if att == 2:
            return "your_turn"
        return "busy" if str(st.get("cs", "")).endswith("…") else "idle"

    def update(self, st: dict, cfg: dict) -> None:
        url = str(cfg.get("ha_webhook_url") or "").strip()
        if not url:
            return
        cat = self.category(st)
        if cat == self._last:
            return
        self._last = cat
        body = {"state": cat, "project": st.get("pj", ""), "status": st.get("cs", "")}
        asyncio.ensure_future(self._post(url, body))

    async def _post(self, url: str, body: dict) -> None:
        try:
            import httpx
            async with httpx.AsyncClient(timeout=4.0) as http:
                r = await http.post(url, json=body)
            if r.status_code >= 400 and not self._warned:
                self._warned = True
                self._log(f"Home Assistant webhook HTTP {r.status_code}")
        except Exception as e:                       # noqa: BLE001 - never break the loop
            if not self._warned:
                self._warned = True
                self._log(f"Home Assistant webhook failed: {e}")
