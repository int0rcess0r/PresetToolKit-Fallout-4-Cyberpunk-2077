"""
PresetToolKit calibration: maps every Fallout 4 face bone onto V's head.

Inputs  : Fallout 4  Base{Female,Male}Head_faceBones.nif  (bone positions, orientations, skin weights)
          Cyberpunk  h0_000_p{w,m}a__morphs.morphtarget.glb (head mesh + 254-joint face rig)
Output  : JSON block "fo4Bones<Gender>" for default_mapping.json

For each FO4 bone it records
  pivot  : bone position on V's head (normalised head space u/v/w) - rotations/scales pivot here
  center : centre of the skin the bone moves (weighted by FO4's own skin weights)
  radius : influence size from the same weights (2.5 standard deviations)
Master bones (MasterNose, MasterMouth, MasterBot, MasterEyebrow) carry their children's skin.
  axes   : the bone's local X/Y/Z axes expressed in head space (right, up, forward),
           so LooksMenu offsets/rotations/scales are applied along the real bone axes.
FO4 -> Cyberpunk placement uses a per-axis least-squares fit over matching landmarks
(nose tip, brows, eyes, lips, chin, nostrils, ears, jaw, cheeks).

usage: python calibrate.py <fo4 Base*Head_faceBones.nif> <fo4 skeleton*_faceBones.nif>
                          <cp morphs.morphtarget.glb> <female|male>
"""
import json, struct, sys, math
import nif

def half(h): return struct.unpack('<e', struct.pack('<H', h))[0]

def fo4_head(path):
    bl = nif.load(path); world = nif.world(bl)
    (root, bd, bones), data = nif.skin(path)
    names = [bl[i].get('name') for i in bones]
    # vertex space -> world offset (vertices are stored relative to the HEAD bone)
    offs = []
    for n, (Rm, T, S, sph) in zip(names, data):
        M = [Rm[0:3], Rm[3:6], Rm[6:9]]
        v = [-(M[0][i]*T[0] + M[1][i]*T[1] + M[2][i]*T[2]) for i in range(3)]
        offs.append([world[n][k] - v[k] for k in range(3)])
    off = [sum(o[k] for o in offs) / len(offs) for k in range(3)]
    # vertices + skin weights from the BSSubIndexTriShape
    b = open(path, 'rb').read(); r = nif.R(b); r.o = b.index(b'\n') + 1
    r.u32(); r.u8(); r.u32(); nb = r.u32(); bsv = r.u32()
    for _ in range(4): r.raw(r.u8())
    nt = r.u16(); types = [r.raw(r.u32()).decode('latin1') for _ in range(nt)]
    tidx = [r.u16() for _ in range(nb)]; sizes = [r.u32() for _ in range(nb)]
    ns = r.u32(); r.u32(); [r.raw(r.u32()) for _ in range(ns)]; ng = r.u32(); [r.u32() for _ in range(ng)]
    o = r.o; starts = []
    for s in sizes: starts.append(o); o += s
    verts = []
    for i in range(nb):
        if types[tidx[i]] in ('BSSubIndexTriShape', 'BSTriShape'):
            r.o = starts[i]; r.i32(); ne = r.u32(); [r.i32() for _ in range(ne)]; r.i32(); r.u32(); r.f(13); r.i32()
            r.f(4); r.i32(); r.i32(); r.i32()
            desc = r.u64(); r.u32(); nv = r.u16(); r.u32()
            vsize = (desc & 0xF) * 4; skinOff = ((desc >> 28) & 0xF) * 4; flags = desc >> 44
            full = bool(flags & (1 << 10)); base = r.o
            for k in range(nv):
                q = base + k * vsize
                p = struct.unpack_from('<3f', b, q) if full else tuple(half(x) for x in struct.unpack_from('<3H', b, q))
                w = [half(x) for x in struct.unpack_from('<4H', b, q + skinOff)]
                ix = struct.unpack_from('<4B', b, q + skinOff + 8)
                verts.append(([p[j] + off[j] for j in range(3)], list(zip(ix, w))))
    rot = {n: [d[0][0:3], d[0][3:6], d[0][6:9]] for n, d in zip(names, data)}  # rows = bone axes in world
    return names, world, rot, verts

