# Maintainer: Hermes <hermes@users.noreply.github.com>
pkgname=pkteditor
pkgver=0.1.0
pkgdesc="Editor for PrehKeyTec MCI-3100 mapping lists (.pmap), incl. native board upload"
url="https://github.com/mr-kendrix/Preh-MCI128-USB-Keyboard-under-Linux"
license=('MIT')
arch=('x86_64')
depends=('qt6-base' 'libusb' 'hidapi')
makedepends=('cmake' 'clang')
source=("https://github.com/mr-kendrix/Preh-MCI128-USB-Keyboard-under-Linux/archive/refs/heads/main.tar.gz")
sha256sums=('SKIP')

build() {
  cmake -B b -S . -DCMAKE_BUILD_TYPE=Release
  cmake --build b
}

package() {
  install -Dm755 b/pkteditor "$pkgdir/usr/bin/$pkgname"
  install -Dm755 b/pktup "$pkgdir/usr/bin/pktup"
  install -Dm644 README.md "$pkgdir/usr/share/doc/$pkgname/README.md"
  install -Dm644 LICENSE "$pkgdir/usr/share/doc/$pkgname/LICENSE"
  install -Dm644 99-prehkeytec.rules "$pkgdir/usr/share/pkteditor/99-prehkeytec.rules"
}
