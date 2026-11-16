# Builds an Arch package from the working copy this file sits in (the repository root), as it is on disk:
# uncommitted changes included, nothing cloned or downloaded.
#
#   makepkg -si
#
# The package replaces the repo's ananicy-cpp (binary in /usr/bin, service in /usr/lib/systemd/system).
# It is the same package as LunarOs' ananicy-cpp-lunar (built there from the release tag v<version>);
# a local build is <version>.r<count>.g<hash>, newer than that release. ananicy-cpp-wp is this
# package's old name.

pkgname=ananicy-cpp-lunar
pkgrel=1
pkgdesc='Ananicy rewritten in C++ - Lunar build of the WhitePeace36 fork'
arch=(x86_64)
url="https://github.com/WhitePeace36/ananicy-cpp-lunar"
license=('GPL-3.0-or-later')
depends=(fmt spdlog systemd-libs libelf zlib libbpf)
makedepends=(git cmake ninja clang nlohmann-json bpf)
optdepends=('lunar-ananicy-rules: rules tuned for the Elara scheduler (github.com/WhitePeace36/ananicy-rules)')
conflicts=(ananicy-cpp ananicy-cpp-wp)
replaces=(ananicy-cpp ananicy-cpp-wp)

# makepkg's scratch directory is ./src next to the PKGBUILD, which here is the repository's own source tree:
# `makepkg -c` or `-C` would delete it. makepkg reads this file before it picks that directory, so point it
# elsewhere: .makepkg/ananicy-cpp-lunar/{src,pkg}. A BUILDDIR set in makepkg.conf is left alone.
if [[ $BUILDDIR -ef $startdir ]]; then
  BUILDDIR="$startdir/.makepkg"
fi

# The version is worked out here rather than in a pkgver() function: makepkg writes a pkgver() result back
# into this file, which would leave it modified in git after every build.
_version() {
  # Upstream's current version (README); its last tag is still v1.1.1. CMake needs it to start with x.y.z.
  local base=1.4.0
  # safe.directory: git refuses repositories owned by another user (common on external drives).
  local git=(git -c safe.directory="$startdir" -C "$startdir")
  if "${git[@]}" rev-parse HEAD >/dev/null 2>&1; then
    printf '%s.r%s.g%s' "$base" "$("${git[@]}" rev-list --count HEAD)" "$("${git[@]}" rev-parse --short=7 HEAD)"
  else
    printf '%s.r%s' "$base" "$(date +%Y%m%d%H%M)"
  fi
}
pkgver=$(_version)
# With the version (1.4.0 of 1.4.0.r123.gabcdef0), so packages that need a newer ananicy-cpp, such as
# lunar-ananicy-rules (depends on ananicy-cpp>=1.4.0 for /usr/share/ananicy.d), accept this one.
provides=("ananicy-cpp=${pkgver%%.r*}")

prepare() {
  # Last line of defence, in case a makepkg version picks the scratch directory before reading this file.
  if [[ $srcdir -ef $startdir/src ]]; then
    echo "==> ERROR: the scratch directory is the repository's src/; refusing to go on (see the PKGBUILD)." >&2
    return 1
  fi
}

build() {
  cmake -S "$startdir" -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DUSE_EXTERNAL_SPDLOG=ON \
    -DUSE_EXTERNAL_JSON=ON \
    -DUSE_EXTERNAL_FMTLIB=ON \
    -DENABLE_SYSTEMD=ON \
    -DUSE_BPF_PROC_IMPL=ON \
    -DBPF_BUILD_LIBBPF=OFF \
    -DVERSION="$pkgver"
  cmake --build build --target ananicy-cpp
}

package() {
  DESTDIR="$pkgdir" cmake --install build --component Runtime
  install -dm755 "$pkgdir/etc/ananicy.d"
}
