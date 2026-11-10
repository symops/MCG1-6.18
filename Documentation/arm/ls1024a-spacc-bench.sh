#!/bin/sh
# AES-CBC throughput: software (OpenSSL's own AES, user space) vs the SPACC engine via AF_ALG.
# Run on the board as root. Needs the afalg engine of OpenSSL and the ls1024a-spacc module.
# Requests below 8 KiB are computed in software by the driver itself (SPACC_MIN_HW_BYTES).
# usage: sh ls1024a-spacc-bench.sh [128|256] [seconds]
BITS=${1:-128}
SEC=${2:-3}
ALG=aes-$BITS-cbc
SIZES="1024 4096 8192 16384 32768 65536 131072 262144 524288 1048576"

grep -q ls1024a-spacc /proc/crypto || { echo "SPACC driver not loaded: modprobe ls1024a-spacc"; exit 1; }
OLDW=$(sysctl -n net.core.wmem_default); OLDR=$(sysctl -n net.core.rmem_default)
# one AF_ALG request must fit into the socket buffers, or sendmsg() blocks
sysctl -q -w net.core.wmem_default=4194304 net.core.rmem_default=4194304

ops() { # $1 = extra openssl args, $2 = block size -> "ops errors"
    out=$(openssl speed $1 -evp $ALG -seconds $SEC -bytes $2 2>&1)
    o=$(echo "$out" | sed -n 's/.*size blocks: \([0-9]*\) .*/\1/p')
    e=$(echo "$out" | grep -c "kernel op failed")
    echo "${o:-0} $e"
}

echo "$ALG, $SEC s per run, $(uname -r)"
printf "%9s %10s %10s %7s %s\n" "block" "soft MB/s" "SPACC MB/s" "ratio" "errors"
for n in $SIZES; do
    set -- $(ops "" $n); so=$1
    set -- $(ops "-engine afalg" $n); ho=$1; he=$2
    awk -v n=$n -v so=$so -v ho=$ho -v he=$he -v s=$SEC 'BEGIN {
        a = so * n / s / 1e6; b = ho * n / s / 1e6;
        printf "%9d %10.1f %10.1f %6.2fx %s\n", n, a, b, (a > 0 ? b / a : 0), (he ? he " (!)" : "-") }'
done
sysctl -q -w net.core.wmem_default=$OLDW net.core.rmem_default=$OLDR
dmesg | grep -i "spacc" | tail -5
