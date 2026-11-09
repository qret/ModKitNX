# -*- coding: utf-8 -*-
"""
nso_min — минимальный разбор NSO0 (сегменты, .dynsym, DT_*) для хост-проверок.

Нужен двум местам: сверке хеша с оригиналом (эталон — NSO системной библиотеки)
и проверке собранного модуля (оба символа видны как GLOBAL FUNC, импортов нет).
Понимает сжатие LZ4 (обычные NSO) и ZBIC: для ZBIC берётся пакет `zbic`, если он
есть, иначе наш scripts/zbic_pure.py — чистый Python без внешних зависимостей.
"""
import struct, hashlib
from pathlib import Path

def _zbic_decompress(blob):
    """ZBIC: быстрый путь — пакет, запасной — наш разбор формата на Python."""
    try:
        import zbic
        return zbic.decompress(blob)
    except ImportError:
        pass
    import importlib.util
    p = Path(__file__).resolve().parent.parent / "scripts" / "zbic_pure.py"
    if not p.is_file():
        raise ImportError("нечем распаковать ZBIC: нет ни пакета zbic, ни %s" % p)
    spec = importlib.util.spec_from_file_location("nxr_zbic_pure", p)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.decompress(blob)

def lz4_block(src):
    d=bytearray(); i=0
    while i<len(src):
        t=src[i]; i+=1
        l=t>>4; m=t&15
        if l==15:
            while True:
                b=src[i]; i+=1; l+=b
                if b!=255: break
        d+=src[i:i+l]; i+=l
        if i>=len(src): break
        off=src[i]|(src[i+1]<<8); i+=2
        ml=m+4
        if m==15:
            while True:
                b=src[i]; i+=1; ml+=b
                if b!=255: break
        s=len(d)-off
        for k in range(ml): d.append(d[s+k])
    return bytes(d)

class NSO:
    def __init__(self,path):
        self.path=path
        raw=open(path,'rb').read(); self.raw=raw
        assert raw[:4]==b'NSO0'
        flags,=struct.unpack_from('<I',raw,0xC); self.flags=flags
        self.buildid=raw[0x40:0x60].hex().upper()
        self.segs=[];self.meta=[]
        for n in range(3):
            fo,mo,sz=struct.unpack_from('<III',raw,0x10+n*0x10)
            csz,=struct.unpack_from('<I',raw,0x60+n*4)
            blob=raw[fo:fo+csz]
            if (flags>>n)&1:
                d=_zbic_decompress(blob) if (flags&0x80) else lz4_block(blob)
            else: d=blob[:sz]
            assert len(d)==sz,(path,n,len(d),sz)
            if (flags>>(n+3))&1:
                h=raw[0xA0+n*0x20:0xC0+n*0x20]
                assert hashlib.sha256(d).digest()==h,(path,n,'hash')
            self.segs.append(d); self.meta.append((mo,sz))
        self.bss,=struct.unpack_from('<I',raw,0x3c)
        self.text,self.ro,self.data=self.segs
        self.TMEM,self.TSZ=self.meta[0]; self.RMEM,self.RSZ=self.meta[1]; self.DMEM,self.DSZ=self.meta[2]
        self.BMEM=self.DMEM+self.DSZ
        self._dyn()
    def mem(self,a,n):
        if self.TMEM<=a<self.TMEM+self.TSZ: return self.text[a-self.TMEM:a-self.TMEM+n]
        if self.RMEM<=a<self.RMEM+self.RSZ: return self.ro[a-self.RMEM:a-self.RMEM+n]
        if self.DMEM<=a<self.DMEM+self.DSZ: return self.data[a-self.DMEM:a-self.DMEM+n]
        if self.BMEM<=a<self.BMEM+self.bss: return b'\0'*n
        return b'\0'*n
    def where(self,a):
        if self.TMEM<=a<self.TMEM+self.TSZ: return '.text'
        if self.RMEM<=a<self.RMEM+self.RSZ: return '.rodata'
        if self.DMEM<=a<self.DMEM+self.DSZ: return '.data'
        if self.BMEM<=a<self.BMEM+self.bss: return '.bss'
        return 'OUT'
    def _dyn(self):
        mod_off,=struct.unpack_from('<I',self.text,4); self.mod_off=mod_off
        magic,dyn_rel=struct.unpack('<4si',self.mem(mod_off,8)); assert magic==b'MOD0'
        self.dynaddr=mod_off+dyn_rel
        DT={};a=self.dynaddr;self.dynents=[]
        while True:
            tag,val=struct.unpack('<qQ',self.mem(a,16))
            if tag==0: break
            DT.setdefault(tag,[]).append(val); self.dynents.append((tag,val)); a+=16
        self.DT=DT
        self.strtab=DT[5][0]; self.strsz=DT[10][0]
        self.symtab=DT[6][0]; self.syment=DT[11][0]
        self.gnuhash=DT.get(0x6ffffef5,[None])[0]
        self.nsyms=self._nsyms()
    def s(self,o):
        b=self.mem(self.strtab+o,1024)
        i=b.find(b'\0')
        return b[:i].decode('utf8','replace')
    def _nsyms(self):
        g=self.gnuhash
        if g is None:
            return (self.strtab-self.symtab)//self.syment
        nbuck,symoff,maskwords,shift=struct.unpack('<IIII',self.mem(g,16))
        bucket_off=g+16+maskwords*8
        buckets=struct.unpack('<%dI'%nbuck,self.mem(bucket_off,nbuck*4))
        last=max(buckets) if buckets else symoff
        if last<symoff: return symoff
        chain=bucket_off+nbuck*4
        i=last
        while True:
            v,=struct.unpack('<I',self.mem(chain+(i-symoff)*4,4))
            i+=1
            if v&1: break
        return i
    def syms(self):
        out=[]
        for i in range(self.nsyms):
            nameo,info,other,shndx,val,size=struct.unpack('<IBBHQQ',self.mem(self.symtab+i*self.syment,24))
            nm=self.s(nameo) if nameo<self.strsz else ''
            out.append((i,nm,info,other,shndx,val,size))
        return out
    def exports(self):
        return {nm:(val,size,info) for i,nm,info,other,shndx,val,size in self.syms() if shndx!=0 and nm}
    def imports(self):
        return {nm:(info,) for i,nm,info,other,shndx,val,size in self.syms() if shndx==0 and nm}
    def needed(self):
        return [self.s(v) for v in self.DT.get(1,[])]
