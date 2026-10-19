cp /nano/bin/serve-${L:-E}.debug /usr/local/bin/cbase
D=/nano/runs/prof_8/perf.data
perf report -i $D --children --sort sym -g none --stdio 2>&1 | grep -E "^ +[0-9]" | grep -E "sq_item|sqlite3_step|sqlite3_prepare|commit_or_import|cb_section_commit|flush|drain_posts|cb_loop_run|worker|cb_fanout_publish|cb_loop_post|table_|cb_section_free|sqlite3RunParser|exec$|el0_svc" | head -20
perf report -i $D --no-children --sort sym -G --stdio -S memset 2>&1 | grep -v "^#" | grep -v "^$" | sed -n 1,25p
perf report -i $D --no-children --sort sym -G --stdio -S wake_up_q 2>&1 | grep -v "^#" | grep -v "^$" | grep -E "write|futex|cond|post|wake|sendto|finish|job|pool" | head -12
