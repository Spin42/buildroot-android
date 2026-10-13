#!/bin/sh
#
# Read (or clear) the U-Boot debug log kept in RAM (CONFIG_DEBUG_UART_RAMLOG)
# through lk2nd's fastboot after a warm reset.
#
# Usage: uboot-ramlog.sh <serial> [clear]
# RAMLOG_BASE overrides the buffer address (CONFIG_DEBUG_UART_BASE).

set -e

SERIAL="$1"
BASE=${RAMLOG_BASE:-0x68000000}
MAX=0x100000
[ -n "$SERIAL" ] || { echo "usage: $0 <serial> [clear]" >&2; exit 1; }

readl() {
	timeout 5 fastboot -s "$SERIAL" oem debug readl "$1" 2>&1 | sed -n 's/^(bootloader) //p' | head -n 1
}

if [ "$2" = "clear" ]; then
	timeout 5 fastboot -s "$SERIAL" oem debug writel "$BASE 0" >/dev/null 2>&1
	exit 0
fi

magic=$(readl $BASE)
if [ "$magic" != "0x474f4c52" ]; then
	echo "no log (magic $magic)" >&2
	exit 1
fi
# DRAM is not refreshed during the reset, a few bits may have decayed
len=$(( $(readl $((BASE + 4))) & (MAX - 1) ))
echo "log: $len bytes" >&2

addr=$((BASE + 16))
end=$((addr + len))
while [ $addr -lt $end ]; do
	printf '%08x\n' $(( $(readl $addr) ))
	addr=$((addr + 4))
done | python3 -c '
import sys
data = b"".join(int(l, 16).to_bytes(4, "little") for l in sys.stdin.read().split())
sys.stdout.write(data[:'"$len"'].decode("latin-1").replace("\r", ""))
'
