# tools/z80bench: fold prof.c's per-address counts by symbol. usage: symprof.py <elf> <prof.txt>
import sys,subprocess,bisect
elf,prof=sys.argv[1],sys.argv[2]
nm=subprocess.run(['arm-none-eabi-nm','-nC','--defined-only',elf],capture_output=True,text=True).stdout.split('\n')
syms=[]
for l in nm:
    p=l.split(' ',2)
    if len(p)==3 and p[1] in 'TtWw': syms.append((int(p[0],16),p[2]))
syms.sort(); addrs=[a for a,_ in syms]
agg={}; tot=0
for l in open(prof):
    a,c=l.split(); a=int(a,16); c=int(c); tot+=c
    i=bisect.bisect_right(addrs,a)-1
    n=syms[i][1] if i>=0 else '?'
    agg[n]=agg.get(n,0)+c
for n,c in sorted(agg.items(),key=lambda x:-x[1])[:40]: print('%6.2f%% %12d %s'%(100*c/tot,c,n))
