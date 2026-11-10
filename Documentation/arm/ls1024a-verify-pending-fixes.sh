#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Run ON THE BOARD (as root), after booting the uImage built from this
# branch, to check the three fixes documented in "Three fixes/features
# ported from petoknm/linux, and one that was withdrawn" in
# ls1024a-wdmycloud.rst:
#
#   1) CPU1 hotplug/wakeup fix          (arch/arm/mach-ls1024a/platsmp.c)
#   2) SATA link power management must stay OFF (withdrawn: it makes the
#      link time out and reset on this board)
#   3) clocksource/clockevent timer upgrade (sched_clock precision)
#   4) PFE software TSO (GSO batching)
#
# None of these is a self-contained kernel selftest -- this just checks
# observable, user-visible symptoms of each fix via sysfs/dmesg/ethtool.
# Exit status is the number of FAILed checks (0 = all good).
#
# Usage: ./ls1024a-verify-pending-fixes.sh [iperf3-peer-ip]
#        (the TSO throughput check only runs if a peer IP is given and
#        iperf3 is installed on both ends)

fail=0
pass() { printf 'PASS: %s\n' "$1"; }
warn() { printf 'WARN: %s\n' "$1"; }
bad()  { printf 'FAIL: %s\n' "$1"; fail=$((fail + 1)); }

echo "=== 1) CPU1 hotplug/wakeup fix ==="
if [ ! -e /sys/devices/system/cpu/cpu1/online ]; then
	bad "cpu1 sysfs online node missing -- CONFIG_HOTPLUG_CPU not in this kernel?"
else
	ok=1
	i=1
	while [ "$i" -le 5 ]; do
		echo 0 > /sys/devices/system/cpu/cpu1/online 2>/dev/null
		sleep 1
		echo 1 > /sys/devices/system/cpu/cpu1/online 2>/dev/null
		sleep 1
		state=$(cat /sys/devices/system/cpu/cpu1/online 2>/dev/null)
		if [ "$state" != "1" ]; then
			bad "cpu1 did not come back online after offline/online cycle $i (state='$state')"
			ok=0
			break
		fi
		i=$((i + 1))
	done
	if [ "$ok" = "1" ]; then
		pass "cpu1 survived 5 offline/online cycles"
	fi
	if dmesg | grep -qi "did not reach standby"; then
		bad "dmesg shows a 'did not reach standby' warning from ls1024a_cpu_kill()"
	fi
fi

echo
echo "=== 2) SATA link power management left alone ==="
for lpm in /sys/class/scsi_host/host*/link_power_management_policy; do
	[ -r "$lpm" ] || { warn "no SCSI/SATA host found to check"; break; }
	policy=$(cat "$lpm")
	case "$policy" in
	keep_firmware_settings|max_performance)
		pass "$lpm is '$policy'" ;;
	*)
		bad "$lpm is '$policy' -- on this board a low-power link policy makes commands time out (SError PHYRdyChg/CommWake, hard reset); set it back to max_performance" ;;
	esac
done
if dmesg | grep -q "ata[0-9]*: hard resetting link"; then
	bad "dmesg shows a SATA hard link reset since boot"
else
	pass "no SATA hard link reset in dmesg"
fi

echo
echo "=== 3) clocksource/clockevent timer upgrade ==="
cs="/sys/devices/system/clocksource/clocksource0/current_clocksource"
if [ -r "$cs" ]; then
	src=$(cat "$cs")
	if [ "$src" = "ls1024a-timer2" ]; then
		pass "clocksource is ls1024a-timer2"
	else
		bad "clocksource is '$src', expected ls1024a-timer2"
	fi
else
	bad "no $cs"
fi
# The kernel does not log clockevent names; ask sysfs. ls1024a-timer1 is the
# broadcast device, each CPU keeps its local timer (twd).
bc=$(cat /sys/devices/system/clockevents/broadcast/current_device 2>/dev/null)
if [ "$bc" = "ls1024a-timer1" ]; then
	pass "broadcast clockevent is ls1024a-timer1"
else
	bad "broadcast clockevent is '$bc', expected ls1024a-timer1"
fi
if dmesg | grep -q "sched_clock: 32 bits at 200MHz"; then
	pass "sched_clock runs off the 200 MHz hardware timer"
else
	warn "no 'sched_clock: 32 bits at 200MHz' line in dmesg (ring buffer may have wrapped)"
fi
if dmesg | grep -q "sched_clock: 32 bits at 250 Hz"; then
	bad "dmesg still shows the old coarse jiffies-based sched_clock message"
else
	pass "no leftover coarse jiffies sched_clock message in dmesg"
fi

echo
echo "=== 4) PFE software TSO ==="
iface="${PFE_IFACE:-eth0}"
if command -v ethtool >/dev/null 2>&1; then
	tso=$(ethtool -k "$iface" 2>/dev/null | awk '/^tcp-segmentation-offload:/{print $2}')
	if [ "$tso" = "on" ]; then
		pass "$iface advertises tcp-segmentation-offload: on"
	else
		bad "$iface tcp-segmentation-offload is '$tso', expected on"
	fi
else
	warn "ethtool not installed, skipping TSO feature check"
fi

# iperf3 is pinned to CPU1 and only its summary is shown: unpinned, the result
# moves by more than the TSO effect depending on which CPU the sending thread
# lands on, and the per-second lines can show bogus "0 Bytes" intervals (the
# reporting thread waiting for the socket lock -- see the RST doc).
peer="$1"
if [ -n "$peer" ] && command -v iperf3 >/dev/null 2>&1 && command -v ethtool >/dev/null 2>&1; then
	rate() { taskset -c 1 iperf3 -c "$peer" -t 10 2>/dev/null | awk '/sender/ {print $(NF-3), $(NF-2), "retr", $(NF-1)}'; }
	on=$(rate)
	ethtool -K "$iface" tso off >/dev/null 2>&1
	off=$(rate)
	ethtool -K "$iface" tso on >/dev/null 2>&1
	echo "TSO on : ${on:-<no result>}"
	echo "TSO off: ${off:-<no result>}"
	echo "Compare by hand: TSO on should be >= TSO off, with 0 retransmits in both."
else
	warn "no peer IP given (or no iperf3/ethtool) -- skipping throughput comparison; run with: $0 <peer-ip>"
fi

echo
echo "=== Summary: $fail FAIL(s) ==="
exit "$fail"
