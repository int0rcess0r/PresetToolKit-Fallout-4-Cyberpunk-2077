"""Builds data/fo4_chargen.json from Fallout4.esm: HumanRace tint layers (per sex) with their colours,
playable eye head parts and hair colours. PresetToolKit uses it to carry Cyberpunk skin tone, makeup,
blemishes, eye and hair colour over to LooksMenu presets.
usage: make_fo4_chargen.py <Fallout4.esm> <out.json>"""
import json, re, struct, sys, zlib

d = open(sys.argv[1], 'rb').read()

def recs(label):
    o = 24 + struct.unpack_from('<I', d, 4)[0]
    while o < len(d):
        t, sz, lab, gt, _, _ = struct.unpack_from('<4sI4sIII', d, o)
        if lab == label:
            p = o + 24
            while p < o + sz:
                rt, rsz, rfl, rfid = struct.unpack_from('<4sIII', d, p)
                if rt == b'GRUP': p += rsz; continue
                body = d[p + 24:p + 24 + rsz]
                if rfl & 0x40000: body = zlib.decompress(body[4:])
                yield rfid & 0xFFFFFF, body
                p += 24 + rsz
        o += sz

def subs(body):
    q = 0; big = None
    while q < len(body):
        st, ss = struct.unpack_from('<4sH', body, q)
        if st == b'XXXX': big = struct.unpack_from('<I', body, q + 6)[0]; q += 10; continue
        if big is not None: ss = big; big = None
        yield st, body[q + 6:q + 6 + ss]; q += 6 + ss

clfm = {}
for fid, body in recs(b'CLFM'):
    S = dict(subs(body))
    c = S.get(b'CNAM', b'\0' * 4)
    clfm[fid] = (S.get(b'EDID', b'').rstrip(b'\0').decode('latin1'), [c[0], c[1], c[2]])

KINDS = [  # texture file name pattern -> kind
    (r'skintone', 'skin'), (r'^lipcolor1female', 'lips_natural'), (r'^lipcolor', 'lipstick'),
    (r'lipliner', 'lipliner'), (r'eyeliner', 'eyeliner'), (r'eyeshadowlower', 'eyeshadow_lower'),
    (r'eyeshadow|smokeyeye', 'eyeshadow'), (r'blush', 'blush'), (r'freckles', 'freckles'), (r'moles', 'moles'),
    (r'blemish|pocks|splochy|craterface', 'blemish'), (r'brow', 'brows'), (r'^scar', 'scar'), (r'facetat', 'tattoo'),
    (r'facepaint', 'facepaint'), (r'dirt', 'dirt'), (r'bruis', 'bruise'), (r'mask', 'mask'),
]

def kind_of(tex):
    n = tex.replace('\\', '/').split('/')[-1].lower()
    for pat, k in KINDS:
        if re.search(pat, n): return k
    return 'other'

out = {'source': 'Fallout4.esm HumanRace', 'tints': {'female': [], 'male': []}, 'eyes': {'female': [], 'male': []},
       'hairColors': [], 'faceMorphs': {'female': {'presets': {}, 'sliders': {}, 'groups': {}},
                                         'male': {'presets': {}, 'sliders': {}, 'groups': {}}}}
for fid, body in recs(b'RACE'):
    S = list(subs(body))
    if dict(S).get(b'EDID', b'').rstrip(b'\0') != b'HumanRace': continue
    sex = None
    group = ''
    for i, (st, v) in enumerate(S):
        if st in (b'MNAM', b'FNAM') and len(v) == 0: sex = 'male' if st == b'MNAM' else 'female'
        # face morph presets (MPPI -> MPPM tri morph) and sliders (MSID -> MSM0 / MSM1 morphs)
        if sex is not None and st == b'MPGN':
            group = v.rstrip(b'\0').decode('latin1') if len(v) != 4 else 'group%d' % struct.unpack('<I', v)[0]
        if sex is not None and st == b'MPPI':
            key = '%X' % struct.unpack('<I', v[:4])[0]
            for j in range(i + 1, min(i + 5, len(S))):
                if S[j][0] == b'MPPI': break
                if S[j][0] == b'MPPM':
                    out['faceMorphs'][sex]['presets'][key] = S[j][1].rstrip(b'\0').decode('latin1')
                    out['faceMorphs'][sex]['groups'][key] = group
                    break
        # the slider list (MSID / MSM0 / MSM1) is shared by both sexes
        if st == b'MSID' and i + 2 < len(S) and S[i + 1][0] == b'MSM0' and S[i + 2][0] == b'MSM1':
            key = '%X' % struct.unpack('<I', v[:4])[0]
            for sx in ('female', 'male'):
                out['faceMorphs'][sx]['sliders'][key] = [S[i + 1][1].rstrip(b'\0').decode('latin1'),
                                                         S[i + 2][1].rstrip(b'\0').decode('latin1')]
        if st != b'TETI' or sex is None: continue
        idx = struct.unpack_from('<HH', v)[1]
        tex = []; cols = []; dflt = None; j = i + 1
        while j < len(S) and S[j][0] not in (b'TETI', b'TTGE'):
            if S[j][0] == b'TTET': tex.append(S[j][1].rstrip(b'\0').decode('latin1'))
            if S[j][0] == b'TTEC':
                for k in range(0, len(S[j][1]), 14):
                    f, a, tid, bl = struct.unpack_from('<IfHi', S[j][1], k)
                    name, rgb = clfm.get(f & 0xFFFFFF, ('', [0, 0, 0]))
                    if not name or name.lower() == 'none': continue
                    cols.append({'id': tid, 'form': '%06X' % (f & 0xFFFFFF), 'name': name, 'rgb': rgb, 'alpha': round(a, 3)})
            if S[j][0] == b'TTED': dflt = struct.unpack_from('<f', S[j][1])[0]
            j += 1
        typ = 1 if cols else (2 if len(tex) >= 2 else 0)
        num = re.findall(r'(\d+)', tex[0].replace('\\', '/').split('/')[-1]) if tex else []
        out['tints'][sex].append({'index': '%X' % idx, 'kind': kind_of(tex[0]) if tex else 'other', 'type': typ,
                                  'texture': tex[0].replace('\\', '/').split('/')[-1] if tex else '',
                                  'number': int(num[0]) if num else 0, 'default': dflt, 'colors': cols})

for fid, body in recs(b'HDPT'):
    S = dict(subs(body))
    pn = struct.unpack('<I', S[b'PNAM'][:4])[0] if b'PNAM' in S else -1
    fl = S.get(b'DATA', b'\0')[0]
    if pn == 2 and fl & 1:
        ed = S.get(b'EDID', b'').rstrip(b'\0').decode('latin1')
        e = {'form': '%06X' % fid, 'name': ed}
        if fl & 2: out['eyes']['male'].append(e)
        if fl & 4: out['eyes']['female'].append(e)

for fid, (name, rgb) in sorted(clfm.items()):
    if re.match(r'HairColor\d', name): out['hairColors'].append({'form': '%06X' % fid, 'name': name})

json.dump(out, open(sys.argv[2], 'w'), indent=1)
for sex in ('female', 'male'):
    kinds = {}
    for t in out['tints'][sex]: kinds[t['kind']] = kinds.get(t['kind'], 0) + 1
    print(sex, len(out['tints'][sex]), 'tints', kinds, len(out['eyes'][sex]), 'eyes')
print(len(out['hairColors']), 'hair colours')
for sex in ('female', 'male'):
    fm = out['faceMorphs'][sex]
    print(sex, len(fm['presets']), 'face presets ->', len(set(fm['presets'].values())), 'morphs;', len(fm['sliders']), 'sliders')
