#!/usr/bin/env python3
"""Generate the wiki's Keyboard-Reference page from the in-app help text.

The shortcut list exists twice: once in `g_help_text` (platform/ui_modals.cpp),
which ships inside the binary and is therefore always right, and once in the
GitHub wiki, which is hand-edited and drifts. It drifted badly enough once
already -- the changelog advertised E and C for Smart Eraser and Clone Stamp
for several releases while neither key was actually bound.

So the wiki page is generated from the help text rather than maintained
alongside it. Everything that is genuinely editorial -- the context-sensitive
keys table, the closing notes -- lives in doc/wiki/ as hand-written fragments
that are pasted around the generated tables. Only the middle is mechanical.

Usage:
    python tools/wiki/gen_keyboard_reference.py --out wiki/Keyboard-Reference.md
    python tools/wiki/gen_keyboard_reference.py --check   # CI: fail if stale
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
HELP_SOURCE = REPO_ROOT / "platform" / "ui_modals.cpp"

# Fragments live beside this script rather than under doc/, which .gitignore
# excludes wholesale -- putting them there would have shipped a page with no
# prose the first time CI ran it.
HEADER_FRAGMENT = Path(__file__).with_name("Keyboard-Reference.header.md")
FOOTER_FRAGMENT = Path(__file__).with_name("Keyboard-Reference.footer.md")

SECTION_TITLE = "KEYBOARD REFERENCE"
DIVIDER = "=" * 8  # the help text separates sections with a long '=' rule

# An entry line is indented exactly two spaces. Continuation lines are indented
# far deeper (the description column), which is what distinguishes them -- the
# key column is simply absent rather than empty.
ENTRY_RE = re.compile(r"^ {2}(\S.*)$")
CONTINUATION_RE = re.compile(r"^ {3,}(\S.*)$")
GROUP_RE = re.compile(r"^(\S.*):$")

# The help text is a fixed-column console layout: descriptions begin at column
# DESC_COLUMN, and keys that outgrow their column simply push the description
# right. Splitting naively on whitespace does not work in either direction --
# "Alt+L  / Alt+S" contains a two-space run *inside* the key, while
# "Alt+PgUp / Alt+PgDn  Move ..." separates key from description with only two.
#
# So: split at the first whitespace run of two or more that ENDS at or past the
# description column. Runs earlier than that are internal to the key. Anything
# that never reaches the column falls back to a three-space rule.
DESC_COLUMN = 23
GAP_RE = re.compile(r"\s{2,}")
KEY_SPLIT_FALLBACK_RE = re.compile(r"\s{3,}")


def split_entry(line: str) -> tuple[str, str]:
    """Split a raw entry line into (key, description)."""
    for gap in GAP_RE.finditer(line):
        if gap.end() >= DESC_COLUMN:
            return line[: gap.start()].strip(), line[gap.end() :].strip()

    parts = KEY_SPLIT_FALLBACK_RE.split(line.strip(), maxsplit=1)
    key = parts[0].strip()
    desc = parts[1].strip() if len(parts) > 1 else ""
    return key, desc


class Entry:
    def __init__(self, key: str, desc: str) -> None:
        self.key = key
        self.desc = desc

    def extend(self, more: str) -> None:
        self.desc = f"{self.desc} {more}".strip()

    def normalized_desc(self) -> str:
        # The help text pads descriptions to line up in a fixed-width console;
        # those runs are layout, not content.
        return re.sub(r"\s+", " ", self.desc).strip()


class Group:
    def __init__(self, title: str) -> None:
        self.title = title
        self.entries: list[Entry] = []


def extract_section(source: str) -> list[str]:
    """Return the raw lines of the KEYBOARD REFERENCE block."""
    lines = source.splitlines()
    try:
        start = next(i for i, ln in enumerate(lines) if ln.strip() == SECTION_TITLE)
    except StopIteration:
        raise SystemExit(
            f"error: '{SECTION_TITLE}' not found in {HELP_SOURCE.name}. "
            "The help text was restructured; update this generator."
        )

    # Skip the title and its '---' underline.
    body_start = start + 1
    while body_start < len(lines) and set(lines[body_start].strip()) <= {"-"}:
        body_start += 1

    end = body_start
    while end < len(lines) and not lines[end].startswith(DIVIDER):
        end += 1
    return lines[body_start:end]


def parse(lines: list[str]) -> list[Group]:
    groups: list[Group] = []
    current: Group | None = None

    for raw in lines:
        if not raw.strip():
            continue

        group_match = GROUP_RE.match(raw)
        if group_match:
            current = Group(group_match.group(1))
            groups.append(current)
            continue

        if current is None:
            continue

        if ENTRY_RE.match(raw):
            # Pass the raw line: column offsets are measured from its start,
            # including the two-space indent.
            key, desc = split_entry(raw)
            current.entries.append(Entry(key, desc))
            continue

        cont_match = CONTINUATION_RE.match(raw)
        if cont_match and current.entries:
            current.entries[-1].extend(cont_match.group(1).strip())

    return [g for g in groups if g.entries]


def looks_like_key(token: str) -> bool:
    """True for things a user presses, false for UI controls.

    The Timeline section of the help text lists controls -- Hold, Auto Anipts,
    World Marked -- in the same column as real keys. Rendering "World Marked"
    as a code span makes it look like something you can type. A token is a key
    if it is a single word or carries a modifier; multi-word labels are prose.
    """
    if "+" in token:
        return True
    return not re.search(r"\s", token.strip())


def format_key(key: str) -> str:
    """Render a key cell as code spans, one per alternative."""
    # Parenthetical placeholders like "(no shortcut)" are prose, not keys.
    if key.startswith("("):
        return key

    # The help text already code-quotes some keys with backticks; strip them so
    # they do not nest inside the span this function adds.
    cleaned = key.replace("`", "")
    alternatives = [a.strip() for a in re.split(r"\s+/\s+", cleaned) if a.strip()]
    if not alternatives:
        return cleaned
    if not all(looks_like_key(alt) for alt in alternatives):
        return cleaned
    return " / ".join(f"`{alt}`" for alt in alternatives)


def escape_cell(text: str) -> str:
    return text.replace("|", r"\|")


def render(groups: list[Group]) -> str:
    out: list[str] = []
    for group in groups:
        out.append(f"## {group.title}")
        out.append("")
        out.append("| Key | Action |")
        out.append("|---|---|")
        for entry in group.entries:
            out.append(
                f"| {escape_cell(format_key(entry.key))} "
                f"| {escape_cell(entry.normalized_desc())} |"
            )
        out.append("")
    return "\n".join(out)


def read_fragment(path: Path) -> str:
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8").rstrip() + "\n"


def validate(groups: list[Group]) -> None:
    """Refuse to emit a table that parsed badly.

    Keys are separated from descriptions by three or more spaces. When an entry
    grows long enough to leave only two, the split lands in the wrong place and
    the row silently turns to garbage -- the description is swallowed into the
    key and the Action cell comes out empty. That is precisely the kind of quiet
    drift this generator exists to stop, so it is a hard error rather than a
    warning nobody reads.
    """
    # An empty Action cell is the obvious symptom. The subtler one is a key that
    # swallowed its description and pushed the real text into a continuation
    # line, which leaves both cells populated but nonsense -- so cap the key
    # length too. No genuine shortcut runs anywhere near this long.
    max_key_len = 28
    broken = [
        (g.title, e.key)
        for g in groups
        for e in g.entries
        if not e.normalized_desc() or len(e.key) > max_key_len
    ]
    if broken:
        lines = "\n".join(f"  [{title}] {key!r}" for title, key in broken)
        raise SystemExit(
            "error: these help-text entries parsed with an empty description,\n"
            "which means the key column ran into the description column.\n"
            "Separate them with at least three spaces in "
            f"{HELP_SOURCE.name}:\n{lines}"
        )


def build_page() -> str:
    groups = parse(extract_section(HELP_SOURCE.read_text(encoding="utf-8")))
    if not groups:
        raise SystemExit("error: parsed zero shortcut groups; refusing to write an empty page.")
    validate(groups)

    parts = [
        "<!-- Generated from platform/ui_modals.cpp (g_help_text) by",
        "     tools/wiki/gen_keyboard_reference.py. Edits to the tables below",
        "     will be overwritten. Change the help text instead; the surrounding",
        "     prose lives in tools/wiki/Keyboard-Reference.{header,footer}.md. -->",
        "",
        "# Keyboard Reference",
        "",
    ]

    header = read_fragment(HEADER_FRAGMENT)
    if header:
        parts.extend([header, ""])

    parts.append(render(groups))

    footer = read_fragment(FOOTER_FRAGMENT)
    if footer:
        parts.extend(["---", "", footer])

    return "\n".join(parts).rstrip() + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, help="path to write Keyboard-Reference.md")
    ap.add_argument(
        "--check",
        action="store_true",
        help="exit 1 if --out differs from freshly generated content",
    )
    args = ap.parse_args()

    page = build_page()

    if not args.out:
        # The fragments contain non-cp1252 characters, so a bare write to a
        # Windows console dies on encoding rather than on anything real.
        try:
            sys.stdout.reconfigure(encoding="utf-8")
        except (AttributeError, OSError):
            pass
        sys.stdout.write(page)
        return 0

    if args.check:
        existing = args.out.read_text(encoding="utf-8") if args.out.exists() else ""
        if existing != page:
            print(f"{args.out} is out of date; regenerate it.", file=sys.stderr)
            return 1
        print(f"{args.out} is up to date.")
        return 0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(page, encoding="utf-8", newline="\n")
    groups = parse(extract_section(HELP_SOURCE.read_text(encoding="utf-8")))
    total = sum(len(g.entries) for g in groups)
    print(f"wrote {args.out} ({len(groups)} groups, {total} shortcuts)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
