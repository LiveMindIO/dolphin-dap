"""Contracts for distro-native builds, isolation, and Flatpak permissions."""
from pathlib import Path
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
    def test_native_builds_compile_and_run_tests_before_packaging(self):
        script = (SCRIPTS / "build-native.sh").read_text()
        self.assertLess(script.index("--target unittests"), script.index("dpkg-deb --"))
        self.assertIn("-DENABLE_AUTOUPDATE=OFF", script)
        self.assertIn("-DDISTRIBUTOR=LiveMindIO", script)
        self.assertIn("dpkg-shlibdeps", script)
        self.assertIn("rpmbuild -bb", script)
        self.assertIn("BINPKG_FORMAT=\"gpkg\"", script)
        self.assertIn("emerge --usepkgonly", script)

    def test_container_scope_and_clean_install_check(self):
        script = (SCRIPTS / "container-build.sh").read_text()
        for image in ("ubuntu:24.04", "fedora:44", "archlinux:base-devel",
                      "gentoo/stage3:amd64-desktop-systemd"):
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
        self.assertIn("--share=network", manifest["finish-args"])
        self.assertNotIn("--filesystem=host", manifest["finish-args"])
        module = manifest["modules"][-1]
        self.assertIn("-DENABLE_NOGUI=ON", module["config-opts"])
        self.assertIn("-DENABLE_AUTOUPDATE=OFF", module["config-opts"])
        self.assertIn("--target unittests", module["build-commands"][0])
        self.assertEqual(module["sources"][0]["path"], str(ROOT))
        self.assertIn(".git", module["sources"][0]["skip"])
