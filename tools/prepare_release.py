#!/usr/bin/env python3
"""Prepare a BT Guard release. Does not publish, commit, tag or push.

Usage:
    python3 tools/prepare_release.py --check
    python3 tools/prepare_release.py minor --dry-run
    python3 tools/prepare_release.py minor        # or patch | major | 1.4.0

Release notes live in CHANGELOG.md. Add a line under "## [Unreleased]" for
each user-visible change as you make it; this script turns those lines into
the release notes.

Modes:
  --check     Report the package.json version, the latest v* git tag, the
              branch, whether the working tree is clean, and the number of
              unreleased entries. Changes nothing.
  --dry-run   Show the new version and the release notes that would be used.
              Changes nothing.
  (neither)   Prepare the release, as described below.

What a real run does:
  1. Checks the git tree is clean (override with --allow-dirty) and that the
     new version is greater than the latest v* git tag. The tags are the
     record of what has been published. If there are no tags yet,
     package.json's current version is the baseline.
  2. Requires entries under "## [Unreleased]" in CHANGELOG.md, then rolls
     them into a dated "## [X.Y.Z]" section, leaving an empty Unreleased
     section behind.
  3. Sets "version" in package.json (only that line is changed).
  4. Runs `pebble clean` and `pebble build` (skip with --no-build), copies the
     .pbw to dist/ and checks that it reports the new version.
  5. Writes plain-text release notes to dist/release-notes-X.Y.Z.txt, for the
     store's release notes field.
  6. Prints the release notes and a checklist, including the git commit and
     tag commands.

If the build fails, package.json and CHANGELOG.md are restored.
Version notes:
  - The new version is computed from package.json's current version. If that
    is already ahead of the latest tag, pass an explicit X.Y.Z to use it as is.
  - A change that only alters the patch number from the baseline gets a
    warning: it isn't verified that the store treats it as a newer version.

Tagging is required. After publishing, tag the release with the command the
checklist prints. The next run compares against the latest v* tag to know
what has been published. If the first release was published before you had
any tags, tag its commit once so there is a baseline.
"""

import argparse
import datetime
import json
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = ROOT / "package.json"
CHANGELOG = ROOT / "CHANGELOG.md"
BUILD = ROOT / "build"
DIST = ROOT / "dist"

APP_NAME = "BT Guard"
FILE_STEM = "bt-guard"

PKG_VERSION_RE = re.compile(r'^(\s*"version"\s*:\s*")([^"]+)(")', re.M)
UNRELEASED_RE = re.compile(r"^## \[?Unreleased\]?[ \t]*$", re.M)
NEXT_HEADING_RE = re.compile(r"^## ", re.M)
COMMENT_RE = re.compile(r"<!--.*?-->", re.S)
EXPLICIT_RE = re.compile(r"^\d+\.\d+\.\d+$")


class ReleaseError(Exception):
    pass


# ---------------------------------------------------------------- versions

def parse_version(text):
    m = re.match(r"^v?(\d+)\.(\d+)(?:\.(\d+))?$", text.strip())
    if not m:
        return None
    return (int(m.group(1)), int(m.group(2)), int(m.group(3) or 0))


def fmt(v):
    return "%d.%d.%d" % v


def bump(current, kind):
    major, minor, patch = current
    if kind == "major":
        return (major + 1, 0, 0)
    if kind == "minor":
        return (major, minor + 1, 0)
    return (major, minor, patch + 1)


def package_version():
    matches = PKG_VERSION_RE.findall(PACKAGE.read_text(encoding="utf-8"))
    if len(matches) != 1:
        raise ReleaseError('expected exactly one top-level "version" in package.json')
    v = parse_version(matches[0][1])
    if v is None:
        raise ReleaseError("can't parse package.json version %r" % matches[0][1])
    return v


def set_package_version(text, version):
    new, count = PKG_VERSION_RE.subn(r"\g<1>%s\g<3>" % version, text)
    if count != 1:
        raise ReleaseError('could not update "version" in package.json')
    json.loads(new)  # still valid JSON
    return new


# --------------------------------------------------------------------- git

def git(*args):
    p = subprocess.run(["git", *args], cwd=ROOT, text=True, capture_output=True)
    if p.returncode != 0:
        raise ReleaseError("git %s failed: %s" % (" ".join(args), p.stderr.strip()))
    return p.stdout


