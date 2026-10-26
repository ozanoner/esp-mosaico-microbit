#!/usr/bin/env python3
"""The one place this project's version lives, and the release mechanics around it.

ESP-Iris reads the application version from CMake's ``PROJECT_VERSION``. IDF copies
it into the app description, which is the ``app_version`` a device reports, and
``system_update.cmake`` passes the same value to the bundle manifest as ``release``.
So the version *is* the ``project(... VERSION x.y.z)`` line in the firmware project,
and this tool edits exactly that line. There is no second copy that can drift.

    tools/release/version.py current
    tools/release/version.py notes [<from>]            # markdown, no writes
    tools/release/version.py tag [--push]              # tag the current version
    tools/release/version.py bump minor [--tag --push] # raise it, then tag
    tools/release/version.py check-tag v1.1.0          # the release gate

The grammar is the vendor's: one to four dot-separated numbers, no leading ``v``
(``mosaico-tools/tools/mosaico_cli/app_commands.py:79``). The git tag carries the
``v``, so a release tag is always ``v`` plus the version the firmware reports.
"""

from __future__ import annotations

import argparse
import datetime as dt
import re
import subprocess
import sys
from pathlib import Path

BASE = Path(__file__).resolve().parents[2]
PROJECT = "microbit_studio"
CMAKE = BASE / "firmware" / "projects" / PROJECT / "CMakeLists.txt"
CHANGELOG = BASE / "CHANGELOG.md"

NUMERIC = re.compile(r"[0-9]+(?:\.[0-9]+){0,3}")
TAG = re.compile(r"v([0-9]+(?:\.[0-9]+){0,3})")
PROJECT_LINE = re.compile(
    r"^([ \t]*project\([ \t]*" + re.escape(PROJECT) + r"[ \t]+VERSION[ \t]+)([^)\s]+)([ \t]*\))",
    re.MULTILINE,
)

SECTIONS = (
    ("feat", "Features"),
    ("fix", "Fixes"),
    ("perf", "Performance"),
    ("refactor", "Refactoring"),
    ("docs", "Documentation"),
    ("test", "Tests"),
    ("ci", "CI"),
    ("build", "Build"),
    ("chore", "Chores"),
    ("style", "Style"),
)
CONVENTIONAL = re.compile(
    r"^(?P<type>[A-Za-z]+)(?:\((?P<scope>[^)]*)\))?(?P<breaking>!)?:[ \t]*(?P<subject>.+)$"
)
SKIP = re.compile(r"^chore\(release\):")

HEADER = """# Changelog

Generated from conventional commits by `tools/release/version.py`. Newest first.
A section heading is the release tag without the `v`, which is also the version
the firmware reports and the value ESP-Iris records as `release` in a system
update manifest.
"""


class ReleaseError(Exception):
    """A release precondition failed. Message is printed as-is, no traceback."""


def rel(path: Path) -> str:
    return str(path.relative_to(BASE))


def validate(value: str, where: str) -> str:
    if not NUMERIC.fullmatch(value):
        raise ReleaseError(
            f"{where}: '{value}' is not the numeric form ESP-Iris accepts: one to four "
            "dot-separated numbers, no leading 'v' (the tag carries the 'v')"
        )
    return value


def read_version() -> str:
    found = PROJECT_LINE.findall(CMAKE.read_text(encoding="utf-8"))
    if len(found) != 1:
        raise ReleaseError(
            f"{rel(CMAKE)}: expected exactly one 'project({PROJECT} VERSION ...)' line, "
            f"found {len(found)}"
        )
    return validate(found[0][1], rel(CMAKE))


def write_version(version: str) -> None:
    text = CMAKE.read_text(encoding="utf-8")
    updated, count = PROJECT_LINE.subn(lambda m: f"{m.group(1)}{version}{m.group(3)}", text)
    if count != 1:
        raise ReleaseError(f"{rel(CMAKE)}: expected to rewrite one version line, rewrote {count}")
    CMAKE.write_text(updated, encoding="utf-8")


def bumped(current: str, part: str) -> str:
    numbers = [int(p) for p in current.split(".")]
    numbers += [0] * (3 - len(numbers))
    major, minor, patch = numbers[:3]
    if part == "major":
        return f"{major + 1}.0.0"
    if part == "minor":
        return f"{major}.{minor + 1}.0"
    return f"{major}.{minor}.{patch + 1}"


