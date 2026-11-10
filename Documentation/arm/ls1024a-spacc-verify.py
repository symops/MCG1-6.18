#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Data check for the ls1024a-spacc driver through AF_ALG, against OpenSSL's software AES.

Run on the board as root:  python3 ls1024a-spacc-verify.py [rounds]

Each case sends one request of a given size and reads the result into a set of
user buffers.  The buffer layout decides how the destination scatterlist looks
to the driver:

  whole   one buffer          -> page-sized entries, the common case
  frag    many small buffers  -> more entries than one DDT holds, so the driver
                                 has to cut the request by entry count
  tiny    64-byte buffers     -> one DDT covers less than the engine threshold,
                                 so the driver finishes in software

Cases mix sizes on ONE socket on purpose (small request, then large), which
also checks that a synchronously completed request does not disturb the next
asynchronous one.  Needs the afalg-capable kernel modules (algif_skcipher) and
the openssl command.
"""
import os
import socket
import subprocess
import sys

KEYS = {128: bytes(range(16)), 256: bytes(range(32))}
IV = bytes(range(16, 32))
SYSCTLS = ('/proc/sys/net/core/wmem_default', '/proc/sys/net/core/rmem_default')
# (label, request bytes, destination buffer size; 0 = one buffer)
CASES = [
    ('small  4K whole', 4096, 0),
    ('large 256K whole', 262144, 0),
    ('small  1K whole', 1024, 0),
    ('16K whole', 16384, 0),
    ('132K whole (128K + 4K tail)', 135168, 0),
    ('1M whole (8 chunks)', 1048576, 0),
    ('256K frag 256', 262144, 256),
    ('256K frag 1008', 262144, 1008),
    ('64K tiny 64', 65536, 64),
    ('small  4K whole', 4096, 0),
    ('512K frag 512', 524288, 512),
]


def reference(bits, data, decrypt):
    cmd = ['openssl', 'enc', '-aes-%d-cbc' % bits, '-nopad', '-K', KEYS[bits].hex(), '-iv', IV.hex()]
    if decrypt:
        cmd.append('-d')
    return subprocess.run(cmd, input=data, stdout=subprocess.PIPE, check=True).stdout


def run(op, data, bufsize, decrypt):
    """One request on the operation socket; returns the output bytes."""
    n = len(data)
    if bufsize:
        bufs = [bytearray(min(bufsize, n - o)) for o in range(0, n, bufsize)]
    else:
        bufs = [bytearray(n)]
    sent = op.sendmsg_afalg([data], op=socket.ALG_OP_DECRYPT if decrypt else socket.ALG_OP_ENCRYPT, iv=IV)
    if sent != n:
        raise RuntimeError('short send %d of %d (socket buffer too small?)' % (sent, n))
    got = 0
    views = [memoryview(b) for b in bufs]
    while got < n:
        # recvmsg takes at most 1024 buffers per call
        r = op.recvmsg_into(views[:1024])[0]
        if r <= 0:
            raise RuntimeError('recvmsg returned %d after %d of %d bytes' % (r, got, n))
        got += r
        while r:
            if r >= len(views[0]):
                r -= len(views[0])
                views.pop(0)
            else:
                views[0] = views[0][r:]
                r = 0
    return b''.join(bytes(b) for b in bufs)


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    with open('/proc/crypto') as f:
        if 'ls1024a-spacc' not in f.read():
            sys.exit('SPACC driver not loaded: modprobe ls1024a-spacc')
    old = []
    for p in SYSCTLS:
        with open(p) as f:
            old.append(f.read())
        with open(p, 'w') as f:
            f.write('4194304')
    bad = 0
    try:
        for bits in (128, 256):
            alg = socket.socket(socket.AF_ALG, socket.SOCK_SEQPACKET, 0)
            alg.bind(('skcipher', 'cbc(aes)'))
            alg.setsockopt(socket.SOL_ALG, socket.ALG_SET_KEY, KEYS[bits])
            op, _ = alg.accept()
            for rnd in range(rounds):
                for label, n, bufsize in CASES:
                    plain = os.urandom(n)
                    for decrypt in (False, True):
                        src = reference(bits, plain, False) if decrypt else plain
                        want = plain if decrypt else reference(bits, plain, False)
                        what = 'AES-%d %s %s' % (bits, 'dec' if decrypt else 'enc', label)
                        try:
                            out = run(op, src, bufsize, decrypt)
                        except (OSError, RuntimeError) as e:
                            print('FAIL  %-44s %s' % (what, e))
                            bad += 1
                            # the socket state is undefined after an error: start a new one
                            op.close()
                            op, _ = alg.accept()
                            continue
                        if out != want:
                            first = next(i for i in range(n) if out[i] != want[i])
                            print('FAIL  %-44s data differs from byte %d' % (what, first))
                            bad += 1
                        elif rnd == 0:
                            print('ok    %s' % what)
            op.close()
            alg.close()
    finally:
        for p, v in zip(SYSCTLS, old):
            with open(p, 'w') as f:
                f.write(v)
    total = rounds * len(CASES) * 4
    print('%d of %d checks failed' % (bad, total) if bad else 'ALL %d CHECKS PASSED' % total)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
