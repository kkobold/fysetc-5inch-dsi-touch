#!/bin/sh
# Package lifecycle test: upgrade, install, reinstall, kernel hook, failed
# updates, remove and purge. Runs as root in a Debian Bookworm arm64 container
# with the Raspberry Pi archive, with the package to test in $1. GITHUB_TOKEN,
# if set, authenticates the GitHub API request. update-initramfs is diverted
# to a stub that records its arguments.
set -eu

DEB=$1
PKG=fysetc-5inch-dsi-touch
REPO=kkobold/fysetc-5inch-dsi-touch
MODULES="fysetc_touch fysetc_sleep"
MODDIR=updates/fysetc_touch
SRC=/usr/src/fysetc-touch
HOOKS="/etc/kernel/header_postinst.d /etc/kernel/postinst.d /etc/kernel/postrm.d"
INITRAMFS_LOG=/tmp/update-initramfs.log

fail() { echo "FAIL: $*" >&2; exit 1; }

# Maintainer scripts run by apt did not use a stub placed first in this
# script's PATH, so the real update-initramfs (installed with the kernel
# packages) is diverted instead.
UPDATE_INITRAMFS=/usr/sbin/update-initramfs
dpkg-divert --quiet --local --rename --divert "$UPDATE_INITRAMFS.real" --add "$UPDATE_INITRAMFS"
printf '#!/bin/sh\necho "$*" >> %s\n' "$INITRAMFS_LOG" > "$UPDATE_INITRAMFS"
chmod +x "$UPDATE_INITRAMFS"
trap 'rm -f "$UPDATE_INITRAMFS"; dpkg-divert --quiet --local --rename --remove "$UPDATE_INITRAMFS"' EXIT

# The maintainer script regenerated the initramfs of every kernel.
assert_initramfs_updated() {
    grep -qx -- "-u -k all" "$INITRAMFS_LOG" 2>/dev/null ||
        fail "$1: update-initramfs -u -k all not called"
    : > "$INITRAMFS_LOG"
}

rpi_kernels() {
    for d in /lib/modules/*+rpt-rpi-*; do
        [ -d "$d" ] && echo "${d##*/}"
    done
}

github_api() {
    if [ -n "${GITHUB_TOKEN:-}" ]; then
        curl -sS -H "Authorization: Bearer $GITHUB_TOKEN" "$@"
    else
        curl -sS "$@"
    fi
}

assert_installed() {
    [ -f /lib/modprobe.d/fysetc-touch.conf ] || fail "blacklist missing"
    for h in $HOOKS; do
        [ -L "$h/fysetc-touch" ] || fail "kernel hook missing in $h"
    done
    status=$(fysetc-touch-build status)
    echo "$status"
    [ -n "$status" ] || fail "no Raspberry Pi kernel found"
    if echo "$status" | grep -v ': installed$'; then
        fail "modules not installed for every kernel"
    fi
}

assert_removed() {
    [ ! -e /lib/modprobe.d/fysetc-touch.conf ] || fail "blacklist left behind"
    for h in $HOOKS; do
        [ ! -e "$h/fysetc-touch" ] || fail "kernel hook left behind in $h"
    done
    for k in $(rpi_kernels); do
        [ ! -e "/lib/modules/$k/$MODDIR" ] || fail "modules left behind for $k"
    done
}

# After a failed update: the previous set is untouched, no temporary file is
# left and the failure is reported.
assert_previous_kept() {
    [ "$(sha256sum "$dest"/*.ko)" = "$previous" ] || fail "$1: module set changed"
    for m in $MODULES; do
        [ ! -e "$dest/.$m.ko.new" ] && [ ! -e "$dest/.$m.ko.old" ] ||
            fail "$1: temporary module file left behind"
    done
    fysetc-touch-build status | grep -q "^$kver: installed (last build failed" ||
        fail "$1: failure not reported"
}

echo "== upgrade from the latest release"
code=$(github_api -o /tmp/latest.json -w '%{http_code}' \
    "https://api.github.com/repos/$REPO/releases/latest")
case "$code" in
    200)
        prev_url=$(sed -n 's/.*"browser_download_url": *"\([^"]*\.deb\)".*/\1/p' /tmp/latest.json | head -n1)
        [ -n "$prev_url" ] || fail "latest release has no .deb"
        curl -fsSLo /tmp/previous.deb "$prev_url"
        apt-get install -y /tmp/previous.deb
        apt-get install -y "$DEB"
        assert_installed
        apt-get purge -y "$PKG"
        assert_removed
        ;;
    404) echo "no previous release, skipped" ;;
    *) fail "GitHub API returned HTTP $code" ;;
esac

echo "== install"
: > "$INITRAMFS_LOG"
apt-get install -y "$DEB"
assert_installed
assert_initramfs_updated install

echo "== reinstall"
apt-get install -y --reinstall "$DEB"
assert_installed

echo "== kernel hook rebuilds the modules"
kver=$(rpi_kernels | head -n1)
dest=/lib/modules/$kver/$MODDIR
rm -rf "$dest"
/etc/kernel/postinst.d/fysetc-touch "$kver"
assert_installed

echo "== failed updates keep the previous module set"
previous=$(sha256sum "$dest"/*.ko)
for f in "$SRC"/*.c; do
    echo 'MODULE_INFO(lifecycle_test, "new");' >> "$f"   # the next build differs
done

# Runs a build with COMMAND replaced by a stub that fails for the second module
# (fysetc_sleep): install fails staging, mv fails promotion, depmod fails last.
build_failing() {
    fail_cmd=$1
    real=$(command -v "$fail_cmd")
    fakebin=$(mktemp -d)
    case "$fail_cmd" in
        depmod) printf '#!/bin/sh\nexit 1\n' > "$fakebin/depmod" ;;
        *) printf '#!/bin/sh\ncase "$*" in *.fysetc_sleep.ko.new*) exit 1 ;; esac\nexec %s "$@"\n' \
            "$real" > "$fakebin/$fail_cmd" ;;
    esac
    chmod +x "$fakebin/$fail_cmd"
    PATH="$fakebin:$PATH" fysetc-touch-build build "$kver"
    rm -rf "$fakebin"
}

build_failing install
assert_previous_kept "staging failure"
build_failing mv
assert_previous_kept "promotion failure"
build_failing depmod
assert_previous_kept "depmod failure"

# The next good build installs the new set and clears the failure.
fysetc-touch-build build "$kver"
[ "$(sha256sum "$dest"/*.ko)" != "$previous" ] || fail "new module set not installed"
assert_installed
apt-get install -y --reinstall "$DEB"   # restores the package's sources

echo "== remove"
: > "$INITRAMFS_LOG"
apt-get remove -y "$PKG"
assert_removed
assert_initramfs_updated remove

echo "== purge"
apt-get install -y "$DEB"
apt-get purge -y "$PKG"
assert_removed
[ ! -e /var/log/fysetc-touch ] || fail "log directory left behind"

echo "lifecycle test passed"
