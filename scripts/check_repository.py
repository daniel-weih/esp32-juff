#!/usr/bin/env python3
"""Check publishable working-tree files and staged content without printing secrets."""

import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys


REQUIRED = ("LICENSE", "README.md", "CONTRIBUTING.md", "SECURITY.md",
            "host/.env.example", "host/package-lock.json")
ARTIFACT_DIRS = {"node_modules", "build", "managed_components", "backups", "dist",
                 "tmp", ".tools", ".venv", "__pycache__", "DerivedData", "coverage"}
ARTIFACT_SUFFIXES = {".bin", ".elf", ".o", ".a", ".zip", ".pyc", ".pyo"}
RULES = {
    "machine-specific absolute path": re.compile(
        r"/(?:Users|home)/[A-Za-z0-9_.-]+/|[A-Za-z]:\\Users\\[^\\\s]+\\"),
    "API credential": re.compile(
        r"sk-[A-Za-z0-9_.-]{16,}|(?:gh[pousr]|github_pat)_[A-Za-z0-9_]{20,}"
        r"|xox[baprs]-[A-Za-z0-9-]{15,}|AIza[A-Za-z0-9_-]{30,}|AKIA[0-9A-Z]{16}"),
    "private key": re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH |DSA |ENCRYPTED )?PRIVATE KEY-----"),
    "JWT credential": re.compile(r"eyJ[A-Za-z0-9_-]{12,}\.[A-Za-z0-9_-]{12,}\.[A-Za-z0-9_-]{12,}"),
    "credential in URL": re.compile(r"https?://[^\s/@:]+:[^\s/@]+@"),
    "hardware-derived device ID": re.compile(r"\besp32s3-[0-9a-f]{12}\b", re.IGNORECASE),
}


def git(root, *arguments):
    return subprocess.check_output(["git", *arguments], cwd=root, stderr=subprocess.PIPE)


def forbidden_path(name):
    path = PurePosixPath(name)
    if any(part in ARTIFACT_DIRS for part in path.parts) or path.suffix in ARTIFACT_SUFFIXES:
        return "generated artifact or private backup"
    if (path.name in {"sdkconfig", "sdkconfig.old", ".DS_Store"}
            or (path.name.startswith(".env") and path.name != ".env.example")
            or path.suffix in {".key", ".pem"}):
        return "private or machine-local configuration"
    return None


def content_issues(name, data, source):
    # Diagnostics contain locations and categories only, never matching values.
    location = json.dumps(name, ensure_ascii=False)
    issues = []
    for number, line in enumerate(data.decode("utf-8", errors="replace").splitlines(), 1):
        for category, pattern in RULES.items():
            if pattern.search(line):
                issues.append(f"{location}:{number} ({source}): {category}")
    return issues


def staged_blobs(root, entries):
    """Read unique staged blobs in one Git process, including deleted working files."""
    hashes = sorted({oid for _, _, oid in entries})
    if not hashes:
        return {}
    output = subprocess.check_output(["git", "cat-file", "--batch"], cwd=root,
                                     input=("\n".join(hashes) + "\n").encode(),
                                     stderr=subprocess.PIPE)
    position = 0
    blobs = {}
    for oid in hashes:
        end = output.index(b"\n", position)
        header = output[position:end].split()
        if len(header) != 3 or header[1] != b"blob":
            raise ValueError("cannot inspect a staged Git object")
        size = int(header[2])
        position = end + 1
        blobs[oid] = output[position:position + size]
        position += size + 1
    return blobs


def check(root):
    root = root.resolve()
    git(root, "rev-parse", "--is-inside-work-tree")
    candidates = sorted(set(git(root, "ls-files", "--cached", "--others",
                                "--exclude-standard", "-z").decode().split("\0")) - {""})
    entries = []
    issues = []
    for record in git(root, "ls-files", "--stage", "-z").split(b"\0"):
        if not record:
            continue
        metadata, name = record.split(b"\t", 1)
        mode, oid, stage = metadata.decode().split()
        name = name.decode()
        if stage != "0":
            issues.append(f"{json.dumps(name)}: unresolved merge conflict")
        if mode != "160000":  # A submodule commit is not a file blob.
            entries.append((name, mode, oid))

    for name in REQUIRED:
        if not (root / name).is_file():
            issues.append(f"missing {name}")

    working = {}
    for name in candidates:
        category = forbidden_path(name)
        if category:
            issues.append(f"{json.dumps(name)}: {category}")
            continue
        path = root / name
        if path.is_symlink():
            # Inspect the link itself; never follow it into private host files.
            data = str(path.readlink()).encode()
            if not path.resolve().is_relative_to(root):
                issues.append(f"{json.dumps(name)}: symlink points outside the repository")
        elif path.is_file():
            if not path.resolve().is_relative_to(root):
                issues.append(f"{json.dumps(name)}: parent symlink points outside the repository")
                continue
            data = path.read_bytes()
        else:
            continue
        working[name] = data
        issues.extend(content_issues(name, data, "working tree"))
        if path.suffix == ".sh" and not path.is_symlink():
            result = subprocess.run(["sh", "-n", str(path)], capture_output=True)
            if result.returncode:
                issues.append(f"{json.dumps(name)}: invalid shell syntax")

    safe_entries = [entry for entry in entries if not forbidden_path(entry[0])]
    blobs = staged_blobs(root, safe_entries)
    for name, mode, oid in safe_entries:
        data = blobs[oid]
        if data != working.get(name):
            issues.extend(content_issues(name, data, "index"))
        # Identical bytes can represent different file types in the index and
        # working tree. Validate the indexed type independently of content deduplication.
        if mode == "120000":
            target = (root / name).parent / data.decode(errors="replace")
            if not target.resolve().is_relative_to(root):
                issues.append(f"{json.dumps(name)} (index): symlink points outside the repository")
        elif name.endswith(".sh"):
            result = subprocess.run(["sh", "-n"], input=data, capture_output=True)
            if result.returncode:
                issues.append(f"{json.dumps(name)} (index): invalid shell syntax")
    return list(dict.fromkeys(issues)), len(working), len(entries)


def main():
    try:
        issues, working_count, staged_count = check(Path(__file__).resolve().parents[1])
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        # An inspection failure must not masquerade as a clean scan.
        print(f"FAIL  repository inspection failed ({type(error).__name__})", file=sys.stderr)
        return 1
    for issue in issues:
        print(f"FAIL  {issue}", file=sys.stderr)
    if issues:
        print(f"\n{len(issues)} repository check(s) failed.\n", file=sys.stderr)
        return 1
    print(f"Repository hygiene checks passed ({working_count} working-tree files, "
          f"{staged_count} indexed files).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
