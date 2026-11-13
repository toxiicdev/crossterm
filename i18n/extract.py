"""Extract source strings (L, N, translate and describe() texts) into i18n/keys.json.

Usage: python i18n/extract.py
"""
import json
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LITERAL = r'"((?:[^"\\]|\\.)*)"'
PATTERNS = [
    re.compile(r'\bL\(\s*' + LITERAL),
    re.compile(r'\bN\(\s*' + LITERAL),
    re.compile(r'translate\("CrossTerm",\s*' + LITERAL),
    re.compile(r'\b(?:tr|ui|translated)\(\s*' + LITERAL),  # module helper functions
    re.compile(r'\{"[^"]+",\s*' + LITERAL + r'\}'),  # describe() map
]


def unescape(text):
    return re.sub(r'\\(["\\n])', lambda m: "\n" if m.group(1) == "n" else m.group(1), text)


keys = []
for name in ("main.cpp", "sftp-panel.h", "terminal.cpp", "profiles.cpp", "askpass.h", "transports.cpp", "monitor.cpp", "xserver.cpp", "proxy.cpp", "mosh.cpp", "foreign.cpp", "mobaimport.cpp", "ftp.cpp", "servers.cpp", "vnc.cpp", "appearance.h", "rdp.cpp"):
    source = open(os.path.join(ROOT, name), encoding="utf-8").read()
    for pattern in PATTERNS:
        if pattern.pattern.startswith(r'\{') and name != "profiles.cpp":
            continue
        for match in pattern.finditer(source):
            key = unescape(match.group(1))
            if key not in keys:
                keys.append(key)
json.dump(sorted(keys), open(os.path.join(ROOT, "i18n", "keys.json"), "w", encoding="utf-8"), ensure_ascii=False, indent=1)
print(len(keys), "keys")
