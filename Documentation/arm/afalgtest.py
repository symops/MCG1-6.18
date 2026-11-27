import os, socket, struct, ctypes

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
    kb = ctypes.create_string_buffer(key, len(key))
    vb = ctypes.create_string_buffer(iv, len(iv))
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


def afalg(alg_name, data, key, iv):
    s = socket.socket(socket.AF_ALG, socket.SOCK_SEQPACKET, 0)
    s.bind(("skcipher", alg_name))
    s.setsockopt(SOL_ALG, ALG_SET_KEY, key)
    op, _ = s.accept()

    cmsgs = [
        (SOL_ALG, ALG_SET_OP, struct.pack("I", ALG_OP_ENCRYPT)),
        (SOL_ALG, ALG_SET_IV, struct.pack("I", len(iv)) + iv),
    ]
    op.sendmsg([b""], cmsgs)

    view = memoryview(data)
    while view:
        sent = op.send(view)
        view = view[sent:]

    out = bytearray()
    while len(out) < len(data):
        out += op.recv(len(data) - len(out))
    op.close()
    s.close()
    return bytes(out)


CASES = [
    (b"AES-256-CTR", "ctr(aes)", 32768, 200),
    (b"AES-256-CTR", "ctr(aes)", 196608, 200),
    (b"AES-256-CBC", "cbc(aes)", 196608, 50),
]

for alg, name, size, iters in CASES:
    fails, samples = 0, []
    for i in range(iters):
        key, iv, pt = os.urandom(32), os.urandom(16), os.urandom(size)
        ref = software(alg, pt, key, iv)
        got = afalg(name, pt, key, iv)
        if got == ref:
            continue
        fails += 1
        if len(got) != len(ref):
            samples.append("  #%d short %d/%d" % (i, len(got), len(ref)))
        else:
            first = next(j for j in range(len(got)) if got[j] != ref[j])
            samples.append("  #%d diff at byte %d (block %d)"
                           % (i, first, first // 16))
    print("%-8s %-11s fails=%d/%d" % (name, alg.decode(), fails, iters))
    for line in samples[:4]:
        print(line)
