#!/bin/sh
set -eu

PACKAGE_NAME="K6602VV-fan"
PACKAGE_VERSION="1.0.0"
MODULE_NAME="K6602VV_fan"
SRC_DIR="/usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

if [ "$(id -u)" -ne 0 ]; then
	echo "Run as root: sudo ./install.sh" >&2
	exit 1
fi

if ! command -v dkms >/dev/null 2>&1; then
	echo "dkms is not installed" >&2
	exit 1
fi

install -d "$SRC_DIR"
install -m 0644 "$SCRIPT_DIR/K6602VV_fan.c" "$SRC_DIR/"
install -m 0644 "$SCRIPT_DIR/Makefile" "$SRC_DIR/"
install -m 0644 "$SCRIPT_DIR/dkms.conf" "$SRC_DIR/"

if dkms status -m "$PACKAGE_NAME" -v "$PACKAGE_VERSION" | grep -q .; then
	dkms remove -m "$PACKAGE_NAME" -v "$PACKAGE_VERSION" --all
fi

dkms add -m "$PACKAGE_NAME" -v "$PACKAGE_VERSION"
dkms build -m "$PACKAGE_NAME" -v "$PACKAGE_VERSION"
dkms install -m "$PACKAGE_NAME" -v "$PACKAGE_VERSION"
depmod

echo "Installed ${MODULE_NAME}. Load it with: modprobe ${MODULE_NAME}"
