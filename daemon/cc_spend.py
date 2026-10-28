"""Live spend estimate from Claude Code's local transcripts (Windows daemon).

The usage API only reports a whole-percent, slow-moving spend figure for
Enterprise accounts. Claude Code writes every assistant message, with its token
usage, to ~/.claude/projects/**/*.jsonl, and usage-based Enterprise is billed
at API rates, so tokens x list price gives a live dollar estimate.

    live spend = official % x limit  +  local Claude Code cost since the
                                        official figure last changed

Only Claude Code on this machine is counted (not the desktop/web chat apps),
and prices are list prices (no discounts, fast mode or regional uplifts), so
this is an estimate. The official figure stays the source of truth and the
baseline resets every time it moves.
"""
from __future__ import annotations

import datetime as _dt
import json
import os
import time
from pathlib import Path

ROOT = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local")) / "Clawdmeter"
STATE_FILE = ROOT / "spend_state.json"

# USD per million tokens: (input, cache write 5m, cache write 1h, cache read, output).
# Source: platform.claude.com/docs/en/about-claude/pricing (Oct 2026).
# Matched by substring against the transcript's model id, first match wins,
# so more specific names come before their families.
_PRICES = [
    ("fable-5-1",  (10.0, 12.50, 20.0, 0.25, 50.0)),
    ("mythos-5-1", (10.0, 12.50, 20.0, 0.25, 50.0)),
    ("fable-5",    (10.0, 12.50, 20.0, 1.00, 50.0)),
    ("mythos",     (10.0, 12.50, 20.0, 1.00, 50.0)),
    ("opus-5-5",   (4.0, 5.00, 8.0, 0.20, 20.0)),
    ("opus-5",     (5.0, 6.25, 10.0, 0.50, 25.0)),
    ("opus-4-8",   (5.0, 6.25, 10.0, 0.50, 25.0)),
    ("opus-4-7",   (5.0, 6.25, 10.0, 0.50, 25.0)),
    ("opus-4-6",   (5.0, 6.25, 10.0, 0.50, 25.0)),
    ("opus-4-5",   (5.0, 6.25, 10.0, 0.50, 25.0)),
    ("opus-4",     (15.0, 18.75, 30.0, 1.50, 75.0)),      # Opus 4 / 4.1
    ("sonnet-5-5", (2.0, 2.50, 4.0, 0.10, 10.0)),
    ("sonnet-5",   (2.0, 2.50, 4.0, 0.20, 10.0)),
    ("sonnet",     (3.0, 3.75, 6.0, 0.30, 15.0)),          # Sonnet 4.x / 3.7
    ("haiku-4-5",  (1.0, 1.25, 2.0, 0.10, 5.0)),
    ("haiku-3-5",  (0.8, 1.00, 1.6, 0.08, 4.0)),
]
_HAIKU_55_SMALL = (0.10, 0.125, 0.20, 0.01, 0.50)      # prompt <= 100K tokens
_HAIKU_55_LARGE = (0.50, 0.625, 1.00, 0.05, 2.50)      # prompt  > 100K tokens
_FALLBACK = (4.0, 5.00, 8.0, 0.20, 20.0)               # unknown model: Opus 5.5 rates


def _claude_dirs() -> list[Path]:
    env = os.environ.get("CLAUDE_CONFIG_DIR")
    dirs = [Path(p.strip()).expanduser() for p in env.split(",")] if env else []
    return dirs or [Path.home() / ".claude"]


