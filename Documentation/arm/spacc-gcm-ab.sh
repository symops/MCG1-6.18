#!/bin/sh
# GCM: движок против софта, один и тот же замер.
#
# Оба прогона делают ровно одно и то же: --gcm-only на gcm(aes). Меняется
# только то, какой CTR подставит шаблон gcm_base. Поэтому отношение из этой
# таблицы — отношение GCM к GCM, без подмены на чистый CTR.
#
# Драйвер возвращается на место в любом случае, включая прерывание.

TEST=${TEST:-spacc_selftest.py}
MODULE_FILE=${MODULE_FILE:-ls1024a-spacc.ko}
PY=${PY:-python3}

HW=$(mktemp)
SW=$(mktemp)

restore() {
	if ! lsmod | grep -q '^ls1024a_spacc'; then
		if [ -f "$MODULE_FILE" ]; then
			insmod "$MODULE_FILE" 2>/dev/null || true
		else
			modprobe ls1024a_spacc 2>/dev/null || \
				echo "верните модуль: insmod <путь>/ls1024a-spacc.ko" >&2
		fi
	fi
}
trap restore EXIT INT TERM

echo "== движок =="
$PY "$TEST" --gcm-only | tee "$HW"

systemctl stop ksmbd 2>/dev/null || true
if ! rmmod ls1024a_spacc; then
	echo "не удалось выгрузить модуль — занят?" >&2
	exit 1
fi

echo
echo "== софт =="
$PY "$TEST" --gcm-only | tee "$SW"

$PY - "$HW" "$SW" <<'EOF'
import re
import sys

def rates(path):
    out = {}
    for line in open(path, encoding="utf-8"):
        m = re.match(r"\s+(\d+)\s+([\d.]+) МБ/с", line)
        if m:
            out[int(m.group(1))] = float(m.group(2))
    return out

hw = rates(sys.argv[1])
sw = rates(sys.argv[2])
print()
print("  размер      движок    софт   отношение")
for n in sorted(hw):
    if n in sw and sw[n]:
        print("  %7d  %8.2f  %6.2f   x%.2f"
              % (n, hw[n], sw[n], hw[n] / sw[n]))
EOF
