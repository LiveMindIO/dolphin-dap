"""Adapt upstream's manifest without changing the upstream Flatpak identity."""
import pathlib
import sys

import yaml

root = pathlib.Path(__file__).resolve().parents[3]
manifest = yaml.safe_load((root / "Flatpak/org.DolphinEmu.dolphin-emu.yml").read_text())
manifest["app-id"] = "io.github.LiveMindIO.DolphinDAP"
# KDE 6.10 is a stable Qt6 SDK baseline; do not follow upstream's rolling SDK.
manifest["runtime-version"] = "6.10"
manifest["command"] = "dolphin-emu"
manifest["separate-locales"] = False
# Fedora-based SDKs otherwise default Meson to lib64, outside Flatpak's
# pkg-config and runtime search paths.
manifest["modules"][0]["config-opts"].append("--libdir=lib")
# The upstream host intermittently blocks CI downloads (HTTP 418). Use Debian's
# immutable source archive; documentation is excluded and disabled below.
manifest["modules"][0]["sources"] = [{
    "type": "archive",
    "url": "https://deb.debian.org/debian/pool/main/libe/libevdev/"
           "libevdev_1.13.6+dfsg.orig.tar.xz",
    "sha256": "6643222f13cfbead0e30b8b74600f6d335f6ebb1ad8cbaf85783e36886689071",
}]
module = manifest["modules"][-1]
module["config-opts"] = [
    "-DCMAKE_BUILD_TYPE=Release", "-DENABLE_QT=ON", "-DENABLE_NOGUI=ON",
    "-DENABLE_TESTS=ON", "-DENABLE_AUTOUPDATE=OFF", "-DDISTRIBUTOR=LiveMindIO",
    "-DUSE_SYSTEM_LIBS=OFF", "-DENABLE_LLVM=OFF", "-DENABLE_ALSA=OFF",
    "-DENCODE_FRAMEDUMPS=OFF",
]
module["build-options"]["env"] = {"QT_QPA_PLATFORM": "offscreen"}
module["build-commands"] = ["cmake --build . --parallel 2 --target unittests"]
module["post-install"] = [
    "install -Dm644 Flatpak/org.DolphinEmu.dolphin-emu.metainfo.xml "
    "/app/share/metainfo/io.github.LiveMindIO.DolphinDAP.metainfo.xml",
    "sed -i 's/org.DolphinEmu.dolphin-emu/io.github.LiveMindIO.DolphinDAP/g; "
    "s/Dolphin Emulator/Dolphin DAP/g; "
    "s/dolphin-emu.desktop/io.github.LiveMindIO.DolphinDAP.desktop/g' "
    "/app/share/metainfo/io.github.LiveMindIO.DolphinDAP.metainfo.xml",
]
module["sources"] = [{"type": "dir", "path": str(root),
                       "skip": [".git", "build", "Build", "release", ".flatpak-builder"]}]
pathlib.Path(sys.argv[1]).write_text(yaml.safe_dump(manifest, sort_keys=False))
