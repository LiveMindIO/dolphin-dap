"""Local checks for release packaging and native Windows debugger build coverage."""

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import unittest
import zipfile

try:
    import yaml
except ImportError:
    yaml = None


ROOT = Path(__file__).resolve().parents[3]


@unittest.skipUnless(yaml, "workflow checks require PyYAML")
class WorkflowTests(unittest.TestCase):
    def setUp(self):
        self.workflow = yaml.load(
            (ROOT / ".github/workflows/build.yml").read_text(), Loader=yaml.BaseLoader
        )

    def test_triggers_and_build_test_matrix(self):
        self.assertEqual(set(self.workflow["on"]), {"push", "pull_request", "workflow_dispatch"})
        self.assertEqual(self.workflow["on"]["push"], {"branches": ["**"], "tags": ["**"]})
        for platform in ("windows", "linux"):
            job = self.workflow["jobs"][platform]
            self.assertEqual(job["strategy"]["matrix"]["configuration"], ["Release", "Debug"])
            self.assertEqual(job["strategy"]["fail-fast"], "false")
            self.assertTrue(any("unit tests" in s["name"] for s in job["steps"]))

    def test_tag_only_publication_waits_for_both_platforms(self):
        publish = self.workflow["jobs"]["publish"]
        self.assertEqual(set(publish["needs"]), {"windows", "linux"})
        self.assertEqual(publish["if"], "github.event_name == 'push' && startsWith(github.ref, 'refs/tags/')")
        self.assertEqual(publish["steps"][0]["with"]["pattern"], "release-*")
        self.assertIn("sha256sum --check", publish["steps"][1]["run"])

    def test_permissions_and_pinned_actions(self):
        self.assertEqual(self.workflow["permissions"], {"contents": "read"})
        for name, job in self.workflow["jobs"].items():
            self.assertEqual(job.get("permissions", {"contents": "read"}),
                             {"contents": "write" if name == "publish" else "read"})
            for step in job["steps"]:
                if "uses" in step:
                    self.assertRegex(step["uses"], r"@[0-9a-f]{40}$")

    def test_release_artifacts_do_not_include_staging_directories(self):
        for platform in ("windows", "linux"):
            steps = self.workflow["jobs"][platform]["steps"]
            artifacts = [s for s in steps if s.get("with", {}).get("name", "").startswith("release-")]
            self.assertEqual(len(artifacts), 1)
            self.assertEqual(artifacts[0]["if"], "matrix.configuration == 'Release'")
            paths = artifacts[0]["with"]["path"].strip().splitlines()
            self.assertEqual(len(paths), 2)
            self.assertTrue(all("*" not in path for path in paths))
            self.assertEqual(paths[1], paths[0] + ".sha256")


@unittest.skipUnless(yaml and os.name == "posix" and shutil.which("pwsh"),
                     "requires PyYAML and PowerShell on POSIX")
class WindowsConfigureTests(unittest.TestCase):
    def test_powershell_expands_cmake_configuration(self):
        workflow = yaml.load((ROOT / ".github/workflows/build.yml").read_text(),
                             Loader=yaml.BaseLoader)
        script = next(step["run"] for step in workflow["jobs"]["windows"]["steps"]
                      if step["name"] == "Configure Windows CMake")
        with tempfile.TemporaryDirectory() as directory:
            cmake = Path(directory) / "cmake"
            cmake.write_text("#!/usr/bin/python3\nimport json, sys\nprint(json.dumps(sys.argv[1:]))\n")
            cmake.chmod(0o755)
            for configuration, fastlog in (("Release", "OFF"), ("Debug", "ON")):
                env = dict(os.environ, PATH=directory + os.pathsep + os.environ["PATH"],
                           BUILD_CONFIGURATION=configuration)
                result = subprocess.run(["pwsh", "-NoProfile", "-Command", script],
                                        env=env, capture_output=True, text=True, check=True)
                args = json.loads(result.stdout)
                self.assertIn("-DCMAKE_CONFIGURATION_TYPES=" + configuration, args)
                self.assertIn("-DFASTLOG=" + fastlog, args)


