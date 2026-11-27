import os, socket, struct, ctypes, threading
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


def software(data, key, iv):
    n = len(data)
    ib = ctypes.create_string_buffer(data, n)
    ob = ctypes.create_string_buffer(n + 32)
    kb = ctypes.create_string_buffer(key, 32)
    vb = ctypes.create_string_buffer(iv, 16)
    ciph = lib.EVP_CIPHER_fetch(None, b"AES-256-CTR", b"provider=default")
    ctx = lib.EVP_CIPHER_CTX_new()
    lib.EVP_CipherInit_ex(ctx, ciph, None, ctypes.cast(kb, vp),
                          ctypes.cast(vb, vp), 1)
    lib.EVP_CIPHER_CTX_set_padding(ctx, 0)
    outl, finl = ctypes.c_int(0), ctypes.c_int(0)
    lib.EVP_CipherUpdate(ctx, ctypes.cast(ob, vp), ctypes.byref(outl),
                         ctypes.cast(ib, vp), n)
    lib.EVP_CipherFinal_ex(ctx, ctypes.cast(ob, vp), ctypes.byref(finl))
    return ob.raw[:outl.value + finl.value]


def kernel(data, key, iv):
    s = socket.socket(socket.AF_ALG, socket.SOCK_SEQPACKET, 0)
    s.bind(("skcipher", "ctr(aes)"))
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


SIZES = [65536, 196608]
lock = threading.Lock()
stats = {"ops": 0, "fails": 0, "samples": []}


def worker(seed):
    for i in range(190):
        size = SIZES[(seed + i) & 1]
        key, iv, pt = os.urandom(32), os.urandom(16), os.urandom(size)
        hw = kernel(pt, key, iv)
        sw = software(pt, key, iv)
        with lock:
            stats["ops"] += 1
            if hw != sw:
                stats["fails"] += 1
                first = next((j for j in range(min(len(hw), len(sw)))
                              if hw[j] != sw[j]), -1)
                if len(stats["samples"]) < 5:
                    stats["samples"].append((size, first))


for r in range(2):
    with ThreadPoolExecutor(max_workers=8) as ex:
        list(ex.map(worker, range(8)))

print("CTR 64/192 KiB: ops=%d fails=%d" % (stats["ops"], stats["fails"]))
for size, first in stats["samples"]:
    print("  sz=%d first diff at %d" % (size, first))
