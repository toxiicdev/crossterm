#!/usr/bin/env sh
# Builds CrossTerm.app on macOS (Release), bundles Qt into it and runs the tests.
# Missing dependencies are installed automatically: Xcode Command Line Tools, then cmake and qt with Homebrew.
# Optional at runtime: XQuartz (X11 forwarding), Microsoft Remote Desktop / Windows App (RDP).
#
# Usage: scripts/build-macos.sh [build-dir]    (Qt elsewhere: export CMAKE_PREFIX_PATH=/path/to/Qt/6.x/macos)
set -eu
cd "$(dirname "$0")/.."
BUILD="${1:-build}"

# --- Dependencies
if ! xcode-select -p >/dev/null 2>&1; then
    echo "Installing the Xcode Command Line Tools: complete the dialog, then run this script again."
    xcode-select --install || true
    exit 1
fi
has_qt() { [ -n "$1" ] && [ -f "$1/lib/cmake/Qt6/Qt6Config.cmake" ]; }
QT_PREFIX="${CMAKE_PREFIX_PATH:-}"
if ! has_qt "$QT_PREFIX"; then
    for candidate in "$HOME"/Qt/6.*/macos; do
        has_qt "$candidate" && QT_PREFIX="$candidate"
    done
fi
missing=""
command -v cmake >/dev/null 2>&1 || missing="$missing cmake"
if ! has_qt "$QT_PREFIX"; then
    if command -v brew >/dev/null 2>&1 && brew list qt >/dev/null 2>&1; then
        QT_PREFIX="$(brew --prefix qt)"
    else
        missing="$missing qt"
    fi
fi
if [ -n "$missing" ]; then
    if ! command -v brew >/dev/null 2>&1; then
        echo "Homebrew is needed to install:$missing" >&2
        echo "Install it from https://brew.sh, then run this script again." >&2
        exit 1
    fi
    echo "Installing with Homebrew:$missing"
    # shellcheck disable=SC2086
    brew install $missing
    has_qt "$QT_PREFIX" || QT_PREFIX="$(brew --prefix qt)"
fi
if ! has_qt "$QT_PREFIX"; then
    echo "Qt 6 not found after installation; set CMAKE_PREFIX_PATH=/path/to/Qt/6.x/macos" >&2
    exit 1
fi
echo "Using Qt: $QT_PREFIX"

# --- Build, tests, bundle
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_PREFIX"
cmake --build "$BUILD" --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir "$BUILD" --output-on-failure || echo "Some tests failed (see above)."
# Homebrew keeps Qt modules and their dependencies (webp, brotli...) in its own lib folder.
BREW_LIB="$(brew --prefix 2>/dev/null || true)/lib"
if [ -d "$BREW_LIB" ]; then
    "$QT_PREFIX/bin/macdeployqt" "$BUILD/CrossTerm.app" -libpath="$BREW_LIB"
else
    "$QT_PREFIX/bin/macdeployqt" "$BUILD/CrossTerm.app"
fi
# macdeployqt rewrites the binaries and invalidates their signatures: re-sign the whole bundle, otherwise macOS
# refuses out-of-process services (file panels, Keychain) to the app.
# 1) Apple certificate (CROSSTERM_SIGN_IDENTITY, or the first "Developer ID Application" found, else the first
#    "Apple Development"): hardened runtime + secure timestamp.
#    Developer ID: other Macs open the app without warnings; notarization when CROSSTERM_NOTARY_PROFILE names a
#    profile saved once with
#    `xcrun notarytool store-credentials <profile> --apple-id <id> --team-id <team> --password <app-specific password>`.
#    Apple Development: runs on this Mac only (other Macs block it, Apple does not notarize it).
# 2) Otherwise a local self-signed certificate (created once in the login keychain).
# Both keep the same signature across builds, so Keychain "Always Allow" stays valid; an ad hoc signature changes
# at every build and the password is asked again.
# An Apple certificate listed but not valid usually lacks its intermediate authority (a certificate created outside
# Xcode, or an old Mac): import Apple's once from apple.com, then look again.
APPLE_CERT='"(Developer ID Application|Apple Development): '
if security find-identity -p codesigning 2>/dev/null | grep -qE "$APPLE_CERT" &&
    ! security find-identity -v -p codesigning 2>/dev/null | grep -qE "$APPLE_CERT"; then
    tmp="$(mktemp -d)"
    for ca in AppleWWDRCAG3 DeveloperIDG2CA; do
        echo "Importing Apple intermediate certificate $ca"
        curl -fsSL "https://www.apple.com/certificateauthority/$ca.cer" -o "$tmp/$ca.cer" &&
            security import "$tmp/$ca.cer" -k "$HOME/Library/Keychains/login.keychain-db" >/dev/null 2>&1 || true
    done
    rm -rf "$tmp"
