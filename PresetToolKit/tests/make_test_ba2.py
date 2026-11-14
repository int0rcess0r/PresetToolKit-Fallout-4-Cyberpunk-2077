"""Writes a Fallout 4 general (GNRL) .ba2 archive with zlib-compressed entries, for testing.
usage: python make_test_ba2.py out.ba2 <data root> <relative path>..."""
import struct, sys, zlib, os
out, root, rels = sys.argv[1], sys.argv[2], sys.argv[3:]
datas = [open(os.path.join(root, r.replace('\\', '/')), 'rb').read() for r in rels]
header = 24; rec = 36
off = header + rec * len(rels)
recs = []; blobs = []
for r, d in zip(rels, datas):
    c = zlib.compress(d, 9)
    ext = (os.path.splitext(r)[1][1:].lower().encode() + b'\0\0\0\0')[:4]
    recs.append(struct.pack('<I4sIIQIII', 0, ext, 0, 0x00100100, off, len(c), len(d), 0xBAADF00D))
    blobs.append(c); off += len(c)
names = b''.join(struct.pack('<H', len(r)) + r.encode() for r in rels)
with open(out, 'wb') as f:
    f.write(b'BTDX' + struct.pack('<I', 1) + b'GNRL' + struct.pack('<IQ', len(rels), off))
    for x in recs: f.write(x)
    for b in blobs: f.write(b)
    f.write(names)
print('wrote', out, len(rels), 'files')
