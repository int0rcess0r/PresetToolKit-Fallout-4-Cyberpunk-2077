"""Writes a minimal Cyberpunk 2077 (2.x layout) sav.dat holding only a CharacetrCustomization_Appearances
node, LZ4-compressed like the game's saves. For testing PresetToolKit's save reader.
usage: make_test_cp_save.py <out_dir> [male] [eyes=h141 nose=h112 ...]"""
import os, struct, sys
import lz4.block

def vlq(v):
    neg = v < 0; a = abs(v)
    b = (a & 0x3F) | (0x80 if neg else 0); a >>= 6
    out = bytearray()
    if a: b |= 0x40
    out.append(b)
    while a:
        c = a & 0x7F; a >>= 7
        if a: c |= 0x80
        out.append(c)
    return bytes(out)

def lps(s):  # length-prefixed 8-bit string
    return vlq(-len(s)) + s.encode()

def group(name, apps, morphs):
    d = lps(name) + struct.pack('<i', len(apps))
    for res, defi, nm in apps:
        d += struct.pack('<Q', res) + lps(defi) + lps(nm) + struct.pack('<II', 0, 0)
    d += struct.pack('<i', len(morphs))
    for r, t in morphs:
        d += lps(r) + lps(t) + struct.pack('<II', 0, 0)
    return d

def main():
    out = sys.argv[1]; male = 'male' in sys.argv[2:]
    choice = {'eyes': 'h141', 'nose': 'h112', 'mouth': 'h063', 'jaw': 'h164', 'ear': 'h115'}
    for a in sys.argv[2:]:
        if '=' in a: k, v = a.split('='); choice[k] = v
    sx = 'pma' if male else 'pwa'
    morphs = list(choice.items())
    head = [group('TPP', [(1, f'h0_000_{sx}__basehead__face_rig', 'tpp_head_face_rig')], morphs),
            group('hairs', [(2, '17_blue_steel', 'hair_color27')], []),
            group('character_customization', [(3, f'h0_000_{sx}__basehead__05_bl_espresso', 'skin_type_05')], morphs)]
    node = struct.pack('<I', 7)                         # node id
    node += b'\x01' + struct.pack('<I', 0) + struct.pack('<I', 13) + bytes([1 if male else 0, 1 if male else 0])
    node += struct.pack('<I', len(head)) + b''.join(head)
    node += struct.pack('<I', 0) + struct.pack('<I', 0)  # arms, body groups
    node += struct.pack('<I', 1) + lps('FPP_Body') + lps('FPP_Body') + lps('TPP_Body')
    node += vlq(1) + lps('blue_steel')
    filler = bytes(range(256)) * 40                      # an unrelated node before ours
    data = struct.pack('<I', 1) + filler + node
    node_off_in_data = 4 + len(filler)

    header = b'VASC' + struct.pack('<III', 260, 2310, 0) + b'\x00' * 9   # 25 bytes
    nchunks_max = 4
    table_size = 8 + nchunks_max * 12
    first = len(header) + table_size
    chunks = [data[i:i + 4096] for i in range(0, len(data), 4096)]
    body = bytearray(); entries = []
    for c in chunks:
        comp = lz4.block.compress(c, store_size=False)
        blob = b'4ZLX' + struct.pack('<I', len(c)) + comp
        entries.append((first + len(body), len(blob), len(c)))
        body += blob
    table = b'FZLC' + struct.pack('<I', len(entries)) + b''.join(struct.pack('<III', *e) for e in entries)
    table += b'\x00' * (table_size - len(table))
    f = bytearray(header + table + body)
    node_table = len(f)
    names = [('SomethingElse', 0, 4 + len(filler)), ('CharacetrCustomization_Appearances', first + node_off_in_data, len(node))]
    nt = b'EDON' + vlq(len(names))
    nt += lps(names[0][0]) + struct.pack('<iiii', 1, -1, first, names[0][2])
    nt += lps(names[1][0]) + struct.pack('<iiii', -1, -1, names[1][1], names[1][2])
    f += nt + struct.pack('<I', node_table) + b'ENOD'
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, 'sav.dat'), 'wb').write(f)
    print('wrote', os.path.join(out, 'sav.dat'), len(f), 'bytes,', len(entries), 'LZ4 chunks')

main()
