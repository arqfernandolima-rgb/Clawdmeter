# What the `fernando-mods` branch adds

Built on juppeee's `csb-buddy` branch, for the Waveshare ESP32-C6-Touch-AMOLED-2.16
driven by the Windows daemon. Everything here is additive: a host that doesn't
send the new messages gets the stock behavior.

## On the board

- **Live Claude Code status** on the bottom line (Thinking..., Running command...,
  Needs you), driven by Claude Code hooks instead of the whimsical rotation, with
  a turn timer and the project name (+N when several sessions are open).
- **Live spend estimate** (Enterprise / usage-based accounts): the official
  utilization moves in $5 steps, so the daemon adds Claude Code's local
  transcripts on top for a live figure with one decimal, plus dollars spent,
  the monthly limit and today's spend.
- **Touchscreen approvals**: permission prompts show Allow / Deny on the screen,
  headed by Claude's own one-line description of the command, with the full
  command underneath in monospace. Commands up to 600 characters (multi-line
  included) scroll, and Allow unlocks only after you've scrolled to the end.
  Long commands page through on their own. When Claude Code offers a
  "don't ask again" rule, a third **Always** button saves that same rule.
  Longer ones, plans and questions, prompts Claude Code reserves for the person
  (such as the read fence's), and anything while the board is face-down or
  disconnected, fall through to Claude Code's normal prompt. Each prompt's
  outcome is logged to `%LOCALAPPDATA%\Clawdmeter\approval-hook.log`.
- **Battery mode**: unplugged, the screen runs at 60% of your brightness and
  sleeps after 2 minutes without a touch (on USB it never sleeps). Tap, a
  button, picking it up, or Claude needing you wakes it; face-down goes dark
  right away. Tunables are in `firmware/src/idle_cfg.h`.
- **Attention screens + sounds**: "Claude needs you" when a permission prompt is
  waiting, "Your turn" after a long turn, a small celebration after a very long
  one. Sounds are synthesized on the ES8311 codec.
- **Screens**: tap to cycle Clawd -> Usage -> Max -> Today.
  - *Usage* alternates the title between the clock and "Usage", and shows a
    month-to-date bar chart of daily spend.
  - *Max* (optional) shows a second Claude account, e.g. a personal Max plan,
    whose limits are shared with the Claude desktop/web/mobile apps: 5-hour and
    weekly utilization, reset times, and a marker on each bar for how far into
    the window you are.
  - *Today* shows tool calls, files edited, prompts, approvals, time with
    Claude and the day streak.
- **Motion** (QMI8658): face-down = do-not-disturb (no sounds, no approvals),
  shake = surprised Clawd, pick-up after a still spell = wink.
- **Volume**: long-press the right button to step Off / Low / Medium / High.
- Type: Tiempos (serif) for the title and big numbers, Styrene everywhere else.
  The Clawd logo bobs while Claude is busy.

## Host messages (BLE RX characteristic, JSON, each write <= ~244 bytes)

| Kind | Marker | Keys |
|---|---|---|
| usage | (none) | `s sr w wr st acct tp pd rd ok` + `sl sd td op` (spend), `vol fz` (settings), `t tf c` |
| status | `"so":1` | `cs a tt pj ns att aq atd atp snd sq` (partial: only keys present change) |
| approval | `"so":1` + `ai` | header `ai at ah aa an snd sq` (`aa` = rule Always saves), then `an` chunks `{"so":1,"ac":id,"d":...}` in order (`ai:0` clears) |
| stats | `"hs":1` | `dh di dm kt kf kp ka km kr` |
| max | `"hm":1` | `s sr w wr e` (+ optional `m mr mn` per-model weekly limit); `e`: 0 ok, 1 not signed in, 2 sign-in expired |

Board -> host, on the REQ notify characteristic: `0x01` refresh, `A<id>:1|0|2`
approval answer (2 = always), `D1`/`D0` face-down, `V<n>` volume changed.

## Windows daemon

New modules next to `claude_usage_daemon_windows.py`:

- `cc_status_hook.py`, `cc_approval_hook.py`: Claude Code hooks (installed into
  `~/.claude/settings.json` by `install_hooks.py`, which backs the file up first).
- `cc_status.py`: turns per-session hook state into the status message.
- `cc_spend.py`: prices Claude Code transcripts for the live spend and stats.
- `cc_approval.py`, `cc_board.py`: approval files, config, message builders,
  optional Home Assistant webhook.
- `cc_accounts.py`: the second (Max) account and the sign-in keep-alive. Claude
  Code tokens last a few hours and renew only when Claude Code runs, so a few
  minutes before one expires the daemon runs one tiny `claude -p` (Haiku, no
  tools, no MCP, nothing saved) and Claude Code renews it itself. The hooks and
  the spend tracker ignore those runs.

Config, `%LOCALAPPDATA%\Clawdmeter\config` (all optional):

```
monthly_limit_usd = 500      # Enterprise spend limit, for the dollar line
approval = on                # approval_timeout_s = 20
sound = on                   # volume = 70
facedown_sign = 1            # -1 if face-down is detected the wrong way round
done_alert_min_s = 60        # celebrate_min_s = 600, remind_after_s = 120
keepalive = on
max_config_dir = auto        # auto: %USERPROFILE%\.claude-personal if signed in
claude_path =                # only if claude isn't on PATH
ha_webhook_url =             # http://homeassistant.local:8123/api/webhook/<id>
```

## Setup

1. Build and flash: `pio run -d firmware -e waveshare_amoled_216_c6 -t upload --upload-port COMx`
   (or flash only the app at 0x10000 to keep the Bluetooth pairing).
2. Install the Windows daemon as usual (`install-windows.ps1`), then add the hooks:
   `.venv\Scripts\python.exe daemon\install_hooks.py <path to base pythonw.exe> <repo>\daemon`
3. Optional Max screen: `powershell -ExecutionPolicy Bypass -File tools\windows\connect-max.ps1`
   signs a second Claude Code login into `%USERPROFILE%\.claude-personal`.
4. Optional Home Assistant: see `tools/windows/home-assistant-clawdmeter.yaml`.
