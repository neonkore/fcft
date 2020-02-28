pkgname=fcft
pkgver=1.1.3
pkgrel=1
pkgdesc="Simple font loading and glyph rasterization library"
arch=('x86_64')
url=https://codeberg.org/dnkl/fcft
license=(mit)
makedepends=('meson' 'ninja' 'tllist>=1.0.0')
depends=('freetype2' 'fontconfig' 'pixman')
source=()

pkgver() {
  cd ../.git &> /dev/null && git describe --tags --long | sed 's/^v//;s/\([^-]*-g\)/r\1/;s/-/./g' ||
      head -3 ../meson.build | grep version | cut -d "'" -f 2
}

build() {
  meson --prefix=/usr --buildtype=release --wrap-mode=nofallback ..
  ninja
}

package() {
  DESTDIR="${pkgdir}/" ninja install
}
