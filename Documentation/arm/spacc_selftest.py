#!/usr/bin/env python3
"""Самопроверка SPACC без внешних утилит.

Сравнивает аппаратные реализации с программными из того же ядра, обе через
AF_ALG. Ничего, кроме python3 и стандартной библиотеки, не нужно.

Запуск:
    sudo python3 spacc_selftest.py                 # всё, до 4 МиБ
    sudo python3 spacc_selftest.py ctr cbc         # только указанные
    sudo python3 spacc_selftest.py --sweep         # искать границу сбоя CTR
    sudo python3 spacc_selftest.py --max-size=8388608
"""

import os
import socket
import subprocess
import struct
import sys
import threading
import time

SOL_ALG = 279
ALG_SET_KEY = 1
ALG_SET_IV = 2
ALG_SET_OP = 3
ALG_SET_AEAD_ASSOCLEN = 4
ALG_SET_AEAD_AUTHSIZE = 5
ALG_OP_ENCRYPT = 1
ALG_OP_DECRYPT = 0

OUR_MODULE = "ls1024a_spacc"
BIG = 16 << 20
MAX_SIZE = 4 << 20


# --------------------------------------------------------------------------
# AF_ALG
# --------------------------------------------------------------------------
def _tune(sock):
    for opt in (socket.SO_SNDBUF, socket.SO_RCVBUF):
        try:
            sock.setsockopt(socket.SOL_SOCKET, opt, BIG)
        except OSError:
            pass


SOFT_MODULES = ("aes_arm_bs", "aes_arm", "aes_generic", "cbc", "ecb", "ctr",
                "gcm", "ghash_arm_ce", "sha1", "sha256", "cmac")


def load_software():
    """Софтовые реализации появляются в /proc/crypto только после загрузки."""
    if os.geteuid() != 0:
        return
    for mod in SOFT_MODULES:
        subprocess.run(["modprobe", mod], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=False)


def _raise_buffers():
    """AF_ALG не принимает запросы больше буфера сокета."""
    if os.geteuid() != 0:
        return
    for path, val in (("/proc/sys/net/core/wmem_max", BIG),
                      ("/proc/sys/net/core/rmem_max", BIG),
                      ("/proc/sys/net/core/wmem_default", BIG),
                      ("/proc/sys/net/core/rmem_default", BIG)):
        try:
            with open(path, "w") as f:
                f.write(str(val))
        except OSError:
            pass


def skcipher(name, key, iv, data, encrypt=True):
    """Один запрос через AF_ALG. Возвращает байты результата."""
    s = socket.socket(socket.AF_ALG, socket.SOCK_SEQPACKET, 0)
    try:
        s.bind(("skcipher", name))
        _tune(s)
        if key is not None:
            s.setsockopt(SOL_ALG, ALG_SET_KEY, key)
        op, _ = s.accept()
        try:
            _tune(op)
            cmsg = [(SOL_ALG, ALG_SET_OP,
                     struct.pack("I", ALG_OP_ENCRYPT if encrypt
                                 else ALG_OP_DECRYPT))]
            if iv is not None:
                cmsg.append((SOL_ALG, ALG_SET_IV,
                             struct.pack("I", len(iv)) + iv))
            op.sendmsg([data], cmsg)
            out = bytearray()
            while len(out) < len(data):
                piece = op.recv(min(1 << 20, len(data) - len(out)))
                if not piece:
                    break
                out += piece
            return bytes(out)
        finally:
            op.close()
    finally:
        s.close()


def hash_digest(name, key, data, digestsize):
    s = socket.socket(socket.AF_ALG, socket.SOCK_SEQPACKET, 0)
    try:
        s.bind(("hash", name))
        if key is not None:
            s.setsockopt(SOL_ALG, ALG_SET_KEY, key)
        op, _ = s.accept()
        try:
            op.sendmsg([data])
            return op.recv(digestsize)
        finally:
            op.close()
    finally:
        s.close()


