cp /nano/bin/serve-${L}.debug /usr/local/bin/cbase
D=/nano/runs/prof_8/perf.data
for s in $S; do echo "##### $s"; perf report -i $D --no-children --sort sym -g caller,0.3,callee,function,percent --stdio -S $s 2>&1 | grep -v "^#" | grep -v '^$' | head -${N:-40}; done
