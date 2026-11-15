# Maintainer: Jappe02 <Jappe00001@gmail.com>
pkgbase=jappeos_core
pkgname=('jappeos_core')
pkgver=1.1.3
_tag=dev-v1.1.3-3
pkgrel=3
arch=('x86_64')
url="https://github.com/JappeOS/$pkgbase"
license=('AGPL-3.0-or-later')
makedepends=(
  'git'
  'cmake>=3.31'
  'ninja'
  'pkgconf'
)
backup=(
  'etc/pam.d/jappeos-greeter'
  'etc/pam.d/jappeos-login'
  'etc/pam.d/jappeos-login-nopassword'
)
source=("$pkgbase-$pkgver.tar.gz::$url/archive/refs/tags/$_tag.tar.gz")
sha256sums=('SKIP')

build() {
  cd "$srcdir/$pkgbase-$_tag"

  cmake -S jappeos_core -B jappeos_core/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr
  cmake --build jappeos_core/build
}

package_jappeos_core() {
  pkgdesc="Core system daemon for JappeOS."
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

  cd "$srcdir/$pkgbase-$_tag"

  DESTDIR="$pkgdir" cmake --install jappeos_core/build
  install -Dm644 LICENSE "$pkgdir/usr/share/licenses/jappeos_core/LICENSE"
}
