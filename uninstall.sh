#!/bin/sh
set -eu

PACKAGE_NAME="K6602VV-fan"
PACKAGE_VERSION="1.0.0"
SRC_DIR="/usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}"

if [ "$(id -u)" -ne 0 ]; then
	echo "Run as root: sudo ./uninstall.sh" >&2
	exit 1
fi

if command -v dkms >/dev/null 2>&1; then
	dkms remove -m "$PACKAGE_NAME" -v "$PACKAGE_VERSION" --all || true
fi

rm -rf "$SRC_DIR"
depmod

echo "Removed ${PACKAGE_NAME} ${PACKAGE_VERSION}"
