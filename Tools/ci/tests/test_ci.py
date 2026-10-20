"""Local checks for release packaging and native Windows debugger build coverage."""

import hashlib
import json
import os
from pathlib import Path
import plistlib
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


class AnalyticsPolicyTests(unittest.TestCase):
    def test_analytics_cannot_be_enabled_by_cached_build_options(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        policy = re.search(r"^set\(ENABLE_ANALYTICS OFF CACHE BOOL .* FORCE\)$",
                           cmake, re.MULTILINE)
        self.assertIsNotNone(policy)
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "check.cmake"
            script.write_text(policy.group() + '\nif(ENABLE_ANALYTICS)\n'
                              '  message(FATAL_ERROR "Analytics enabled")\nendif()\n')
            subprocess.run(["cmake", "-DENABLE_ANALYTICS=ON", "-P", str(script)],
                           check=True, capture_output=True, text=True)

    def test_fork_does_not_install_an_upstream_reporting_backend(self):
        source = (ROOT / "Source/Core/Core/DolphinAnalytics.cpp").read_text()
        reload_config = source.split("void DolphinAnalytics::ReloadConfig()", 1)[1]
        reload_config = reload_config.split("void DolphinAnalytics::GenerateNewIdentity()", 1)[0]
        self.assertIn("m_reporter.SetBackend(nullptr);", reload_config)
        self.assertNotIn("HttpAnalyticsBackend", source)
        self.assertNotIn("analytics.dolphin-emu.org", source)
        self.assertNotIn("GenerateNewIdentity()", reload_config)


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
        self.assertEqual(set(publish["needs"]), {"windows", "linux", "linux-packages", "macos"})
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
        for platform in ("windows",):
            steps = self.workflow["jobs"][platform]["steps"]
            artifacts = [s for s in steps if s.get("with", {}).get("name", "").startswith("release-")]
            self.assertEqual(len(artifacts), 1)
            self.assertEqual(artifacts[0]["if"], "matrix.configuration == 'Release'")
            paths = artifacts[0]["with"]["path"].strip().splitlines()
            self.assertEqual(len(paths), 2)
            self.assertTrue(all("*" not in path for path in paths))
            self.assertEqual(paths[1], paths[0] + ".sha256")

    def test_linux_package_formats_are_gated(self):
        job = self.workflow["jobs"]["linux-packages"]
        self.assertEqual(set(job["strategy"]["matrix"]["format"]),
                         {"deb", "rpm", "appimage", "gpkg", "arch", "flatpak"})
        self.assertEqual(job["needs"], "linux")
        self.assertEqual(job["strategy"]["fail-fast"], "false")
        publish = self.workflow["jobs"]["publish"]["steps"][-1]["run"]
        for extension in (".deb", ".rpm", ".AppImage", ".gpkg.tar", ".pkg.tar.zst", ".flatpak"):
            self.assertIn(extension, publish)

    def test_macos_architectures_and_dmg_gate(self):
        job = self.workflow["jobs"]["macos"]
        self.assertEqual({entry["arch"] for entry in job["strategy"]["matrix"]["include"]},
                         {"arm64", "x86_64"})
        self.assertTrue(any("unit tests" in step["name"] for step in job["steps"]))
        publish = self.workflow["jobs"]["publish"]["steps"][-1]["run"]
        self.assertIn("dolphin-dap-macos-arm64.dmg", publish)
        self.assertIn("dolphin-dap-macos-x86_64.dmg", publish)
        configure = next(step["run"] for step in job["steps"]
                         if step.get("name") == "Configure macOS")
        self.assertIn("-DENABLE_CLI_TOOL=ON", configure)
        native = (ROOT / "Tools/ci/linux-packages/build-native.sh").read_text()
        self.assertIn("-DENABLE_CLI_TOOL=ON", native)
        flatpak = (ROOT / "Tools/ci/linux-packages/prepare-flatpak.py").read_text()
        self.assertIn('"-DENABLE_CLI_TOOL=ON"', flatpak)

    def test_legacy_builds_use_supported_compilers(self):
        legacy = yaml.load((ROOT / ".github/workflows/main.yml").read_text(),
                           Loader=yaml.BaseLoader)
        windows = legacy["jobs"]["windows"]
        self.assertEqual(windows["runs-on"], "windows-2025-vs2026")
        self.assertEqual(windows["strategy"]["matrix"]["build_type"], ["Netplay", "Playback"])
        linux = legacy["jobs"]["linux"]
        self.assertEqual(linux["env"], {"CC": "gcc-12", "CXX": "g++-12"})
        install = next(step["run"] for step in linux["steps"]
                       if step.get("name") == "Install prerequisites")
        self.assertIn("gcc-12", install)
        self.assertIn("g++-12", install)

    def test_legacy_release_cannot_upload_to_composite_dap_tags(self):
        release = yaml.load((ROOT / ".github/workflows/release.yml").read_text(),
                            Loader=yaml.BaseLoader)
        job = release["jobs"]["package"]
        self.assertEqual(job["if"], "github.event.release.target_commitish == 'slippi' && "
                         "!contains(github.event.release.tag_name, '+')")
        download = next(step["with"] for step in job["steps"]
                        if step.get("name") == "Download latest artifacts")
        self.assertEqual(download["workflow"], "main.yml")
        self.assertEqual(download["branch"], "slippi")
        self.assertEqual(download["commit"], "${{ github.sha }}")


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


@unittest.skipUnless(yaml, "requires PyYAML")
class RustPackageTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "requires a C++ compiler")
    def test_vendored_vcdiff_builds_without_installing_development_files(self):
        source = (ROOT / "CMakeLists.txt").read_text()
        subdirectory = re.search(r"^add_subdirectory\(Externals/open-vcdiff[^\n]*$",
                                 source, re.MULTILINE).group()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Externals").mkdir()
            (root / "Externals/open-vcdiff").symlink_to(ROOT / "Externals/open-vcdiff")
            (root / "main.cpp").write_text("int main() { return 0; }\n")
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.20)\nproject(VendorInstall C CXX)\n' + subdirectory + '\n'
                'add_executable(consumer main.cpp)\n'
                'target_link_libraries(consumer PRIVATE vcdcom vcddec vcdenc)\n'
                'install(TARGETS consumer RUNTIME DESTINATION bin)\n')
            build = root / "build"
            stage = root / "stage"
            subprocess.run(["cmake", "-S", str(root), "-B", str(build),
                            f"-DCMAKE_INSTALL_PREFIX={stage}"], check=True, capture_output=True)
            subprocess.run(["cmake", "--build", str(build), "--parallel", "2"],
                           check=True, capture_output=True)
            subprocess.run(["cmake", "--install", str(build)], check=True, capture_output=True)
            subprocess.run([str(stage / "bin/consumer")], check=True)
            for library in ("vcdcom", "vcddec", "vcdenc"):
                self.assertTrue((build / f"Externals/open-vcdiff/lib{library}.a").is_file())
            self.assertFalse((stage / "include").exists())
            self.assertFalse((stage / "lib").exists())

    @unittest.skipUnless(shutil.which("rpmbuild") and shutil.which("rpm") and shutil.which("cc"),
                         "requires RPM tools and a C compiler")
    def test_rpm_spec_packages_the_installed_rust_library(self):
        source = (ROOT / "Tools/ci/linux-packages/build-native.sh").read_text()
        spec = source.split('cat > /work/rpmbuild/SPECS/dolphin-dap.spec <<EOF\n', 1)[1]
        spec = spec.split('\nEOF', 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stage = root / "stage"
            for filename in ("usr/bin/dolphin-emu", "usr/lib/libslippi_rust_extensions.so",
                             "usr/share/dolphin-emu/resource"):
                path = stage / filename
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("package fixture\n")
            (root / "rust.c").write_text("int rust_value(void) { return 42; }\n")
            (root / "main.c").write_text(
                "int rust_value(void); int main(void) { return rust_value() != 42; }\n")
            subprocess.run(["cc", "-shared", "-fPIC", str(root / "rust.c"), "-o",
                            str(stage / "usr/lib/libslippi_rust_extensions.so")],
                           check=True, capture_output=True)
            subprocess.run(["cc", str(root / "main.c"), f"-L{stage / 'usr/lib'}",
                            "-lslippi_rust_extensions", "-Wl,-rpath,$ORIGIN/../lib",
                            "-o", str(stage / "usr/bin/dolphin-emu")],
                           check=True, capture_output=True)
            spec_path = root / "dolphin-dap.spec"
            spec_path.write_text(spec.replace("$version", "1.0.0")
                                 .replace("/work/stage", str(stage)))
            subprocess.run(["rpmbuild", "-bb", "--define", f"_topdir {root}",
                            "--define", "debug_package %{nil}", str(spec_path)],
                           check=True, capture_output=True, text=True)
            package, = (root / "RPMS/x86_64").glob("*.rpm")
            files = subprocess.check_output(["rpm", "--dbpath", str(root / "rpmdb"),
                                             "-qpl", str(package)], text=True)
            self.assertIn("/usr/lib/libslippi_rust_extensions.so", files.splitlines())
            requires = subprocess.check_output(["rpm", "--dbpath", str(root / "rpmdb"),
                                                "-qp", "--requires", str(package)], text=True)
            provides = subprocess.check_output(["rpm", "--dbpath", str(root / "rpmdb"),
                                                "-qp", "--provides", str(package)], text=True)
            for dependency in requires.splitlines():
                if "libslippi_rust_extensions" in dependency:
                    self.assertIn(dependency, provides.splitlines())

    @unittest.skipUnless(shutil.which("cc"), "requires a C compiler")
    def test_linux_install_includes_rust_library_and_relocatable_frontends(self):
        cmake = (ROOT / "Source/Core/CMakeLists.txt").read_text()
        install = cmake.split("corrosion_import_crate(", 1)[1].split("\n", 1)[1]
        install = install.split("# slippi change: end", 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "rust.c").write_text("int rust_value(void) { return 42; }\n")
            library = root / "libslippi_rust_extensions.so"
            subprocess.run(["cc", "-shared", "-fPIC", str(root / "rust.c"), "-o", str(library)],
                           check=True, capture_output=True)
            (root / "main.c").write_text(
                "int rust_value(void); int main(void) { return rust_value() != 42; }\n")
            for libdir, frontends in (("lib", ["dolphin-emu", "dolphin-nogui", "dolphin-tool"]),
                                      ("lib64", ["dolphin-nogui"]),
                                      ("lib/x86_64-linux-gnu", ["dolphin-tool"])):
                with self.subTest(libdir=libdir):
                    (root / "CMakeLists.txt").write_text(
                        'cmake_minimum_required(VERSION 3.20)\nproject(InstallRust C)\n'
                        'include(GNUInstallDirs)\n'
                        'add_library(slippi_rust_extensions-shared SHARED IMPORTED GLOBAL)\n'
                        'set_target_properties(slippi_rust_extensions-shared PROPERTIES\n'
                        '  IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/libslippi_rust_extensions.so"\n'
                        '  IMPORTED_NO_SONAME TRUE)\n'
                        f'foreach(frontend {" ".join(frontends)})\n'
                        '  add_executable(${frontend} main.c)\n'
                        '  target_link_libraries(${frontend} PRIVATE slippi_rust_extensions-shared)\n'
                        '  install(TARGETS ${frontend} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})\n'
                        'endforeach()\n' + install)
                    build = root / libdir.replace("/", "-")
                    stage = build / "stage"
                    subprocess.run(["cmake", "-S", str(root), "-B", str(build),
                                    f"-DCMAKE_INSTALL_LIBDIR={libdir}", "-DCMAKE_INSTALL_PREFIX=/usr"],
                                   check=True, capture_output=True)
                    subprocess.run(["cmake", "--build", str(build)], check=True, capture_output=True)
                    subprocess.run(["cmake", "--install", str(build)],
                                   env=dict(os.environ, DESTDIR=str(stage)), check=True, capture_output=True)
                    self.assertEqual((stage / "usr" / libdir / library.name).read_bytes(),
                                     library.read_bytes())
                    env = dict(os.environ)
                    env.pop("LD_LIBRARY_PATH", None)
                    hidden = library.with_suffix(".hidden")
                    library.rename(hidden)
                    try:
                        for frontend in frontends:
                            subprocess.run([str(stage / "usr/bin" / frontend)], env=env,
                                           check=True, capture_output=True)
                    finally:
                        hidden.rename(library)

    def test_gentoo_configure_uses_portage_active_abi_libdir(self):
        source = (ROOT / "Tools/ci/linux-packages/build-native.sh").read_text()
        setup = source.split('case "$format" in', 1)[0]
        setup = setup[setup.index("version="):]
        gentoo = source.split("  gpkg)", 1)[1].split("    # Use generic", 1)[0]
        configure = source.split("# Each distro compiles", 1)[1]
        configure = configure[configure.index("cmake -S"):].split("cmake --build", 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for command in ("emerge-webrsync", "eselect"):
                path = root / command
                path.write_text("#!/bin/sh\nexit 0\n")
                path.chmod(0o755)
            portageq = root / "portageq"
            portageq.write_text(
                '#!/bin/sh\ncase "$*" in\n'
                '"envvar ABI") echo "$TEST_ABI";;\n'
                '"envvar LIBDIR_$TEST_ABI") echo "$TEST_LIBDIR";;\n'
                '*) exit 9;;\nesac\n')
            portageq.chmod(0o755)
            cmake = root / "cmake"
            cmake.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
            cmake.chmod(0o755)
            for abi, libdir in (("amd64", "lib64"), ("x86", "lib")):
                with self.subTest(abi=abi):
                    result = subprocess.run(
                        ["bash", "-euo", "pipefail", "-c", setup + gentoo + configure],
                        env=dict(os.environ, PATH=str(root) + os.pathsep + os.environ["PATH"],
                                 PACKAGE_VERSION="1.0", TEST_ABI=abi, TEST_LIBDIR=libdir),
                        capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn(f"-DCMAKE_INSTALL_LIBDIR={libdir}", result.stdout.splitlines())
            result = subprocess.run(
                ["bash", "-euo", "pipefail", "-c", setup + gentoo + configure],
                env=dict(os.environ, PATH=str(root) + os.pathsep + os.environ["PATH"],
                         PACKAGE_VERSION="1.0", TEST_ABI="unknown", TEST_LIBDIR=""),
                capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn("-DCMAKE_INSTALL_PREFIX", result.stdout)

    def test_flatpak_vendors_locked_crates_for_offline_sdk_build(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cargo = root / "cargo"
            cargo.write_text(
                '#!/usr/bin/env python3\nimport json, os, pathlib, sys\n'
                'pathlib.Path(os.environ["CARGO_ARGS"]).write_text(json.dumps(sys.argv[1:]))\n'
                'vendor = pathlib.Path(sys.argv[-1]); vendor.mkdir()\n'
                'print(\'[source.crates-io]\\nreplace-with = "vendored-sources"\\n\''
                '      \'[source.vendored-sources]\\ndirectory = "\' + str(vendor) + \'"\')\n')
            cargo.chmod(0o755)
            env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ["PATH"],
                       CARGO_ARGS=str(root / "args.json"))
            manifest_path = root / "manifest.yml"
            subprocess.run([shutil.which("python3"),
                            str(ROOT / "Tools/ci/linux-packages/prepare-flatpak.py"),
                            str(manifest_path)], env=env, check=True, capture_output=True)
            args = json.loads((root / "args.json").read_text())
            self.assertEqual(args[:3], ["vendor", "--locked", "--versioned-dirs"])
            manifest = yaml.safe_load(manifest_path.read_text())
            self.assertEqual(manifest["sdk-extensions"],
                             ["org.freedesktop.Sdk.Extension.rust-stable"])
            module = manifest["modules"][-1]
            self.assertIn("/usr/lib/sdk/rust-stable/bin", module["build-options"]["append-path"])
            self.assertEqual(module["build-options"]["env"]["CARGO_HOME"],
                             "/run/build/dolphin-emu/cargo-home")
            self.assertEqual({s.get("dest") for s in module["sources"][1:]},
                             {"cargo-vendor", "cargo-home"})
            config = (root / "cargo-config/config.toml").read_text()
            self.assertIn('directory = "/run/build/dolphin-emu/cargo-vendor"', config)
            self.assertIn("offline = true", config)
            self.assertNotIn(str(root), config)

    def test_native_package_bootstrap_exports_pinned_rust_before_configure(self):
        source = (ROOT / "Tools/ci/linux-packages/build-native.sh").read_text()
        toolchain = source.split("curl --proto", 1)[1].split("# Each distro", 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bindir = root / "bin"
            bindir.mkdir()
            curl = bindir / "curl"
            curl.write_text(
                '#!/bin/sh\ncat <<\'SCRIPT\'\n'
                'test "$*" = "-y --profile minimal --default-toolchain 1.88.0 --no-modify-path" || exit 7\n'
                'mkdir -p "$HOME/.cargo/bin"\n'
                'for tool in rustc cargo; do\n'
                'printf \'#!/bin/sh\\necho %s-1.88.0\\n\' "$tool" > "$HOME/.cargo/bin/$tool"\n'
                'chmod +x "$HOME/.cargo/bin/$tool"\ndone\nSCRIPT\n')
            curl.chmod(0o755)
            result = subprocess.run(["bash", "-euo", "pipefail", "-c", "curl --proto" + toolchain],
                                    env=dict(os.environ, HOME=str(root),
                                             PATH=str(bindir) + os.pathsep + os.environ["PATH"]),
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), ["rustc-1.88.0", "cargo-1.88.0"])
        self.assertLess(source.index("rustc --version"), source.index("cmake -S /source"))
        for case in ("deb|appimage)", "rpm)", "arch)", "gpkg)"):
            section = source.split(case, 1)[1].split(";;", 1)[0]
            self.assertIn("curl", section)
            self.assertIn("ca-certificates", section)


class WindowsProjectTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("cc"), "requires a C compiler")
    def test_nogui_links_zlib_through_its_portable_imported_target(self):
        cmake = (ROOT / "Source/Core/DolphinNoGUI/CMakeLists.txt").read_text()
        links = re.search(r"target_link_libraries\(dolphin-nogui\n.*?\n\)",
                          cmake, re.DOTALL).group()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "main.c").write_text(
                '#include <zlib.h>\nint main(void) { return compressBound(16) < 16; }\n')
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.20)\nproject(NoGUI C)\n'
                'find_package(ZLIB REQUIRED)\nadd_executable(dolphin-nogui main.c)\n'
                'foreach(name slippi_rust_extensions core uicommon cpp-optparse)\n'
                '  add_library(${name} INTERFACE)\nendforeach()\n' + links + '\n'
                'get_target_property(libraries dolphin-nogui LINK_LIBRARIES)\n'
                'foreach(library IN LISTS libraries)\n'
                '  if(NOT TARGET "${library}")\n'
                '    message(FATAL_ERROR "Nonportable raw library: ${library}")\n'
                '  endif()\nendforeach()\n')
            build = root / "build"
            subprocess.run(["cmake", "-S", str(root), "-B", str(build)],
                           check=True, capture_output=True, text=True)
            subprocess.run(["cmake", "--build", str(build)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(build / "dolphin-nogui")], check=True)

    def test_pipe_backend_sources_follow_the_enabled_unix_backend(self):
        cmake = (ROOT / "Source/Core/InputCommon/CMakeLists.txt").read_text()
        block = re.search(r"if\([^\n]+\)\n  target_sources\(inputcommon PRIVATE\n"
                          r"    ControllerInterface/Pipes/Pipes.cpp.*?endif\(\)",
                          cmake, re.DOTALL).group()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for unix, windows in (("FALSE", "TRUE"), ("TRUE", "FALSE")):
                script = root / "platform.cmake"
                script.write_text(
                    f"set(UNIX {unix})\nset(WIN32 {windows})\n"
                    'function(target_sources)\nfile(WRITE "${CMAKE_CURRENT_LIST_DIR}/sources" "${ARGN}")\n'
                    "endfunction()\n" + block)
                output = root / "sources"
                output.unlink(missing_ok=True)
                subprocess.run(["cmake", "-P", str(script)], check=True, capture_output=True)
                self.assertEqual(output.exists(), unix == "TRUE")
                if output.exists():
                    self.assertIn("ControllerInterface/Pipes/Pipes.cpp", output.read_text())

    @unittest.skipUnless(shutil.which("c++"), "requires a C++ preprocessor")
    def test_windows_si_translation_unit_retains_portable_tests_without_posix_pipes(self):
        source = ROOT / "Source/UnitTests/Core/SerialInterfaceTest.cpp"
        text = source.read_text()
        # Isolate this translation unit's platform guards, not dependencies' host-platform guards.
        # Deliberately provide no POSIX headers or PipeDevice declaration.
        forbidden = {"fcntl.h", "sys/ioctl.h", "unistd.h",
                     "InputCommon/ControllerInterface/Pipes/Pipes.h"}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for header in re.findall(r'^#include [<"]([^>"]+)[>"]', text, re.MULTILINE):
                if header not in forbidden:
                    path = root / header
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.touch()
            result = subprocess.run(
                ["c++", "-E", "-P", "-nostdinc", "-nostdinc++", "-D_WIN32",
                 "-I", str(root), str(source)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
        tests = set(re.findall(r"TEST_F\(SerialInterfaceTest, (\w+)\)", result.stdout))
        self.assertEqual(tests, {
            "HostUpdateDoesNotConsumeSlippiRequest", "OnSIReadDefersAndLatchesOnePoll",
            "OnSIReadDefersNewRequestBetweenChannelReads",
            "OnSIReadLeavesCoalescedMenuRequestForNextGeneration",
            "ConsolePollingBehaviorIsUnchanged", "OnSIReadDoesNotRequireBlockingPipes",
            "DefaultPollingMatchesLegacySlippi", "LeavingOnSIReadClearsDeferredPoll",
            "OnSIReadAppliesResponseErrorsAtFirstRead", "SaveStateRestoresDeferredPollAndPipeRequest",
            "SaveStateRestoresInputReadyForReadTimeRefresh",
        })
        self.assertNotRegex(result.stdout, r"\b(?:pipe|fcntl|ioctl|write|close|PipeBackedSIDevice)\s*\(")
        cmake = (ROOT / "Source/UnitTests/Core/CMakeLists.txt").read_text()
        self.assertIn("add_dolphin_test(SerialInterfaceTest SerialInterfaceTest.cpp)",
                      cmake.split("if(NOT WIN32)", 1)[0])

    @unittest.skipUnless(shutil.which("c++"), "requires a C++ compiler")
    def test_slippi_code_kinds_have_linkable_storage_in_debug_builds(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "code-kinds.cpp"
            source.write_text(
                '#include <cstdint>\n#include "Core/Slippi/SlippiDirectCodes.h"\n'
                'int main() { const auto* direct = &SlippiDirectCodes::DIRECT; '
                'const auto* teams = &SlippiDirectCodes::TEAMS; '
                'return *direct == 0 && *teams == 1 ? 0 : 1; }\n')
            executable = root / "code-kinds"
            subprocess.run(["c++", "-std=c++23", "-O0", "-I", str(ROOT / "Source/Core"),
                            str(source), "-o", str(executable)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(executable)], check=True)

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

    def test_rust_dll_copy_uses_corrosion_artifacts_for_each_configuration(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        block = cmake.split("if (WIN32)\n", 1)[1].split("\nendif()", 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            # Execute the production target against Corrosion-style staged artifacts,
            # not an invented Cargo work-directory layout.
            for configuration in ("Release", "Debug"):
                artifacts = root / configuration
                artifacts.mkdir()
                for suffix in ("dll", "dll.lib", "pdb"):
                    (artifacts / ("slippi_rust_extensions." + suffix)).write_text(configuration + suffix)
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.20)\nset(CMAKE_SYSTEM_NAME Windows)\nproject(Config NONE)\n'
                'set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Binaries")\n'
                'set(CMAKE_PDB_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/$<CONFIG>")\n'
                'add_custom_target(common)\n'
                'add_library(slippi_rust_extensions-shared SHARED IMPORTED GLOBAL)\n'
                'set_target_properties(slippi_rust_extensions-shared PROPERTIES\n'
                'IMPORTED_LOCATION_RELEASE "${CMAKE_SOURCE_DIR}/Release/slippi_rust_extensions.dll"\n'
                'IMPORTED_LOCATION_DEBUG "${CMAKE_SOURCE_DIR}/Debug/slippi_rust_extensions.dll"\n'
                'IMPORTED_IMPLIB_RELEASE "${CMAKE_SOURCE_DIR}/Release/slippi_rust_extensions.dll.lib"\n'
                'IMPORTED_IMPLIB_DEBUG "${CMAKE_SOURCE_DIR}/Debug/slippi_rust_extensions.dll.lib")\n'
                + block + '\n')
            for generator in ("Ninja", "Ninja Multi-Config"):
                build = root / generator
                subprocess.run(["cmake", "-S", str(root), "-B", str(build), "-G", generator,
                                "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CONFIGURATION_TYPES=Release;Debug"],
                               check=True, capture_output=True, text=True)
                for configuration in ("Release", "Debug"):
                    if generator == "Ninja":
                        subprocess.run(["cmake", "-S", str(root), "-B", str(build),
                                        "-DCMAKE_BUILD_TYPE=" + configuration],
                                       check=True, capture_output=True)
                    subprocess.run(["cmake", "--build", str(build), "--config", configuration],
                                   check=True, capture_output=True, text=True)
                    for suffix in ("dll", "dll.lib", "pdb"):
                        self.assertEqual((build / "Binaries" / ("slippi_rust_extensions." + suffix))
                                         .read_text(), configuration + suffix)

    def test_packaging_matches_slippi_cmake_names(self):
        qt = (ROOT / "Source/Core/DolphinQt/CMakeLists.txt").read_text()
        self.assertRegex(qt, r"OUTPUT_NAME\s+Slippi_Dolphin")
        self.assertIn('set(OUTPUT_NAME "Slippi_Dolphin")', qt)
        mac = (ROOT / "Tools/ci/package-macos.sh").read_text()
        self.assertIn('"$build/Binaries/Slippi_Dolphin.app"', mac)
        self.assertIn("for binary in Slippi_Dolphin dolphin-emu-nogui", mac)
        self.assertNotIn("DolphinQt.app", mac)


@unittest.skipUnless(os.name == "posix" and shutil.which("cmake"), "requires CMake on POSIX")
class MacBundleBuildTests(unittest.TestCase):
    def test_all_target_uses_qt_target_bundle_and_executable_names(self):
        cmake = (ROOT / "Source/Core/CMakeLists.txt").read_text()
        final_bundle = cmake[cmake.index("if (APPLE AND ENABLE_QT)"):]
        for name in ("Slippi_Dolphin", "DolphinQt"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                build = root / "build with spaces"
                binaries = build / "Binaries/Release"
                bundle = binaries / (name + ".app")
                executable = bundle / "Contents/MacOS" / name
                stale = binaries / "Dolphin.app/stale"
                stale.parent.mkdir(parents=True)
                stale.touch()
                tools = root / "tools"
                tools.mkdir()
                plutil = tools / "plutil"
                plutil.write_text(
                    "#!/usr/bin/env python3\nimport plistlib, pathlib, sys\n"
                    "assert sys.argv[1:5] == ['-replace', 'CFBundleExecutable', '-string', 'Dolphin']\n"
                    "path = pathlib.Path(sys.argv[5])\n"
                    "data = plistlib.loads(path.read_bytes())\n"
                    "data['CFBundleExecutable'] = sys.argv[4]\npath.write_bytes(plistlib.dumps(data))\n")
                plutil.chmod(0o755)
                (root / "CMakeLists.txt").write_text(
                    'cmake_minimum_required(VERSION 3.15)\nproject(Bundle C)\n'
                    # Build a real bundle with the host linker, without requiring an Apple SDK.
                    'set(CMAKE_C_LINK_EXECUTABLE "<CMAKE_C_COMPILER> <OBJECTS> -o <TARGET>")\n'
                    'set(APPLE TRUE)\nset(ENABLE_QT TRUE)\n'
                    'set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Binaries/Release")\n'
                    'add_executable(dolphin-emu main.c)\n'
                    'set_target_properties(dolphin-emu PROPERTIES MACOSX_BUNDLE TRUE '
                    'OUTPUT_NAME "' + name + '")\n' + final_bundle)
                (root / "main.c").write_text("int main(void) { return 0; }\n")
                configure = subprocess.run(["cmake", "-S", str(root), "-B", str(build),
                                            "-DCMAKE_SYSTEM_NAME=Darwin", "-DCMAKE_C_COMPILER_WORKS=TRUE"],
                                           capture_output=True, text=True)
                self.assertEqual(configure.returncode, 0, configure.stdout + configure.stderr)
                result = subprocess.run(["cmake", "--build", str(build)],
                                        env=dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"]),
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                final = binaries / "Dolphin.app"
                copied = final / "Contents/MacOS/Dolphin"
                self.assertEqual(copied.read_bytes(), executable.read_bytes())
                subprocess.run([str(copied)], check=True)
                self.assertEqual(plistlib.loads((final / "Contents/Info.plist").read_bytes())[
                    "CFBundleExecutable"], "Dolphin")
                self.assertFalse(stale.exists())
                self.assertFalse((final / "Contents/MacOS" / name).exists())
                self.assertTrue(executable.exists())


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
        for filename in ("Slippi_Dolphin.exe", "DolphinNoGUI.exe", "slippi_rust_extensions.dll", "qt.conf", "COPYING",
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
        (self.output / "Slippi_DolphinD.exe").write_text("debug binary")
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
            for filename in ("Slippi_Dolphin.exe", "DolphinNoGUI.exe", "slippi_rust_extensions.dll", "qt.conf", "COPYING",
                             "Qt6Widgets.dll", "QtPlugins/platforms/qwindows.dll",
                             "Sys/resource.txt", "Languages/de/dolphin-emu.mo", "LICENSES/test.txt",
                             "vcruntime140.dll", "msvcp140.dll"):
                self.assertIn(filename, bundle.namelist())
            self.assertNotIn("Slippi_DolphinD.exe", bundle.namelist())
        checksum = Path(str(archive) + ".sha256").read_text().split()
        self.assertEqual(checksum, [hashlib.sha256(archive.read_bytes()).hexdigest(), archive.name])

    def test_missing_qt_plugin_fails_without_archive(self):
        (self.output / "QtPlugins/platforms/qwindows.dll").unlink()
        result = self.package()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing release file", result.stderr)
        self.assertFalse((self.root / "release/dolphin-dap-windows-x64.zip").exists())

    def test_missing_slippi_rust_dll_fails_without_archive(self):
        (self.output / "slippi_rust_extensions.dll").unlink()
        result = self.package()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing release file: slippi_rust_extensions.dll", result.stderr)
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
