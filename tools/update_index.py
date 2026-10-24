#!/usr/bin/env python3
"""Generate the firmware update channel files (update-stable.json, update-beta.json).

Input is the tree the deploy-web-flasher workflow downloads from the GitHub releases:
    <firmware-root>/<tag>/smart-grind-by-weight-<tag>.bin
    <firmware-root>/<tag>/smart-grind-by-weight-<tag>.manifest.json

Only unified releases (manifest name contains "auto-detect": one image for the V1 and V2 boards) are
offered; older single-board releases must never reach the device's pull-update flow. Versions are ordered by
SemVer 2.0. stable = highest non-prerelease, beta = highest overall. Stdlib only.

The schema and the SemVer rule are documented in docs/DESIGN-NOTES.md ("Release index"); the firmware
counterpart is src/network/release_index.cpp (keep the SemVer vectors in both test suites identical).
"""

import argparse
import datetime
import functools
import hashlib
import json
import os
import re
import sys

SCHEMA = 1
BASE_URL = "https://devnullgamer.github.io/smart-grind-by-weight/"  # == UPDATE_PULL_BASE_URL (src/config/update.h)
CHANNEL_FILES = {"stable": "update-stable.json", "beta": "update-beta.json"}
UNIFIED_MARKER = "auto-detect"
MIN_IMAGE_SIZE = 128 * 1024  # == UPDATE_IMAGE_MIN_SIZE
MAX_VERSION_LEN = 32  # == UPDATE_VERSION_MAX_LEN
MAX_STRING_LEN = 128  # == UPDATE_PULL_STRING_MAX
MAX_INDEX_BYTES = 2048  # == UPDATE_PULL_INDEX_MAX_BYTES
IMAGE_NAME = "smart-grind-by-weight-{tag}.bin"
MANIFEST_NAME = "smart-grind-by-weight-{tag}.manifest.json"

_NUMERIC = r"(?:0|[1-9][0-9]{0,8})"
_PRE_ID = r"(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)"
_BUILD_ID = r"[0-9A-Za-z-]+"
SEMVER_RE = re.compile(
    rf"({_NUMERIC})\.({_NUMERIC})\.({_NUMERIC})"
    rf"(?:-({_PRE_ID}(?:\.{_PRE_ID})*))?"
    rf"(?:\+{_BUILD_ID}(?:\.{_BUILD_ID})*)?"
)


# --- SemVer 2.0 ------------------------------------------------------------------------------------------------

def semver_parse(text):
    """Return (major, minor, patch, prerelease_identifiers_tuple) or None. Build metadata is dropped."""
    if not isinstance(text, str) or len(text) > MAX_VERSION_LEN:
        return None
    match = SEMVER_RE.fullmatch(text)  # fullmatch: "$" would accept a trailing newline
    if not match:
        return None
    major, minor, patch, pre = match.groups()
    return int(major), int(minor), int(patch), tuple(pre.split(".")) if pre is not None else ()


def _compare_identifiers(a, b):
    a_num, b_num = a.isdigit(), b.isdigit()
    if a_num != b_num:
        return -1 if a_num else 1  # numeric < alphanumeric
    if a_num:
        a, b = int(a), int(b)
    return (a > b) - (a < b)


def semver_compare(a, b):
    """-1 / 0 / 1 for two parsed versions (see semver_parse)."""
    for x, y in zip(a[:3], b[:3]):
        if x != y:
            return -1 if x < y else 1
    pre_a, pre_b = a[3], b[3]
    if bool(pre_a) != bool(pre_b):
        return 1 if not pre_a else -1  # a release outranks its own prereleases
    for x, y in zip(pre_a, pre_b):
        c = _compare_identifiers(x, y)
        if c:
            return c
    return (len(pre_a) > len(pre_b)) - (len(pre_a) < len(pre_b))  # shorter list is lower


# --- Release discovery -----------------------------------------------------------------------------------------

def tag_version(tag):
    """Parsed version for a release tag "v<semver>" without build metadata, else None (tag is skipped)."""
    if not isinstance(tag, str) or not tag.startswith("v") or "+" in tag:
        return None
    return semver_parse(tag[1:])


def manifest_is_unified(tag_dir, tag):
    try:
        with open(os.path.join(tag_dir, MANIFEST_NAME.format(tag=tag)), encoding="utf-8") as handle:
            name = json.load(handle).get("name", "")
    except (OSError, ValueError, AttributeError):
        return False
    return isinstance(name, str) and UNIFIED_MARKER in name


def file_size_and_sha256(path):
    digest = hashlib.sha256()
    size = 0
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
            size += len(chunk)
    return size, digest.hexdigest()


