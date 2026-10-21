#!/bin/sh
# Installs the build dependencies and the given kernel header packages, then
# compiles the modules in src/ for every installed kernel, with warnings as
# errors. Runs as root in a Debian container, from the repository root.
set -eu

apt-get update
apt-get install -y --no-install-recommends build-essential "$@"

for build in /lib/modules/*/build; do
    echo "::group::$build"
    rm -rf /tmp/mod
    cp -r src /tmp/mod
    make -C "$build" M=/tmp/mod W=1 KCFLAGS=-Werror modules
    echo "::endgroup::"
done
