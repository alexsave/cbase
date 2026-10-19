cp /nano/bin/serve-${L:-E}.debug /usr/local/bin/cbase
D=${D:-/nano/runs/prof_8/perf.data}
perf report -i $D --no-children --sort sym -g none --stdio 2>&1 | grep -E "^ +[0-9]" | head -${N:-24}
echo === srcline
perf report -i $D --no-children --sort sym,srcline -g none --stdio 2>&1 | grep -E "^ +[0-9]" | head -${N:-24}
