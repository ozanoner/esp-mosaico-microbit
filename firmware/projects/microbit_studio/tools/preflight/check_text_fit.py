#!/usr/bin/env python3
"""Fail the build if the panel's text cannot fit the column it is drawn in.

Every runtime-bound string is measured with the real TTF at the size the scene
gives it. This exists because a sentence that overflowed its box shipped as
":bit is not adver" on the panel once.

Exit 0 = all text fits. Exit 1 = at least one overflow (printed with widths).
"""

import pathlib
import re
import sys

from PIL import ImageFont

BASE = pathlib.Path(__file__).resolve().parents[2]
UI = BASE / "components/app_studio/ui"
SRC = BASE / "components/app_studio/app_studio.c"

COLUMN_W = 288  # app column (APP_W) in the 480 px scene
STATE_SIZE = 20
MESSAGE_SIZE = 40


def font(name: str, size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(UI / "fonts" / name), size)


def main() -> int:
    src = SRC.read_text()
    state_font = font("Fredoka-SemiBold.ttf", STATE_SIZE)
    msg_font = font("Fredoka-Medium.ttf", MESSAGE_SIZE)
    problems = []

    # screens table: {"HEADLINE", "line1", "line2", bear, progress, success}
    table = re.search(r"static const studio_screen_t screens\[[^\]]*\] = \{(.*?)\n\};",
                      src, re.S)
    if table is None:
        print("check_text_fit: screens table not found", file=sys.stderr)
        return 1
    for m in re.finditer(r'\[(APP_STUDIO_[A-Z_]+)\]\s*=\s*\{([^}]*)\}', table.group(1)):
        state, body = m.group(1), m.group(2)
        strings = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
        for idx, text in enumerate(strings[:3]):
            text = text.encode().decode("unicode_escape")
            if not text:
                continue
            f = state_font if idx == 0 else msg_font
            size = int(f.size)
            width = f.getlength(text)
            if width > COLUMN_W:
                problems.append((state, text, round(width), size))

    # failure table: {sentence, line1, line2}
    ftable = re.search(r"static const failure_line_t failure_lines\[\] = \{(.*?)\n\};",
                       src, re.S)
    if ftable is None:
        print("check_text_fit: failure table not found", file=sys.stderr)
        return 1
    for m in re.finditer(r'\{"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"\}',
                         ftable.group(1)):
        raw, line1, line2 = (g.encode().decode("unicode_escape") for g in m.groups())
        for text in (line1, line2):
            if not text:
                problems.append(("FAILED", f"empty line for: {raw}", 0, 0))
                continue
            width = msg_font.getlength(text)
            if width > COLUMN_W:
                problems.append(("FAILED", text, round(width), MESSAGE_SIZE))

    for state, text, width, size in problems:
        print(f"OVERFLOW {state:16} {size:>2}px {width:>3}px > {COLUMN_W}px  {text!r}")
    print(f"check_text_fit: {'PASS' if not problems else f'FAIL ({len(problems)})'}")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