def latest_tag():
    """Return (version_tuple, tag_name) for the highest v* tag, or None."""
    found = []
    for name in git("tag", "--list", "v*").split():
        v = parse_version(name)
        if v:
            found.append((v, name))
    return max(found) if found else None


# --------------------------------------------------------------- changelog

def split_changelog(text):
    m = UNRELEASED_RE.search(text)
    if not m:
        raise ReleaseError("CHANGELOG.md has no '## [Unreleased]' heading")
    nxt = NEXT_HEADING_RE.search(text, m.end())
    end = nxt.start() if nxt else len(text)
    return text[:m.start()], text[m.end():end], text[end:]


def unreleased_body(text):
    _, body, _ = split_changelog(text)
    return COMMENT_RE.sub("", body).strip()


def rolled_changelog(text, version, today):
    before, body, after = split_changelog(text)
    body = COMMENT_RE.sub("", body).strip()
    out = "%s## [Unreleased]\n\n## [%s] - %s\n\n%s\n" % (before, version, today, body)
    if after.strip():
        out += "\n" + after
    return out


def plain_notes(body):
    """Strip markdown to plain lines suitable for the store's notes field."""
    lines = []
    for line in COMMENT_RE.sub("", body).splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        s = re.sub(r"^[-*+]\s+", "- ", s)
        s = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", s)
        s = re.sub(r"\*\*|__|`", "", s)
        lines.append(s)
    return "\n".join(lines)


# ------------------------------------------------------------------- build

def build():
    for cmd in (["pebble", "clean"], ["pebble", "build"]):
        print("$ " + " ".join(cmd))
        if subprocess.run(cmd, cwd=ROOT).returncode != 0:
            raise ReleaseError("'%s' failed" % " ".join(cmd))
    pbws = sorted(BUILD.glob("*.pbw"))
    if len(pbws) != 1:
        raise ReleaseError("expected one .pbw in build/, found %d" % len(pbws))
    return pbws[0]


def pbw_version_label(pbw):
    try:
        with zipfile.ZipFile(pbw) as z:
            if "appinfo.json" in z.namelist():
                return json.loads(z.read("appinfo.json")).get("versionLabel")
    except (OSError, zipfile.BadZipFile, ValueError):
        pass
    return None


