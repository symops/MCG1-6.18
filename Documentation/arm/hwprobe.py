import os, sys, socket, struct, ctypes, threading
from concurrent.futures import ThreadPoolExecutor

SOL_ALG = 279
ALG_SET_KEY, ALG_SET_IV, ALG_SET_OP = 1, 2, 3
ALG_OP_ENCRYPT = 1

# Fault-injection diagnostic: needs the engine ON and deliberately runs it
# under concurrent load, which is the known unsafe mode.
try:
    hw = open("/sys/module/ls1024a_spacc/parameters/hw").read().strip()
except OSError:
    sys.exit("ls1024a_spacc is not loaded")
if hw not in ("1", "Y", "y"):
    sys.exit("engine is OFF; enable it for the diagnostic:\n"
             "  echo 1 > /sys/module/ls1024a_spacc/parameters/hw\n"
             "and set it back to 0 after the run")

lib = ctypes.CDLL("libcrypto.so.3", mode=ctypes.RTLD_GLOBAL)
vp = ctypes.c_void_p
ip = ctypes.POINTER(ctypes.c_int)

lib.EVP_CIPHER_fetch.restype = vp
lib.EVP_CIPHER_fetch.argtypes = [vp, ctypes.c_char_p, ctypes.c_char_p]
lib.EVP_CIPHER_CTX_new.restype = vp
lib.EVP_CipherInit_ex.argtypes = [vp, vp, vp, vp, vp, ctypes.c_int]
lib.EVP_CipherUpdate.argtypes = [vp, vp, ip, vp, ctypes.c_int]
lib.EVP_CipherFinal_ex.argtypes = [vp, vp, ip]
lib.EVP_CIPHER_CTX_set_padding.argtypes = [vp, ctypes.c_int]


def software(alg, data, key, iv):
    n = len(data)
    ib = ctypes.create_string_buffer(data, n)
    ob = ctypes.create_string_buffer(n + 32)
    kb = ctypes.create_string_buffer(key, 32)
    vb = ctypes.create_string_buffer(iv, 16)
    ciph = lib.EVP_CIPHER_fetch(None, alg, b"provider=default")
    ctx = lib.EVP_CIPHER_CTX_new()
    lib.EVP_CipherInit_ex(ctx, ciph, None, ctypes.cast(kb, vp),
                          ctypes.cast(vb, vp), 1)
    lib.EVP_CIPHER_CTX_set_padding(ctx, 0)
    outl, finl = ctypes.c_int(0), ctypes.c_int(0)
    lib.EVP_CipherUpdate(ctx, ctypes.cast(ob, vp), ctypes.byref(outl),
                         ctypes.cast(ib, vp), n)
    lib.EVP_CipherFinal_ex(ctx, ctypes.cast(ob, vp), ctypes.byref(finl))
    return ob.raw[:outl.value + finl.value]


def kernel(name, data, key, iv):
    s = socket.socket(socket.AF_ALG, socket.SOCK_SEQPACKET, 0)
    s.bind(("skcipher", name))
    s.setsockopt(SOL_ALG, ALG_SET_KEY, key)
    op, _ = s.accept()
    op.sendmsg([b""], [
        (SOL_ALG, ALG_SET_OP, struct.pack("I", ALG_OP_ENCRYPT)),
        (SOL_ALG, ALG_SET_IV, struct.pack("I", 16) + iv),
    ])
    view = memoryview(data)
    while view:
        view = view[op.send(view):]
    out = bytearray()
    while len(out) < len(data):
        chunk = op.recv(len(data) - len(out))
        if not chunk:
            break
        out += chunk
    op.close()
    s.close()
    return bytes(out)


CASES = [("ctr(aes)", b"AES-256-CTR", 49152),
         ("cbc(aes)", b"AES-256-CBC", 49152),
         ("ctr(aes)", b"AES-256-CTR", 16384),
         ("cbc(aes)", b"AES-256-CBC", 16384)]

