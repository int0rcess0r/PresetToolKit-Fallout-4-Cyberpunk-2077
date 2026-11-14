"""Minimal Fallout 4 NIF reader: node transforms + BSTriShape vertex positions."""
import struct, sys
def half(h):
    return struct.unpack('<e', struct.pack('<H', h))[0]
class R:
    def __init__(s,b): s.b=b; s.o=0
    def u8(s): v=s.b[s.o]; s.o+=1; return v
    def u16(s): v=struct.unpack_from('<H',s.b,s.o)[0]; s.o+=2; return v
    def u32(s): v=struct.unpack_from('<I',s.b,s.o)[0]; s.o+=4; return v
    def i32(s): v=struct.unpack_from('<i',s.b,s.o)[0]; s.o+=4; return v
    def u64(s): v=struct.unpack_from('<Q',s.b,s.o)[0]; s.o+=8; return v
    def f(s,n=1):
        v=struct.unpack_from('<%df'%n,s.b,s.o); s.o+=4*n; return v if n>1 else v[0]
    def raw(s,n): v=s.b[s.o:s.o+n]; s.o+=n; return v
def load(path):
    b=open(path,'rb').read(); r=R(b)
    nl=b.index(b'\n'); r.o=nl+1
    ver=r.u32(); endian=r.u8(); uver=r.u32(); nblocks=r.u32(); bsver=r.u32()
    for _ in range(4 if bsver>=130 else 3): n=r.u8(); r.raw(n)
    if bsver>=130 and bsver<=130: pass
    ntypes=r.u16(); types=[r.raw(r.u32()).decode() for _ in range(ntypes)]
    tidx=[r.u16() for _ in range(nblocks)]; sizes=[r.u32() for _ in range(nblocks)]
    nstr=r.u32(); r.u32(); strings=[r.raw(r.u32()).decode('latin1') for _ in range(nstr)]
    ng=r.u32(); [r.u32() for _ in range(ng)]
    blocks=[]
    for i in range(nblocks):
        start=r.o; t=types[tidx[i]&0x7fff]; blk={'type':t}
        if t in ('NiNode','BSFadeNode','BSTriShape','BSSubIndexTriShape','BSDynamicTriShape','BSLeafAnimNode'):
            name=r.i32(); blk['name']=strings[name] if 0<=name<len(strings) else ''
            ne=r.u32(); [r.i32() for _ in range(ne)]; r.i32()
            r.u32(); blk['t']=r.f(3); blk['R']=r.f(9); blk['s']=r.f(); r.i32()
            if t in ('NiNode','BSFadeNode','BSLeafAnimNode'):
                nc=r.u32(); blk['children']=[r.i32() for _ in range(nc)]
            else:
                r.f(4); blk['skin']=r.i32(); r.i32(); r.i32()
                desc=r.u64(); ntri=r.u32(); nv=r.u16(); dsize=r.u32()
                vsize=(desc&0xF)*4; flags=desc>>44
                full=bool(flags & (1<<10))
                pts=[]
                if dsize>0:
                    base=r.o
                    for k in range(nv):
                        o=base+k*vsize
                        if full: pts.append(struct.unpack_from('<3f',b,o))
                        else: pts.append(tuple(half(x) for x in struct.unpack_from('<3H',b,o)))
                blk['verts']=pts; blk['nv']=nv; blk['ntri']=ntri; blk['full']=full; blk['vsize']=vsize
        blocks.append(blk); r.o=start+sizes[i]
    return blocks
def world(blocks):
    """returns {name: world translation} by walking node hierarchy from block 0"""
    out={}
    def mat(R): return [R[0:3],R[3:6],R[6:9]]
    def mul(A,B): return [[sum(A[i][k]*B[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    def app(A,v): return tuple(sum(A[i][k]*v[k] for k in range(3)) for i in range(3))
    def visit(i,R,T,S):
        bl=blocks[i]
        if 't' not in bl: return
        Rl=mat(bl['R']); tl=bl['t']; sl=bl['s']
        Tw=tuple(T[k]+S*app(R,tl)[k] for k in range(3)); Rw=mul(R,Rl); Sw=S*sl
        bl['wT']=Tw; bl['wR']=Rw; bl['wS']=Sw
        if bl.get('name'): out[bl['name']]=Tw
        for c in bl.get('children',[]):
            if 0<=c<len(blocks): visit(c,Rw,Tw,Sw)
    visit(0,[[1,0,0],[0,1,0],[0,0,1]],(0,0,0),1.0)
    return out
if __name__=='__main__':
    bl=load(sys.argv[1]); w=world(bl)
    from collections import Counter
    print(Counter(b['type'] for b in bl))
    for b in bl:
        if 'verts' in b and b['verts']:
            v=b['verts']; mn=[min(p[k] for p in v) for k in range(3)]; mx=[max(p[k] for p in v) for k in range(3)]
            print('shape',b['name'],b['type'],'nv',b['nv'],'full',b['full'],'vsize',b['vsize'],'min',[round(x,2) for x in mn],'max',[round(x,2) for x in mx],'xform',[round(x,2) for x in b['t']])
    for n,p in sorted(w.items()):
        if 'bone' in n.lower() or n in ('Head','HEAD','Neck'): print('%-28s %8.3f %8.3f %8.3f'%(n,*p))

def skin(path):
    """Returns (bones names list, per-bone skin transforms) from BSSkin::Instance/BoneData."""
    import struct
    b=open(path,'rb').read(); r=R(b)
    nl=b.index(b'\n'); r.o=nl+1
    ver=r.u32(); r.u8(); r.u32(); nblocks=r.u32(); bsver=r.u32()
    for _ in range(4): n=r.u8(); r.raw(n)
    ntypes=r.u16(); types=[r.raw(r.u32()).decode('latin1') for _ in range(ntypes)]
    tidx=[r.u16() for _ in range(nblocks)]; sizes=[r.u32() for _ in range(nblocks)]
    nstr=r.u32(); r.u32(); strings=[r.raw(r.u32()).decode('latin1') for _ in range(nstr)]
    ng=r.u32(); [r.u32() for _ in range(ng)]
    starts=[];o=r.o
    for s in sizes: starts.append(o); o+=s
    inst=None; data=None
    for i in range(nblocks):
        t=types[tidx[i]]
        if t=='BSSkin::Instance':
            r.o=starts[i]; root=r.i32(); bd=r.i32(); nb=r.u32(); bones=[r.i32() for _ in range(nb)]
            inst=(root,bd,bones)
        if t=='BSSkin::BoneData':
            r.o=starts[i]; nb=r.u32(); data=[]
            for _ in range(nb):
                sph=r.f(4); Rm=r.f(9); T=r.f(3); S=r.f(); data.append((Rm,T,S,sph))
    return inst,data
