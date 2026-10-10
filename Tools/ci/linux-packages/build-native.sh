#!/usr/bin/env bash
# Runs INSIDE the selected distribution container, never directly on the host.
set -euo pipefail
format=${1:?deb, rpm, arch, gpkg, or appimage}
export DEBIAN_FRONTEND=noninteractive QT_QPA_PLATFORM=offscreen
jobs=${BUILD_JOBS:-2}
version=${PACKAGE_VERSION:?numeric package version required}
case "$format" in
  deb|appimage)
    # Minimal Ubuntu images exclude locale files even during package installation.
    printf 'path-include=/usr/share/locale/*\n' > /etc/dpkg/dpkg.cfg.d/zz-dolphin-dap-test
    apt-get update
    apt-get install -y --no-install-recommends build-essential cmake ninja-build pkg-config \
      gettext qt6-base-dev qt6-base-private-dev libqt6svg6-dev libevdev-dev libudev-dev \
      libxrandr-dev libxi-dev libbluetooth-dev libasound2-dev libpulse-dev \
      libgl1-mesa-dev libegl1-mesa-dev libusb-1.0-0-dev dpkg-dev curl ca-certificates file git
    ;;
  rpm)
    dnf install -y gcc-c++ cmake ninja-build pkgconf-pkg-config gettext qt6-qtbase-devel \
      qt6-qtbase-private-devel \
      qt6-qtsvg-devel libevdev-devel systemd-devel libXrandr-devel libXi-devel bluez-libs-devel \
      alsa-lib-devel pulseaudio-libs-devel mesa-libGL-devel mesa-libEGL-devel libusb1-devel \
      rpm-build rpmdevtools git
    ;;
  arch)
    pacman -Syu --noconfirm --needed base-devel cmake ninja pkgconf gettext qt6-base qt6-svg \
      libevdev systemd libxrandr libxi bluez-libs alsa-lib libpulse mesa libusb git
    ;;
  gpkg)
    emerge-webrsync
    # Desktop defaults keep Qt/ALSA/PulseAudio USE dependencies consistent, while
    # the minimal image avoids preinstalled desktop packages from an older tree.
    eselect profile set default/linux/amd64/23.0/desktop/systemd
    # Use generic x86-64 code rather than optimizations for the ephemeral CI host.
    cat >> /etc/portage/make.conf <<'EOF'
CFLAGS="-O2 -pipe -march=x86-64 -mtune=generic"
CXXFLAGS="${CFLAGS}"
BINPKG_FORMAT="gpkg"
USE="X gui widgets opengl vulkan"
EOF
    export MAKEOPTS="-j$jobs"
    emerge --getbinpkg --usepkg --autounmask=n dev-vcs/git dev-build/cmake dev-build/ninja dev-util/pkgconf \
      sys-devel/gettext dev-qt/qtbase:6 dev-qt/qtsvg:6 dev-libs/libevdev virtual/libudev \
      x11-libs/libXrandr x11-libs/libXi net-wireless/bluez media-libs/alsa-lib \
      media-libs/libpulse media-libs/mesa dev-libs/libusb
    ;;
  *) echo "Unsupported format: $format" >&2; exit 2 ;;
esac
git config --global --add safe.directory /source

# Each distro compiles its own binaries. Do not use the Ubuntu CI tarball here.
cmake -S /source -B /work/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
  -DENABLE_QT=ON -DENABLE_NOGUI=ON -DENABLE_TESTS=ON -DENABLE_AUTOUPDATE=OFF \
  -DDISTRIBUTOR=LiveMindIO -DUSE_SYSTEM_LIBS=OFF -DENABLE_LLVM=OFF \
  -DENCODE_FRAMEDUMPS=OFF
cmake --build /work/build --parallel "$jobs"
cmake --build /work/build --parallel "$jobs" --target unittests
DESTDIR=/work/stage cmake --install /work/build
mkdir -p /release
case "$format" in
  deb)
    mkdir -p /work/debian /work/stage/DEBIAN
    cat > /work/debian/control <<EOF
Source: dolphin-dap
Section: games
Priority: optional
Maintainer: LiveMindIO <noreply@github.com>

