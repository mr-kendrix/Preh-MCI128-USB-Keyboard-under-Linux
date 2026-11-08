#!/bin/sh
# usage: run_oracle.sh /path/to/dir-with-mwxc32.dll+mwxc_loader.exe in.mwf out.mwx
set -e
d="$1"; in="$(readlink -f "$2")"; out="$(readlink -f -m "$3")"
cd "$d"
WINEDEBUG=-all WINEPREFIX="${WINEPREFIX:-$HOME/.wine-pkt}" wine mwxc_loader.exe "$in" "$out"