def git(*args: str, check: bool = True, stdin: str | None = None) -> subprocess.CompletedProcess:
    proc = subprocess.run(
        ["git", "-C", str(BASE), *args],
        text=True,
        capture_output=True,
        input=stdin,
    )
    if check and proc.returncode != 0:
        raise ReleaseError(f"git {' '.join(args)} failed: {(proc.stderr or proc.stdout).strip()}")
    return proc


def last_tag() -> str:
    proc = git("describe", "--tags", "--abbrev=0", "--match", "v[0-9]*", check=False)
    return proc.stdout.strip() if proc.returncode == 0 else ""


def records(since: str) -> list[tuple[str, str]]:
    """(subject, body) for every non-merge commit since a ref."""
    spec = f"{since}..HEAD" if since else "HEAD"
    raw = git("log", "--no-merges", "--format=%s%x1f%b%x1e", spec).stdout
    out = []
    for chunk in raw.split("\x1e"):
        chunk = chunk.strip("\n")
        if not chunk:
            continue
        subject, _, body = chunk.partition("\x1f")
        out.append((subject.strip(), body.strip()))
    return out


def notes(version: str, since: str, date: str) -> str:
    breaking: list[str] = []
    grouped: dict[str, list[str]] = {name: [] for _, name in SECTIONS}
    other: list[str] = []

    for subject, body in records(since):
        if SKIP.match(subject) or subject.startswith("Merge "):
            continue
        match = CONVENTIONAL.match(subject)
        if not match:
            other.append(subject)
            continue
        scope = match.group("scope")
        text = match.group("subject").strip()
        line = f"**{scope}:** {text}" if scope else text
        is_breaking = bool(match.group("breaking")) or "BREAKING CHANGE" in body
        if is_breaking:
            breaking.append(line)
            continue
        grouped[dict(SECTIONS).get(match.group("type").lower(), "")].append(line)

    if other:
        print(
            f"warning: {len(other)} commit(s) do not follow the conventional format and "
            "are listed under Other:",
            file=sys.stderr,
        )
        for subject in other:
            print(f"  {subject}", file=sys.stderr)

    blocks = [("Breaking changes", breaking)]
    blocks += [(name, grouped[name]) for _, name in SECTIONS]
    blocks.append(("Other", other))
    parts = [f"## [{version}] - {date}"]
    if not since:
        parts.append("_First tagged release, so this covers the whole history._")
    for name, lines in blocks:
        if lines:
            parts.append("\n".join([f"### {name}"] + [f"- {line}" for line in lines]))
    if len(parts) == 1 + (0 if since else 1):
        parts.append(f"_No user-facing changes since {since}._" if since else "_No user-facing changes._")
    return "\n\n".join(parts).rstrip() + "\n"


def write_changelog(entry: str) -> None:
    if not CHANGELOG.exists():
        CHANGELOG.write_text(HEADER + "\n" + entry, encoding="utf-8")
        return
    text = CHANGELOG.read_text(encoding="utf-8")
    marker = text.find("\n## ")
    if marker == -1:
        CHANGELOG.write_text(text.rstrip() + "\n\n" + entry, encoding="utf-8")
        return
    CHANGELOG.write_text(text[: marker + 1] + entry + "\n" + text[marker + 1 :], encoding="utf-8")


def changelog_has(version: str) -> bool:
    if not CHANGELOG.exists():
        return False
    return f"## [{version}]" in CHANGELOG.read_text(encoding="utf-8")


def snapshot():
    """Restore the two files a release touches, so a failed run leaves no wreckage."""
    cmake_text = CMAKE.read_text(encoding="utf-8")
    changelog_text = CHANGELOG.read_text(encoding="utf-8") if CHANGELOG.exists() else None

    def restore() -> None:
        CMAKE.write_text(cmake_text, encoding="utf-8")
        if changelog_text is None:
            CHANGELOG.unlink(missing_ok=True)
        else:
            CHANGELOG.write_text(changelog_text, encoding="utf-8")
        git("reset", "-q", "--", rel(CMAKE), rel(CHANGELOG), check=False)

    return restore


def release(version: str, tag: str, entry: str, push: bool) -> None:
    restore = snapshot()
    write_version(version)
    write_changelog(entry)
    git("add", rel(CMAKE), rel(CHANGELOG))
    try:
        git("commit", "-m", f"chore(release): {version}")
    except ReleaseError:
        restore()
        raise
    try:
        git("tag", "-a", tag, "-F", "-", stdin=entry)
    except ReleaseError as error:
        raise ReleaseError(
            f"{error}\nThe version commit is in place; run `git tag -a {tag} -F -` with the "
            "changelog section as the message, or `git reset --hard HEAD~1` to back out"
        )
    print(f"tagged {tag} with the changelog entry as its annotation")
    if push:
        git("push", "origin", "HEAD")
        git("push", "origin", tag)
        print(f"pushed {tag}; publishing the release runs the reproducibility gate")
    else:
        print(f"next: git push origin HEAD && git push origin {tag}")


