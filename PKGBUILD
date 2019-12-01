pkgname=fcft
pkgver=0.0.0
pkgrel=1
pkgdesc="Simple font loading and glyph rasterization library"
arch=('x86_64')
url=https://codeberg.org/dnkl/fcft
license=(mit)
makedepends=('meson' 'ninja' 'scdoc')
depends=('freetype2' 'fontconfig' 'pixman')
source=()

pkgver() {
  cd ../.git &> /dev/null && git describe --tags --long | sed 's/^v//;s/\([^-]*-g\)/r\1/;s/-/./g' ||
      head -3 ../meson.build | grep version | cut -d "'" -f 2
}

build() {
  meson --prefix=/usr --buildtype=release ..
  ninja
}

package() {
  DESTDIR="${pkgdir}/" ninja install
}
