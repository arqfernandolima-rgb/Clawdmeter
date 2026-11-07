#!/usr/bin/env python3
"""Windows daemon (fernando-mods): pushing usage to the e-paper dashboard.

Run: python -m pytest daemon/tests/test_windows_dashboard.py -q
"""
import asyncio
from pathlib import Path
from unittest.mock import AsyncMock, MagicMock, patch

import daemon.claude_usage_daemon_windows as mod


def _run(coro):
    return asyncio.run(coro)


class _Client:
    def __init__(self, resp=None, exc=None, sent=None):
        self.resp, self.exc, self.sent = resp, exc, sent if sent is not None else []

    async def __aenter__(self):
        return self

    async def __aexit__(self, *a):
        return False

    async def post(self, url, json=None, headers=None):
        if self.exc:
            raise self.exc
        self.sent.append((url, json, headers))
        return self.resp


def _cfg(tmp_path, monkeypatch, text, name="config"):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")
    (tmp_path / name).write_bytes(text.encode("utf-8"))


# ---------------------------------------------------------------- settings

def test_settings_plain(tmp_path, monkeypatch):
    _cfg(tmp_path, monkeypatch, "chime = on\ndashboard_url = http://h:8090/claude\ndashboard_token = abc\n")
    assert mod.read_dashboard_settings() == ("http://h:8090/claude", "abc")


def test_settings_bom_quotes_crlf(tmp_path, monkeypatch):
    _cfg(tmp_path, monkeypatch, "﻿dashboard_url = \"http://h:8090/claude\"\r\ndashboard_token = 'abc'\r\n")
    assert mod.read_dashboard_settings() == ("http://h:8090/claude", "abc")


def test_settings_from_notepad_config_txt(tmp_path, monkeypatch):
    _cfg(tmp_path, monkeypatch, "dashboard_url = http://h:8090/claude\n", name="config.txt")
    assert mod.read_dashboard_settings() == ("http://h:8090/claude", "")