class WindowsProjectTests(unittest.TestCase):
    def test_debugger_sources_match_cmake(self):
        cmake = (ROOT / "Source/Core/Core/CMakeLists.txt").read_text()
        sources = re.findall(r"^  (Debugger/\S+\.cpp)$", cmake, re.MULTILINE)
        self.assertTrue(sources)
        for source in sources:
            with self.subTest(source=source):
                self.assertTrue((ROOT / "Source/Core/Core" / source).is_file())
        workflow = (ROOT / ".github/workflows/build.yml").read_text()
        self.assertNotIn("dolphin-emu.sln", workflow)
        self.assertIn("Visual Studio 18 2026", workflow)
        self.assertIn("windows-2025-vs2026", workflow)

    def test_debugger_tests_match_cmake(self):
        cmake = (ROOT / "Source/UnitTests/Core/CMakeLists.txt").read_text()
        sources = re.findall(r"(?:Debugger|Boot|ConfigLoaders)/[\w/]+Test\.cpp", cmake)
        self.assertTrue(sources)
        for source in sources:
            with self.subTest(source=source):
                self.assertTrue((ROOT / "Source/UnitTests/Core" / source).is_file())
        self.assertIn("CommandLineParseTest.cpp",
                      (ROOT / "Source/UnitTests/UICommon/CMakeLists.txt").read_text())
        self.assertIn('-C "$<CONFIG>"',
                      (ROOT / "Source/UnitTests/CMakeLists.txt").read_text())


