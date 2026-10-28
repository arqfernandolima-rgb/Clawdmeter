"""Daemon side of the Clawdmeter approval prompt (see cc_approval_hook.py)."""
from __future__ import annotations

import json
import os
import time
from pathlib import Path

ROOT = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local")) / "Clawdmeter"
REQ = ROOT / "approval_req.json"
RESP = ROOT / "approval_resp.json"
ALIVE = ROOT / "daemon_alive"
DND = ROOT / "dnd"                 # "1" while the board lies face-down
COUNTS = ROOT / "approvals_today.json"

REQ_MAX_AGE = 150.0   # ignore a request file a crashed hook left behind


def current_request() -> dict | None:
    """The pending request {"id", "title", "detail"} or None."""
    try:
        if time.time() - REQ.stat().st_mtime > REQ_MAX_AGE:
            return None
        r = json.loads(REQ.read_text(encoding="utf-8"))
        rid = int(r.get("id", 0))
        if rid <= 0:
            return None
        return {"id": rid, "title": str(r.get("title", ""))[:30], "detail": str(r.get("detail", ""))[:150]}
    except (OSError, ValueError, TypeError):
        return None


def add_fields(payload: dict, req: dict | None) -> None:
    """ai = request id (0 = none); at/ad only while one is pending."""
    for k in ("ai", "at", "ad"):
        payload.pop(k, None)
    if req:
        payload["ai"] = req["id"]
        payload["at"] = req["title"]
        payload["ad"] = req["detail"]
    else:
        payload["ai"] = 0


def parse_dnd(data: bytes) -> bool | None:
    """Board notify "D1" (face-down) / "D0" (face-up) -> bool; anything else -> None."""
    s = bytes(data)
    if s in (b"D1", b"D0"):
        return s == b"D1"
    return None


def set_dnd(on: bool) -> None:
    try:
        ROOT.mkdir(parents=True, exist_ok=True)
        DND.write_text("1" if on else "0", encoding="utf-8")
    except OSError:
        pass


def dnd_on() -> bool:
    try:
        return DND.read_text(encoding="utf-8").strip() == "1"
    except OSError:
        return False


def count_approval() -> None:
    today = time.strftime("%Y-%m-%d")
    try:
        d = json.loads(COUNTS.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        d = {}
    if d.get("date") != today:
        d = {"date": today, "n": 0}
    d["n"] = int(d.get("n", 0)) + 1
    try:
        COUNTS.write_text(json.dumps(d), encoding="utf-8")
    except OSError:
        pass


def approvals_today() -> int:
    try:
        d = json.loads(COUNTS.read_text(encoding="utf-8"))
        return int(d.get("n", 0)) if d.get("date") == time.strftime("%Y-%m-%d") else 0
    except (OSError, ValueError):
        return 0


def parse_answer(data: bytes) -> tuple[int, bool] | None:
    """Board notify "A<id>:1" / "A<id>:0" -> (id, allow); anything else -> None."""
    try:
        s = bytes(data).decode("ascii", "replace")
        if not s.startswith("A") or ":" not in s:
            return None
        rid, flag = s[1:].split(":", 1)
        return int(rid), flag.strip().startswith("1")
    except (ValueError, TypeError):
        return None


def write_answer(rid: int, allow: bool) -> None:
    ROOT.mkdir(parents=True, exist_ok=True)
    tmp = RESP.with_suffix(".tmp")
    tmp.write_text(json.dumps({"id": rid, "allow": allow, "ts": time.time()}), encoding="utf-8")
    os.replace(tmp, RESP)


def touch_alive() -> None:
    try:
        ROOT.mkdir(parents=True, exist_ok=True)
        ALIVE.write_text(str(time.time()), encoding="utf-8")
    except OSError:
        pass