def read_published_tsv(path):
    """tag<TAB>ISO timestamp per line (from the GitHub releases API) -> {tag: "YYYY-MM-DD"}."""
    dates = {}
    if not path:
        return dates
    try:
        with open(path, encoding="utf-8") as handle:
            for line in handle:
                tag, _, stamp = line.rstrip("\r\n").partition("\t")
                if tag and re.match(r"^\d{4}-\d{2}-\d{2}", stamp):
                    dates[tag] = stamp[:10]
    except OSError:
        pass
    return dates


def published_date(tag, image_path, published):
    if tag in published:
        return published[tag]
    mtime = datetime.datetime.fromtimestamp(os.path.getmtime(image_path), datetime.timezone.utc)
    return mtime.strftime("%Y-%m-%d")


def limit_problem(document):
    """Why the device parser would refuse this release's index file, or None. Checked with the longest channel name."""
    for key in ("version", "tag", "url"):
        if len(document[key]) > MAX_STRING_LEN:
            return f"{key} longer than {MAX_STRING_LEN}"
    if len(document["version"]) > MAX_VERSION_LEN:
        return f"version longer than {MAX_VERSION_LEN}"
    if not re.fullmatch(r"[A-Za-z0-9._/:-]+", document["url"]) or ".." in document["url"]:
        return "url has characters the device rejects"
    raw = json.dumps(dict(channel_document("stable", None), **document), separators=(",", ":")) + chr(10)
    if len(raw.encode("utf-8")) >= MAX_INDEX_BYTES:
        return f"index file of {MAX_INDEX_BYTES} bytes or more"
    return None


def release_entry(firmware_root, tag, base_url, published):
    """Index entry for one tag directory, or None (with a note on stderr) if it does not qualify."""
    version = tag_version(tag)
    if version is None:
        print(f"skip {tag}: not a v<semver> tag", file=sys.stderr)
        return None
    tag_dir = os.path.join(firmware_root, tag)
    if not manifest_is_unified(tag_dir, tag):
        print(f"skip {tag}: not a unified (auto-detect) release", file=sys.stderr)
        return None
    image_path = os.path.join(tag_dir, IMAGE_NAME.format(tag=tag))
    if not os.path.isfile(image_path):
        print(f"skip {tag}: image missing", file=sys.stderr)
        return None
    size, sha256 = file_size_and_sha256(image_path)
    if size < MIN_IMAGE_SIZE:
        print(f"skip {tag}: image too small ({size} bytes)", file=sys.stderr)
        return None
    entry = {
        "version": version,
        "tag": tag,
        "document": {
            "version": tag[1:],
            "tag": tag,
            "prerelease": bool(version[3]),
            "published": published_date(tag, image_path, published),
            "size": size,
            "sha256": sha256,
            "url": f"{base_url}firmware/{tag}/{IMAGE_NAME.format(tag=tag)}",
        },
    }
    problem = limit_problem(entry["document"])
    if problem:
        print(f"skip {tag}: {problem} (device parser limit)", file=sys.stderr)
        return None
    return entry


def collect_releases(firmware_root, base_url=BASE_URL, published=None):
    """All qualifying releases, highest version first."""
    published = published or {}
    entries = []
    if os.path.isdir(firmware_root):
        for tag in sorted(os.listdir(firmware_root)):
            if not os.path.isdir(os.path.join(firmware_root, tag)):
                continue
            entry = release_entry(firmware_root, tag, base_url, published)
            if entry:
                entries.append(entry)
    entries.sort(key=functools.cmp_to_key(lambda a, b: semver_compare(b["version"], a["version"])))
    return entries


# --- Channels --------------------------------------------------------------------------------------------------

def pick_channel(releases, channel):
    """Highest release of the channel (releases are sorted high to low), or None."""
    for entry in releases:
        if channel == "beta" or not entry["version"][3]:
            return entry
    return None


def channel_document(channel, entry):
    if entry is None:
        return {"schema": SCHEMA, "channel": channel, "none": True}
    document = {"schema": SCHEMA, "channel": channel}
    document.update(entry["document"])
    return document


def write_channel_files(output_dir, releases):
    os.makedirs(output_dir, exist_ok=True)
    written = []
    for channel, file_name in CHANNEL_FILES.items():
        document = channel_document(channel, pick_channel(releases, channel))
        path = os.path.join(output_dir, file_name)
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(json.dumps(document, separators=(",", ":")) + "\n")
        written.append(path)
    return written


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--firmware-root", required=True, help="directory holding one sub-directory per release tag")
    parser.add_argument("--output-dir", required=True, help="where update-stable.json / update-beta.json are written")
    parser.add_argument("--base-url", default=BASE_URL, help="public base URL of the Pages site (ends with /)")
    parser.add_argument("--published-tsv", help="optional file of '<tag><TAB><ISO timestamp>' lines")
    args = parser.parse_args(argv)

    base_url = args.base_url if args.base_url.endswith("/") else args.base_url + "/"
    releases = collect_releases(args.firmware_root, base_url, read_published_tsv(args.published_tsv))
    for path in write_channel_files(args.output_dir, releases):
        print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
