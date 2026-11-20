# Maintainer: Jappe02 <Jappe00001@gmail.com>
pkgbase=jappeos_core
pkgname=('jappeos_core' 'jappeos_notification_manager')
pkgver=2.1.2
_tag=dev-v2.1.1-1
pkgrel=1
arch=('x86_64')
url="https://github.com/JappeOS/$pkgbase"
license=('AGPL-3.0-or-later')
makedepends=(
  'git'
  'cmake>=3.31'
  'ninja'
  'pkgconf'
  'dbus'
  'pam'
  'glib2'
  'networkmanager'
  'libpipewire'
  'libpulse'
  'parted'
  'icu'
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

  # jappeos_core

  cmake -S jappeos_core -B jappeos_core/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr
  cmake --build jappeos_core/build

  # jappeos_notification_manager

  cmake -S jappeos_notification_manager -B jappeos_notification_manager/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr
  cmake --build jappeos_notification_manager/build
}

package_jappeos_core() {
  pkgdesc="Core system daemon for JappeOS."
  depends=(
    'dbus'
    'systemd'
    'pacman'
    'udev'
    'pam'
    'glib2'
    'networkmanager'
    'libpipewire'
    'libpulse'
    'parted'
    'icu'
  )

  cd "$srcdir/$pkgbase-$_tag"

  DESTDIR="$pkgdir" cmake --install jappeos_core/build
  install -Dm644 LICENSE "$pkgdir/usr/share/licenses/jappeos_core/LICENSE"
}

package_jappeos_notification_manager() {
  pkgdesc="Session notification daemon for JappeOS."
  depends=(
    'dbus'
    'systemd'
    'pacman'
    'glib2'
  )

  cd "$srcdir/$pkgbase-$_tag"

  DESTDIR="$pkgdir" cmake --install jappeos_notification_manager/build
  install -Dm644 LICENSE "$pkgdir/usr/share/licenses/jappeos_notification_manager/LICENSE"
}
