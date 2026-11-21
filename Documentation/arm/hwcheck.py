import os, sys, socket, struct, ctypes, threading
from concurrent.futures import ThreadPoolExecutor

SOL_ALG = 279
ALG_SET_KEY, ALG_SET_IV, ALG_SET_OP = 1, 2, 3
ALG_OP_ENCRYPT = 1

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


# 57 344 and 49 152 both fit one 15-entry command
CASES = [("ctr(aes)", b"AES-256-CTR", 49152),
         ("cbc(aes)", b"AES-256-CBC", 49152),
         ("ctr(aes)", b"AES-256-CTR", 40960),
         ("cbc(aes)", b"AES-256-CBC", 40960)]

lock = threading.Lock()
stats = {"ops": 0, "fails": 0, "samples": []}


def worker(seed):
    for i in range(125):
        name, alg, size = CASES[(seed + i) % len(CASES)]
        key, iv, pt = os.urandom(32), os.urandom(16), os.urandom(size)
        hw = kernel(name, pt, key, iv)
        sw = software(alg, pt, key, iv)
        with lock:
            stats["ops"] += 1
            if hw != sw:
                stats["fails"] += 1
                first = next((j for j in range(min(len(hw), len(sw)))
                              if hw[j] != sw[j]), -1)
                if len(stats["samples"]) < 5:
                    stats["samples"].append(
                        "%s size=%d first=%d" % (alg.decode(), size, first))


threads = max(1, int(sys.argv[1]) if len(sys.argv) > 1 else 8)
rounds = max(1, round(3000 / (125 * threads)))
for r in range(rounds):
    with ThreadPoolExecutor(max_workers=threads) as ex:
        list(ex.map(worker, range(threads)))

print("ops=%d threads=%d fails=%d"
      % (stats["ops"], threads, stats["fails"]))
for s in stats["samples"]:
    print("  " + s)
