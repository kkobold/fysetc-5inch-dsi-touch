#!/bin/sh
# Adds the Raspberry Pi archive (Bookworm) to a Debian Bookworm container.
set -eu

# Fingerprint of the Raspberry Pi archive signing key.
KEY_FPR=CF8A1AF502A2AA2D763BAE7E82B129927FA3303E
KEYRING=/usr/share/keyrings/raspberrypi-archive-keyring.gpg

apt-get update
apt-get install -y --no-install-recommends ca-certificates curl gpg

key=$(mktemp)
curl -fsSL https://archive.raspberrypi.com/debian/raspberrypi.gpg.key -o "$key"
fpr=$(gpg --show-keys --with-colons "$key" | awk -F: '/^fpr/ { print $10; exit }')
if [ "$fpr" != "$KEY_FPR" ]; then
    echo "Raspberry Pi archive key fingerprint is $fpr, expected $KEY_FPR" >&2
    exit 1
fi
gpg --dearmor -o "$KEYRING" "$key"
rm -f "$key"

echo "deb [signed-by=$KEYRING] http://archive.raspberrypi.com/debian bookworm main" \
    > /etc/apt/sources.list.d/raspi.list
apt-get update