def aead_encrypt(name, key, iv, aad, data, authsize):
    s = socket.socket(socket.AF_ALG, socket.SOCK_SEQPACKET, 0)
    try:
        s.bind(("aead", name))
        _tune(s)
        s.setsockopt(SOL_ALG, ALG_SET_KEY, key)
        s.setsockopt(SOL_ALG, ALG_SET_AEAD_AUTHSIZE, None, authsize)
        op, _ = s.accept()
        try:
            _tune(op)
            cmsg = [(SOL_ALG, ALG_SET_OP, struct.pack("I", ALG_OP_ENCRYPT)),
                    (SOL_ALG, ALG_SET_IV, struct.pack("I", len(iv)) + iv),
                    (SOL_ALG, ALG_SET_AEAD_ASSOCLEN,
                     struct.pack("I", len(aad)))]
            op.sendmsg([aad + data], cmsg)
            want = len(data) + authsize
            out = bytearray()
            while len(out) < want:
                piece = op.recv(want - len(out))
                if not piece:
                    break
                out += piece
            return bytes(out)
        finally:
            op.close()
    finally:
        s.close()


# --------------------------------------------------------------------------
# /proc/crypto
# --------------------------------------------------------------------------
def read_proc_crypto():
    entries = []
    cur = {}
    with open("/proc/crypto") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip():
                if cur:
                    entries.append(cur)
                    cur = {}
                continue
            key, _, val = line.partition(":")
            cur[key.strip()] = val.strip()
    if cur:
        entries.append(cur)
    return entries


def implementations(entries, name):
    """Все записи с данным cra_name, лучшие первыми."""
    found = [e for e in entries if e.get("name") == name]
    found.sort(key=lambda e: int(e.get("priority", 0)), reverse=True)
    return found


SW_NAMES = {
    "ecb(aes)": ["ecb-aes-neonbs", "ecb-aes-generic"],
    "cbc(aes)": ["cbc-aes-neonbs", "cbc-aes-generic"],
    "ctr(aes)": ["ctr-aes-neonbs", "ctr-aes-generic"],
    "cmac(aes)": ["cmac(aes-generic)", "cmac-aes-generic"],
    "sha1": ["sha1-lib", "sha1-generic"],
    "sha256": ["sha256-lib", "sha256-generic"],
    "hmac(sha256)": ["hmac-sha256-lib"],
}


def can_bind(kind, name):
    """Существует ли реализация с таким именем драйвера."""
    try:
        s = socket.socket(socket.AF_ALG, socket.SOCK_SEQPACKET, 0)
        try:
            s.bind((kind, name))
            return True
        finally:
            s.close()
    except OSError:
        return False


def pick_pair(entries, name, kind="skcipher"):
    """(наш драйвер, софтовый драйвер, digestsize) или None.

    Софтовый модуль может быть ещё не загружен, поэтому кроме /proc/crypto
    проверяются известные имена драйверов: первое привязываемое и берётся.
    """
    impls = implementations(entries, name)
    ours = [e for e in impls if e.get("module") == OUR_MODULE]
    if not ours:
        return None

    size = ours[0].get("digestsize")

    if kind == "hash":
        # у нас два экземпляра: shash (для ksmbd) и ahash (для AF_ALG)
        for e in ours:
            if can_bind(kind, e["driver"]):
                hw = e["driver"]
                size = e.get("digestsize") or size
                break
        else:
            return None
    else:
        hw = ours[0]["driver"]

    direct = [e["driver"] for e in impls if e.get("module") != OUR_MODULE]
    if kind != "hash" and direct:
        return (hw, direct[0], int(size) if size else None)

    # модуль софтовой реализации мог ещё не загрузиться
    for cand in direct + SW_NAMES.get(name, []):
        for _ in range(3):
            if can_bind(kind, cand):
                return (hw, cand, int(size) if size else None)
            time.sleep(0.05)
    return None


# --------------------------------------------------------------------------
# Сравнение
# --------------------------------------------------------------------------
def first_diff(a, b):
    n = min(len(a), len(b))
    if a[:n] == b[:n]:
        return None if len(a) == len(b) else n
    x = int.from_bytes(a[:n], "big") ^ int.from_bytes(b[:n], "big")
    bits = x.bit_length()
    return n - (bits + 7) // 8


def dump(a, b, off, span=16):
    lo = max(0, off - span)
    hi = min(len(a), off + span)
    return ("  ref[%d:%d] %s\n  got[%d:%d] %s"
            % (lo, hi, a[lo:hi].hex(), lo, hi, b[lo:hi].hex()))


def rate(fn, rounds=3):
    best = None
    for _ in range(rounds):
        t0 = time.perf_counter()
        fn()
        dt = time.perf_counter() - t0
        if best is None or dt < best:
            best = dt
    return best


RESULTS = []

