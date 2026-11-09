#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Run ON THE BOARD (as root), after booting the uImage built from this
# branch, to check the four pending fixes documented in
# "Four more fixes/features, ported from petoknm/linux (pending hardware
# test)" in ls1024a-wdmycloud.rst:
#
#   1) CPU1 hotplug/wakeup fix          (arch/arm/mach-ls1024a/platsmp.c)
#   2) PCIe ASPM / SATA ALPM power saving
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
echo "=== 2) PCIe ASPM / SATA ALPM power saving ==="
lpm="/sys/class/scsi_host/host0/link_power_management_policy"
if [ -r "$lpm" ]; then
	policy=$(cat "$lpm")
	if [ "$policy" = "max_performance" ]; then
		bad "SATA LPM policy is '$policy' (expected med_power_with_dipm) -- AHCI_HFLAG_YES_ALPM not taking effect?"
	else
		pass "SATA LPM policy is '$policy'"
	fi
else
	warn "no $lpm -- no SCSI/SATA host found to check"
fi
if command -v lspci >/dev/null 2>&1; then
	if lspci -vv 2>/dev/null | grep -q "ASPM.*Enabled"; then
		pass "at least one PCIe link reports ASPM Enabled"
	else
		warn "no PCIe link reporting ASPM Enabled (maybe nothing is plugged into PCIe on this board)"
	fi
else
	warn "lspci not installed, skipping PCIe ASPM check"
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
if dmesg | grep -q "clockevents: ls1024a-timer1 added\|ls1024a-timer1"; then
	pass "ls1024a-timer1 clockevent registered (see dmesg)"
else
	bad "no ls1024a-timer1 clockevent registration found in dmesg"
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

peer="$1"
if [ -n "$peer" ] && command -v iperf3 >/dev/null 2>&1; then
	echo "Running iperf3 to $peer (TSO on, then off, for comparison)..."
	iperf3 -c "$peer" -t 10 || warn "iperf3 TSO-on run failed/unreachable"
	if command -v ethtool >/dev/null 2>&1; then
		ethtool -K "$iface" tso off 2>/dev/null
		iperf3 -c "$peer" -t 10 || warn "iperf3 TSO-off run failed/unreachable"
		ethtool -K "$iface" tso on 2>/dev/null
		echo "Compare the two throughput numbers above by hand: TSO-on should be >= TSO-off."
	fi
else
	warn "no peer IP given (or no iperf3) -- skipping throughput comparison; run with: $0 <peer-ip>"
fi

echo
echo "=== Summary: $fail FAIL(s) ==="
exit "$fail"
