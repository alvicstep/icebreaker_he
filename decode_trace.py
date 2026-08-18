import struct, sys

d = open("icebreaker_trace.bin", "rb").read()
print("file size:", len(d))

print("\n=== STAGE WORDS (0x08020000 + stage*4) ===")
for s in range(64):
    v = struct.unpack_from("<I", d, s * 4)[0]
    if v != 0xFFFFFFFF:
        print("  stage %2d: 0x%08X" % (s, v))

print("\n=== HEARTBEAT (0x08020100 + idx*4) ===")
hb = []
for i in range(512):
    v = struct.unpack_from("<I", d, 0x100 + i * 4)[0]
    if v != 0xFFFFFFFF:
        hb.append(i)
if hb:
    print("  min=%d max=%d count=%d" % (hb[0], hb[-1], len(hb)))
else:
    print("  (none)")

print("\n=== VALUE SLOTS (0x08021000 + slot*4) ===")
for slot in range(32):
    v = struct.unpack_from("<I", d, 0x1000 + slot * 4)[0]
    if v != 0xFFFFFFFF:
        print("  slot %2d = %d (0x%08X)" % (slot, v, v))
