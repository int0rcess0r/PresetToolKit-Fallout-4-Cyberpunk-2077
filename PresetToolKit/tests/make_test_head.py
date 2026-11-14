"""Writes a synthetic Y-up head+neck GLB facing +Z (with a nose) for testing PresetToolKit.
Usage: python make_test_head.py out.glb"""
import json, math, struct, sys

def build():
    pos, idx = [], []
    rings, segs = 48, 64
    for i in range(rings + 1):
        v = i / rings                      # 0 = neck bottom, 1 = skull top
        y = v * 0.30                       # 30 cm tall
        for j in range(segs):
            a = 2 * math.pi * j / segs
            if v < 0.3:  rx, rz = 0.055, 0.06          # neck
            else:
                t = (v - 0.3) / 0.7
                s = math.sin(math.pi * min(1.0, t * 1.02)) ** 0.6
                rx, rz = 0.075 * s + 0.001, 0.095 * s + 0.001
            x, z = rx * math.cos(a), rz * math.sin(a)
            # nose: forward bump around v=0.6 at the front centre
            if 0.5 < v < 0.68 and abs(math.cos(a)) < 0.2 and z > 0:
                z += 0.02 * math.sin(math.pi * (v - 0.5) / 0.18) * (1 - abs(math.cos(a)) / 0.2)
            # lips ridge
            if 0.47 < v < 0.52 and abs(math.cos(a)) < 0.35 and z > 0:
                z += 0.006
            pos.append((x, y, z))
    for i in range(rings):
        for j in range(segs):
            a, b = i * segs + j, i * segs + (j + 1) % segs
            c, d = a + segs, b + segs
            idx += [a, c, b, b, c, d]
    return pos, idx

pos, idx = build()
n = len(pos)
bin_ = bytearray()
def add(data):
    while len(bin_) % 4: bin_.append(0)
    off = len(bin_); bin_.extend(data); return off
pbytes = b"".join(struct.pack("<3f", *p) for p in pos)
ibytes = b"".join(struct.pack("<I", i) for i in idx)
tbytes = b"".join(struct.pack("<3f", 0.0, 0.01 if p[1] > 0.25 else 0.0, 0.0) for p in pos)  # an existing target
nbytes = b"".join(struct.pack("<3f", 0, 0, 0) for _ in pos)
nrm = b"".join(struct.pack("<3f", *( (lambda l: (p[0]/l, 0.0, p[2]/l))(max(1e-9,(p[0]**2+p[2]**2)**0.5)) )) for p in pos)
offs = [add(pbytes), add(ibytes), add(tbytes), add(nbytes), add(nrm)]
mn = [min(p[k] for p in pos) for k in range(3)]; mx = [max(p[k] for p in pos) for k in range(3)]
doc = {
  "asset": {"version": "2.0", "generator": "PresetToolKit test"},
  "scene": 0, "scenes": [{"nodes": [0]}],
  "nodes": [{"name": "root", "translation": [0, 1.5, 0], "children": [1]}, {"name": "head", "mesh": 0}],
  "meshes": [{"name": "h0_000_pwa_c__basehead", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 4}, "indices": 1,
              "targets": [{"POSITION": 2, "NORMAL": 3}]}], "weights": [0.0], "extras": {"targetNames": ["vanilla_test"]}}],
  "buffers": [{"byteLength": len(bin_)}],
  "bufferViews": [{"buffer": 0, "byteOffset": offs[0], "byteLength": len(pbytes)},
                  {"buffer": 0, "byteOffset": offs[1], "byteLength": len(ibytes)},
                  {"buffer": 0, "byteOffset": offs[2], "byteLength": len(tbytes)},
                  {"buffer": 0, "byteOffset": offs[3], "byteLength": len(nbytes)},
                  {"buffer": 0, "byteOffset": offs[4], "byteLength": len(nrm)}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": n, "type": "VEC3", "min": mn, "max": mx},
                {"bufferView": 1, "componentType": 5125, "count": len(idx), "type": "SCALAR"},
                {"bufferView": 2, "componentType": 5126, "count": n, "type": "VEC3", "min": [0,0,0], "max": [0,0.01,0]},
                {"bufferView": 3, "componentType": 5126, "count": n, "type": "VEC3", "min": [0,0,0], "max": [0,0,0]},
                {"bufferView": 4, "componentType": 5126, "count": n, "type": "VEC3"}],
}
js = json.dumps(doc).encode()
while len(js) % 4: js += b" "
while len(bin_) % 4: bin_.append(0)
total = 12 + 8 + len(js) + 8 + len(bin_)
with open(sys.argv[1], "wb") as f:
    f.write(struct.pack("<III", 0x46546C67, 2, total))
    f.write(struct.pack("<II", len(js), 0x4E4F534A)); f.write(js)
    f.write(struct.pack("<II", len(bin_), 0x004E4942)); f.write(bin_)
print("wrote", sys.argv[1], n, "vertices")
