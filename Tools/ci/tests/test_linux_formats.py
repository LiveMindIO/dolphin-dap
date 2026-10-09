"""Contracts for distro-native builds, isolation, and Flatpak permissions."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

try:
    import yaml
except ImportError:
    yaml = None

ROOT = Path(__file__).resolve().parents[3]
SCRIPTS = ROOT / "Tools/ci/linux-packages"


class NativeFormatTests(unittest.TestCase):
    def test_moltenvk_fetch_retries_and_records_only_success(self):
        for succeeds in (True, False):
            with self.subTest(succeeds=succeeds), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source = root / "source with spaces"
                source.mkdir()
                stamp = root / "stamp"
                stamp.mkdir()
                commands = root / "bin"
                commands.mkdir()
                sleep = commands / "sleep"
                sleep.write_text('#!/bin/sh\nexit 0\n')
                sleep.chmod(0o755)
                fetch = source / "fetchDependencies"
                fetch.write_text('#!/bin/sh\n'
                                 'count=$(cat "$(dirname "$0")/count" 2>/dev/null || echo 0)\n'
                                 'count=$((count + 1))\n'
                                 'echo "$count" > "$(dirname "$0")/count"\n' +
                                 ('[ "$count" = 2 ]\n' if succeeds else 'exit 1\n'))
                fetch.chmod(0o755)
                env = dict(os.environ, PATH=str(commands) + os.pathsep + os.environ["PATH"])
                result = subprocess.run(["bash", str(ROOT / "Externals/MoltenVK/configure.sh"),
                                         str(stamp), str(source), "v1.2.8"],
                                        env=env, capture_output=True)
                self.assertEqual(result.returncode, 0 if succeeds else 1)
                marker = stamp / "MoltenVK-last-version.txt"
                self.assertEqual(marker.exists(), succeeds)
                self.assertEqual((source / "count").read_text().strip(), "2" if succeeds else "3")
                if succeeds:
                    subprocess.run(["bash", str(ROOT / "Externals/MoltenVK/configure.sh"),
                                    str(stamp), str(source), "v1.2.8"], env=env, check=True)
                    self.assertEqual((source / "count").read_text().strip(), "2")

    def test_appimage_launcher_resolves_apprun_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binaries = root / "usr/bin"
            binaries.mkdir(parents=True)
            launcher = binaries / "dolphin-dap-launcher"
            shutil.copy(SCRIPTS / "appimage-launcher", launcher)
            launcher.chmod(0o755)
            (root / "AppRun").symlink_to(launcher.relative_to(root))
            for binary in ("dolphin-emu", "dolphin-emu-nogui"):
                path = binaries / binary
                path.write_text('#!/bin/sh\necho "' + binary + ' $*"\n')
                path.chmod(0o755)
            for args, expected in ((["--version"], "dolphin-emu --version"),
                                   (["--nogui", "--version"], "dolphin-emu-nogui --version")):
                result = subprocess.run([str(root / "AppRun"), *args],
                                        capture_output=True, text=True, check=True)
                self.assertEqual(result.stdout.strip(), expected)

    def test_installed_smoke_uses_supported_tool_help(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "bin").mkdir()
            resource = root / "share/dolphin-emu/Sys/GC/font_western.bin"
            resource.parent.mkdir(parents=True)
            resource.write_bytes(b"font")
            translation = root / "share/locale/de/LC_MESSAGES/dolphin-emu.mo"
            translation.parent.mkdir(parents=True)
            translation.write_bytes(b"translation")
            for binary in ("dolphin-emu", "dolphin-emu-nogui", "dolphin-tool", "ldd"):
                path = root / "bin" / binary
                if binary == "dolphin-tool":
                    path.write_text('#!/bin/sh\n[ "$*" = "convert --help" ]\n')
                else:
                    path.write_text('#!/bin/sh\necho fixture\n')
                path.chmod(0o755)
            script = (SCRIPTS / "smoke-binaries.sh").read_text()
            script = script.replace("/usr/bin/", str(root / "bin") + "/")
            script = script.replace("/usr/share/", str(root / "share") + "/")
            env = dict(os.environ, PATH=str(root / "bin") + os.pathsep + os.environ["PATH"])
            subprocess.run(["bash", "-c", script], env=env, check=True, capture_output=True)

    def test_native_builds_compile_and_run_tests_before_packaging(self):
        script = (SCRIPTS / "build-native.sh").read_text()
        self.assertLess(script.index("--target unittests"), script.index("dpkg-deb --"))
        self.assertIn("-DENABLE_AUTOUPDATE=OFF", script)
        self.assertIn("-DDISTRIBUTOR=LiveMindIO", script)
        self.assertIn("dpkg-shlibdeps", script)
        self.assertIn("rpmbuild -bb", script)
        self.assertIn("BINPKG_FORMAT=\"gpkg\"", script)
        self.assertIn("emerge --usepkgonly", script)
        self.assertIn("src_prepare() { default; }", (SCRIPTS / "dolphin-dap.ebuild").read_text())
        self.assertLess(script.index('" manifest'), script.index("FEATURES='buildpkg"))
        self.assertLess(script.index("path-include=/usr/share/locale/*"), script.index("dpkg -i"))

    def test_container_scope_and_clean_install_check(self):
        script = (SCRIPTS / "container-build.sh").read_text()
        for image in ("ubuntu:24.04", "fedora:44", "archlinux:base-devel",
                      "gentoo/stage3:amd64-systemd"):
            self.assertIn(image, script)
        self.assertIn("target=/source,readonly", script)
        self.assertIn("smoke-installed.sh", script)
        self.assertNotIn("--privileged", script)

    def test_shell_syntax(self):
        for script in [*SCRIPTS.glob("*.sh"), SCRIPTS / "PKGBUILD",
                       SCRIPTS / "dolphin-dap.ebuild", SCRIPTS / "appimage-launcher"]:
            with self.subTest(script=script.name):
                subprocess.run(["bash", "-n", str(script)], check=True)


@unittest.skipUnless(yaml, "requires PyYAML")
class FlatpakManifestTests(unittest.TestCase):
    def test_fork_identity_tests_and_sandbox(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "manifest.yml"
            subprocess.run(["python3", str(SCRIPTS / "prepare-flatpak.py"), str(output)],
                           check=True)
            manifest = yaml.safe_load(output.read_text())
        self.assertEqual(manifest["app-id"], "io.github.LiveMindIO.DolphinDAP")
        self.assertEqual(manifest["runtime-version"], "6.10")
        self.assertFalse(manifest["separate-locales"])
        self.assertIn("--libdir=lib", manifest["modules"][0]["config-opts"])
        self.assertTrue(manifest["modules"][0]["sources"][0]["url"].startswith("https://deb.debian.org/"))
        self.assertIn("--share=network", manifest["finish-args"])
        self.assertNotIn("--filesystem=host", manifest["finish-args"])
        module = manifest["modules"][-1]
        self.assertIn("-DENABLE_NOGUI=ON", module["config-opts"])
        self.assertIn("-DENABLE_AUTOUPDATE=OFF", module["config-opts"])
        self.assertIn("--target unittests", module["build-commands"][0])
        self.assertEqual(module["sources"][0]["path"], str(ROOT))
        self.assertIn(".git", module["sources"][0]["skip"])
