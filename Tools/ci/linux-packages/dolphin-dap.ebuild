# Binary packaging after compilation/tests in a Gentoo container.
EAPI=8
DESCRIPTION="Dolphin emulator with source debugging and DAP"
HOMEPAGE="https://github.com/LiveMindIO/dolphin-dap"
LICENSE="GPL-2+"
SLOT="0"
KEYWORDS="~amd64"
RDEPEND="dev-qt/qtbase:6 dev-qt/qtsvg:6 dev-libs/libevdev virtual/libudev
 x11-libs/libXrandr x11-libs/libXi net-wireless/bluez media-libs/alsa-lib
 media-libs/libpulse media-libs/mesa dev-libs/libusb
 !games-emulation/dolphin"
S="${WORKDIR}"
src_unpack() { :; }
src_prepare() { default; }
src_configure() { :; }
src_compile() { :; }
src_install() {
  cp -a /work/stage/usr "${D}/" || die
}
