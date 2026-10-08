#!/bin/sh
# Installs VeeaStats from a terminal, on any x86_64 Linux with glibc 2.34 or
# newer (Fedora, Ubuntu, Debian, openSUSE, Arch and their relatives).
#
#   sh install.sh                     download the latest release and install it
#   sh install.sh VeeaStats.AppImage  install an AppImage you already downloaded
#   sh install.sh --uninstall         remove VeeaStats
#
# It also works straight from the web:
#   curl -fsSL https://raw.githubusercontent.com/GuestVeea/VeeaStats/main/install.sh | sh
#
# VeeaStats installs into your home folder (~/.local/share/veeastats) and adds
# itself to your app menu, so this needs no root password. The AppImage does the
# actual installing (see packaging/AppRun); this script fetches it and runs it.
#
# Everything is inside main(), which is only called on the last line: if a
# download of this script is cut off, the shell runs nothing at all instead of
# half of it.

set -eu

REPO="GuestVeea/VeeaStats"
ASSET="VeeaStats-x86_64.AppImage"
DOWNLOAD_URL="https://github.com/$REPO/releases/latest/download/$ASSET"

say()  { printf '%s\n' "$*"; }
fail() { printf 'VeeaStats install: %s\n' "$*" >&2; exit 1; }

# An XDG_* folder only counts when it's an absolute path (as the XDG spec says).
xdg_dir() {
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *)  printf '%s\n' "$HOME/$2" ;;
    esac
}

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

main() {
    [ "$(id -u)" -ne 0 ] || fail "run this as your normal user, not with sudo. VeeaStats installs into your home folder and asks for a password itself if it needs one."
    [ "$(uname -m)" = "x86_64" ] || fail "VeeaStats is only built for x86_64 PCs; this one is $(uname -m)."

    glibc="$(ldd --version 2>&1 | head -n 1 | grep -oE '[0-9]+\.[0-9]+$' || true)"
    if [ -z "$glibc" ]; then
        fail "couldn't find glibc. VeeaStats needs a glibc-based distro (most are; Alpine and other musl-based ones aren't supported)."
    fi
    if ! awk -v version="$glibc" 'BEGIN { split(version, part, "."); exit !(part[1] > 2 || (part[1] == 2 && part[2] >= 34)) }'; then
        fail "this system has glibc $glibc; VeeaStats needs 2.34 or newer (Fedora 35+, Ubuntu 22.04+, Debian 12+)."
    fi

    installed="$(xdg_dir "${XDG_DATA_HOME:-}" .local/share)/veeastats/VeeaStats.AppImage"

    # The work folder is in the user's cache folder, not /tmp: the AppImage is
    # run from it, and /tmp is often mounted "noexec" (no programs may run there).
    cache="$(xdg_dir "${XDG_CACHE_HOME:-}" .cache)"
    mkdir -p "$cache"
    workdir="$(mktemp -d "$cache/veeastats-install.XXXXXX")"
    trap 'rm -rf "$workdir"' EXIT
    trap 'exit 130' INT    # Ctrl+C stops here (the EXIT trap still cleans up)
    trap 'exit 143' TERM

    # ---- Uninstall --------------------------------------------------------------

    if [ "${1:-}" = "--uninstall" ]; then
        [ -x "$installed" ] || fail "VeeaStats doesn't seem to be installed (no $installed)."
        status=0
        run_appimage "$installed" --uninstall || status=$?
        case $status in
            0)  ;;   # the AppImage said what it did
            10) say "Nothing was removed."; exit 0 ;;
            *)  fail "VeeaStats wasn't removed (see the message above)." ;;
        esac
        exit 0
    fi

    # ---- Get the AppImage -------------------------------------------------------

    appimage="$workdir/$ASSET"

    if [ -n "${1:-}" ]; then
        [ -f "$1" ] || fail "no such file: $1"
        cp -- "$1" "$appimage"
    else
        say "Downloading the latest VeeaStats..."
        if command -v curl >/dev/null 2>&1; then
            curl -fL --proto '=https' --tlsv1.2 -o "$appimage" "$DOWNLOAD_URL" ||
                fail "couldn't download $DOWNLOAD_URL (see the error above)."
        elif command -v wget >/dev/null 2>&1; then
            wget -q -O "$appimage" "$DOWNLOAD_URL" ||
                fail "couldn't download $DOWNLOAD_URL."
        else
            fail "downloading needs curl or wget; install one of them (for example: sudo apt install curl), or download $ASSET yourself and run: sh install.sh ./$ASSET"
        fi
    fi

    # Make sure it's really an AppImage (a program), not an error page.
    [ "$(head -c 4 "$appimage" | od -An -c | tr -d ' ')" = '177ELF' ] || fail "$appimage isn't an AppImage."
    chmod +x "$appimage"

    # ---- Install ----------------------------------------------------------------

    say "Installing..."
    run_appimage "$appimage" --install || fail "the install didn't finish (see the message above)."
}

main "$@"
