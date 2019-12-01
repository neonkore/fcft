pkgname=fcft
pkgver=0.1.1
pkgrel=1
pkgdesc="Simple font loading and glyph rasterization library"
arch=('x86_64')
url=https://codeberg.org/dnkl/fcft
license=(mit)
makedepends=('meson' 'ninja')
depends=('freetype2' 'fontconfig' 'pixman' 'tllist>=1.0.0')
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
