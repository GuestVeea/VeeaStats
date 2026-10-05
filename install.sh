#!/bin/sh
# Installs VeeaStats from a terminal, on any x86_64 Linux with glibc 2.34 or
# newer (Fedora, Ubuntu, Debian, openSUSE, Arch and their relatives).
#
#   sh install.sh                     download the latest release and install it
#   sh install.sh VeeaStats.AppImage  install an AppImage you already downloaded
#   sh install.sh --uninstall         remove VeeaStats
#
# Once the repository is public, it also works straight from the web:
#   curl -fsSL https://raw.githubusercontent.com/GuestVeea/VeeaStats/main/install.sh | sh
#
# VeeaStats installs into your home folder (~/.local/share/veeastats) and adds
# itself to your app menu, so this needs no root password. The AppImage does the
# actual installing (see packaging/AppRun); this script fetches it and runs it.

set -eu

REPO="GuestVeea/VeeaStats"
ASSET="VeeaStats-x86_64.AppImage"
DOWNLOAD_URL="https://github.com/$REPO/releases/latest/download/$ASSET"
INSTALLED="${XDG_DATA_HOME:-$HOME/.local/share}/veeastats/VeeaStats.AppImage"

say()  { printf '%s\n' "$*"; }
fail() { printf 'VeeaStats install: %s\n' "$*" >&2; exit 1; }

# Runs an AppImage. Without FUSE (some minimal or container systems) the
# AppImage can't mount itself, so it unpacks itself instead: into this script's
# own temporary folder, since a copy unpacked in /tmp by another user (or by
# root) would be in the way.
run_appimage() {
    if [ -c /dev/fuse ] && { command -v fusermount3 >/dev/null 2>&1 || command -v fusermount >/dev/null 2>&1; }; then
        "$@"
    else
        APPIMAGE_EXTRACT_AND_RUN=1 TMPDIR="$workdir" "$@"
    fi
}

# ---- Checks -------------------------------------------------------------------

[ "$(id -u)" -ne 0 ] || fail "run this as your normal user, not with sudo. VeeaStats installs into your home folder and asks for a password itself if it needs one."
[ "$(uname -m)" = "x86_64" ] || fail "VeeaStats is only built for x86_64 PCs; this one is $(uname -m)."

glibc="$(ldd --version 2>&1 | head -n 1 | grep -oE '[0-9]+\.[0-9]+$' || true)"
if [ -z "$glibc" ]; then
    fail "couldn't find glibc. VeeaStats needs a glibc-based distro (most are; Alpine and other musl-based ones aren't supported)."
fi
if ! awk -v version="$glibc" 'BEGIN { split(version, part, "."); exit !(part[1] > 2 || (part[1] == 2 && part[2] >= 34)) }'; then
    fail "this system has glibc $glibc; VeeaStats needs 2.34 or newer (Fedora 35+, Ubuntu 22.04+, Debian 12+)."
fi

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT INT TERM

# ---- Uninstall ----------------------------------------------------------------

if [ "${1:-}" = "--uninstall" ]; then
    [ -x "$INSTALLED" ] || fail "VeeaStats doesn't seem to be installed (no $INSTALLED)."
    run_appimage "$INSTALLED" --uninstall
    say "VeeaStats has been removed."
    exit 0
fi

# ---- Get the AppImage -----------------------------------------------------------

appimage="$workdir/$ASSET"

if [ -n "${1:-}" ]; then
    [ -f "$1" ] || fail "no such file: $1"
    cp "$1" "$appimage"
else
    say "Downloading the latest VeeaStats..."
    downloaded=no
    if command -v curl >/dev/null 2>&1; then
        curl -fL --proto '=https' --tlsv1.2 -o "$appimage" "$DOWNLOAD_URL" 2>/dev/null && downloaded=yes
    elif command -v wget >/dev/null 2>&1; then
        wget -q --https-only -O "$appimage" "$DOWNLOAD_URL" && downloaded=yes
    elif ! command -v gh >/dev/null 2>&1; then
        fail "downloading needs curl or wget; install one of them (for example: sudo apt install curl), or download $ASSET yourself and run: sh install.sh ./$ASSET"
    fi
    # A private repository can't be downloaded anonymously; the GitHub CLI can
    # do it if you're signed in.
    if [ "$downloaded" = no ] && command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
        rm -f "$appimage"
        gh release download --repo "$REPO" --pattern "$ASSET" --dir "$workdir" && downloaded=yes
    fi
    [ "$downloaded" = yes ] || fail "couldn't download $DOWNLOAD_URL.
If the repository is private, sign in with 'gh auth login' and try again, or download
$ASSET from the Releases page and run: sh install.sh ./$ASSET"
fi

# Make sure it's really an AppImage (a program), not an error page.
[ "$(head -c 4 "$appimage" | od -An -c | tr -d ' ')" = '177ELF' ] || fail "$appimage isn't an AppImage."
chmod +x "$appimage"

# ---- Install ------------------------------------------------------------------

say "Installing..."
run_appimage "$appimage" --install