def cp_head(path):
    d = open(path, 'rb').read(); jl = struct.unpack_from('<I', d, 12)[0]; doc = json.loads(d[20:20 + jl])
    joints = {n['name']: n.get('translation', [0, 0, 0]) for n in doc['nodes'] if 'name' in n}
    acc = doc['accessors'][doc['meshes'][0]['primitives'][0]['attributes']['POSITION']]
    return joints, acc['min'], acc['max']

def mid(*pts): return [sum(p[k] for p in pts) / len(pts) for k in range(3)]

def landmarks(cp):
    """FO4 bone -> Cyberpunk position (centre line and character-left side)."""
    j = lambda n: cp[n]
    L = {
        'skin_bone_C_Nose':        j('mid_J_mug_nose_tip_rowA_0_JNT'),
        'skin_bone_C_Nose_Bridge': mid(j('mid_J_nose_nosabial_rowB_0_JNT'), j('mid_J_eye_nose_rowA_0_JNT')),
        'skin_bone_C_EyebrowMid':  j('mid_J_eye_brows_rowB_0_JNT'),
        'skin_bone_C_ForeheadMid': j('mid_J_eye_brows_rowD_0_JNT'),
        'skin_bone_C_MouthTop':    [0.0] + j('l_J_mug_lip_up_0_JNT')[1:],
        'skin_bone_C_MouthBot':    [0.0] + j('l_J_mug_lip_dn_0_JNT')[1:],
        'skin_bone_C_Chin':        j('mid_J_mug_chin_rowA_1_JNT'),
        'skin_bone_L_Eye':         mid(j('l_J_eye_lid_corner_inside_0_JNT'), j('l_J_eye_lid_up_rowD_4_JNT')),
        'skin_bone_L_MouthCorner': j('l_J_mug_mouth_rowA_3_JNT'),
        'skin_bone_L_Nostril':     j('l_J_mug_nose_nostril_0_JNT'),
        'skin_bone_L_Ear':         j('l_J_jaw_ear_0_JNT'),
        'skin_bone_L_JawSide':     j('l_J_jaw_rowB_0_JNT'),
        'skin_bone_L_Cheekbone':   j('l_J_eye_check_rowE_0_JNT'),
        'skin_bone_L_EyebrowIn':   j('l_J_eye_brows_rowB_0_JNT'),
        'skin_bone_L_EyebrowOut':  j('l_J_eye_brows_rowB_3_JNT'),
    }
    # eyeball centre sits behind the lid line
    L['skin_bone_L_Eye'][2] += 0.012
    return L

def fit(fo4, cpL):
    """CP_axis = a * FO4_axis + b; axis pairs: cp.x<-fo4.x, cp.y<-fo4.z, cp.z<-fo4.y (cp forward is -z)."""
    pairs = [(fo4[n], cpL[n]) for n in cpL]
    out = []
    for cpk, fk in ((0, 0), (1, 2), (2, 1)):
        xs = [p[0][fk] for p in pairs]; ys = [p[1][cpk] for p in pairs]
        mx = sum(xs) / len(xs); my = sum(ys) / len(ys)
        a = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
        out.append((a, my - a * mx))
    res = []
    for f, c in pairs:
        pred = [out[0][0] * f[0] + out[0][1], out[1][0] * f[2] + out[1][1], out[2][0] * f[1] + out[2][1]]
        res.append(math.dist(pred, c))
    return out, res

def hierarchy(skelp):
    bl = nif.load(skelp); nif.world(bl)
    out = {}
    def desc(i):
        r = []
        for c in bl[i].get('children', []):
            if 0 <= c < len(bl) and bl[c].get('name'): r.append(bl[c]['name']); r += desc(c)
        return r
    for i, b in enumerate(bl):
        if b.get('name', '').startswith('skin_bone_'): out[b['name']] = desc(i)
    nodes = {b['name']: b for b in bl if b.get('name')}
    return out, nodes

