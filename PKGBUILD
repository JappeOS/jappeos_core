pkgname=jappeos_core
pkgver=1.0.8
_tag=dev-v1.0.7-1
pkgrel=1
pkgdesc="Core system daemon for JappeOS."
arch=('x86_64')
url="https://github.com/JappeOS/$pkgname"
license=('AGPL-3.0-only')
depends=(
  'dbus'
  'glib2'
  'icu'
  'libpipewire'
  'libpulse'
  'networkmanager'
  'pam'
  'parted'
  'systemd'
)
makedepends=(
  'cmake'
  'ninja'
  'pkgconf'
)
backup=(
  'etc/pam.d/jappeos-greeter'
  'etc/pam.d/jappeos-login'
  'etc/pam.d/jappeos-login-nopassword'
)
source=("$pkgname-$pkgver.tar.gz::$url/archive/refs/tags/$_tag.tar.gz")
sha256sums=('SKIP')

build() {
  cd "$srcdir/$pkgname-$_tag"

  cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr
  cmake --build build
}

package() {
  cd "$srcdir/$pkgname-$_tag"

  DESTDIR="$pkgdir" cmake --install build
  install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
