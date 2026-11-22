#!/usr/bin/env python3
# This file is part of the OSCQuery Automation Plugin —
# https://github.com/pob31/OSCQuery-Automation-plugin
#
# Copyright (C) 2026 Pierre-Olivier Boulant, Julien Bayle
#
# The OSCQuery Automation Plugin is free software: you can redistribute it
# and/or modify it under the terms of the GNU General Public License as
# published by the Free Software Foundation, either version 3 of the License,
# or (at your option) any later version. It is distributed in the hope that it
# will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
# of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
# Public License (LICENSE, at the repository root) for more details.
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""GPL headers on every source file of this repository.

Produces: nothing. Exits 0 when every tracked source file carries the SPDX line
         and either this project's copyright line or an `Origin:` line, 1 otherwise.
Usage:   python3 scripts/check-headers.py
Needs:   python3 and git.

The rule (CLAUDE.md, standing rules): GPLv3 headers in go.dot's form reading
"Copyright (C) 2026 Pierre-Olivier Boulant, Julien Bayle". A file copied from
WFS-DIY or go.dot keeps its original header and gains a one-line `Origin:`
note instead. Documents (*.md), fixtures (*.json) and ThirdParty/ are not
source files and are not checked; docs/spec/ is CC BY 4.0 and says so itself.
"""

import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

SOURCE_SUFFIXES = {".h", ".hpp", ".c", ".cpp", ".mm", ".cmake", ".py", ".sh", ".ps1", ".yml", ".yaml", ".iss"}
SOURCE_NAMES = {"CMakeLists.txt"}

SPDX = "SPDX-License-Identifier: GPL-3.0-or-later"
OURS = "Copyright (C) 2026 Pierre-Olivier Boulant, Julien Bayle"
ORIGIN = "Origin:"

HEADER_LINES = 30


def tracked_files() -> list[Path]:
    out = subprocess.run(["git", "ls-files", "-z"], cwd=REPO_ROOT, check=True,
                         capture_output=True, text=True).stdout
    return [REPO_ROOT / p for p in out.split("\0") if p]


def is_source(path: Path) -> bool:
    rel = path.relative_to(REPO_ROOT).as_posix()
    if rel.startswith("ThirdParty/"):
        return False
    return path.name in SOURCE_NAMES or path.suffix in SOURCE_SUFFIXES


def main() -> int:
    print(f"check-headers: {REPO_ROOT}")
    failures: list[str] = []
    checked = 0

    for path in tracked_files():
        if not path.is_file() or not is_source(path):
            continue
        checked += 1
        rel = path.relative_to(REPO_ROOT).as_posix()
        with path.open(encoding="utf-8", errors="replace") as f:
            head = "".join(line for _, line in zip(range(HEADER_LINES), f))
        if SPDX not in head:
            failures.append(f"{rel}: no '{SPDX}' in its first {HEADER_LINES} lines")
        elif OURS not in head and ORIGIN not in head:
            failures.append(f"{rel}: neither this project's copyright line nor an 'Origin:' line")

    if failures:
        print("\ncheck-headers: FAILED\n")
        for i, f in enumerate(failures, start=1):
            print(f"  {i}. {f}")
        return 1

    print(f"  ok  {checked} source files")
    print("\ncheck-headers: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
