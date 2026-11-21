import ctypes, sys

lib = ctypes.CDLL("libcrypto.so.3", mode=ctypes.RTLD_GLOBAL)
vp = ctypes.c_void_p
ip = ctypes.POINTER(ctypes.c_int)

lib.OSSL_PROVIDER_load.restype = vp
lib.OSSL_PROVIDER_load.argtypes = [vp, ctypes.c_char_p]
lib.EVP_CIPHER_fetch.restype = vp
lib.EVP_CIPHER_fetch.argtypes = [vp, ctypes.c_char_p, ctypes.c_char_p]
lib.EVP_CIPHER_CTX_new.restype = vp
lib.EVP_EncryptInit_ex2.argtypes = [vp, vp, vp, vp, vp]
lib.EVP_EncryptUpdate.argtypes = [vp, vp, ip, vp, ctypes.c_int]
lib.EVP_EncryptFinal_ex.argtypes = [vp, vp, ip]

if not lib.OSSL_PROVIDER_load(None, b"afalgprov"):
    sys.exit("provider load failed")

KEY = bytes(range(16))


def encrypt(props, iv, data, chunk):
    ciph = lib.EVP_CIPHER_fetch(None, b"AES-128-CTR", props)
    if not ciph:
        sys.exit("fetch failed: " + props.decode())
    kb = ctypes.create_string_buffer(KEY, 16)
    vb = ctypes.create_string_buffer(iv, 16)
    ctx = lib.EVP_CIPHER_CTX_new()
    if lib.EVP_EncryptInit_ex2(ctx, ciph, ctypes.cast(kb, vp),
                               ctypes.cast(vb, vp), None) != 1:
        sys.exit("init failed")

    out = bytearray()
    for off in range(0, len(data), chunk):
        part = data[off:off + chunk]
        ib = ctypes.create_string_buffer(part, len(part))
        ob = ctypes.create_string_buffer(len(part) + 16)
        n = ctypes.c_int(0)
        if lib.EVP_EncryptUpdate(ctx, ctypes.cast(ob, vp), ctypes.byref(n),
                                 ctypes.cast(ib, vp), len(part)) != 1:
            sys.exit("update failed")
        out += ob.raw[:n.value]

    tb = ctypes.create_string_buffer(32)
    m = ctypes.c_int(0)
    if lib.EVP_EncryptFinal_ex(ctx, ctypes.cast(tb, vp), ctypes.byref(m)) != 1:
        sys.exit("final failed")
    out += tb.raw[:m.value]
    return bytes(out)


plain = bytes(32768)

cases = [
    ("no wrap", bytes(12) + bytes.fromhex("00000000"), 32768),
    ("wrap at block 1", bytes(12) + bytes.fromhex("ffffffff"), 32768),
    ("wrap at block 256", bytes(12) + bytes.fromhex("ffffff00"), 32768),
    ("ends exactly at wrap", bytes(12) + bytes.fromhex("ffffff00"), 4096),
    ("wrap between HW chunks", bytes(12) + bytes.fromhex("fffffe00"), 32768),
]

for name, iv, length in cases:
    data = plain[:length]
    hw = encrypt(b"provider=afalgprov", iv, data, 32768)
    sw = encrypt(b"provider=default", iv, data, 32768)
    if hw == sw:
        print("%-22s %5d bytes  ok" % (name, length))
        continue
    first = next(i for i in range(len(hw)) if hw[i] != sw[i])
    print("%-22s %5d bytes  DIFF at byte %d (block %d)"
          % (name, length, first, first // 16))
