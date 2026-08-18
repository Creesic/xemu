#!/usr/bin/env python3
import datetime
import pathlib
import subprocess
import sys


repo = pathlib.Path(sys.argv[1])


def git(*args):
    try:
        return subprocess.check_output(
            ["git", "-C", str(repo), *args],
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


commit = git("rev-parse", "HEAD")
if not commit and (repo / "XEMU_COMMIT").exists():
    commit = (repo / "XEMU_COMMIT").read_text().strip()

version = git("describe", "--tags", "--match", "v*")
if version.startswith("v"):
    version = version[1:]
if not version and (repo / "XEMU_VERSION").exists():
    version = (repo / "XEMU_VERSION").read_text().strip()

fields = version.split("-") if version else []
numbers = fields[0].split(".") if fields else []
if len(numbers) < 3 or not all(part.isdigit() for part in numbers[:3]):
    version = "0.0.0"
    numbers = ["0", "0", "0"]
    version_commit = "0"
else:
    version_commit = fields[1] if len(fields) > 1 and fields[1].isdigit() else "0"

date = datetime.datetime.now(datetime.timezone.utc).strftime(
    "%a %b %d %H:%M:%S UTC %Y"
)
print(f'#define XEMU_VERSION       "{version}"')
print(f"#define XEMU_VERSION_MAJOR {numbers[0]}")
print(f"#define XEMU_VERSION_MINOR {numbers[1]}")
print(f"#define XEMU_VERSION_PATCH {numbers[2]}")
print(f"#define XEMU_VERSION_COMMIT {version_commit}")
print(f'#define XEMU_COMMIT        "{commit}"')
print(f'#define XEMU_DATE          "{date}"')
