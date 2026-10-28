"""Checks the second Claude Code login that feeds the Clawdmeter's Max screen.

Prints the plan type and usage numbers only - never the token.
usage: <repo>\\.venv\\Scripts\\python.exe clawd_max_check.py [config folder]
"""
import asyncio
import datetime
import os
import sys
from pathlib import Path

REPO = Path(os.environ.get("USERPROFILE", str(Path.home()))) / "Clawdmeter"
sys.path.insert(0, str(REPO))
from daemon import cc_accounts  # noqa: E402
from daemon import claude_usage_daemon_windows as d  # noqa: E402

d.log = lambda m: None          # keep the check's own lines out of daemon.log

folder = Path(sys.argv[1]) if len(sys.argv) > 1 else cc_accounts.DEFAULT_MAX_DIR
tok, exp = cc_accounts.read_creds(folder / ".credentials.json")
if not tok:
    print(f"No sign-in found in {folder}")
    sys.exit(1)
if exp:
    until = datetime.datetime.fromtimestamp(exp).strftime("%H:%M")
    print(f"Sign-in found (token good until {until}; the daemon renews it on its own)")

try:
    p = asyncio.run(d.poll_api(tok))
except d.AuthError:
    print("Anthropic rejected that sign-in. Run connect-max.ps1 again and use /login.")
    sys.exit(1)
if p is None:
    print("Couldn't reach Anthropic right now - try again in a minute.")
    sys.exit(1)
if p.get("acct") != "pro":
    print("Heads-up: this login is a usage-based (work / Enterprise) account, not a Max plan.")
    print("Run connect-max.ps1 again, type /logout, then /login with your personal account.")
    sys.exit(2)


def span(m: int) -> str:
    return f"{m // 1440}d {m % 1440 // 60}h" if m >= 1440 else f"{m // 60}h {m % 60}m"


print(f"Personal plan OK: current session {p['s']}% (resets in {span(p['sr'])}), "
      f"weekly {p['w']}% (resets in {span(p['wr'])})")
if "m" in p:
    print(f"Also on the board: {p['mn']} only {p['m']}% this week")
