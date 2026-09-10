#!/usr/bin/env python3

"""Merge all top-level Skia COFF libraries and extra objects into one library."""

from pathlib import Path
import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 4:
        print(
            "usage: bundle_windows.py <ninja-out-dir> <destination.lib> "
            "<librarian> [object ...]",
            file=sys.stderr,
        )
        return 2

    out_dir = Path(sys.argv[1]).resolve()
    destination = Path(sys.argv[2]).resolve()
    librarian = Path(sys.argv[3])
    objects = [Path(value).resolve() for value in sys.argv[4:]]
    archives = sorted(
        archive
        for archive in out_dir.glob("*.lib")
        if archive.resolve() != destination
    )
    if not archives:
        print(f"no .lib archives found in {out_dir}", file=sys.stderr)
        return 1
    for object_file in objects:
        if not object_file.is_file():
            print(f"object file does not exist: {object_file}", file=sys.stderr)
            return 1

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.unlink(missing_ok=True)

    command = [str(librarian)]
    if librarian.name.lower() in {"lld-link", "lld-link.exe"}:
        command.append("/lib")
    command.extend(["/nologo", f"/out:{destination}"])
    command.extend(str(archive) for archive in archives)
    command.extend(str(object_file) for object_file in objects)
    subprocess.run(command, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
