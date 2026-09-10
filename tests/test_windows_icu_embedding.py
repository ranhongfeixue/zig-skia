from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class WindowsIcuEmbeddingTests(unittest.TestCase):
    def test_assembly_embeds_icu_payload_in_coff_object(self) -> None:
        payload = bytes(range(256)) * 4 + b"lightpanda-skia-icu-payload"

        with tempfile.TemporaryDirectory() as temporary_directory:
            workdir = Path(temporary_directory)
            (workdir / "icudtl.dat").write_bytes(payload)
            output = workdir / "icudtl_data.obj"

            subprocess.run(
                [
                    "zig",
                    "cc",
                    "-target",
                    "x86_64-windows-msvc",
                    "-c",
                    ROOT / "build-tools" / "icu_data_windows.S",
                    "-o",
                    output,
                ],
                cwd=workdir,
                check=True,
                capture_output=True,
                text=True,
            )

            symbols = subprocess.run(
                [os.environ.get("NM", "nm"), "-P", output],
                check=True,
                capture_output=True,
                text=True,
            ).stdout
            self.assertRegex(symbols, r"(?m)^lp_skia_icudtl_dat R 0 ")
            self.assertRegex(
                symbols,
                rf"(?m)^lp_skia_icudtl_dat_end R {len(payload):x} ",
            )
            self.assertIn(payload, output.read_bytes())

    def test_compiler_invalidates_cached_object_when_icu_data_changes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            workdir = Path(temporary_directory)
            data_file = workdir / "icudtl.dat"
            environment = os.environ.copy()
            environment["ZIG_GLOBAL_CACHE_DIR"] = str(workdir / "global-cache")
            environment["ZIG_LOCAL_CACHE_DIR"] = str(workdir / "local-cache")

            for size in (257, 4099):
                data_file.write_bytes(bytes(index % 251 for index in range(size)))
                output = workdir / f"icudtl_data_{size}.obj"
                subprocess.run(
                    [
                        "python3",
                        ROOT / "build-tools" / "compile_icu_data.py",
                        "--zig",
                        "zig",
                        "--target",
                        "x86_64-windows-msvc",
                        "--assembly",
                        ROOT / "build-tools" / "icu_data_windows.S",
                        "--data",
                        data_file,
                        "--output",
                        output,
                    ],
                    check=True,
                    capture_output=True,
                    text=True,
                    env=environment,
                )

                symbols = subprocess.run(
                    [os.environ.get("NM", "nm"), "-P", output],
                    check=True,
                    capture_output=True,
                    text=True,
                ).stdout
                match = next(
                    line for line in symbols.splitlines()
                    if line.startswith("lp_skia_icudtl_dat_end ")
                )
                self.assertEqual(int(match.split()[2], 16), size)

    def test_skia_loader_initializes_icu_from_embedded_payload_once(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            workdir = Path(temporary_directory)
            unicode_dir = workdir / "unicode"
            unicode_dir.mkdir()
            (workdir / "SkLoadICU.h").write_text(
                "bool SkLoadICU();\n",
                encoding="utf-8",
            )
            (unicode_dir / "udata.h").write_text(
                """
#pragma once
using UErrorCode = int;
constexpr UErrorCode U_ZERO_ERROR = 0;
constexpr int UDATA_ONLY_PACKAGES = 1;
constexpr int UDATA_NO_FILES = 2;
extern "C" void udata_setCommonData(const void*, UErrorCode*);
extern "C" void udata_setFileAccess(int, UErrorCode*);
""",
                encoding="utf-8",
            )
            harness = workdir / "loader_test.cpp"
            harness.write_text(
                """
#include <cassert>
#include "SkLoadICU.h"
#include "unicode/udata.h"

extern "C" const unsigned char lp_skia_icudtl_dat[] = {0xda, 0x27};
static const void* initialized_from = nullptr;
static int initialize_count = 0;
static int file_access = 0;

extern "C" void udata_setCommonData(const void* data, UErrorCode* error) {
    initialized_from = data;
    ++initialize_count;
    *error = U_ZERO_ERROR;
}

extern "C" void udata_setFileAccess(int access, UErrorCode* error) {
    file_access = access;
    *error = U_ZERO_ERROR;
}

int main() {
    assert(SkLoadICU());
    assert(SkLoadICU());
    assert(initialized_from == lp_skia_icudtl_dat);
    assert(initialize_count == 1);
    assert(file_access == UDATA_NO_FILES);
}
""",
                encoding="utf-8",
            )
            executable = workdir / "loader_test"

            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-D_WIN32",
                    "-DSK_USING_THIRD_PARTY_ICU",
                    "-I",
                    workdir,
                    ROOT / "build-tools" / "SkLoadICU.cpp",
                    harness,
                    "-o",
                    executable,
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([executable], check=True)

    def test_windows_archive_contains_complete_embedded_icu_payload(self) -> None:
        archive_value = os.environ.get("SKIA_WINDOWS_ARCHIVE")
        data_value = os.environ.get("SKIA_ICU_DATA")
        if not archive_value or not data_value:
            self.skipTest("SKIA_WINDOWS_ARCHIVE and SKIA_ICU_DATA are required")

        archive = Path(archive_value)
        data_file = Path(data_value)
        symbols = subprocess.run(
            [os.environ.get("NM", "nm"), "-P", archive],
            check=True,
            capture_output=True,
            text=True,
        ).stdout

        current_member = ""
        records: list[tuple[str, str, str, int | None]] = []
        for line in symbols.splitlines():
            if line.endswith(":") and len(line.split()) == 1:
                current_member = line
                continue
            fields = line.split()
            if len(fields) < 2:
                continue
            value = None
            if len(fields) >= 3:
                try:
                    value = int(fields[2], 16)
                except ValueError:
                    pass
            records.append((current_member, fields[0], fields[1], value))

        starts = [
            value
            for _, name, kind, value in records
            if name == "lp_skia_icudtl_dat" and kind.upper() == "R"
        ]
        ends = [
            value
            for _, name, kind, value in records
            if name == "lp_skia_icudtl_dat_end" and kind.upper() == "R"
        ]
        loader_references = [
            member
            for member, name, kind, _ in records
            if "SkLoadICU" in member
            and name == "lp_skia_icudtl_dat"
            and kind == "U"
        ]

        self.assertEqual(len(starts), 1)
        self.assertEqual(len(ends), 1)
        self.assertEqual(ends[0] - starts[0], data_file.stat().st_size)
        # The bundle flattens several top-level Skia libraries, and more than
        # one of them may carry the same ICU loader archive member.
        self.assertGreaterEqual(len(loader_references), 1)


if __name__ == "__main__":
    unittest.main()
