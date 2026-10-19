cp /nano/bin/serve-${L:-E}.debug /usr/local/bin/cbase
D=${D:-/nano/runs/prof_8/perf.data}
perf report -i $D --no-children --sort sym -g caller,0.5,callee --stdio -S wake_up_q 2>&1 | grep -v "^#" | grep -v '^$' | head -${N:-60}
