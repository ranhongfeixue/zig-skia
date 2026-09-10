#!/usr/bin/env python3

"""Compile Skia's ICU package into a cache-safe Windows COFF object."""

from argparse import ArgumentParser
from hashlib import sha256
from pathlib import Path
import subprocess


def main() -> int:
    parser = ArgumentParser()
    parser.add_argument("--zig", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--assembly", type=Path, required=True)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()

    assembly = arguments.assembly.resolve()
    data = arguments.data.resolve()
    output = arguments.output.resolve()
    if data.name != "icudtl.dat":
        parser.error(f"ICU data file must be named icudtl.dat: {data}")

    # Zig's C compiler does not include .incbin inputs in its cache key. Put
    # the data digest in the generated assembly source so changing the package
    # can never reuse an object compiled from stale ICU bytes.
    digest = sha256(data.read_bytes()).hexdigest()
    generated_assembly = output.with_suffix(".S")
    output.parent.mkdir(parents=True, exist_ok=True)
    generated_assembly.write_text(
        f"// ICU data SHA256: {digest}\n" + assembly.read_text(encoding="utf-8"),
        encoding="utf-8",
    )

    subprocess.run(
        [
            arguments.zig,
            "cc",
            "-target",
            arguments.target,
            "-c",
            generated_assembly,
            "-o",
            output,
        ],
        cwd=data.parent,
        check=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