# Ниже этого размера драйвер считает сам, выше — в софтовом fallback.
HW_MIN = {"ecb": 8192, "cbc": 8192, "ctr": 8192, "cmac": 4096}
HASH_LIMIT = 64 << 10


def path_note(name, size):
    base = name.split("(")[0]
    if base in ("cmac", "sha1", "sha256", "hmac") and size > HASH_LIMIT:
        return "сверх 64 КиБ (AF_ALG)"
    th = HW_MIN.get(base)
    if th is None:
        return ""
    return "движок" if size >= th else "софт"


def report(alg, size, ok, diff, hw_rate=None, sw_rate=None, note=""):
    RESULTS.append((alg, size, ok))
    line = "  %-14s %9d  %s" % (alg, size, "ok" if ok else "FAIL")
    if not ok and diff is not None:
        line += "  first_diff=%d" % diff
    if hw_rate and sw_rate:
        line += "  %6.1f / %6.1f МБ/с (x%.2f)" % (hw_rate, sw_rate,
                                                  hw_rate / sw_rate)
    if note:
        line += "  " + note
    print(line)


# --------------------------------------------------------------------------
# Наборы тестов
# --------------------------------------------------------------------------
BASE_SKCIPHER = [16, 64, 1024, 8192, 16384, 65536, 131072, 262144, 917504,
                 1048576, 1052672]
BASE_HASH = [3, 64, 1024, 4096, 16384, 65536]


def build_sizes(base, max_size, align=16):
    """Базовые размеры плюс степени двойки до max_size."""
    out = [s for s in base if s <= max_size]
    s = 1 << 20
    while s <= max_size:
        out.append(s)
        if s + (1 << 16) <= max_size:
            out.append(s + (1 << 16))
        s <<= 1
    out.append(max_size)
    return sorted({x for x in out if x and x % align == 0})


SKCIPHER = [
    ("ecb(aes)", BASE_SKCIPHER),
    ("cbc(aes)", BASE_SKCIPHER),
    ("ctr(aes)", BASE_SKCIPHER),
]

HASHES = [
    ("cmac(aes)", 16, BASE_HASH),
]


def test_skcipher(entries, name, sizes, timing=True):
    pair = pick_pair(entries, name)
    if not pair:
        print("  %-14s нет пары (наш драйвер + софт), пропуск" % name)
        return
    hw, sw, _ = pair
    print("  %-14s движок %s, софт %s" % (name, hw, sw))

    key = bytes(range(16))
    iv = None if name.startswith("ecb") else bytes(16)

    for size in sizes:
        data = os.urandom(size)
        try:
            got = skcipher(hw, key, iv, data)
            ref = skcipher(sw, key, iv, data)
        except OSError as exc:
            report(name, size, False, None, note="ошибка AF_ALG: %s" % exc)
            continue

        diff = first_diff(ref, got)
        ok = diff is None
        hw_rate = sw_rate = None
        if timing and ok:
            rounds = 3 if size >= 65536 else 20
            hw_rate = size / rate(lambda: skcipher(hw, key, iv, data), rounds) / 1e6
            sw_rate = size / rate(lambda: skcipher(sw, key, iv, data), rounds) / 1e6
        report(name, size, ok, diff, hw_rate, sw_rate,
               note=path_note(name, size))
        if not ok and diff is not None:
            print(dump(ref, got, diff))

        # расшифровка тем же драйвером должна вернуть исходные данные
        if not name.startswith("ecb") and ok:
            back = skcipher(hw, key, iv, got, encrypt=False)
            rd = first_diff(data, back)
            if rd is not None:
                report(name + " dec", size, False, rd)
                print(dump(data, back, rd))


