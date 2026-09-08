from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

import yaml


ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = ROOT / ".github" / "workflows"


def load_workflow(name: str) -> dict:
    path = WORKFLOWS / name
    if not path.is_file():
        raise AssertionError(f"missing workflow: {path.relative_to(ROOT)}")
    # BaseLoader follows YAML 1.2 closely enough for GitHub's `on` key and
    # keeps scalar values as strings; these tests only need mappings/lists.
    with path.open(encoding="utf-8") as workflow_file:
        document = yaml.load(workflow_file, Loader=yaml.BaseLoader)
    if not isinstance(document, dict):
        raise AssertionError(f"workflow is not a mapping: {path.relative_to(ROOT)}")
    return document


class ReleaseWorkflowTests(unittest.TestCase):
    def test_windows_branch_builds_on_a_windows_runner(self) -> None:
        workflow = load_workflow("build-windows.yml")

        self.assertEqual(
            workflow["on"]["push"]["branches"],
            ["zig-skia/windows-msvc"],
        )
        self.assertIn("workflow_dispatch", workflow["on"])
        self.assertEqual(workflow["permissions"], {"contents": "read"})

        job = workflow["jobs"]["build-x86_64-windows"]
        self.assertEqual(job["uses"], "./.github/workflows/_prebuild-skia.yml")
        self.assertEqual(job["with"]["runner"], "windows-2022")
        self.assertEqual(job["with"]["target"], "x86_64-windows-msvc")

    def test_reusable_workflow_packages_windows_archive_as_a_file(self) -> None:
        workflow = load_workflow("_prebuild-skia.yml")
        steps = workflow["jobs"]["build"]["steps"]
        package = next(step for step in steps if step.get("name") == "Package Skia archive")
        script = package["run"]

        self.assertIn("libskia_bundle.lib", script)
        self.assertIn("llvm-lib", script)
        self.assertIn("libc_skia_", script)
        self.assertIn(".a", script)

    def test_windows_archive_listing_preserves_llvm_lib_option_in_git_bash(self) -> None:
        workflow = load_workflow("_prebuild-skia.yml")
        steps = workflow["jobs"]["build"]["steps"]
        package = next(step for step in steps if step.get("name") == "Package Skia archive")

        self.assertRegex(
            package["run"],
            re.compile(r"(?m)^\s*MSYS2_ARG_CONV_EXCL=/list\s+llvm-lib\.exe\s+/list\b"),
            "Git Bash must not rewrite llvm-lib's /list option as a filesystem path",
        )

    def test_entry_workflow_dispatches_exact_supported_targets(self) -> None:
        workflow = load_workflow("build-release.yml")

        self.assertEqual(
            workflow["on"]["push"]["tags"],
            ["**"],
            "tag pushes must build release assets",
        )
        self.assertIn("workflow_dispatch", workflow["on"])

        build_jobs = {
            name: job
            for name, job in workflow["jobs"].items()
            if job.get("uses") == "./.github/workflows/_prebuild-skia.yml"
        }
        actual_jobs = {
            name: {
                "uses": job["uses"],
                "runner": job["with"]["runner"],
                "target": job["with"]["target"],
            }
            for name, job in build_jobs.items()
        }
        self.assertEqual(
            actual_jobs,
            {
                "build-x86_64-linux": {
                    "uses": "./.github/workflows/_prebuild-skia.yml",
                    "runner": "ubuntu-22.04",
                    "target": "x86_64-linux",
                },
                "build-aarch64-linux": {
                    "uses": "./.github/workflows/_prebuild-skia.yml",
                    "runner": "ubuntu-22.04-arm",
                    "target": "aarch64-linux",
                },
                "build-aarch64-macos": {
                    "uses": "./.github/workflows/_prebuild-skia.yml",
                    "runner": "macos-26",
                    "target": "aarch64-macos",
                },
                "build-x86_64-macos": {
                    "uses": "./.github/workflows/_prebuild-skia.yml",
                    "runner": "macos-26-intel",
                    "target": "x86_64-macos",
                },
                "build-x86_64-windows": {
                    "uses": "./.github/workflows/_prebuild-skia.yml",
                    "runner": "windows-2022",
                    "target": "x86_64-windows-msvc",
                },
            },
        )

    def test_reusable_workflow_builds_and_checks_archive(self) -> None:
        workflow = load_workflow("_prebuild-skia.yml")

        inputs = workflow["on"]["workflow_call"]["inputs"]
        self.assertEqual(set(inputs), {"runner", "target"})
        self.assertEqual(inputs["runner"]["required"], "true")
        self.assertEqual(inputs["target"]["required"], "true")

        build_job = workflow["jobs"]["build"]
        self.assertEqual(build_job["runs-on"], "${{ inputs.runner }}")
        self.assertEqual(workflow["permissions"], {"contents": "read"})
        steps = {step.get("name", step.get("uses")): step for step in build_job["steps"]}

        build_script = steps["Build Skia"]["run"]
        self.assertIn('-Dtarget="${{ inputs.target }}"', build_script)
        self.assertIn("-Doptimize=ReleaseSafe", build_script)
        self.assertIn("build-skia", build_script)

        package_script = steps["Package Skia archive"]["run"]
        self.assertIn("test -s", package_script)
        self.assertIn("ar -t", package_script)
        self.assertIn(".skia-cache/skia/out/canvas/bundle/libskia_bundle.a", package_script)

    def test_release_archive_name_uses_git_tag(self) -> None:
        workflow = load_workflow("_prebuild-skia.yml")
        steps = workflow["jobs"]["build"]["steps"]
        package_step = next(step for step in steps if step.get("name") == "Package Skia archive")

        with tempfile.TemporaryDirectory() as temporary_directory:
            workdir = Path(temporary_directory)
            source = workdir / ".skia-cache/skia/out/canvas/bundle/libskia_bundle.a"
            source.parent.mkdir(parents=True)
            member = workdir / "member.txt"
            member.write_text("archive member\n", encoding="utf-8")
            subprocess.run(["ar", "rcs", source, member], check=True)

            environment = os.environ.copy()
            environment.update(
                {
                    "ARCH": "x86_64",
                    "GITHUB_REF_NAME": "v0.1.2",
                    "OS": "linux",
                    "SKIA_REVISION": "7219df0fb0ff64f26adad448f94e8c001b964e6a",
                }
            )
            subprocess.run(
                ["bash", "-c", package_step["run"]],
                cwd=workdir,
                env=environment,
                check=True,
                capture_output=True,
                text=True,
            )

            self.assertTrue((workdir / "libc_skia_v0.1.2_linux_x86_64.a").is_file())
            self.assertEqual(
                list(workdir.glob("libc_skia_7219df0fb0ff64f26adad448f94e8c001b964e6a_*.a")),
                [],
            )

    def test_reusable_workflow_pins_the_supported_zig_version(self) -> None:
        workflow = load_workflow("_prebuild-skia.yml")
        steps = workflow["jobs"]["build"]["steps"]

        setup_zig = next(
            step for step in steps if step.get("uses", "").startswith("mlugg/setup-zig@")
        )
        self.assertEqual(
            setup_zig.get("with", {}).get("version"),
            "0.16.0",
            "using setup-zig's moving default can break the Zig build API",
        )

    def test_every_build_keeps_one_downloadable_artifact(self) -> None:
        workflow = load_workflow("_prebuild-skia.yml")
        steps = workflow["jobs"]["build"]["steps"]

        artifact_steps = [
            step for step in steps if step.get("uses", "").startswith("actions/upload-artifact@")
        ]
        self.assertEqual(len(artifact_steps), 1)
        self.assertEqual(artifact_steps[0]["with"]["path"], "libc_skia_*.a")
        self.assertNotIn("if", artifact_steps[0], "manual runs must retain downloadable output")

        release_steps = [
            step for step in steps if step.get("uses", "").startswith("ncipollo/release-action@")
        ]
        self.assertEqual(release_steps, [], "parallel build jobs must not mutate releases")

    def test_tag_publisher_waits_for_and_validates_all_five_builds(self) -> None:
        workflow = load_workflow("build-release.yml")
        self.assertIn(
            "publish-release",
            workflow["jobs"],
            "tag releases must be published once after all native builds finish",
        )
        publisher = workflow["jobs"]["publish-release"]

        expected_builds = {
            "build-x86_64-linux",
            "build-aarch64-linux",
            "build-aarch64-macos",
            "build-x86_64-macos",
            "build-x86_64-windows",
        }
        self.assertEqual(set(publisher["needs"]), expected_builds)
        self.assertEqual(publisher["if"], "startsWith(github.ref, 'refs/tags/')")
        self.assertEqual(publisher["permissions"], {"actions": "read", "contents": "write"})
        self.assertEqual(publisher["concurrency"]["group"], "release-${{ github.ref }}")
        self.assertEqual(publisher["concurrency"]["cancel-in-progress"], "false")
        self.assertEqual(workflow["permissions"], {"contents": "read"})

        steps = publisher["steps"]
        download = next(
            step for step in steps if step.get("uses", "").startswith("actions/download-artifact@")
        )
        self.assertEqual(download["with"]["pattern"], "skia-*")
        self.assertEqual(download["with"]["path"], "release-assets")
        self.assertEqual(download["with"]["merge-multiple"], "true")

        verify_script = next(step["run"] for step in steps if step.get("name") == "Verify release archives")
        self.assertIn('"${#archives[@]}" -ne 5', verify_script)
        self.assertIn("windows_x86_64", verify_script)
        self.assertIn("ar -t", verify_script)

        release_state_steps = [step for step in steps if step.get("id") == "release-state"]
        self.assertEqual(len(release_state_steps), 1, "publisher must inspect existing release state")
        release_state = release_state_steps[0]
        self.assertIn("gh release view", release_state["run"])
        self.assertIn("--json isDraft", release_state["run"])
        self.assertEqual(release_state["env"]["GH_TOKEN"], "${{ github.token }}")

        update_steps = [step for step in steps if step.get("name") == "Update existing release assets"]
        self.assertEqual(len(update_steps), 1)
        update = update_steps[0]
        self.assertEqual(update["if"], "steps.release-state.outputs.exists == 'true'")
        self.assertIn("gh release upload", update["run"])
        self.assertIn("--clobber", update["run"])
        self.assertEqual(update["env"]["GH_TOKEN"], "${{ github.token }}")

        recover_steps = [step for step in steps if step.get("name") == "Publish recovered draft"]
        self.assertEqual(len(recover_steps), 1)
        recover_draft = recover_steps[0]
        self.assertEqual(recover_draft["if"], "steps.release-state.outputs.draft == 'true'")
        self.assertIn("gh release edit", recover_draft["run"])
        self.assertIn("--draft=false", recover_draft["run"])

        release = next(
            step for step in steps if step.get("uses", "").startswith("ncipollo/release-action@")
        )
        self.assertEqual(release["if"], "steps.release-state.outputs.exists == 'false'")
        self.assertEqual(release["with"]["artifacts"], "release-assets/libc_skia_*.a")
        self.assertNotIn("allowUpdates", release["with"])
        self.assertEqual(release["with"]["artifactErrorsFailBuild"], "true")
        self.assertEqual(release["with"]["immutableCreate"], "true")

    def test_remote_actions_are_pinned_to_immutable_commits(self) -> None:
        remote_uses: list[str] = []
        for name in ("_prebuild-skia.yml", "build-release.yml"):
            workflow = load_workflow(name)
            for job in workflow["jobs"].values():
                if uses := job.get("uses"):
                    if not uses.startswith("./"):
                        remote_uses.append(uses)
                for step in job.get("steps", []):
                    if uses := step.get("uses"):
                        if not uses.startswith("./"):
                            remote_uses.append(uses)

        self.assertGreater(len(remote_uses), 0)
        for uses in remote_uses:
            with self.subTest(uses=uses):
                self.assertRegex(uses, re.compile(r"^[^@]+@[0-9a-f]{40}$"))

    def test_linux_llvm_installer_is_checksum_verified(self) -> None:
        workflow = load_workflow("_prebuild-skia.yml")
        steps = workflow["jobs"]["build"]["steps"]
        install_script = next(
            step["run"] for step in steps if step.get("name") == "Install dependencies (Linux)"
        )

        self.assertRegex(install_script, re.compile(r"[0-9a-f]{64}  llvm\.sh"))
        self.assertIn("sha256sum -c -", install_script)


if __name__ == "__main__":
    unittest.main()
