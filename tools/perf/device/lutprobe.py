# lutprobe.py: each CRTC's GAMMA_LUT (blob id, and its first, middle and last entries), read-only (no DRM master needed)
import ctypes, fcntl, os, struct
fd = os.open("/dev/dri/card0", os.O_RDWR)
def ioc(nr, size): return (3 << 30) | (size << 16) | (ord('d') << 8) | nr
def ioctl(nr, buf): fcntl.ioctl(fd, ioc(nr, len(buf)), buf); return buf
b = ioctl(0xA0, bytearray(64)); f = struct.unpack("QQQQIIIIIIII", b); ncrtc = f[5]
crtcs = (ctypes.c_uint32 * ncrtc)()
b = bytearray(struct.pack("QQQQIIIIIIII", 0, ctypes.addressof(crtcs), 0, 0, 0, ncrtc, 0, 0, 0, 0, 0, 0)); ioctl(0xA0, b)
for c in crtcs:
    b = ioctl(0xB9, bytearray(struct.pack("QQIII4x", 0, 0, 0, c, 0xcccccccc))); n = struct.unpack("QQIII4x", b)[2]
    ids = (ctypes.c_uint32 * n)(); vals = (ctypes.c_uint64 * n)()
    ioctl(0xB9, bytearray(struct.pack("QQIII4x", ctypes.addressof(ids), ctypes.addressof(vals), n, c, 0xcccccccc)))
    for i in range(n):
        pb = ioctl(0xAA, bytearray(struct.pack("QQII32sII", 0, 0, ids[i], 0, b"", 0, 0)))
        name = struct.unpack("QQII32sII", pb)[4].split(b"\0")[0].decode()
        if name != "GAMMA_LUT": continue
        blob = vals[i]; out = "crtc %d GAMMA_LUT blob %d" % (c, blob)
        if blob:
            gb = ioctl(0xAC, bytearray(struct.pack("IIQ", blob, 0, 0))); ln = struct.unpack("IIQ", gb)[1]
            data = (ctypes.c_uint8 * ln)(); ioctl(0xAC, bytearray(struct.pack("IIQ", blob, ln, ctypes.addressof(data))))
            e = [struct.unpack_from("HHHH", bytes(data), k * 8)[:3] for k in (0, ln // 16, ln // 8 - 1)]
            out += " entries %d: first %s mid %s last %s" % (ln // 8, e[0], e[1], e[2])
        print(out)
