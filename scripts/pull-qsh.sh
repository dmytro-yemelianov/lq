#! /bin/sh
# pull-qsh.sh — copy QRV's userland/sh tree into QSOE's userland/qsh/.
#
# QRV's shell is the mksh-derived "mksh" shell. We rename to "qsh" and
# attempt to compile it against musl + libqsoe — the goal at v0.6.2 is
# NOT to make it work, just to collect the missing symbols/headers so
# Yuri can analyse what to do next.
#
# Run once. Re-running overwrites; uncommitted local changes to
# userland/qsh/* would be lost.

set -e

SRC="${SRC:-/home/yuriz/proj/QRV-OS/userland/sh}"
DST="$(cd "$(dirname "$0")/.." && pwd)/userland/qsh"

if [ ! -d "$SRC" ]; then
    echo "pull-qsh.sh: source $SRC not found" >&2
    exit 1
fi

mkdir -p "$DST"
cp -r "$SRC"/. "$DST/"
echo "pull-qsh.sh: copied $(find "$DST" -name '*.c' | wc -l) C files," \
     "$(find "$DST" -name '*.h' | wc -l) headers into $DST"