def main():
    nifp, skelp, glbp, gender = sys.argv[1:5]
    names, world, rot, verts = fo4_head(nifp)
    kids, skelNodes = hierarchy(skelp)
    # bones that exist only in the skeleton (masters): position + rotation from the skeleton
    for n, b in skelNodes.items():
        if n.startswith('skin_bone_') and n not in world:
            world[n] = b['wT']
        if n.startswith('skin_bone_') and n not in rot and 'wR' in b:
            wR = b['wR']; rot[n] = [[wR[0][k], wR[1][k], wR[2][k]] for k in range(3)]   # columns -> rows
    cp, mn, mx = cp_head(glbp)
    (ax, ay, az), res = fit(world, landmarks(cp))
    print(f'[{gender}] fit scale m/unit: x {ax[0]:.5f} y {ay[0]:.5f} z {az[0]:.5f};  landmark error mean {1000*sum(res)/len(res):.1f} mm, max {1000*max(res):.1f} mm', file=sys.stderr)
    ext = [mx[k] - mn[k] for k in range(3)]
    unit = (abs(ax[0]) + abs(ay[0]) + abs(az[0])) / 3

    # skin-weight statistics per bone (FO4 world units)
    allBones = sorted(set(n for n in world if n.startswith('skin_bone_')))
    skinIdx = {n: i for i, n in enumerate(names)}
    stats = {}
    for n in allBones:
        group = {skinIdx[m] for m in [n] + kids.get(n, []) if m in skinIdx}
        W = 0.0; S1 = [0.0] * 3; S2 = [0.0] * 3
        for p, inf in verts:
            w = sum(wt for bi, wt in inf if bi in group and wt > 0.02)
            if w <= 0: continue
            W += w
            for k in range(3): S1[k] += w * p[k]; S2[k] += w * p[k] * p[k]
        stats[n] = (W, S1, S2)
    locs = {}
    def toCp(b): return [ax[0] * b[0] + ax[1], ay[0] * b[2] + ay[1], az[0] * b[1] + az[1]]
    def norm(c): return [(c[0] - mn[0]) / ext[0], (c[1] - mn[1]) / ext[1], (mx[2] - c[2]) / ext[2]]
    for n in allBones:
        if n not in rot: continue
        key = n[len('skin_bone_'):].lower()          # e.g. "r_eye", "c_nose"
        pivot = norm(toCp(world[n]))
        W, S1, S2 = stats[n]
        if W > 0:
            meanp = [S1[k] / W for k in range(3)]
            sd = [math.sqrt(max(0.0, S2[k] / W - meanp[k] ** 2)) for k in range(3)]   # fo4 x, y(fwd), z(up)
            center = norm(toCp(meanp))
        else:
            sd = [1.0, 1.0, 1.0]; center = pivot
        sdh = [sd[0] * abs(ax[0]), sd[2] * abs(ay[0]), sd[1] * abs(az[0])]               # metres, head axes
        radius = [max(0.008, 2.5 * v) for v in sdh]
        radius = [radius[0] / ext[0], radius[1] / ext[1], radius[2] / ext[2]]
        # bone axes (rows of world->bone rotation) in head space (right, up, forward)
        R = rot[n]
        axes = [[R[k][0], R[k][2], R[k][1]] for k in range(3)]
        locs[key] = {'pivot': [round(v, 4) for v in pivot], 'center': [round(v, 4) for v in center],
                     'radius': [round(v, 4) for v in radius],
                     'axes': [[round(v, 4) for v in a] for a in axes]}
    block = {'metersPerUnit': round(unit, 6), 'source': f'{nifp.split("/")[-1]} -> {glbp.split("/")[-1]}',
             'landmarkErrorMm': round(1000 * sum(res) / len(res), 2), 'locations': locs}
    print(json.dumps(block))

if __name__ == '__main__':
    main()