def guard_tag(tag: str) -> None:
    dirty = [line for line in git("status", "--porcelain").stdout.splitlines() if line.strip()]
    ours = {rel(CMAKE), rel(CHANGELOG)}
    stray = [line for line in dirty if line[3:].strip() not in ours]
    if stray:
        raise ReleaseError(
            "worktree has changes that are not part of a release; commit or stash them first:\n  "
            + "\n  ".join(stray)
        )
    if git("rev-parse", "-q", "--verify", f"refs/tags/{tag}", check=False).returncode == 0:
        raise ReleaseError(f"tag {tag} already exists")
    if changelog_has(tag[1:]):
        raise ReleaseError(f"{rel(CHANGELOG)} already has a section for {tag[1:]}")


def cmd_current(args: argparse.Namespace) -> int:
    print(read_version())
    return 0


def cmd_notes(args: argparse.Namespace) -> int:
    version = validate(args.version, "--version") if args.version else read_version()
    since = args.since if args.since is not None else last_tag()
    print(notes(version, since, args.date))
    return 0


def cmd_tag(args: argparse.Namespace) -> int:
    version = read_version()
    tag = f"v{version}"
    since = last_tag()
    entry = notes(version, since, args.date)
    if args.dry_run:
        print(f"would tag {tag}" + (f" (since {since})" if since else " (full history)"))
        print(entry)
        return 0
    guard_tag(tag)
    release(version, tag, entry, args.push)
    return 0


def cmd_bump(args: argparse.Namespace) -> int:
    current = read_version()
    version = bumped(current, args.part)
    tag = f"v{version}"
    since = last_tag()
    entry = notes(version, since, args.date)
    if args.dry_run:
        print(f"would bump {current} -> {version} and tag {tag}")
        print(entry)
        return 0
    if not args.tag:
        write_version(version)
        print(f"version: {current} -> {version}")
        print("next: commit the version line, then tag with `version.py tag`")
        return 0
    guard_tag(tag)
    print(f"version: {current} -> {version}")
    release(version, tag, entry, args.push)
    return 0


def cmd_check_tag(args: argparse.Namespace) -> int:
    match = TAG.fullmatch(args.tag or "")
    if not match:
        raise ReleaseError(
            f"'{args.tag}' is not a release tag: expected v<major>.<minor>.<patch>, "
            f"for example v{read_version()}"
        )
    want = match.group(1)
    got = read_version()
    if got != want:
        raise ReleaseError(
            f"tag {args.tag} names {want} but {rel(CMAKE)} says {got}. The tag and the "
            "version the firmware reports are the same number: bump the version in the "
            "commit the tag points at (tools/release/version.py bump)"
        )
    print(f"tag {args.tag} names the version the firmware reports: {got}")
    return 0


def main(argv: list[str] | None = None) -> int:
    today = dt.datetime.now(dt.timezone.utc).date().isoformat()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("current", help="print the version ESP-Iris reads").set_defaults(func=cmd_current)

    p_notes = sub.add_parser("notes", help="print the changelog section, write nothing")
    p_notes.add_argument("--since", default=None, help="ref to start after; default is the last tag")
    p_notes.add_argument("--version", default=None, help="heading version; default is the current one")
    p_notes.add_argument("--date", default=today)
    p_notes.set_defaults(func=cmd_notes)

    p_tag = sub.add_parser("tag", help="changelog and tag the current version, no bump")
    p_tag.add_argument("--push", action="store_true", help="push the commit and the tag")
    p_tag.add_argument("--dry-run", action="store_true")
    p_tag.add_argument("--date", default=today)
    p_tag.set_defaults(func=cmd_tag)

    p_bump = sub.add_parser("bump", help="raise the version, changelog it and optionally tag")
    p_bump.add_argument("part", choices=("major", "minor", "patch"))
    p_bump.add_argument("--tag", action="store_true", help="also commit and tag")
    p_bump.add_argument("--push", action="store_true", help="with --tag, push the commit and the tag")
    p_bump.add_argument("--dry-run", action="store_true")
    p_bump.add_argument("--date", default=today)
    p_bump.set_defaults(func=cmd_bump)

    p_check = sub.add_parser("check-tag", help="the release gate: tag matches the version")
    p_check.add_argument("tag")
    p_check.set_defaults(func=cmd_check_tag)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except ReleaseError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
