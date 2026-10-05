# drmprops.py: the colour-related KMS properties of every CRTC, plane and connector (read-only; no DRM master needed)
import ctypes, fcntl, os, struct, sys
fd = os.open("/dev/dri/card0", os.O_RDWR)
def ioc(nr, size, rw=3): return (rw << 30) | (size << 16) | (ord('d') << 8) | nr
def ioctl(nr, buf): return fcntl.ioctl(fd, ioc(nr, len(buf)), buf)
# universal planes + atomic caps so all properties are listed
for cap in (2, 3):
    try: fcntl.ioctl(fd, (1 << 30) | (16 << 16) | (ord('d') << 8) | 0x0d, struct.pack("QQ", cap, 1))
    except OSError as e: print("cap", cap, e)
def getres():
    b = bytearray(struct.pack("QQQQIIIIIIII", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)); ioctl(0xA0, b)
    f = struct.unpack("QQQQIIIIIIII", b); nfb, ncrtc, nconn, nenc = f[4], f[5], f[6], f[7]
    crtcs = (ctypes.c_uint32 * ncrtc)(); conns = (ctypes.c_uint32 * nconn)(); encs = (ctypes.c_uint32 * nenc)(); fbs = (ctypes.c_uint32 * max(nfb, 1))()
    b = bytearray(struct.pack("QQQQIIIIIIII", ctypes.addressof(fbs), ctypes.addressof(crtcs), ctypes.addressof(conns), ctypes.addressof(encs), nfb, ncrtc, nconn, nenc, 0, 0, 0, 0)); ioctl(0xA0, b)
    return list(crtcs), list(conns)
def planes():
    b = bytearray(struct.pack("QI4x", 0, 0)); ioctl(0xB5, b); n = struct.unpack("QI4x", b)[1]
    arr = (ctypes.c_uint32 * n)(); b = bytearray(struct.pack("QI4x", ctypes.addressof(arr), n)); ioctl(0xB5, b); return list(arr)
def props(obj, typ):
    b = bytearray(struct.pack("QQIII4x", 0, 0, 0, obj, typ)); ioctl(0xB9, b); n = struct.unpack("QQIII4x", b)[2]
    ids = (ctypes.c_uint32 * n)(); vals = (ctypes.c_uint64 * n)()
    b = bytearray(struct.pack("QQIII4x", ctypes.addressof(ids), ctypes.addressof(vals), n, obj, typ)); ioctl(0xB9, b)
    out = []
    for i in range(n):
        pb = bytearray(struct.pack("QQIII32sII", 0, 0, ids[i], 0, 0, b"", 0, 0)); ioctl(0xAA, pb)
        f = struct.unpack("QQIII32sII", pb); out.append((f[5].split(b"\0")[0].decode(), vals[i], f[3]))
    return out
crtcs, conns = getres()
for c in crtcs: print("crtc", c, [(n, v) for n, v, fl in props(c, 0xcccccccc)])
for c in conns: print("connector", c, [(n, v) for n, v, fl in props(c, 0xc0c0c0c0) if n not in ("EDID", "DPMS", "link-status", "non-desktop", "TILE")])
for p in planes(): print("plane", p, [(n, v) for n, v, fl in props(p, 0xeeeeeeee) if n not in ("IN_FORMATS", "FB_ID", "CRTC_ID", "IN_FENCE_FD", "SRC_X", "SRC_Y", "SRC_W", "SRC_H", "CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H")])
