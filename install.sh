#!/bin/bash
# install.sh: build and install kindle-mtp-mount. Safe to run again at any time.
#
#   ./install.sh                         install to ~/.local/bin
#   ./install.sh --prefix /usr/local/bin install somewhere else (must be writable)
#
# It installs libmtp with Homebrew if needed. It never installs macFUSE, never
# uses sudo, and never changes System Settings. The one manual step is printed
# at the end.
set -euo pipefail

PREFIX="$HOME/.local/bin"
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="${2:?--prefix needs a directory}"; shift 2 ;;
    --prefix=*) PREFIX="${1#--prefix=}"; shift ;;
    -h|--help) sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see ./install.sh --help)" >&2; exit 2 ;;
  esac
done

cd "$(dirname "$0")"
ok()   { printf '  ok    %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
die()  { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

FSKIT_STEP="System Settings > General > Login Items & Extensions > File System Extensions > macFUSE (switch on)"

step "Checking macOS"
[ "$(uname -s)" = Darwin ] || die "this only runs on macOS."
[ "$(uname -m)" = arm64 ] || die "this needs a Mac with Apple silicon (found $(uname -m))."
VER="$(sw_vers -productVersion)"
MAJ="${VER%%.*}"; REST="${VER#*.}"; MIN="${REST%%.*}"; [ "$REST" = "$VER" ] && MIN=0
if [ "$MAJ" -lt 15 ] || { [ "$MAJ" -eq 15 ] && [ "$MIN" -lt 4 ]; }; then
  die "macOS $VER is too old. macFUSE's FSKit backend needs macOS 15.4 or later."
fi
ok "macOS $VER on Apple silicon"

step "Checking libmtp (Homebrew)"
command -v brew >/dev/null 2>&1 || die "Homebrew isn't installed. Get it from https://brew.sh, then run ./install.sh again."
if brew list --versions libmtp >/dev/null 2>&1; then
  ok "libmtp already installed"
else
  echo "  installing libmtp with Homebrew..."
  brew install libmtp
fi
BREW="$(brew --prefix)"
MTPH="$(brew --prefix libmtp)/include/libmtp.h"
[ -f "$MTPH" ] || die "can't find $MTPH after installing libmtp."
MTPVER="$(sed -nE 's/^#define LIBMTP_VERSION_STRING "([^"]+)".*/\1/p' "$MTPH")"
[ "$MTPVER" = "1.1.23" ] || die "libmtp is $MTPVER, but this tool only works with 1.1.23 (it relies on its internals to avoid a USB reset). Nothing was installed."
ok "libmtp $MTPVER"

step "Checking macFUSE"
if [ ! -d /Library/Filesystems/macfuse.fs ] || [ ! -f /usr/local/include/fuse3/fuse.h ] || ! ls /usr/local/lib/libfuse3*.dylib >/dev/null 2>&1; then
  cat >&2 <<MSG

macFUSE isn't installed (or its libfuse3 headers are missing).
Download macFUSE 5.x from https://macfuse.github.io/ and run its installer.
That's a manual step for you: macOS asks for your password and approval.
Then run ./install.sh again.
MSG
  exit 1
fi
FVER="$(defaults read /Library/Filesystems/macfuse.fs/Contents/Info CFBundleShortVersionString 2>/dev/null || echo unknown)"
case "$FVER" in 5.*|6.*|unknown) ;; *) die "macFUSE $FVER found; version 5 or later is needed for the FSKit backend. Update from https://macfuse.github.io/";; esac
ok "macFUSE $FVER"

step "Building"
make -s clean >/dev/null
make -s BREW="$BREW"
make -s test >/dev/null && ok "unit tests passed" || die "unit tests failed; nothing was installed."
ok "built ./kindle-mtpfs"

step "Installing to $PREFIX"
mkdir -p "$PREFIX" 2>/dev/null || die "can't create $PREFIX. Pick a folder you can write to with --prefix."
[ -w "$PREFIX" ] || die "$PREFIX isn't writable. This script doesn't use sudo; pick another folder with --prefix (default ~/.local/bin)."
install -m 755 kindle-mtpfs scripts/kindle-mount scripts/kindle-unmount "$PREFIX/"
ok "installed kindle-mount, kindle-unmount and kindle-mtpfs"

step "Checking the install (dry run: doesn't touch the Kindle)"
"$PREFIX/kindle-mtpfs" --help >/dev/null || die "kindle-mtpfs didn't start; check that macFUSE and libmtp are installed correctly."
ok "kindle-mtpfs starts and finds libfuse3 and libmtp"
"$PREFIX/kindle-mount" --help >/dev/null && ok "kindle-mount runs"
case ":$PATH:" in
  *":$PREFIX:"*) ok "$PREFIX is on your PATH" ;;
  *) echo "  note  $PREFIX isn't on your PATH. Add this line to ~/.zshrc:"
     echo "        export PATH=\"$PREFIX:\$PATH\"" ;;
esac

cat <<MSG

Done. One manual step, only once (this script never changes System Settings):

  $FSKIT_STEP

Don't enable kernel extensions in Recovery Mode; that isn't needed.

Then quit Calibre, plug in and unlock the Kindle, and run:
  kindle-mount        # read-write: deletes on the Kindle are permanent
  kindle-mount --ro   # read-only
  kindle-unmount      # when you're done (or eject "Kindle" in Finder)
MSG