Package: dolphin-dap
Architecture: amd64
Description: Dolphin emulator with source debugging and DAP
EOF
    cd /work
    dependencies=$(dpkg-shlibdeps -O -e/work/stage/usr/bin/dolphin-emu \
      -e/work/stage/usr/bin/dolphin-emu-nogui -e/work/stage/usr/bin/dolphin-tool)
    dependencies=${dependencies#shlibs:Depends=}
    cat > /work/stage/DEBIAN/control <<EOF
Package: dolphin-dap
Version: $version
Architecture: amd64
Section: games
Priority: optional
Maintainer: LiveMindIO <noreply@github.com>
Depends: $dependencies, qt6-qpa-plugins
Conflicts: dolphin-emu
Provides: dolphin-emu
Description: Dolphin emulator with source debugging and DAP
 Includes the Qt debugger and NoGUI DAP frontend. Built for Ubuntu 24.04.
EOF
    dpkg-deb --root-owner-group --build /work/stage /release/dolphin-dap-ubuntu24.04-amd64.deb
    dpkg -i /release/*.deb
    ;;
  rpm)
    mkdir -p /work/rpmbuild/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
    cat > /work/rpmbuild/SPECS/dolphin-dap.spec <<EOF
Name: dolphin-dap
Version: $version
Release: 1%{?dist}
Summary: Dolphin emulator with source debugging and DAP
License: GPL-2.0-or-later
URL: https://github.com/LiveMindIO/dolphin-dap
Conflicts: dolphin-emu
Provides: dolphin-emu
%description
Includes the Qt debugger and NoGUI DAP frontend.
%install
cp -a /work/stage/. %{buildroot}/
%files
/usr/bin/*
/usr/share/*
EOF
    rpmbuild -bb --define '_topdir /work/rpmbuild' --define 'debug_package %{nil}' \
      /work/rpmbuild/SPECS/dolphin-dap.spec
    cp /work/rpmbuild/RPMS/x86_64/*.rpm /release/dolphin-dap-fedora44-x86_64.rpm
    dnf install -y /release/*.rpm
    ;;
  arch)
    useradd -m builder
    mkdir -p /work/arch
    cp /source/Tools/ci/linux-packages/PKGBUILD /work/arch/
    chown -R builder:builder /work/arch /work/stage
    cd /work/arch
    runuser -u builder -- env PACKAGE_VERSION="$version" makepkg --nodeps --noconfirm
    cp ./*.pkg.tar.zst /release/dolphin-dap-archlinux-x86_64.pkg.tar.zst
    pacman -U --noconfirm /release/*.pkg.tar.zst
    ;;
  gpkg)
    mkdir -p /var/db/repos/dolphin-dap/{metadata,profiles,games-emulation/dolphin-dap}
    echo dolphin-dap > /var/db/repos/dolphin-dap/profiles/repo_name
    echo 'masters = gentoo' > /var/db/repos/dolphin-dap/metadata/layout.conf
    mkdir -p /etc/portage/repos.conf
    printf '[dolphin-dap]\nlocation = /var/db/repos/dolphin-dap\n' \
      > /etc/portage/repos.conf/dolphin-dap.conf
    cp /source/Tools/ci/linux-packages/dolphin-dap.ebuild \
      "/var/db/repos/dolphin-dap/games-emulation/dolphin-dap/dolphin-dap-$version.ebuild"
    ebuild "/var/db/repos/dolphin-dap/games-emulation/dolphin-dap/dolphin-dap-$version.ebuild" manifest
    mkdir -p /etc/portage/package.accept_keywords
    echo 'games-emulation/dolphin-dap ~amd64' > /etc/portage/package.accept_keywords/dolphin-dap
    FEATURES='buildpkg -sandbox -usersandbox' emerge --usepkg=n "=games-emulation/dolphin-dap-$version"
    package=$(find /var/cache/binpkgs/games-emulation/dolphin-dap -name '*.gpkg.tar' -print -quit)
    test -n "$package"
    cp "$package" /release/dolphin-dap-gentoo-amd64.gpkg.tar
    # Reinstall through Portage from the binary package, not merely the staged files.
    emerge --usepkgonly --oneshot "=games-emulation/dolphin-dap-$version"
    ;;
  appimage)
    bash /source/Tools/ci/linux-packages/package-appimage.sh
    exit 0
    ;;
esac
bash /source/Tools/ci/linux-packages/smoke-binaries.sh
