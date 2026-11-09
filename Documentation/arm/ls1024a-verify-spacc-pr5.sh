#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Run ON THE BOARD (as root) to verify PR #5 ("crypto: ls1024a: Add SPACC
# crypto engine driver", merged 2026-11-09, commit f2744470b) actually
# works on real hardware: the SPACC block probes, ecb(aes)/cbc(aes) are
# registered as the HIGH-PRIORITY (hardware) implementation rather than
# software, the kernel's own crypto self-test doesn't flag a failure, and
# (best-effort, only if the tools exist) a real encrypt/decrypt roundtrip
# through the exact hardware driver succeeds and a speed number is taken.
#
# Exit status = number of FAILed checks (0 = all good). WARN is not a
# failure -- it just means a given check couldn't run (missing tool).

fail=0
pass() { printf 'PASS: %s\n' "$1"; }
warn() { printf 'WARN: %s\n' "$1"; }
bad()  { printf 'FAIL: %s\n' "$1"; fail=$((fail + 1)); }

echo "=== 1) Driver probed ==="
if dmesg | grep -q "SPACC crypto engine probed"; then
	dmesg | grep "SPACC crypto engine probed" | tail -1
	pass "SPACC probe message found in dmesg"
else
	bad "no 'SPACC crypto engine probed' line in dmesg -- driver never probed (wrong compatible string, clock/reset failure, or dmesg ring buffer already wrapped past it)"
fi

echo
echo "=== 2) Algorithms registered as the HARDWARE (priority 300) implementation, not software fallback ==="
for pair in "ecb(aes):ecb-aes-ls1024a-spacc" "cbc(aes):cbc-aes-ls1024a-spacc"; do
	cra_name=${pair%%:*}
	driver_name=${pair##*:}

	# /proc/crypto lists one block per registered transform; find the block
	# whose "name" line matches cra_name, then check ITS driver/priority.
	block=$(awk -v want="$cra_name" '
		/^name[ \t]*:/ { name=$0; sub(/^name[ \t]*:[ \t]*/, "", name) }
		/^driver[ \t]*:/ { driver=$0; sub(/^driver[ \t]*:[ \t]*/, "", driver) }
		/^priority[ \t]*:/ { priority=$0; sub(/^priority[ \t]*:[ \t]*/, "", priority) }
		/^type[ \t]*:/ {
			if (name == want) print driver "|" priority
		}
	' /proc/crypto)

	if [ -z "$block" ]; then
		bad "$cra_name: no registered skcipher transform found in /proc/crypto at all"
		continue
	fi

	selected_driver=$(echo "$block" | head -1 | cut -d'|' -f1)
	selected_prio=$(echo "$block" | head -1 | cut -d'|' -f2)

	if [ "$selected_driver" = "$driver_name" ] && [ "$selected_prio" -ge 300 ] 2>/dev/null; then
		pass "$cra_name -> driver=$selected_driver priority=$selected_prio (hardware, as expected)"
	else
		bad "$cra_name -> driver=$selected_driver priority=$selected_prio (expected driver=$driver_name priority>=300 -- hardware path is NOT the one the crypto API will actually pick)"
	fi

	if grep -q "^driver[ \t]*: $driver_name\$" /proc/crypto; then
		pass "$driver_name is present in /proc/crypto at all (confirms the hardware transform itself registered, regardless of which one 'wins' by priority)"
	else
		bad "$driver_name does not appear anywhere in /proc/crypto -- the hardware transform never registered"
	fi
done

echo
echo "=== 3) Kernel's own crypto self-test (runs automatically at registration if CONFIG_CRYPTO_MANAGER_TESTS=y) ==="
if dmesg | grep -i "ls1024a-spacc" | grep -qi "fail"; then
	dmesg | grep -i "ls1024a-spacc" | grep -i "fail"
	bad "dmesg has a failure line mentioning ls1024a-spacc (self-test or probe failure)"
else
	pass "no failure line mentioning ls1024a-spacc anywhere in dmesg"
fi

echo
echo "=== 4) Speed (tcrypt), best-effort -- needs CONFIG_CRYPTO_TEST / tcrypt.ko available ==="
if modprobe -q tcrypt mode=10 sec=1 2>/dev/null; then
	sleep 2
	dmesg | tail -40 | grep -iE "testing speed of|cbc-aes-ls1024a-spacc|ecb-aes-ls1024a-spacc|ecb(aes)|cbc(aes)" || warn "tcrypt ran but no matching speed-test lines found in the recent dmesg tail -- check 'dmesg' manually"
	modprobe -r tcrypt 2>/dev/null
	pass "tcrypt mode=10 ran -- see dmesg output above for the throughput numbers to compare against the previously-claimed 3-5x speedup"
else
	warn "tcrypt module not available/loadable (CONFIG_CRYPTO_TEST not built, or not installed on this rootfs) -- skipping speed test"
fi

echo
echo "=== 5) Real encrypt/decrypt roundtrip through the exact hardware driver (best-effort, needs python3) ==="
if command -v python3 >/dev/null 2>&1; then
	python3 - <<'PYEOF'
import socket, os, sys

def roundtrip(driver_name, keylen, ivlen):
    key = os.urandom(keylen)
    iv = os.urandom(ivlen)
    plaintext = os.urandom(4096)

    try:
        algo = socket.socket(socket.AF_ALG, socket.SOCK_SEQPACKET, 0)
        algo.bind((driver_name, 0))
    except OSError as e:
        print("WARN: could not bind AF_ALG socket to driver '%s': %s (kernel AF_ALG support missing, or driver name not found by exact match)" % (driver_name, e))
        return None

    algo.setsockopt(socket.SOL_ALG, socket.ALG_SET_KEY, key)
    op, _ = algo.accept()
    try:
        if ivlen:
            op.sendmsg_afalg([plaintext], op=socket.ALG_OP_ENCRYPT, iv=iv)
        else:
            op.sendmsg_afalg([plaintext], op=socket.ALG_OP_ENCRYPT)
        ciphertext = op.recv(len(plaintext))

        if ivlen:
            op.sendmsg_afalg([ciphertext], op=socket.ALG_OP_DECRYPT, iv=iv)
        else:
            op.sendmsg_afalg([ciphertext], op=socket.ALG_OP_DECRYPT)
        decrypted = op.recv(len(ciphertext))
    finally:
        op.close()
        algo.close()

    return decrypted == plaintext

results = []
for driver, keylen, ivlen in [("ecb-aes-ls1024a-spacc", 16, 0), ("cbc-aes-ls1024a-spacc", 16, 16)]:
    r = roundtrip(driver, keylen, ivlen)
    if r is None:
        results.append((driver, "WARN"))
    elif r:
        results.append((driver, "PASS"))
    else:
        results.append((driver, "FAIL"))

fails = 0
for driver, status in results:
    if status == "PASS":
        print("PASS: %s AES-128 4KiB encrypt+decrypt roundtrip matches original plaintext (hardware path exercised directly by driver name)" % driver)
    elif status == "FAIL":
        print("FAIL: %s roundtrip MISMATCH -- decrypted data does not match original plaintext" % driver)
        fails += 1
    else:
        pass  # WARN already printed above
sys.exit(fails)
PYEOF
	py_rc=$?
	if [ "$py_rc" -gt 0 ]; then
		fail=$((fail + py_rc))
	fi
else
	warn "python3 not available -- skipping the direct AF_ALG encrypt/decrypt roundtrip test (structural checks in sections 1-3 above still stand on their own)"
fi

echo
echo "=== Summary: $fail FAIL(s) ==="
exit "$fail"
