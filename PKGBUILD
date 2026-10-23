pkgname=nerd-dictation-remote
pkgver=1.0.0
pkgrel=2
pkgdesc="Remote ASR backend for nerd-dictation: recognition on another machine over ssh"
arch=('x86_64' 'loong64')
url="https://github.com/nerd-voice-interface/nerd-dictation-remote"
license=('MIT')
depends=('gcc-libs' 'glibc' 'openssh' 'python')
makedepends=('git' 'cmake')
provides=('nerd-dictation-backend')
source=()
sha256sums=()

_srcdir="${startdir}"

prepare() {
    cd "$_srcdir"
    if [ ! -f "ThirdParty/nerd-dictation-api/include/nerd_dictation/backend_api.h" ]; then
        git submodule update --init --recursive
    fi
}

build() {
    cd "$_srcdir"
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build -j$(nproc)
}

package() {
    cd "$_srcdir"
    install -Dm755 build/libnerd-dictation-remote.so.${pkgver} \
        "${pkgdir}/usr/lib/libnerd-dictation-remote.so.${pkgver}"
    ln -s libnerd-dictation-remote.so.${pkgver} "${pkgdir}/usr/lib/libnerd-dictation-remote.so.1"
    ln -s libnerd-dictation-remote.so.${pkgver} "${pkgdir}/usr/lib/libnerd-dictation-remote.so"
    # The server side of the bridge, for the host that serves recognition
    install -Dm755 src/bridge.py "${pkgdir}/usr/share/${pkgname}/nerd-dictation-asr-bridge"
    install -Dm644 ThirdParty/nerd-dictation-api/LICENSE \
        "${pkgdir}/usr/share/licenses/${pkgname}/LICENSE"
}
