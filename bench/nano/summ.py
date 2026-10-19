import sys,os,glob,re,bisect,collections
out=sys.argv[1]
def rd(f):
    try: return open(f).read()
    except: return ""
def stat(txt):
    d={}
    for l in txt.splitlines():
        p=l.split()
        if len(p)==2 and p[1].isdigit(): d[p[0]]=int(p[1])
    nums=[l.strip() for l in txt.splitlines() if re.fullmatch(r'\d+',l.strip())]
    return d,nums
moves=[]; recv=collections.defaultdict(list); errs=collections.Counter(); refuse=collections.Counter(); stalls=0
owner={}
for f in glob.glob(out+'/rdv*/ev.*'):
    who=f.rsplit('ev.',1)[1]
    for l in open(f):
        p=l.split()
        if not p: continue
        if p[0]=='M': moves.append((p[1],int(p[2]),int(p[3]),int(p[4]),who))
        elif p[0]=='R': recv[(p[1],who)].append((int(p[3]),int(p[2])))
        elif p[0]=='E': errs[p[1]+' '+re.sub(r'/[A-Za-z0-9_-]{8,}','/:id',p[2])]+=1
        elif p[0]=='X': refuse[' '.join(p[3:])]+=1
        elif p[0]=='S': stalls+=1
signfail=sum(1 for f in glob.glob(out+'/*.err') if 'sign-up failed' in rd(f))
signfail_detail=collections.Counter(re.sub(r'^\S+: ','',l) for f in glob.glob(out+'/*.err') for l in rd(f).splitlines() if 'sign-up failed' in l)
other_err=collections.Counter(re.sub(r'^\S+: ','',re.sub(r'\d+','N',l)) for f in glob.glob(out+'/*.err') for l in rd(f).splitlines() if ('failed' in l or 'STALL' in l or 'refused' in l) and 'sign-up' not in l)
gids=collections.defaultdict(set)
for (g,w) in recv: gids[g].add(w)
lat=sorted((m[3]-m[2])/1000 for m in moves)
def pc(a,q): return a[min(len(a)-1,int(q*(len(a)-1)+0.5))] if a else float('nan')
# push: opponent's first receipt of a version > v after the move started
push=[]; lost=0
bygame=collections.defaultdict(list)
for m in moves: bygame[m[0]].append(m)
for g,ms in bygame.items():
    ms.sort(key=lambda m:m[2])
    for idx,m in enumerate(ms):
        opp=[w for w in gids[g] if w!=m[4]]
        best=None
        for w in opp:
            for t,v in recv[(g,w)]:
                if v>m[1] and t>=m[2]-1000 and (best is None or t<best): best=t
        if best is None:
            if idx<len(ms)-1: lost+=1
        else: push.append((best-m[2])/1000)
push.sort()
tgo=float(rd(out+'/t_go') or 0); tdone=float(rd(out+'/t_done') or 0)
win=(max(m[3] for m in moves)-min(m[2] for m in moves))/1e6 if moves else float('nan')
b,bn=stat(rd(out+'/srv_before.txt'))
if os.path.exists(out+'/srv_go.txt'): b.update(stat(rd(out+'/srv_go.txt'))[0]); a,an=stat(rd(out+'/srv_after.txt'))
cpu_s=(a.get('usage_usec',0)-b.get('usage_usec',0))/1e6
thr=(a.get('nr_throttled',0)-b.get('nr_throttled',0)); per=(a.get('nr_periods',0)-b.get('nr_periods',0))
logb=int(an[0])-int(bn[0]) if an and bn else 0
diskk=int(an[1].split()[0]) if False else None
dk=re.findall(r'^(\d+)\s+/data/cbase',rd(out+'/srv_after.txt'),re.M); dk0=re.findall(r'^(\d+)\s+/data/cbase',rd(out+'/srv_before.txt'),re.M)
peak=an[-1] if an else '?'
lc,_=stat(rd(out+'/load_cpu.txt'))
# rss from sampler in window
rssmax=0
for f in glob.glob(os.path.dirname(os.path.dirname(out))+'/rss_*.log'):
    for l in open(f):
        p=l.split()
        if len(p)>=4 and p[2].isdigit():
            t=float(p[0])
            if tgo-2<=t<=tdone+2: rssmax=max(rssmax,int(p[2]))
# server CPU inside [t_go, t_done] from the sampler's cumulative ticks (100 Hz)
samp=[]
for f in glob.glob(os.path.dirname(os.path.dirname(out))+'/rss_*.log'):
    for l in open(f):
        p=l.split()
        if len(p)>=5 and p[4].isdigit(): samp.append((float(p[0]),int(p[4]),p[1]))
samp.sort()
def ticks_at(t):
    prev=None
    for x in samp:
        if x[0]>=t:
            if prev is None or prev[2]!=x[2]: return x[1]
            return prev[1]+(x[1]-prev[1])*(t-prev[0])/max(1e-9,x[0]-prev[0])
        prev=x
    return prev[1] if prev else 0
mv_cpu=(ticks_at(tdone)-ticks_at(tgo))/100.0 if samp else float('nan')
games_over=sum(rd(f).count(' over at ') for f in glob.glob(out+'/a*.err'))
n=len(moves)
print(f"label={os.path.basename(out)} window_s={win:.1f} moves={n} moves_per_s={n/win if win==win and win>0 else 0:.2f} games_over={games_over}")
print(f"  move_ms p50={pc(lat,.5):.1f} p95={pc(lat,.95):.1f} p99={pc(lat,.99):.1f} max={lat[-1] if lat else 0:.1f}")
print(f"  push_ms(move start->opponent receipt) p50={pc(push,.5):.1f} p95={pc(push,.95):.1f} max={push[-1] if push else 0:.1f} late>2s={sum(1 for x in push if x>2000)} lost={lost}")
print(f"  server_cpu_s(go..end, cgroup)={cpu_s:.2f} play_cpu_s={mv_cpu:.2f} cores_used_in_play={mv_cpu/win if win==win and win>0 else 0:.3f} throttled={thr}/{per} server_cpu_ms_per_move={mv_cpu*1000/n if n else 0:.2f}")
print(f"  load_cpu_s={lc.get('usage_usec',0)/1e6:.2f} load_throttled={lc.get('nr_throttled',0)}/{lc.get('nr_periods',0)}")
print(f"  rss_max_kb={rssmax} cgroup_mem_peak={peak} log_bytes={logb} log_bytes_per_move={logb/n if n else 0:.0f} db_kb={dk0[0] if dk0 else '?'}->{dk[0] if dk else '?'}")
print(f"  http_err={dict(errs)} refused={dict(refuse)} stalls={stalls} signup_failed={signfail} {dict(signfail_detail)} other={dict(other_err)}")
