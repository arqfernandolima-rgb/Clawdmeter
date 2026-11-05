"""Read Claude Code's sign-in from Windows Credential Manager (read-only).

Newer Claude Code (rolled out with a server-side flag) keeps the OAuth sign-in
in Windows Credential Manager instead of .credentials.json, as generic
credentials holding the same JSON the file used to:

  "Claude Code-credentials"             the default config folder (~/.claude)
  "Claude Code-credentials-<8 hex>"     CLAUDE_CONFIG_DIR=<folder>;
                                        hex = sha256(folder)[:8]

Claude Code owns writing and renewing these; the daemon only reads them, for
the signed-in Windows user it runs as. Nothing here logs or prints secrets.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import sys
import unicodedata
from pathlib import Path

_NAME_RE = re.compile(r"^Claude Code(?P<mid>[^/]*?)-credentials(?P<hash>-[0-9a-f]{8})?(?:/.*)?$")


def _enumerate() -> dict[str, bytes]:
    """{target name: blob} for generic credentials named "Claude Code*"."""
    if sys.platform != "win32":
        return {}
    try:
        import ctypes
        from ctypes import wintypes

        class FILETIME(ctypes.Structure):
            _fields_ = [("lo", wintypes.DWORD), ("hi", wintypes.DWORD)]

        class CREDENTIALW(ctypes.Structure):
            _fields_ = [
                ("Flags", wintypes.DWORD), ("Type", wintypes.DWORD),
                ("TargetName", wintypes.LPWSTR), ("Comment", wintypes.LPWSTR),
                ("LastWritten", FILETIME), ("CredentialBlobSize", wintypes.DWORD),
                ("CredentialBlob", ctypes.POINTER(ctypes.c_ubyte)), ("Persist", wintypes.DWORD),
                ("AttributeCount", wintypes.DWORD), ("Attributes", ctypes.c_void_p),
                ("TargetAlias", wintypes.LPWSTR), ("UserName", wintypes.LPWSTR),
            ]

        advapi = ctypes.WinDLL("advapi32", use_last_error=True)
        enum = advapi.CredEnumerateW
        enum.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                         ctypes.POINTER(ctypes.POINTER(ctypes.POINTER(CREDENTIALW)))]
        enum.restype = wintypes.BOOL
        free = advapi.CredFree
        free.argtypes = [ctypes.c_void_p]
        free.restype = None

        count = wintypes.DWORD(0)
        arr = ctypes.POINTER(ctypes.POINTER(CREDENTIALW))()
        if not enum("Claude Code*", 0, ctypes.byref(count), ctypes.byref(arr)):
            return {}                       # none found (or not allowed): no entries
        out: dict[str, bytes] = {}
        try:
            for i in range(count.value):
                c = arr[i].contents
                n = int(c.CredentialBlobSize)
                blob = bytes(c.CredentialBlob[:n]) if n and c.CredentialBlob else b""
                out[c.TargetName or ""] = blob
        finally:
            free(arr)
        return out
    except Exception:                       # never let Credential Manager break the daemon
        return {}


def _decode(blob: bytes) -> dict | None:
    for enc in ("utf-8", "utf-16-le"):
        try:
            d = json.loads(blob.decode(enc).strip("\x00").strip())
            if isinstance(d, dict):
                return d
        except (UnicodeDecodeError, ValueError):
            continue
    return None


def _hashes_for(config_dir: Path) -> set[str]:
    """Possible 8-hex suffixes for a config folder (Claude Code hashes the path
    string; cover the usual spellings of the same folder)."""
    raw = str(config_dir)
    variants = {raw, raw.rstrip("\\/"), os.path.normpath(raw)}
    try:
        variants.add(str(Path(raw).expanduser().resolve()))
    except OSError:
        pass
    for v in list(variants):
        if len(v) > 1 and v[1] == ":":
            variants.add(v[0].upper() + v[1:])
            variants.add(v[0].lower() + v[1:])
    return {"-" + hashlib.sha256(unicodedata.normalize("NFC", v).encode("utf-8")).hexdigest()[:8]
            for v in variants}


def read(config_dir: Path | None = None, entries: dict[str, bytes] | None = None
         ) -> tuple[str | None, float | None]:
    """(access token, expiry epoch seconds) from Credential Manager, or (None, None).

    config_dir None = the default ~/.claude login; otherwise the login made with
    CLAUDE_CONFIG_DIR=<config_dir>.
    """
    entries = _enumerate() if entries is None else entries
    want = None if config_dir is None else _hashes_for(config_dir)
    plain, hashed = [], []
    for name, blob in entries.items():
        m = _NAME_RE.match(name)
        if not m or m.group("mid"):
            continue
        (hashed if m.group("hash") else plain).append((m.group("hash"), blob))
    if want is None:
        pool = plain
    else:
        pool = [h for h in hashed if h[0] in want]
        if not pool and len(hashed) == 1:   # one extra login, path spelled differently
            pool = hashed
    for _, blob in pool:
        d = _decode(blob)
        oauth = d.get("claudeAiOauth") if d else None
        if not isinstance(oauth, dict):
            continue
        tok = oauth.get("accessToken")
        try:
            exp = float(oauth["expiresAt"]) / 1000.0 if oauth.get("expiresAt") is not None else None
        except (TypeError, ValueError):
            exp = None
        if isinstance(tok, str) and tok.strip():
            return tok, exp
    return None, None


def has_login(config_dir: Path | None = None) -> bool:
    return read(config_dir)[0] is not None
