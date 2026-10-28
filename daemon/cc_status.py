"""Live Claude Code status for the Clawdmeter (Windows).

cc_status_hook.py (run by Claude Code hooks) keeps one small JSON file per
session in %LOCALAPPDATA%\\Clawdmeter\\sessions\\. This module turns those into
what the board shows: the status text ("cs"), the Clawd animation ("a"), the
project and turn timer, the session count, and attention events (needs you /
your turn / big finish) with their sounds.

Presentation choices live here (not in the hook), so they can change without
touching Claude Code's settings.json.
"""
from __future__ import annotations

import json
import os
import time
import unicodedata
from pathlib import Path

ROOT = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local")) / "Clawdmeter"
SESSIONS = ROOT / "sessions"
STATE_FILE = ROOT / "cc_state.json"          # legacy single-state file (still written by the hook)

ELLIPSIS = "…"          # firmware spins the glyph only for texts ending in this
DONE_HOLD_S = 120            # show "Done" this long after a turn ends, then "Idle"
STALE_BUSY_S = 15 * 60       # a busy state with no new event for this long -> "Idle"
STALE_WAIT_S = 30 * 60       # "Needs you" with no new event for this long -> "Idle"
ATTENTION_HOLD_S = 10 * 60   # "Your turn" / "All done!" screen stays up at most this long
ACTIVE_S = 30 * 60           # a session counts as active if it did something this recently

IDLE = ("Idle", "idle breathe")

# attention kinds (firmware "att") and sound ids (firmware CHIME_*)
ATT_NONE, ATT_NEEDS_YOU, ATT_YOUR_TURN, ATT_CELEBRATE = 0, 1, 2, 3
SND_ATTENTION, SND_DING, SND_FANFARE = 1, 2, 3

_TOOL_MAP = {
    "Read": ("Reading" + ELLIPSIS, "work think"),
    "Glob": ("Reading" + ELLIPSIS, "work think"),
    "Grep": ("Searching code" + ELLIPSIS, "work think"),
    "LS": ("Reading" + ELLIPSIS, "work think"),
    "NotebookRead": ("Reading" + ELLIPSIS, "work think"),
    "Edit": ("Writing code" + ELLIPSIS, "write"),
    "MultiEdit": ("Writing code" + ELLIPSIS, "write"),
    "Write": ("Writing code" + ELLIPSIS, "write"),
    "NotebookEdit": ("Writing code" + ELLIPSIS, "write"),
    "Bash": ("Running command" + ELLIPSIS, "work coding"),
    "PowerShell": ("Running command" + ELLIPSIS, "work coding"),
    "BashOutput": ("Running command" + ELLIPSIS, "work coding"),
    "KillShell": ("Running command" + ELLIPSIS, "work coding"),
    "WebFetch": ("Browsing web" + ELLIPSIS, "work think"),
    "WebSearch": ("Searching web" + ELLIPSIS, "work think"),
    "Task": ("Delegating" + ELLIPSIS, "work coding"),
    "Agent": ("Delegating" + ELLIPSIS, "work coding"),
    "TodoWrite": ("Planning" + ELLIPSIS, "think"),
    "Skill": ("Using a skill" + ELLIPSIS, "work coding"),
}
_PREFIX_MAP = (
    ("mcp__", ("Using tools" + ELLIPSIS, "work coding")),
    ("Task", ("Planning" + ELLIPSIS, "think")),       # TaskCreate / TaskUpdate / ...
)


def ascii_only(s: str, n: int) -> str:
    """The board's fonts are ASCII-only: fold accents ("Projeção" -> "Projecao")."""
    s = unicodedata.normalize("NFKD", str(s)).encode("ascii", "ignore").decode("ascii")
    return s[:n]


def _tool_status(tool: str) -> tuple[str, str]:
    if tool in _TOOL_MAP:
        return _TOOL_MAP[tool]
    for prefix, result in _PREFIX_MAP:
        if tool.startswith(prefix):
            return result
    return ("Working" + ELLIPSIS, "work coding")


