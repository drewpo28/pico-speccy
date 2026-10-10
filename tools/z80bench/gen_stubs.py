# tools/z80bench: generate a weak stub for every symbol the core objects (Z80_JLS.o +
# CPU.o) need and harness.cpp does not define — functions return 0, data is zeroed
# storage of the firmware's own size. Reads undef.txt + fw.sym from the cwd (the
# bench's work dir), writes stubs.s. Symbol kinds come from the firmware ELF's
# STT_FUNC/STT_OBJECT, not from nm letters: some data lives in .time_critical.
import sys,subprocess
undef=[l.strip() for l in open('undef.txt') if l.strip()]
# Defined by harness.cpp itself.
own={'_ZN5Ports5inputEt','_ZN5Ports6outputEth','_ZN5VIDEO10tsDrawTickEv','time_us_64'}
fw={}
for l in open('fw.sym'):
    p=l.split()
    if len(p)!=3: continue
    name,typ,size=p
    try: size=int(size,0)
    except: size=0
    fw.setdefault(name,('T' if typ=='FUNC' else 'D', size))
libc={'memcmp','strlen','memcpy','memset','memmove'}
out=['.syntax unified','.thumb']
for s in undef:
    if s in own or s in libc or s.startswith('_ZNSt7__cxx11'): continue
    t,sz=fw.get(s,('T',0))
    if t in 'TtWw':
        out+=['.text','.weak '+s,'.thumb_func','.type %s,%%function'%s,s+':','movs r0,#0','movs r1,#0','bx lr']
    else:
        sz=max(sz,8); sz=max(sz,64) if sz<64 else sz
        out+=['.bss','.weak '+s,'.balign 8','.type %s,%%object'%s,s+':','.space %d'%sz]
open('stubs.s','w').write('\n'.join(out)+'\n')
