#!/usr/bin/env python3
"""Windows daemon: several Claude logins (config_dirs) + dashboard push.

Run: python -m pytest daemon/tests/test_windows_multidir.py -x -q
"""
import asyncio
import json
from pathlib import Path
from unittest.mock import AsyncMock, MagicMock, patch

import daemon.claude_usage_daemon_windows as mod
from daemon.claude_usage_daemon_windows import PlanSelector, read_config_dirs, read_token_for


def _run(coro):
    return asyncio.run(coro)


def _creds(d: Path, token: str) -> Path:
    d.mkdir(parents=True, exist_ok=True)
    (d / ".credentials.json").write_text(json.dumps({"claudeAiOauth": {"accessToken": token}}))
    return d


def test_config_dirs_unset_means_legacy(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")
    assert read_config_dirs() is None


def test_config_dirs_parsed_and_expanded(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("chime = on\nconfig_dirs = ~/.claude, ~/.claude-personal  # both plans\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_config_dirs() == [Path.home() / ".claude", Path.home() / ".claude-personal"]


def test_read_token_for_reads_that_dir(tmp_path):
    assert read_token_for(_creds(tmp_path / "a", "tok-a")) == "tok-a"
    assert read_token_for(tmp_path / "missing") is None


def test_selector_follows_rising_usage_not_resets():
    sel = PlanSelector()
    a, b = Path("a"), Path("b")
    assert sel.choose({a: 10, b: 50}) == b          # startup: highest wins
    assert sel.choose({a: 12, b: 50}) == a          # a rose
    assert sel.choose({a: 12, b: 0}) == a           # b's reset isn't use
    assert sel.choose({a: 12, b: 3}) == b           # b rose


def test_legacy_single_plan_unchanged(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")
    with patch.object(mod, "read_token", return_value="tok"), \
         patch.object(mod, "poll_api", AsyncMock(return_value={"s": 5})) as poll, \
         patch.object(mod, "push_dashboard", AsyncMock()) as push:
        assert _run(mod.poll_active_payload(None, PlanSelector())) == {"s": 5}
    poll.assert_awaited_once_with("tok")
    push.assert_awaited_once_with([{"s": 5}])


def test_two_plans_both_pushed_active_returned(tmp_path, monkeypatch):
    ent, mx = _creds(tmp_path / "ent", "tok-ent"), _creds(tmp_path / "max", "tok-max")
    cfg = tmp_path / "config"
    cfg.write_text(f"config_dirs = {ent}, {mx}\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    answers = {"tok-ent": {"s": 20, "acct": "ent"}, "tok-max": {"s": 40, "acct": "pro"}}
    with patch.object(mod, "poll_api", AsyncMock(side_effect=lambda t: answers[t])), \
         patch.object(mod, "push_dashboard", AsyncMock()) as push:
        out = _run(mod.poll_active_payload(None, PlanSelector()))
    assert out["acct"] == "pro"                       # highest session at startup
    push.assert_awaited_once_with([answers["tok-ent"], answers["tok-max"]])


def test_one_login_expired_other_still_works(tmp_path, monkeypatch):
    ent, mx = _creds(tmp_path / "ent", "tok-ent"), _creds(tmp_path / "max", "tok-max")
    cfg = tmp_path / "config"
    cfg.write_text(f"config_dirs = {ent}, {mx}\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)

    async def poll(t):
        if t == "tok-ent":
            raise mod.AuthError("401")
        return {"s": 7, "acct": "pro"}
    tray = MagicMock()
    with patch.object(mod, "poll_api", AsyncMock(side_effect=poll)), \
         patch.object(mod, "push_dashboard", AsyncMock()):
        assert _run(mod.poll_active_payload(tray, PlanSelector())) == {"s": 7, "acct": "pro"}
    tray.set_error.assert_not_called()


def test_no_tokens_anywhere_flags_tray(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text(f"config_dirs = {tmp_path / 'x'}, {tmp_path / 'y'}\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    tray = MagicMock()
    with patch.object(mod, "push_dashboard", AsyncMock()) as push:
        assert _run(mod.poll_active_payload(tray, PlanSelector())) is None
    tray.set_error.assert_called_once()
    push.assert_not_awaited()


def test_transient_failure_leaves_tray_alone(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")
    tray = MagicMock()
    with patch.object(mod, "read_token", return_value="tok"), \
         patch.object(mod, "poll_api", AsyncMock(return_value=None)):
        assert _run(mod.poll_active_payload(tray, PlanSelector())) is None
    tray.set_error.assert_not_called()


# ---------------------------------------------------------------------------
# config file pitfalls on Windows (Notepad) + dashboard log lines
# ---------------------------------------------------------------------------

def test_notepad_config_txt_is_accepted(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")
    (tmp_path / "config.txt").write_text("dashboard_url = http://h:8090/claude\ndashboard_token = abc\n")
    assert mod.config_path() == tmp_path / "config.txt"
    assert mod.read_dashboard_settings() == ("http://h:8090/claude", "abc")


def test_real_config_wins_over_txt(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")
    (tmp_path / "config").write_text("chime = on\n")
    (tmp_path / "config.txt").write_text("chime = off\n")
    assert mod.read_chime_setting() == "on"


def test_bom_and_quotes_tolerated(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_bytes("﻿dashboard_url = \"http://h:8090/claude\"\r\ndashboard_token = 'abc'\r\n"
                    "config_dirs = ~/.claude\r\n".encode("utf-8"))
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert mod.read_dashboard_settings() == ("http://h:8090/claude", "abc")
    assert read_config_dirs() == [Path.home() / ".claude"]


def test_dashboard_log_says_what_happened(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")
    monkeypatch.setattr(mod, "_last_dashboard_status", "")
    logs = []
    monkeypatch.setattr(mod, "log", logs.append)
    _run(mod.push_dashboard([{"s": 1}]))
    _run(mod.push_dashboard([{"s": 1}]))
    assert len(logs) == 1 and "no dashboard_url" in logs[0]          # said once, not every poll

    (tmp_path / "config").write_text("dashboard_url = http://h:8090/claude\ndashboard_token = abc\n")
    ok = MagicMock(status_code=204, text="")
    bad = MagicMock(status_code=403, text="forbidden")

    class Client:
        def __init__(self, resp):
            self.resp = resp

        async def __aenter__(self):
            return self

        async def __aexit__(self, *a):
            return False

        async def post(self, *a, **k):
            return self.resp
    with patch.object(mod.httpx, "AsyncClient", lambda **k: Client(ok)):
        _run(mod.push_dashboard([{"s": 1}, {"s": 2}]))
        _run(mod.push_dashboard([{"s": 1}, {"s": 2}]))
    assert logs[-1] == "Dashboard: sending 2 plan(s) to http://h:8090/claude" and len(logs) == 2
    with patch.object(mod.httpx, "AsyncClient", lambda **k: Client(bad)):
        _run(mod.push_dashboard([{"s": 1}]))
        _run(mod.push_dashboard([{"s": 1}]))
    assert "HTTP 403 (check dashboard_token)" in logs[-1] and len(logs) == 4   # failures every time
