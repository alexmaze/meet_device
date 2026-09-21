#!/usr/bin/env python3
"""Generate LVGL CJK font covering GB2312 level-1 + Meet UI extras."""

from __future__ import annotations

import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "main/ui/fonts/font_meet_cjk_16_4.c"
TTF = pathlib.Path("/tmp/meet-cjk-medium.ttf")
TTC = pathlib.Path("/System/Library/Fonts/STHeiti Medium.ttc")


def gb2312_level1() -> set[str]:
    chars: set[str] = set()
    for b1 in range(0xB0, 0xD8):
        for b2 in range(0xA1, 0xFF):
            try:
                ch = bytes((b1, b2)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if ch:
                chars.add(ch)
    return chars


def meet_source_chars() -> set[str]:
    chars: set[str] = set()
    src = ROOT / "main"
    for path in src.rglob("*"):
        if path.suffix.lower() not in {".cc", ".h", ".c", ".html", ".txt"}:
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except OSError:
            continue
        for ch in text:
            if ord(ch) > 127:
                chars.add(ch)
    return chars


def extract_ttf() -> None:
    from fontTools.ttLib import TTCollection

    col = TTCollection(str(TTC))
    chosen = col.fonts[0]
    for i, font in enumerate(col.fonts):
        names = []
        for rec in font["name"].names:
            if rec.nameID in (1, 4, 16):
                try:
                    names.append(rec.toUnicode())
                except Exception:
                    continue
        blob = " ".join(names)
        print(f"ttc face {i}: {blob}")
        if "SC" in blob or "GB" in blob or "简" in blob:
            chosen = font
            if "SC" in blob or "GB" in blob:
                break
    TTF.parent.mkdir(parents=True, exist_ok=True)
    chosen.save(str(TTF))
    print(f"wrote {TTF} ({TTF.stat().st_size} bytes)")


def main() -> int:
    extra = "·—…→≈「」『』（）《》！？：；、"
    chars = gb2312_level1() | meet_source_chars() | set(extra)
    symbols = "".join(sorted(chars, key=ord))
    print(f"symbol count={len(symbols)}")

    if not TTF.exists():
        extract_ttf()

    OUT.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        "npx",
        "--yes",
        "lv_font_conv@1.5.3",
        "--font",
        str(TTF),
        "--size",
        "16",
        "--bpp",
        "4",
        "--format",
        "lvgl",
        "--no-compress",
        "--no-prefilter",
        "--autohint-strong",
        "--force-fast-kern-format",
        "--lv-include",
        "lvgl.h",
        "--lv-font-name",
        "font_meet_cjk_16_4",
        "-r",
        "0x20-0x7E",
        "--symbols",
        symbols,
        "-o",
        str(OUT),
    ]
    print("running lv_font_conv…")
    subprocess.run(cmd, check=True)
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
