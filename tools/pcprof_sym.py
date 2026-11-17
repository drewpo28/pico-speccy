import sys,re,bisect
L,SY,start=sys.argv[1],sys.argv[2],int(sys.argv[3])
lines=open(L,errors='replace').read().split('\n')
dumps=[]; cur=None
for i,l in enumerate(lines):
    if i<start: continue
    if l.startswith('PROF ') and 'smp' in l: cur={}; dumps.append(cur); body=l.split('smp:')[1]
    elif l.startswith('PROF+') and cur is not None: body=l[5:]
    else: cur=None; continue
    for m in re.finditer(r'\b([0-9A-F]{8}):(\d+)\b',body):
        c=int(m.group(2))
        if c<=1000: cur[m.group(1)]=cur.get(m.group(1),0)+c
good=[d for d in dumps if 700<=sum(d.values())<=1050]
print(len(dumps),'dumps,',len(good),'clean (sum 700..1050)')
acc={}
for d in good:
    for a,c in d.items(): acc[a]=acc.get(a,0)+c
syms=[]
for l in open(SY):
    p=l.split(None,3)
    try:
        if p[2] in 'tTwW': syms.append((int(p[0],16),int(p[1],16),p[3].strip()))
    except: pass
syms.sort(); A=[s[0] for s in syms]
tot={}; N=sum(acc.values())
for a,c in acc.items():
    a=int(a,16); i=bisect.bisect_right(A,a)-1
    name=syms[i][2][:58]+(' [F]' if a<0x20000000 else '') if i>=0 and a<syms[i][0]+max(syms[i][1],2) else '?%08X'%a
    tot[name]=tot.get(name,0)+c
for n,c in sorted(tot.items(),key=lambda x:-x[1])[:int(sys.argv[4]) if len(sys.argv)>4 else 30]: print(f"{100*c/N:5.1f}%  {n}")
