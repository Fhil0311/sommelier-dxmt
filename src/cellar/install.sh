#!/bin/sh
# Added by the Sommelier project, 2026-10-22 (LGPL-2.1-or-later). Next to winemetal.so, so an engine carries it.
INSTALL_DIR="${DESTDIR%%/}${MESON_INSTALL_PREFIX}/$2"
mkdir -p "${INSTALL_DIR}"
cp "$1" "${INSTALL_DIR}/cellar"