def load_sessions(now: float | None = None) -> list[dict]:
    """Session files touched in the last ACTIVE_S, newest first (ended ones dropped)."""
    now = time.time() if now is None else now
    out = []
    try:
        files = list(SESSIONS.glob("*.json"))
    except OSError:
        return out
    for f in files:
        try:
            age = now - f.stat().st_mtime
            if age > 2 * 86400:
                try:
                    f.unlink()                      # tidy up old sessions
                except OSError:
                    pass
                continue
            if age > ACTIVE_S and age > ATTENTION_HOLD_S:
                continue
            s = json.loads(f.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if s.get("state") == "ended":
            continue
        out.append(s)
    out.sort(key=lambda s: float(s.get("ts", 0)), reverse=True)
    return out


def project_name(s: dict | None) -> str:
    if not s or not s.get("cwd"):
        return ""
    return ascii_only(Path(str(s["cwd"]).replace("\\", "/")).name, 20)


def session_status(s: dict | None, now: float) -> tuple[str, str]:
    if not s:
        return IDLE
    state = str(s.get("state", ""))
    age = now - float(s.get("ts", 0) or 0)
    if state == "tool":
        return _tool_status(str(s.get("tool", ""))) if age < STALE_BUSY_S else IDLE
    if state == "thinking":
        return ("Thinking" + ELLIPSIS, "think") if age < STALE_BUSY_S else IDLE
    if state == "needs_you":
        return ("Needs you", "allow") if age < STALE_WAIT_S else IDLE
    if state in ("done", "waiting"):
        return ("Done", "done") if age < DONE_HOLD_S else IDLE
    if state == "ready":
        return ("Ready", "idle blink") if age < DONE_HOLD_S else IDLE
    return IDLE


class StatusView:
    """Everything the status message carries, recomputed each tick."""

    def __init__(self) -> None:
        self.att_seq = 0
        self.snd_seq = 0
        self.snd = 0
        self._att_key = None
        self._reminded = False
        self._key_seq: dict = {}     # attention event -> its seq, so a re-surfacing event
                                     # keeps its seq (stays dismissed, no second sound)

    def compute(self, api_status: str | None, cfg: dict, approval_pending: bool,
                dnd: bool, now: float | None = None) -> dict:
        now = time.time() if now is None else now
        sessions = load_sessions(now)
        active = [s for s in sessions if now - float(s.get("ts", 0) or 0) < ACTIVE_S]
        busy_or_wait = [s for s in active if session_status(s, now) != IDLE]

        # A session waiting on you wins over one that's busy elsewhere.
        needs = [s for s in active if s.get("state") == "needs_you"
                 and now - float(s.get("ts", 0) or 0) < STALE_WAIT_S]
        focus = needs[0] if needs else (active[0] if active else None)

        if api_status and not str(api_status).startswith("allowed") and api_status != "unknown":
            text, anim = "Limit reached", "limit"
        else:
            text, anim = session_status(focus, now)

        tt = -1
        if focus and float(focus.get("turn_start") or 0) and text.endswith(ELLIPSIS):
            tt = max(0, int(now - float(focus["turn_start"])))

        # ---- attention screen + sound
        att, atd, key, snd, att_s = ATT_NONE, -1, None, 0, None
        done_min = cfg.get("done_alert_min_s", 60)
        party_min = cfg.get("celebrate_min_s", 600)
        if approval_pending:
            pass                                   # the approval prompt has the screen
        elif needs:
            s = att_s = needs[0]
            att, atd = ATT_NEEDS_YOU, max(0, int(now - float(s.get("ts", now))))
            key = ("needs", s.get("sid"), round(float(s.get("ts", 0)), 1))
            snd = SND_ATTENTION
        else:
            finished = [s for s in active if s.get("state") in ("done", "waiting")
                        and float(s.get("turn_end") or 0)
                        and now - float(s["turn_end"]) < ATTENTION_HOLD_S
                        and int(s.get("last_turn_secs") or 0) >= done_min]
            if finished:
                s = finished[0]
                secs = int(s.get("last_turn_secs") or 0)
                att = ATT_CELEBRATE if secs >= party_min else ATT_YOUR_TURN
                atd = secs
                key = ("done", s.get("sid"), round(float(s["turn_end"]), 1))
                snd = SND_FANFARE if att == ATT_CELEBRATE else SND_DING
                att_s = s
        if key != self._att_key:
            self._att_key = key
            self._reminded = False
            if att != ATT_NONE and key not in self._key_seq:
                self.att_seq = (self.att_seq + 1) % 60000
                self._key_seq[key] = self.att_seq
                if len(self._key_seq) > 64:
                    self._key_seq.pop(next(iter(self._key_seq)))
                self._play(snd, cfg, dnd)
        elif att == ATT_NEEDS_YOU and not self._reminded and atd >= cfg.get("remind_after_s", 120):
            self._reminded = True                  # one reminder if it's still waiting
            self._play(SND_ATTENTION, cfg, dnd)

        return {
            "cs": ascii_only(text.replace(ELLIPSIS, "\x00"), 22).replace("\x00", ELLIPSIS),
            "a": anim,
            "tt": tt,
            "pj": project_name(focus),
            "ns": len(busy_or_wait) if busy_or_wait else len(active[:1]),
            "att": att,
            "aq": self._key_seq.get(key, self.att_seq),
            "atd": atd,
            "atp": project_name(att_s),
            "snd": self.snd,
            "sq": self.snd_seq,
        }

    def _play(self, snd: int, cfg: dict, dnd: bool) -> None:
        if not snd or dnd or str(cfg.get("sound", "on")).lower() == "off":
            return
        self.snd = snd
        self.snd_seq = (self.snd_seq + 1) % 60000

    def approval_sound(self, cfg: dict, dnd: bool) -> None:
        self._play(SND_ATTENTION, cfg, dnd)


def state_mtime() -> float:
    """Cheap change detector for the poll loop: newest hook write."""
    try:
        m = STATE_FILE.stat().st_mtime
    except OSError:
        m = 0.0
    try:
        m = max([m] + [f.stat().st_mtime for f in SESSIONS.glob("*.json")])
    except OSError:
        pass
    return m


# Back-compat helper (older callers): status of the most recent session.
def current_status(api_status: str | None = None, now: float | None = None) -> tuple[str, str]:
    now = time.time() if now is None else now
    if api_status and not str(api_status).startswith("allowed") and api_status != "unknown":
        return ("Limit reached", "limit")
    s = load_sessions(now)
    return session_status(s[0] if s else None, now)
