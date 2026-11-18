apk add --no-cache strace >/dev/null
export CBASE_LISTEN=0.0.0.0:8642
strace -f -k -e trace=mmap,brk,munmap,clone,clone3,mprotect -o /out/strace_u.txt /out/minimal.unstripped serve >/dev/null 2>&1 &
sleep 4; pid=$(pgrep -f "^/out/minimal.unstripped" | head -1); cat /proc/$pid/smaps > /out/smaps_strace_u.txt; kill %1
