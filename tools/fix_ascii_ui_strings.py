#!/usr/bin/env python3
"""Replace fancy Unicode punctuation in firmware/ui sources.

Adafruit GFX only has glyphs for printable ASCII (0x20-0x7E). Em dashes,
arrows, ellipsis, etc. in toast/print strings show as garbage on the T-Deck.
"""
from __future__ import annotations

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1] / "firmware" / "ui"

# Order matters for multi-char replacements first
REPL = [
    ("\u2014", "-"),   # em dash
    ("\u2013", "-"),   # en dash
    ("\u2026", "..."),  # ellipsis
    ("\u2192", "->"),
    ("\u2190", "<-"),
    ("\u00d7", "x"),
    ("\u2264", "<="),
    ("\u2265", ">="),
    ("\u2248", "~"),
    ("\u2022", "*"),
    ("\u00b7", "*"),
    ("\u201c", '"'),
    ("\u201d", '"'),
    ("\u2018", "'"),
    ("\u2019", "'"),
    ("\u00a0", " "),
    ("\u2500", "-"),
    ("\u2501", "-"),
    ("\u2011", "-"),
    ("\u2012", "-"),
    ("\u2015", "-"),
    ("\ufeff", ""),    # BOM
]


def scrub(text: str) -> str:
    for a, b in REPL:
        text = text.replace(a, b)
    return text


def main() -> int:
    if not ROOT.is_dir():
        print("missing", ROOT, file=sys.stderr)
        return 1
    changed = []
    left = []
    for p in sorted(ROOT.rglob("*")):
        if p.suffix not in {".cpp", ".h", ".c"}:
            continue
        raw = p.read_bytes()
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            text = raw.decode("utf-8", errors="replace")
        new = scrub(text)
        if new != text:
            p.write_bytes(new.encode("utf-8"))
            changed.append(p.relative_to(ROOT).as_posix())
        hi = sorted({hex(ord(c)) for c in new if ord(c) > 127})
        if hi:
            left.append(f"{p.name}: {', '.join(hi[:12])}")
    print("changed:", ", ".join(changed) if changed else "(none)")
    if left:
        print("remaining non-ascii (comments/ok if not printed):")
        for row in left:
            print(" ", row)
    else:
        print("ok: no non-ASCII codepoints left under firmware/ui")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
