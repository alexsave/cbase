import sys,re
rows=[];cur=None
for l in open(sys.argv[1]):
    m=re.match(r'^([0-9a-f]+)-([0-9a-f]+) (\S+) \S+ \S+ \S+\s*(.*)$',l)
    if m:
        cur={'range':m.group(1)+'-'+m.group(2),'name':m.group(4) or '[anon]','size':(int(m.group(2),16)-int(m.group(1),16))//1024,'rss':0};rows.append(cur)
    elif l.startswith('Rss:') and cur: cur['rss']=int(l.split()[1])
    elif l.startswith('threads'): print(l.strip())
tot={}
for r in rows: tot[r['name']]=tot.get(r['name'],0)+r['rss']
print('by name:',sorted(tot.items(),key=lambda x:-x[1])[:5])
for r in sorted(rows,key=lambda r:-r['rss'])[:10]: print(r['rss'],'KB rss of',r['size'],'KB',r['name'],r['range'])
print('anon maps with rss>0 and size<=1100KB:',sum(1 for r in rows if r['name']=='[anon]' and r['rss']>0 and r['size']<=1100), 'sum rss',sum(r['rss'] for r in rows if r['name']=='[anon]' and r['size']<=1100))
