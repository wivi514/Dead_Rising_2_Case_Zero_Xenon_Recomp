#!/usr/bin/env python3
"""Map every texture an F9 census binds back to the zombie model file it came from.

WHY THIS EXISTS (2026-10-07, open item 0zf): a census names textures by guest ADDRESS,
which says nothing about which zombie part a draw is. The close-range zombie models
(`data/models/zombies.big` -> `<type>_2.big` / `<type>_2.tex`) are part kits, and the
question "which head is this close zombie wearing" needs every bound texture identified
by NAME. This reads 256 bytes of each bound texture out of the LIVE process
(process_vm_readv, no stop) and finds its first 64 bytes inside the unpacked `_2.tex`
containers. A small or flat texture can match several files — all are printed, so read
the long, unique ones.

Usage: zombie_tex_census.py <pid> <session.log> <capture_fNNN.census> [vs-prefix] --texdir DIR
DIR holds the unpacked `<type>_2.tex` files (big_list --extract + big_decompress).
.tex layout: u32 count at 0xC, then 0x1C-byte records from 0x18:
(name_off, hash, size, size, data_off, 0x10, 0), each texture a .bct (05 01 01 xx).
"""
import ctypes,re,sys,glob,struct,collections,os
texdir=sys.argv[sys.argv.index('--texdir')+1]; sys.argv=sys.argv[:sys.argv.index('--texdir')]
pid=int(sys.argv[1]); log=sys.argv[2]; census=sys.argv[3]; vsf=sys.argv[4] if len(sys.argv)>4 else '70bb3e'
base=int(re.search(r'guest memory at 0x([0-9a-fA-F]+)',open(log,errors='replace').read()).group(1),16)
libc=ctypes.CDLL(None,use_errno=True)
class iov(ctypes.Structure): _fields_=[('b',ctypes.c_void_p),('l',ctypes.c_size_t)]
def rd(a,n):
    buf=ctypes.create_string_buffer(n); l=iov(ctypes.addressof(buf),n); r=iov(a,n)
    got=libc.process_vm_readv(pid,ctypes.byref(l),1,ctypes.byref(r),1,0)
    return buf.raw[:got] if got>0 else b''
files={}
for f in glob.glob(texdir+'/*_2.tex'):
    d=open(f,'rb').read(); n=struct.unpack_from('<I',d,0xc)[0]; idx=[]
    for i in range(n):
        no,h,s1,s2,off,x,y=struct.unpack_from('<7I',d,0x18+i*0x1c)
        idx.append((d[no:d.index(b'\0',no)].decode(),off,s1))
    files[os.path.basename(f)[:-6]]=(d,idx)
use=collections.Counter(); draws=collections.defaultdict(list)
for line in open(census,errors='replace'):
    if not line.startswith('draw'): continue
    if f'vs={vsf}' not in line: continue
    dn=int(line.split()[1])
    for m in re.finditer(r's\d+=([0-9A-F]{8})',line): use[m.group(1)]+=1; draws[m.group(1)].append(dn)
res={}
for a in use:
    blk=rd(base+0xA0000000+int(a,16),256)
    if len(blk)<256 or blk.count(blk[:8])>20: res[a]='(uniform/unreadable)'; continue
    hits=[]
    for fn,(d,idx) in files.items():
        i=d.find(blk[:64])
        if i>=0:
            for name,off,size in idx:
                if off<=i<off+size: hits.append(f'{fn}:{name}')
    res[a]=','.join(hits) if hits else '?'
for a,_ in sorted(use.items(),key=lambda x:res[x[0]]):
    print(f'{a} draws={use[a]:3d} {res[a]}  first={draws[a][0]}')