def test_hashes(entries, name, keylen, sizes):
    pair = pick_pair(entries, name, "hash")
    if not pair:
        print("  %-14s нет пары (наш драйвер + софт), пропуск" % name)
        return
    hw, sw, digestsize = pair
    if not digestsize:
        digestsize = 32
    print("  %-14s движок %s, софт %s, дайджест %d"
          % (name, hw, sw, digestsize))

    key = bytes(range(keylen)) if keylen else None

    # AF_ALG работает только с ahash, а драйвер регистрирует shash: наружу
    # хеш-блок не виден, его проверяет hash-test2.ko.
    try:
        hash_digest(hw, key, b"probe", digestsize)
    except OSError as exc:
        if exc.errno in (2, 95):
            print("  %-14s недоступно через AF_ALG: %s"
                  % (name, exc.strerror))
            print("                 драйвер регистрирует shash, а AF_ALG"
                  " работает только с ahash")
            return
        raise

    for size in sizes:
        data = os.urandom(size)
        try:
            got = hash_digest(hw, key, data, digestsize)
            ref = hash_digest(sw, key, data, digestsize)
        except OSError as exc:
            report(name, size, False, None, note="ошибка AF_ALG: %s" % exc)
            continue
        diff = first_diff(ref, got)
        hw_rate = sw_rate = None
        if diff is None:
            rounds = 3 if size >= 65536 else 20
            hw_rate = size / rate(lambda: hash_digest(hw, key, data,
                                                      digestsize), rounds) / 1e6
            sw_rate = size / rate(lambda: hash_digest(sw, key, data,
                                                      digestsize), rounds) / 1e6
        report(name, size, diff is None, diff, hw_rate, sw_rate,
               note=path_note(name, size))
        if diff is not None:
            print(dump(ref, got, diff))


GCM_VECTOR = {
    "key": bytes(16),
    "iv": bytes(12),
    "aad": b"",
    "pt": bytes(16),
    "ct": bytes.fromhex("0388dace60b6a392f328c2b971b2fe78"),
    "tag": bytes.fromhex("ab6e47d42cec13bdf53a67b21257bddf"),
}


def test_gcm():
    v = GCM_VECTOR
    try:
        out = aead_encrypt("gcm(aes)", v["key"], v["iv"], v["aad"], v["pt"],
                           16)
    except OSError as exc:
        print("  gcm(aes)        пропуск: %s" % exc)
        return
    want = v["ct"] + v["tag"]
    diff = first_diff(want, out)
    report("gcm(aes)", len(v["pt"]), diff is None, diff)
    if diff is not None:
        print(dump(want, out, diff))


def read_param(name):
    try:
        with open("/sys/module/%s/parameters/%s" % (OUR_MODULE, name)) as f:
            return f.read().strip()
    except OSError:
        return None


PIPELINE_ALGS = [("ecb(aes)", "skcipher"), ("cbc(aes)", "skcipher"),
                 ("ctr(aes)", "skcipher"), ("sha256", "hash"),
                 ("cmac(aes)", "hash")]


def test_pipeline(entries, name="cbc(aes)", kind="skcipher", size=65536,
                  threads=4, verify=4, rounds=16):
    """Корректность и скорость под параллельной нагрузкой."""
    pair = pick_pair(entries, name, kind)
    if not pair:
        print("  %-14s нет пары, пропуск" % name)
        return
    hw, sw, digestsize = pair

    depth = read_param("depth")
    peak0 = read_param("stat_peak")
    key = bytes(range(16))
    iv = None if name.startswith("ecb") else bytes(16)
    keyed = name.split("(")[0] in ("cmac", "hmac")
    pool = [os.urandom(size) for _ in range(8)]

    def call(drv, data):
        if kind == "hash":
            return hash_digest(drv, key if keyed else None, data,
                               digestsize or 32)
        return skcipher(drv, key, iv, data)

    errors = []
    barrier = threading.Barrier(threads)

    def verify_worker(tid):
        barrier.wait()
        for i in range(verify):
            data = pool[(tid + i) % len(pool)]
            if call(hw, data) != call(sw, data):
                errors.append(tid)

    th = [threading.Thread(target=verify_worker, args=(i,))
          for i in range(threads)]
    for t in th:
        t.start()
    for t in th:
        t.join()

    def pump(count, nthreads):
        gate = threading.Barrier(nthreads)

        def run(tid):
            gate.wait()
            for i in range(count):
                call(hw, pool[(tid + i) % len(pool)])

        workers = [threading.Thread(target=run, args=(i,))
                   for i in range(nthreads)]
        t0 = time.perf_counter()
        for w in workers:
            w.start()
        for w in workers:
            w.join()
        return time.perf_counter() - t0

    pump(4, threads)                       # прогрев
    single = pump(rounds, 1)
    multi = pump(rounds, threads)

    one_rate = rounds * size / single / 1e6
    many_rate = threads * rounds * size / multi / 1e6
    peak1 = read_param("stat_peak")

    print("  %-14s режим %s, глубина %s, потоков %d, по %d байт"
          % (name, kind, depth, threads, size))
    print("  сверка с софтом: %d запросов, расхождений %d"
          % (threads * verify, len(errors)))
    print("  один поток %6.1f МБ/с, %d потоков %6.1f МБ/с (x%.2f)"
          % (one_rate, threads, many_rate, many_rate / one_rate))
    if peak0 is not None:
        print("  stat_peak: %s -> %s" % (peak0, peak1))
    if depth is not None and depth != "1" and peak1 is not None \
            and int(peak1) < 2:
        print("  ВНИМАНИЕ: глубина %s, но в полёте не было двух команд"
              % depth)

    RESULTS.append((name + " pipeline", size, not errors))