@unittest.skipUnless(os.name == "posix" and shutil.which("pwsh"), "requires PowerShell on POSIX")
class WindowsPackageTests(unittest.TestCase):
    """Exercise PowerShell packaging with synthetic executables and a fake VS installation."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        tools = self.root / "Tools/ci"
        tools.mkdir(parents=True)
        self.script = tools / "package-windows.ps1"
        shutil.copy(ROOT / "Tools/ci/package-windows.ps1", self.script)
        self.output = self.root / "build/Binaries"
        for filename in ("Dolphin.exe", "DolphinNoGUI.exe", "qt.conf", "COPYING",
                         "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Svg.dll",
                         "Sys/resource.txt", "Languages/de/dolphin-emu.mo", "LICENSES/test.txt",
                         "QtPlugins/platforms/qwindows.dll"):
            path = self.output / filename
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture")
        exe = self.output / "DolphinNoGUI.exe"
        exe.write_text("#!/bin/sh\necho test-version\n")
        exe.chmod(0o755)
        # A stray local Debug executable must not get into the Release ZIP.
        (self.output / "DolphinD.exe").write_text("debug binary")
        self.vs = self.root / "VS"
        crt = self.vs / "VC/Redist/MSVC/14.44.35211/x64/Microsoft.VC143.CRT"
        crt.mkdir(parents=True)
        for filename in ("vcruntime140.dll", "msvcp140.dll"):
            (crt / filename).write_text("runtime")
        self.programfiles = self.root / "programfiles"
        vswhere = self.programfiles / "Microsoft Visual Studio/Installer/vswhere.exe"
        vswhere.parent.mkdir(parents=True)
        vswhere.write_text('#!/bin/sh\nprintf "%s\\n" "$FAKE_VS_DIR"\n')
        vswhere.chmod(0o755)

    def package(self):
        env = dict(os.environ, FAKE_VS_DIR=str(self.vs))
        env["ProgramFiles(x86)"] = str(self.programfiles)
        return subprocess.run(
            ["pwsh", "-NoLogo", "-NoProfile", "-File", str(self.script)],
            env=env, text=True, capture_output=True, check=False,
        )

    def test_package_contents_and_checksum(self):
        result = self.package()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        archive = self.root / "release/dolphin-dap-windows-x64.zip"
        with zipfile.ZipFile(archive) as bundle:
            for filename in ("Dolphin.exe", "DolphinNoGUI.exe", "qt.conf", "COPYING",
                             "Qt6Widgets.dll", "QtPlugins/platforms/qwindows.dll",
                             "Sys/resource.txt", "Languages/de/dolphin-emu.mo", "LICENSES/test.txt",
                             "vcruntime140.dll", "msvcp140.dll"):
                self.assertIn(filename, bundle.namelist())
            self.assertNotIn("DolphinD.exe", bundle.namelist())
        checksum = Path(str(archive) + ".sha256").read_text().split()
        self.assertEqual(checksum, [hashlib.sha256(archive.read_bytes()).hexdigest(), archive.name])

    def test_missing_qt_plugin_fails_without_archive(self):
        (self.output / "QtPlugins/platforms/qwindows.dll").unlink()
        result = self.package()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing release file", result.stderr)
        self.assertFalse((self.root / "release/dolphin-dap-windows-x64.zip").exists())

    def test_failed_startup_fails_without_archive(self):
        (self.output / "DolphinNoGUI.exe").write_text("#!/bin/sh\nexit 1\n")
        result = self.package()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("startup check failed", result.stderr)
        self.assertFalse((self.root / "release/dolphin-dap-windows-x64.zip").exists())


@unittest.skipUnless(shutil.which("bash") and shutil.which("tar"), "requires POSIX tools")
class LinuxPackageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.build = self.root / "build"
        self.output = self.root / "release"
        tools = self.root / "Tools/ci"
        tools.mkdir(parents=True)
        for filename in ("package-linux.sh", "linux-release-README.txt"):
            shutil.copy(ROOT / "Tools/ci" / filename, tools / filename)
        self.script = tools / "package-linux.sh"
        (self.root / "Data/Sys").mkdir(parents=True)
        (self.root / "Data/Sys/resource.txt").write_text("system resource")
        (self.root / "LICENSES").mkdir()
        (self.root / "LICENSES/test.txt").write_text("license")
        (self.root / "COPYING").write_text("GPL")
        (self.build / "Binaries").mkdir(parents=True)
        for name in ("dolphin-emu", "dolphin-emu-nogui"):
            path = self.build / "Binaries" / name
            path.write_text("#!/bin/sh\necho test-version\n")
            path.chmod(0o755)
        self.translation = self.build / "Binaries/Languages/de/dolphin-emu.mo"
        self.translation.parent.mkdir(parents=True)
        self.translation.write_bytes(b"compiled translation")
        self.mockbin = self.root / "mockbin"
        self.mockbin.mkdir()
        self.ldd = self.mockbin / "ldd"
        self.ldd.write_text("#!/bin/sh\necho 'libc.so.6 => /lib/libc.so.6 (0x1234)'\n")
        self.ldd.chmod(0o755)

    def package(self):
        env = dict(os.environ, PATH=str(self.mockbin) + os.pathsep + os.environ["PATH"])
        return subprocess.run(
            ["bash", str(self.script), str(self.build), str(self.output)],
            env=env, text=True, capture_output=True, check=False,
        )

    def test_package_contents_permissions_and_checksum(self):
        result = self.package()
        self.assertEqual(result.returncode, 0, result.stderr)
        archive = self.output / "dolphin-dap-linux-x64.tar.gz"
        with tarfile.open(archive) as bundle:
            prefix = "dolphin-dap-linux-x64/"
            for name in ("dolphin-emu", "dolphin-emu-nogui", "Sys/resource.txt",
                         "Languages/de/dolphin-emu.mo", "COPYING", "LICENSES/test.txt",
                         "README.txt", "shared-libraries.txt"):
                self.assertIn(prefix + name, bundle.getnames())
            self.assertTrue(bundle.getmember(prefix + "dolphin-emu").mode & 0o111)
            self.assertNotIn(prefix + "Tests", bundle.getnames())
        checksum = Path(str(archive) + ".sha256").read_text().split()
        self.assertEqual(checksum, [hashlib.sha256(archive.read_bytes()).hexdigest(), archive.name])

    def assert_no_archive(self):
        self.assertFalse((self.output / "dolphin-dap-linux-x64.tar.gz").exists())

    def test_missing_library_fails_without_archive(self):
        self.ldd.write_text("#!/bin/sh\necho 'libmissing.so => not found'\n")
        result = self.package()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not found", result.stderr)
        self.assert_no_archive()

    def test_missing_translation_fails_without_archive(self):
        self.translation.unlink()
        result = self.package()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing compiled Qt translations", result.stderr)
        self.assert_no_archive()

    def test_missing_executable_fails_without_archive(self):
        (self.build / "Binaries/dolphin-emu").unlink()
        self.assertNotEqual(self.package().returncode, 0)
        self.assert_no_archive()

    def test_failed_startup_fails_without_archive(self):
        (self.build / "Binaries/dolphin-emu-nogui").write_text("#!/bin/sh\nexit 1\n")
        self.assertNotEqual(self.package().returncode, 0)
        self.assert_no_archive()

    def test_refuses_existing_staging_directory(self):
        self.output.mkdir()
        stage = self.output / "dolphin-dap-linux-x64"
        stage.mkdir()
        sentinel = stage / "keep"
        sentinel.write_text("do not overwrite")
        result = self.package()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sentinel.read_text(), "do not overwrite")
        self.assert_no_archive()


if __name__ == "__main__":
    unittest.main()
