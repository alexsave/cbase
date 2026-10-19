cp /nano/bin/serve-E.debug /usr/local/bin/cbase
D=/nano/runs/prof_8/perf.data
perf report -i $D --no-children --sort sym -g none --stdio 2>&1 | grep -E "Samples:|^ +[0-9]" | head -22
echo === srcline
perf report -i $D --no-children --sort sym,srcline -g none --stdio 2>&1 | grep -E "^ +[0-9]" | head -24
for s in printf_core yy_reduce.isra.0 OSSL_PARAM_locate_const pipe_write sock_def_readable; do echo "=== callers of $s"; perf report -i $D --no-children --sort sym -G --stdio -S $s 2>&1 | grep -v "^#" | grep -v "^$" | head -28; done