# -------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bump", nargs="?",
                    help="patch | minor | major | explicit X.Y.Z")
    ap.add_argument("--check", action="store_true",
                    help="report release state and exit")
    ap.add_argument("--dry-run", action="store_true",
                    help="show what would happen; change nothing")
    ap.add_argument("--allow-dirty", action="store_true",
                    help="don't require a clean git working tree")
    ap.add_argument("--no-build", action="store_true",
                    help="skip pebble build and the .pbw checks")
    args = ap.parse_args()
    if not args.check and not args.bump:
        ap.error("give patch, minor, major or an explicit X.Y.Z (or use --check)")

    current = package_version()
    tag = latest_tag()
    baseline = tag[0] if tag else current
    branch = git("rev-parse", "--abbrev-ref", "HEAD").strip()
    dirty = bool(git("status", "--porcelain").strip())
    changelog_text = CHANGELOG.read_text(encoding="utf-8") if CHANGELOG.exists() else None
    if changelog_text is None:
        raise ReleaseError("CHANGELOG.md not found")
    body = unreleased_body(changelog_text)

    if args.check:
        print("package.json version : %s" % fmt(current))
        print("latest v* git tag    : %s" % (tag[1] if tag else "(none)"))
        print("branch               : %s" % branch)
        print("working tree         : %s" % ("DIRTY" if dirty else "clean"))
        notes = plain_notes(body)
        n = len(notes.splitlines()) if notes else 0
        print("unreleased entries   : %d" % n)
        if tag and current > tag[0]:
            print("note: package.json is ahead of the last tag (unreleased bump?)")
        if not tag:
            print("note: no v* tags yet, so the published baseline is unknown")
        return 0

    # Work out the new version.
    if args.bump in ("patch", "minor", "major"):
        new = bump(current, args.bump)
        if tag and current > tag[0]:
            print("note: package.json (%s) is already ahead of tag %s; bumping from it. "
                  "Pass an explicit version to use %s as is." %
                  (fmt(current), tag[1], fmt(current)))
    elif EXPLICIT_RE.match(args.bump):
        new = parse_version(args.bump)
    else:
        raise ReleaseError("bump must be patch, minor, major or X.Y.Z")
    version = fmt(new)

    if new <= baseline:
        raise ReleaseError("new version %s is not greater than %s (%s)" % (
            version, fmt(baseline), "latest tag " + tag[1] if tag else "package.json"))
    if dirty and not args.allow_dirty:
        raise ReleaseError("git working tree is not clean; commit or stash first "
                           "(or use --allow-dirty)")
    if not body:
        raise ReleaseError("nothing under '## [Unreleased]' in CHANGELOG.md; "
                           "add release notes first")
    notes = plain_notes(body)
    if not notes:
        raise ReleaseError("Unreleased section has no usable note lines")
    if not args.no_build and not shutil.which("pebble"):
        raise ReleaseError("`pebble` not found on PATH")

    today = datetime.date.today().isoformat()
    print("Releasing %s %s (was %s; baseline %s) on branch %s" %
          (APP_NAME, version, fmt(current), fmt(baseline), branch))
    if new[:2] == baseline[:2]:
        print("warning: patch-only change from the baseline. It isn't verified that the "
              "store treats this as a newer version; check after publishing.")
    if args.dry_run:
        print("\nRelease notes that would be used:\n")
        print(notes)
        print("\n(dry run: nothing changed)")
        return 0

    orig_pkg = PACKAGE.read_text(encoding="utf-8")
    orig_log = changelog_text
    try:
        PACKAGE.write_text(set_package_version(orig_pkg, version), encoding="utf-8")
        CHANGELOG.write_text(rolled_changelog(orig_log, version, today), encoding="utf-8")
        pbw = None
        if not args.no_build:
            pbw = build()
    except BaseException:
        PACKAGE.write_text(orig_pkg, encoding="utf-8")
        CHANGELOG.write_text(orig_log, encoding="utf-8")
        print("\nRestored package.json and CHANGELOG.md.", file=sys.stderr)
        raise

    DIST.mkdir(exist_ok=True)
    notes_file = DIST / ("release-notes-%s.txt" % version)
    notes_file.write_text(notes + "\n", encoding="utf-8")
    out_pbw = None
    if pbw:
        out_pbw = DIST / ("%s-%s.pbw" % (FILE_STEM, version))
        shutil.copyfile(pbw, out_pbw)
        label = pbw_version_label(out_pbw)
        if label is None:
            print("warning: couldn't read a version from the .pbw; check it by hand")
        elif label != version:
            print("warning: .pbw reports version %r, expected %r" % (label, version))

    print("\n%s %s prepared." % (APP_NAME, version))
    if out_pbw:
        print("  package: %s (%d bytes)" % (out_pbw.relative_to(ROOT), out_pbw.stat().st_size))
    print("  notes  : %s" % notes_file.relative_to(ROOT))
    print("\nRelease notes:\n")
    print(notes)
    print("""
Checklist:
  [ ] Review the changes:   git diff
  [ ] Commit the release:
        git add package.json CHANGELOG.md
        git commit -m "Release %(v)s"
  [ ] Update store screenshots if the UI changed
  [ ] Update description/category/icons on the dashboard if they changed
  [ ] Publish dist/%(stem)s-%(v)s.pbw with the release notes above
  [ ] Once published, TAG THE RELEASE. This is required: the next run of this
      script compares against the latest v* tag to know what has been published.
        git tag -a v%(v)s -m "%(app)s %(v)s"
  [ ] Optionally push:      git push && git push origin v%(v)s""" % {
        "v": version, "stem": FILE_STEM, "app": APP_NAME})
    if not tag:
        print("\nNote: there are no v* tags yet. Tag the commit of the already-published\n"
              "release too, so future checks have a baseline, for example:\n"
              "        git tag -a v%s -m \"%s %s\" <commit>" % (fmt(current), APP_NAME, fmt(current)))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ReleaseError, FileNotFoundError) as e:
        print("error: %s" % e, file=sys.stderr)
        sys.exit(1)