lock = threading.Lock()
stats = {"ops": 0, "fails": 0, "plain": 0, "other": 0,
         "readstale": 0, "zerofill": 0,
         "suffix": {}, "cases": {}, "samples": [], "sig": {}}


def worker(seed):
    prev_exp = None
    for i in range(125):
        name, alg, size = CASES[(seed + i) % len(CASES)]
        tag = "%s/%d" % (alg.decode().split("-", 2)[2], size)
        key, iv, pt = os.urandom(32), os.urandom(16), os.urandom(size)
        hw_out = kernel(name, pt, key, iv)
        sw = software(alg, pt, key, iv)
        # expected output of the previous op on this thread
        prev_sw = prev_exp
        prev_exp = sw
        with lock:
            stats["ops"] += 1
            c = stats["cases"].setdefault(tag, [0, 0])
            c[0] += 1
            if hw_out == sw:
                continue
            stats["fails"] += 1
            c[1] += 1
            first = next((j for j in range(size)
                          if hw_out[j] != sw[j]), -1)
            suffix = size - first
            stats["suffix"][suffix] = stats["suffix"].get(suffix, 0) + 1
            if hw_out[first:] == pt[first:]:
                stats["plain"] += 1
                kind = "lost-write (plaintext left in page)"
            elif hw_out[first:] == b"\x00" * suffix:
                stats["zerofill"] += 1
                kind = "zero-fill (write lost, fresh page)"
            else:
                # Does the tail equal a run where the engine READ zeros
                # from `first` onward?  For CTR that is the bare keystream,
                # for CBC-encrypt the chain is already correct up to
                # `first`.  A match means the engine saw stale DDR on the
                # source side, not a broken write path.
                tail = hw_out[first:]
                right = sw[first:]
                sig = "random"
                if prev_sw is not None and \
                        tail == prev_sw[len(prev_sw) - suffix:]:
                    sig = "stale-prev (bytes of previous command)"
                elif sw.find(tail) not in (-1, first):
                    sig = "misplaced (correct bytes from another offset)"
                elif hw_out.find(right) not in (-1, first):
                    sig = "displaced (right tail written elsewhere)"
                else:
                    for sh in (4, 8, 16, 32):
                        if first - sh >= 0 and \
                                tail == sw[first - sh:first - sh + suffix]:
                            sig = "shifted-by-%d" % sh
                            break
                    else:
                        if suffix == 16 and \
                                hw_out[first:first + 8] == sw[first + 8:] and \
                                hw_out[first + 8:] == sw[first:first + 8]:
                            sig = "beats-swapped (8-byte halves)"
                stats["sig"][sig] = stats["sig"].get(sig, 0) + 1
                stats["other"] += 1
                kind = "wrong-data [%s]" % sig
            if len(stats["samples"]) < 3:
                stats["samples"].append(
                    (tag, suffix, kind, hw_out[-32:].hex(), pt[-32:].hex()))


for r in range(3):
    with ThreadPoolExecutor(max_workers=8) as ex:
        list(ex.map(worker, range(8)))

print("ops=%d fails=%d" % (stats["ops"], stats["fails"]))
for tag in sorted(stats["cases"]):
    ops, fails = stats["cases"][tag]
    print("  %-12s ops=%-5d fails=%d" % (tag, ops, fails))
print("suffix length from end (bytes):",
      dict(sorted(stats["suffix"].items())))
print("tail content: lost-write=%d read-stale=%d "
      "zero-fill=%d wrong-data=%d"
      % (stats["plain"], stats["readstale"],
         stats["zerofill"], stats["other"]))
print("wrong-data signatures:", dict(sorted(stats["sig"].items())))
for tag, suf, kind, hwp, ptp in stats["samples"]:
    print("  %s suffix=%d %s" % (tag, suf, kind))
    print("    hw[-32:]: %s" % hwp)
    print("    pt[-32:]: %s" % ptp)
print("REMINDER: echo 0 > /sys/module/ls1024a_spacc/parameters/hw")