fi
IDENTITIES="$(security find-identity -v -p codesigning 2>/dev/null || true)"
DEVELOPER_ID="${CROSSTERM_SIGN_IDENTITY:-$(printf '%s\n' "$IDENTITIES" |
    sed -n 's/.*"\(Developer ID Application: [^"]*\)".*/\1/p' | head -n 1)}"
DEVELOPER_ID="${DEVELOPER_ID:-$(printf '%s\n' "$IDENTITIES" | sed -n 's/.*"\(Apple Development: [^"]*\)".*/\1/p' | head -n 1)}"
if [ -n "$DEVELOPER_ID" ]; then
    codesign --force --deep --options runtime --timestamp --sign "$DEVELOPER_ID" "$BUILD/CrossTerm.app"
    codesign --verify --deep --strict "$BUILD/CrossTerm.app" && echo "Signed with \"$DEVELOPER_ID\""
    case "$DEVELOPER_ID" in "Developer ID Application:"*) NOTARY="${CROSSTERM_NOTARY_PROFILE:-}" ;; *) NOTARY= ;; esac
    if [ -n "$NOTARY" ]; then
        ditto -c -k --keepParent "$BUILD/CrossTerm.app" "$BUILD/CrossTerm-notarize.zip"
        xcrun notarytool submit "$BUILD/CrossTerm-notarize.zip" --keychain-profile "$CROSSTERM_NOTARY_PROFILE" --wait
        xcrun stapler staple "$BUILD/CrossTerm.app"
        rm -f "$BUILD/CrossTerm-notarize.zip"
        spctl --assess --type execute --verbose "$BUILD/CrossTerm.app"
    fi
    echo "Built: $BUILD/CrossTerm.app"
    exit 0
fi
IDENTITY="CrossTerm Local Signing"
if ! security find-certificate -c "$IDENTITY" >/dev/null 2>&1; then
    echo "Creating the local signing certificate \"$IDENTITY\" (once)"
    tmp="$(mktemp -d)"
    cat > "$tmp/cert.cnf" <<EOF
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no
[dn]
CN = $IDENTITY
[ext]
basicConstraints = critical, CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = critical, codeSigning
EOF
    # /usr/bin/openssl (LibreSSL) writes a PKCS#12 that `security import` accepts.
    if /usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -config "$tmp/cert.cnf" \
           -keyout "$tmp/key.pem" -out "$tmp/cert.pem" >/dev/null 2>&1 &&
       /usr/bin/openssl pkcs12 -export -inkey "$tmp/key.pem" -in "$tmp/cert.pem" -out "$tmp/id.p12" \
           -passout pass:crossterm >/dev/null 2>&1; then
        security import "$tmp/id.p12" -k "$HOME/Library/Keychains/login.keychain-db" -P crossterm -T /usr/bin/codesign ||
            echo "Could not import the signing certificate; signing ad hoc."
    fi
    rm -rf "$tmp"
fi
if security find-certificate -c "$IDENTITY" >/dev/null 2>&1 &&
   codesign --force --deep --sign "$IDENTITY" "$BUILD/CrossTerm.app"; then
    echo "Signed with \"$IDENTITY\""
else
    codesign --force --deep --sign - "$BUILD/CrossTerm.app"
    echo "Signed ad hoc (Keychain will ask again after every build)"
fi
codesign --verify --deep --strict "$BUILD/CrossTerm.app" && echo "Signature OK"
echo "Built: $BUILD/CrossTerm.app"