def message_cost(model: str, usage: dict) -> tuple[float, bool]:
    """(USD, known_model) for one assistant message's usage block."""
    def n(k: str) -> int:
        try:
            return int(usage.get(k) or 0)
        except (TypeError, ValueError):
            return 0

    inp, out, cread = n("input_tokens"), n("output_tokens"), n("cache_read_input_tokens")
    cc = usage.get("cache_creation") or {}
    try:
        w5 = int(cc.get("ephemeral_5m_input_tokens") or 0)
        w1 = int(cc.get("ephemeral_1h_input_tokens") or 0)
    except (TypeError, ValueError, AttributeError):
        w5 = w1 = 0
    if w5 + w1 == 0:
        w5 = n("cache_creation_input_tokens")       # older transcripts: no split

    m = (model or "").lower()
    known = True
    if "haiku-5-5" in m:
        prompt = inp + cread + w5 + w1
        p = _HAIKU_55_LARGE if prompt > 100_000 else _HAIKU_55_SMALL
    else:
        p = next((pr for key, pr in _PRICES if key in m), None)
        if p is None:
            p, known = _FALLBACK, False
    pin, p5, p1, pr_, pout = p
    usd = (inp * pin + w5 * p5 + w1 * p1 + cread * pr_ + out * pout) / 1_000_000
    return usd, known


def _parse_ts(s: str) -> float | None:
    try:
        return _dt.datetime.fromisoformat(str(s).replace("Z", "+00:00")).timestamp()
    except (ValueError, TypeError):
        return None


KEEP_S = 40 * 86400          # cost history kept for the month chart and streak
EDIT_TOOLS = ("Edit", "MultiEdit", "Write", "NotebookEdit")


