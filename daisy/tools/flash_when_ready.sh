#!/bin/sh
# Wait for the Daisy Seed's DFU bootloader device (0483:df11), then flash it
# with dfu-util. The Seed must already be in DFU mode by hand: hold BOOT, tap
# RESET, release BOOT; running firmware cannot put itself into DFU.
#
# Usage: daisy/tools/flash_when_ready.sh [FILE.bin]
#   FILE.bin defaults to daisy/build/lex224.bin, resolved relative to this
#   script (so it works from any working directory).
#
# Polls `dfu-util -l` every 0.2 s for up to 120 s, then runs:
#   dfu-util -a 0 -s 0x90040000:leave -D FILE.bin -d ,0483:df11
set -e
script_dir=$(cd "$(dirname "$0")" && pwd)
file=${1:-"$script_dir/../build/lex224.bin"}

for i in $(seq 1 600); do
    if dfu-util -l 2>/dev/null | grep -q '0483:df11'; then
        exec dfu-util -a 0 -s 0x90040000:leave -D "$file" -d ,0483:df11
    fi
    sleep 0.2
done
echo "no DFU device within 120 s" >&2
exit 1
