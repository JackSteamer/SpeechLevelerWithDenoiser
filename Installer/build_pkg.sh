#!/bin/bash
# Buduje instalator .pkg z juz zbudowanej wtyczki "Speech Leveler.component".
# Uruchamiac z katalogu glownego projektu, po: cmake --build build --target SpeechLeveler_AU
#
# Zmienne opcjonalne (bez nich wtyczka jest podpisana "ad-hoc", co wystarcza do uzytku wlasnego):
#   APP_SIGN_IDENTITY   np. "Developer ID Application: Imie Nazwisko (TEAMID)"
#   PKG_SIGN_IDENTITY   np. "Developer ID Installer: Imie Nazwisko (TEAMID)"

set -euo pipefail

cd "$(dirname "$0")/.."

VERSION="$(sed -n 's/.*project(SpeechLeveler VERSION \([0-9.]*\)).*/\1/p' CMakeLists.txt | head -n1)"
VERSION="${VERSION:-1.0.0}"

COMPONENT="$(find build -type d -name 'Speech Leveler.component' | head -n1)"
if [ -z "$COMPONENT" ]; then
    echo "Nie znaleziono 'Speech Leveler.component' w katalogu build/. Najpierw zbuduj wtyczke:" >&2
    echo "  cmake -B build && cmake --build build --config Release --target SpeechLeveler_AU" >&2
    exit 1
fi
echo "Wtyczka: $COMPONENT"

# 1. Podpis wtyczki (na Apple Silicon kod bez podpisu w ogole sie nie uruchomi)
if [ -n "${APP_SIGN_IDENTITY:-}" ]; then
    codesign --force --deep --options runtime --timestamp --sign "$APP_SIGN_IDENTITY" "$COMPONENT"
else
    codesign --force --deep --sign - "$COMPONENT"
fi
codesign --verify --deep --strict "$COMPONENT"

# 2. Zawartosc instalatora: wtyczka trafi do /Library/Audio/Plug-Ins/Components (dla wszystkich uzytkownikow)
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/root/Library/Audio/Plug-Ins/Components"
ditto "$COMPONENT" "$STAGE/root/Library/Audio/Plug-Ins/Components/Speech Leveler.component"

mkdir -p dist
OUT="dist/SpeechLeveler-${VERSION}.pkg"
UNSIGNED="$STAGE/SpeechLeveler-unsigned.pkg"

pkgbuild \
    --root "$STAGE/root" \
    --identifier com.speechleveler.plugin.pkg \
    --version "$VERSION" \
    --install-location / \
    "$UNSIGNED"

# 3. Opcjonalny podpis instalatora
if [ -n "${PKG_SIGN_IDENTITY:-}" ]; then
    productsign --sign "$PKG_SIGN_IDENTITY" "$UNSIGNED" "$OUT"
else
    cp "$UNSIGNED" "$OUT"
fi

echo "Gotowe: $OUT"