class SpendTracker:
    """Incrementally tails transcript files; dedupes streamed duplicates.

    Besides cost it keeps what the Today screen needs: tool calls, edited
    files, prompts and per-file activity timestamps (for time with Claude).
    """

    def __init__(self) -> None:
        self._offsets: dict[Path, int] = {}
        self._entries: dict[str, tuple[float, float, str]] = {}   # key -> (ts, usd, model)
        self._tools: dict[str, tuple[float, str, str]] = {}       # tool_use id -> (ts, name, file)
        self._prompts: dict[str, tuple[float, str]] = {}          # uuid -> (ts, transcript)
        self._activity: dict[str, set[float]] = {}                # transcript -> event times
        self.unknown_models: set[str] = set()

    def scan(self, since_ts: float) -> None:
        for base in _claude_dirs():
            proj = base / "projects"
            if not proj.is_dir():
                continue
            for f in proj.rglob("*.jsonl"):
                # the daemon's own token keep-alive runs in ...\Clawdmeter\keepalive
                if f.relative_to(proj).parts[0].lower().endswith("clawdmeter-keepalive"):
                    continue
                try:
                    st = f.stat()
                except OSError:
                    continue
                if st.st_mtime < since_ts - 120:
                    continue
                off = self._offsets.get(f, 0)
                if st.st_size < off:
                    off = 0                                   # file rewritten
                if st.st_size == off:
                    continue
                try:
                    with open(f, "rb") as fh:
                        fh.seek(off)
                        chunk = fh.read()
                except OSError:
                    continue
                end = chunk.rfind(b"\n")
                if end < 0:
                    continue                                  # no complete line yet
                self._offsets[f] = off + end + 1
                for raw in chunk[: end + 1].splitlines():
                    self._ingest(raw, since_ts, str(f))
        now = time.time()
        cutoff = now - KEEP_S
        self._entries = {k: v for k, v in self._entries.items() if v[0] >= cutoff}
        day = now - 2 * 86400
        self._tools = {k: v for k, v in self._tools.items() if v[0] >= day}
        self._prompts = {k: v for k, v in self._prompts.items() if v[0] >= day}
        self._activity = {k: {t for t in v if t >= day} for k, v in self._activity.items()}

    def _ingest(self, raw: bytes, since_ts: float, src: str = "") -> None:
        if b'"type":"user"' in raw or b'"type": "user"' in raw:
            self._ingest_user(raw, src)
            return
        if b'"usage"' not in raw:
            return
        try:
            rec = json.loads(raw)
        except ValueError:
            return
        if rec.get("type") != "assistant":
            return
        msg = rec.get("message") or {}
        ts0 = _parse_ts(rec.get("timestamp"))
        if ts0 is not None and ts0 >= time.time() - 2 * 86400 and not rec.get("isSidechain"):
            self._activity.setdefault(src, set()).add(ts0)
            for block in msg.get("content") or []:
                if isinstance(block, dict) and block.get("type") == "tool_use" and block.get("id"):
                    inp = block.get("input") or {}
                    path = str(inp.get("file_path") or inp.get("notebook_path") or "")
                    self._tools[block["id"]] = (ts0, str(block.get("name", "")), path)
        usage = msg.get("usage")
        model = str(msg.get("model") or "")
        if not isinstance(usage, dict) or model.startswith("<"):    # "<synthetic>"
            return
        ts = _parse_ts(rec.get("timestamp"))
        if ts is None or ts < time.time() - KEEP_S:
            return
        key = f"{msg.get('id', '')}:{rec.get('requestId', '')}"
        if key == ":":
            key = f"{ts}:{len(raw)}"
        usd, known = message_cost(model, usage)
        if not known:
            self.unknown_models.add(model)
        prev = self._entries.get(key)
        if prev is None or usd > prev[1]:                 # streamed lines: keep the fullest
            self._entries[key] = (ts, usd, model)

    def _ingest_user(self, raw: bytes, src: str) -> None:
        try:
            rec = json.loads(raw)
        except ValueError:
            return
        if rec.get("type") != "user" or rec.get("isMeta") or rec.get("isSidechain"):
            return
        ts = _parse_ts(rec.get("timestamp"))
        if ts is None or ts < time.time() - 2 * 86400:
            return
        content = (rec.get("message") or {}).get("content")
        is_prompt = False
        if isinstance(content, str):
            is_prompt = bool(content.strip()) and not content.lstrip().startswith("<")
        elif isinstance(content, list):
            kinds = {c.get("type") for c in content if isinstance(c, dict)}
            if "tool_result" in kinds:
                self._activity.setdefault(src, set()).add(ts)      # Claude still working
            elif "text" in kinds:
                texts = [c.get("text", "") for c in content if isinstance(c, dict) and c.get("type") == "text"]
                is_prompt = any(t.strip() and not t.lstrip().startswith("<") for t in texts)
        if is_prompt:
            self._prompts[str(rec.get("uuid") or f"{src}:{ts}")] = (ts, src)

    def today_stats(self, midnight: float) -> dict:
        tools = [v for v in self._tools.values() if v[0] >= midnight]
        files = {v[2].replace("\\", "/").lower() for v in tools if v[1] in EDIT_TOOLS and v[2]}
        prompts = [v for v in self._prompts.values() if v[0] >= midnight]
        # Time with Claude: each prompt until the last activity before the next
        # prompt in the same transcript (capped at 3 h per turn).
        secs = 0.0
        by_src: dict[str, list[float]] = {}
        for ts, src in prompts:
            by_src.setdefault(src, []).append(ts)
        for src, starts in by_src.items():
            starts.sort()
            acts = sorted(t for t in self._activity.get(src, ()) if t >= midnight)
            for i, st in enumerate(starts):
                nxt = starts[i + 1] if i + 1 < len(starts) else float("inf")
                ends = [t for t in acts if st <= t < nxt]
                if ends:
                    secs += min(ends[-1] - st, 3 * 3600)
        return {"tools": len(tools), "files": len(files), "prompts": len(prompts),
                "minutes": int(secs // 60)}

    def daily_costs(self, start_ts: float) -> dict[str, float]:
        out: dict[str, float] = {}
        for ts, usd, _ in self._entries.values():
            if ts >= start_ts:
                d = _dt.datetime.fromtimestamp(ts).strftime("%Y-%m-%d")
                out[d] = out.get(d, 0.0) + usd
        return out

    def cost_since(self, ts: float) -> float:
        return sum(v[1] for v in self._entries.values() if v[0] >= ts)

    def breakdown_since(self, ts: float) -> dict[str, tuple[int, float]]:
        out: dict[str, tuple[int, float]] = {}
        for t, usd, model in self._entries.values():
            if t >= ts:
                c, s = out.get(model, (0, 0.0))
                out[model] = (c + 1, s + usd)
        return out


def local_midnight(now: float | None = None) -> float:
    now = time.time() if now is None else now
    d = _dt.datetime.fromtimestamp(now)
    return d.replace(hour=0, minute=0, second=0, microsecond=0).timestamp()


def _load_state() -> dict:
    try:
        return json.loads(STATE_FILE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def _save_state(state: dict) -> None:
    try:
        ROOT.mkdir(parents=True, exist_ok=True)
        tmp = STATE_FILE.with_suffix(".tmp")
        tmp.write_text(json.dumps(state), encoding="utf-8")
        os.replace(tmp, STATE_FILE)
    except OSError:
        pass


_tracker = SpendTracker()


def live_figures(official_pct: float, limit: float, now: float | None = None) -> dict:
    """{'live_usd', 'live_pct', 'today_usd', 'since'} for the current poll.

    The baseline window starts when the official % last changed. On the very
    first run it starts at local midnight, because the official figure lags
    by about a day and so doesn't include today yet.
    """
    now = time.time() if now is None else now
    midnight = local_midnight(now)
    state = _load_state()
    if "official_pct" not in state:
        state = {"official_pct": official_pct, "since": midnight}
        _save_state(state)
    elif float(state["official_pct"]) != float(official_pct):
        state = {"official_pct": official_pct, "since": now}
        _save_state(state)
    since = float(state.get("since", midnight))
    _tracker.scan(min(since, now - 35 * 86400))        # 35 days: month chart + streak
    live_usd = official_pct / 100.0 * limit + _tracker.cost_since(since)
    return {
        "live_usd": live_usd,
        "live_pct": live_usd / limit * 100.0 if limit > 0 else 0.0,
        "today_usd": _tracker.cost_since(midnight),
        "since": since,
    }


def stats_figures(now: float | None = None) -> dict:
    """Today's counters, this month's daily spend (0..100 scaled) and the streak."""
    now = time.time() if now is None else now
    midnight = local_midnight(now)
    _tracker.scan(now - 35 * 86400)
    st = _tracker.today_stats(midnight)

    today = _dt.date.fromtimestamp(now)
    month_start = _dt.datetime(today.year, today.month, 1).timestamp()
    days_in_month = (_dt.date(today.year + (today.month == 12), today.month % 12 + 1, 1) - _dt.timedelta(days=1)).day
    daily = _tracker.daily_costs(month_start)
    vals = [daily.get(_dt.date(today.year, today.month, d).isoformat(), 0.0) for d in range(1, days_in_month + 1)]
    top = max(vals) if vals else 0.0
    hist = [int(round(v / top * 100)) if top > 0 else 0 for v in vals]

    allcost = _tracker.daily_costs(now - 40 * 86400)
    streak, d = 0, today
    if allcost.get(d.isoformat(), 0) <= 0:
        d -= _dt.timedelta(days=1)                    # no Claude yet today: count up to yesterday
    while allcost.get(d.isoformat(), 0) > 0:
        streak += 1
        d -= _dt.timedelta(days=1)

    st.update({"hist": hist, "today_idx": today.day - 1, "top_usd": round(top, 2), "streak": streak})
    return st


def report() -> str:
    """Human-readable self-check, used by the update script."""
    mid = local_midnight()
    _tracker.scan(mid)
    lines = [f"Claude Code today (since local midnight): ${_tracker.cost_since(mid):.2f}"]
    for model, (count, usd) in sorted(_tracker.breakdown_since(mid).items(), key=lambda x: -x[1][1]):
        lines.append(f"  {model:<36} {count:>5} msgs  ${usd:8.2f}")
    if _tracker.unknown_models:
        lines.append("  priced at Opus 5.5 rates (unknown ids): " + ", ".join(sorted(_tracker.unknown_models)))
    st = stats_figures()
    lines.append(f"Today: {st['tools']} tool calls, {st['files']} files edited, {st['prompts']} prompts, "
                 f"{st['minutes']} min with Claude, streak {st['streak']} day(s)")
    lines.append(f"This month, busiest day ${st['top_usd']:.2f}: "
                 + " ".join(str(v) for v in st['hist'][: st['today_idx'] + 1]))
    return "\n".join(lines)


if __name__ == "__main__":
    print(report())