def test_settings_absent(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")
    assert mod.read_dashboard_settings() == ("", "")


# ---------------------------------------------------------------- push

def test_push_sends_only_usage_fields_and_logs_once(tmp_path, monkeypatch):
    _cfg(tmp_path, monkeypatch, "dashboard_url = http://h:8090/claude\ndashboard_token = abc\n")
    monkeypatch.setattr(mod, "_last_dashboard_status", "")
    logs, sent = [], []
    monkeypatch.setattr(mod, "log", logs.append)
    payload = {"s": 37.4, "sr": 900, "st": "allowed", "acct": "ent", "tp": 20, "rd": "Dec 1",
               "sd": 166.5, "td": 3.2, "c": 1, "vol": 40}
    with patch.object(mod.httpx, "AsyncClient", lambda **k: _Client(MagicMock(status_code=204, text=""), sent=sent)):
        _run(mod.push_dashboard([payload]))
        _run(mod.push_dashboard([payload]))
    url, body, headers = sent[0]
    assert url == "http://h:8090/claude" and headers == {"X-Dashboard-Token": "abc"}
    assert body["plans"] == [{"s": 37.4, "sr": 900, "st": "allowed", "acct": "ent", "tp": 20, "rd": "Dec 1"}]
    assert logs == ["Dashboard: sending ent to http://h:8090/claude"]          # once, not every poll


def test_push_wrong_token_and_unreachable_are_logged_every_time(tmp_path, monkeypatch):
    _cfg(tmp_path, monkeypatch, "dashboard_url = http://h:8090/claude\ndashboard_token = nope\n")
    logs = []
    monkeypatch.setattr(mod, "log", logs.append)
    with patch.object(mod.httpx, "AsyncClient", lambda **k: _Client(MagicMock(status_code=403, text="bad token"))):
        _run(mod.push_dashboard([{"s": 1, "acct": "pro"}]))
        _run(mod.push_dashboard([{"s": 1, "acct": "pro"}]))
    assert len(logs) == 2 and "HTTP 403 (check dashboard_token)" in logs[0]
    with patch.object(mod.httpx, "AsyncClient", lambda **k: _Client(exc=mod.httpx.ConnectTimeout("t"))):
        _run(mod.push_dashboard([{"s": 1, "acct": "pro"}]))
    assert "ConnectTimeout" in logs[-1] and "same network" in logs[-1]


def test_no_url_no_request(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")
    called = []
    with patch.object(mod.httpx, "AsyncClient", lambda **k: called.append(1)):
        _run(mod.push_dashboard([{"s": 1, "acct": "pro"}]))
    assert not called


# ---------------------------------------------------------------- Max account

def test_poll_max_pushes_the_max_account(monkeypatch, tmp_path):
    raw = {"s": 42, "sr": 133, "w": 61, "wr": 4000, "st": "allowed", "acct": "pro"}
    push = AsyncMock()
    monkeypatch.setattr(mod, "push_dashboard", push)
    monkeypatch.setattr(mod, "poll_api", AsyncMock(return_value=raw))
    monkeypatch.setattr(mod, "KEEPER", MagicMock())
    with patch.object(mod.cc_accounts, "best_creds", return_value=("tok", None)):
        msg = _run(mod.poll_max(tmp_path, {}))
    push.assert_awaited_once_with([raw])
    assert msg["hm"] == 1 and msg["s"] == 42                       # the board still gets its Max message


# ---------------------------------------------------------------- board away

def _offline(monkeypatch, tmp_path, url=True, age=60, token="tok", mdir="max"):
    _cfg(tmp_path, monkeypatch, "dashboard_url = http://h:8090/claude\n" if url else "chime = on\n")
    monkeypatch.setattr(mod, "POLL_INTERVAL", 0.05)
    work = {"s": 30, "acct": "ent"}
    push, pmax = AsyncMock(), AsyncMock(return_value=None)
    monkeypatch.setattr(mod, "push_dashboard", push)
    monkeypatch.setattr(mod, "poll_max", pmax)
    monkeypatch.setattr(mod, "poll_api", AsyncMock(return_value=work))
    monkeypatch.setattr(mod, "read_token", lambda: token)
    monkeypatch.setattr(mod, "add_spend_fields", lambda p: p.__setitem__("s", 30.5))
    monkeypatch.setattr(mod, "KEEPER", MagicMock())
    monkeypatch.setattr(mod.cc_board, "read_cfg", lambda: {})
    monkeypatch.setattr(mod.cc_accounts, "max_dir", lambda cfg: Path(mdir) if mdir else None)
    monkeypatch.setattr(mod, "primary_cred_path", lambda: None)

    async def go():
        mod._last_poll_at = mod.time.time() - age
        stop = asyncio.Event()
        task = asyncio.create_task(mod.dashboard_while_offline(stop))
        await asyncio.sleep(0.09)
        stop.set()
        await asyncio.wait_for(task, 1)
    _run(go())
    return push, pmax


def test_board_away_still_feeds_both_accounts(monkeypatch, tmp_path):
    push, pmax = _offline(monkeypatch, tmp_path)
    push.assert_awaited_with([{"s": 30.5, "acct": "ent"}])        # work account, live spend figure
    pmax.assert_awaited()                                         # Max account (pushes itself)


def test_board_connected_no_extra_polls(monkeypatch, tmp_path):
    push, pmax = _offline(monkeypatch, tmp_path, age=0)
    push.assert_not_awaited()
    pmax.assert_not_awaited()


def test_no_dashboard_no_offline_polls(monkeypatch, tmp_path):
    push, pmax = _offline(monkeypatch, tmp_path, url=False)
    push.assert_not_awaited()
    pmax.assert_not_awaited()


def test_no_max_account_only_work(monkeypatch, tmp_path):
    push, pmax = _offline(monkeypatch, tmp_path, mdir=None)
    push.assert_awaited()
    pmax.assert_not_awaited()