def sweep_ctr(entries, upto, step):
    """Ищем границу, начиная с которой CTR расходится с софтом."""
    pair = pick_pair(entries, "ctr(aes)")
    if not pair:
        print("  нет пары ctr(aes)")
        return
    hw, sw, _ = pair
    key = bytes(range(16))
    iv = bytes(16)
    print("  шаг %d байт, до %d" % (step, upto))
    for size in range(step, upto + 1, step):
        data = bytes(size)
        got = skcipher(hw, key, iv, data)
        ref = skcipher(sw, key, iv, data)
        diff = first_diff(ref, got)
        if diff is None:
            continue
        print("  размер %d: первое расхождение %d" % (size, diff))
        print(dump(ref, got, diff, 16))
        return
    print("  расхождений до %d нет" % upto)


# --------------------------------------------------------------------------
def parse_args(argv):
    args = []
    explicit = []
    max_size = MAX_SIZE
    sweep = False
    step = 1 << 17
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--sweep":
            sweep = True
        elif a.startswith("--max-size="):
            max_size = int(a.split("=", 1)[1], 0)
        elif a == "--max-size":
            i += 1
            max_size = int(argv[i], 0)
        elif a.startswith("--step="):
            step = int(a.split("=", 1)[1], 0)
        elif a.startswith("--sizes="):
            explicit = [int(x, 0) for x in a.split("=", 1)[1].split(",") if x]
        elif not a.startswith("-"):
            args.append(a)
        i += 1
    return args, max_size, sweep, step, explicit


def main():
    args, max_size, sweep, step, explicit = parse_args(sys.argv[1:])

    _raise_buffers()
    load_software()
    entries = read_proc_crypto()

    print("== реализации SPACC ==")
    seen = set()
    for e in entries:
        if e.get("module") != OUR_MODULE:
            continue
        key = (e.get("name"), e.get("driver"))
        if key in seen:
            continue
        seen.add(key)
        print("  %-16s %-34s prio %s"
              % (e.get("name"), e.get("driver"), e.get("priority")))
    if not seen:
        print("  драйвер не загружен")
        return 2

    sk_sizes = build_sizes(BASE_SKCIPHER, max_size, 16)
    hash_sizes = (explicit if explicit
                  else build_sizes(BASE_HASH, min(max_size, 64 << 10), 1))

    print()
    print("== skcipher: движок против софта (до %d байт) ==" % max_size)
    for name, _base in SKCIPHER:
        if args and name not in args and name.split("(")[0] not in args:
            continue
        test_skcipher(entries, name, sk_sizes)
        print()

    print("== хеши: движок против софта ==")
    have_hash = any(e.get("module") == OUR_MODULE and
                    e.get("name") in ("sha1", "sha256", "cmac(aes)",
                                      "hmac(sha256)")
                    for e in entries)
    if not have_hash:
        print("  драйвер не регистрирует хеши — ускорять нечего")
        print()
    for name, keylen, _base in (HASHES if have_hash else []):
        if args and name not in args and name.split("(")[0] not in args:
            continue
        test_hashes(entries, name, keylen, hash_sizes)
        print()

    print("== AEAD ==")
    test_gcm()
    print()

    if "--no-pipeline" not in sys.argv and read_param("depth") is not None:
        print("== конвейер ==")
        for alg, kind in PIPELINE_ALGS:
            if args and alg not in args and alg.split("(")[0] not in args:
                continue
            test_pipeline(entries, alg, kind)
            print()

    if sweep:
        print("== развёртка CTR ==")
        sweep_ctr(entries, max_size, step)
        print()

    bad = [r for r in RESULTS if not r[2]]
    print("ИТОГ: проверок %d, расхождений %d" % (len(RESULTS), len(bad)))
    for alg, size, _ in bad:
        print("  %s @ %d" % (alg, size))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
