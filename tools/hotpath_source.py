#!/usr/bin/env python3
# hotpath_source.py - the source half of hotpath_proof.sh: each named C function's text, as BASE
# (default origin/main) has it and as the working tree has it, compared; prints one line per
# function and exits 1 when any differs or is missing. A function is found by its definition line
# (a line starting with its return type and ending in "<name>(...) {") and runs to the brace that
# closes it.
#   c_base/tools/hotpath_source.py [-b BASE] file:function...
import re, subprocess, sys, os
root = subprocess.check_output(['git', 'rev-parse', '--show-toplevel'], cwd=os.path.dirname(os.path.abspath(__file__))).decode().strip()
args = sys.argv[1:]
base = 'origin/main'
if args[:1] == ['-b']: base, args = args[1], args[2:]
def body(text, name):
    m = re.search(r'^[A-Za-z_][^\n;]*\b' + re.escape(name) + r'\([^;{]*\)\s*\{', text, re.M)
    if not m: return None
    i, depth = m.end() - 1, 0
    for j in range(i, len(text)):
        if text[j] == '{': depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0: return text[m.start():j + 1]
    return None
bad = 0
for spec in args:
    path, name = spec.split(':')
    old = subprocess.run(['git', 'show', f'{base}:{path}'], cwd=root, capture_output=True, text=True).stdout
    new = open(os.path.join(root, path)).read()
    a, b = body(old, name), body(new, name)
    if a is None or b is None: print(f'  MISSING  {name} ({path})'); bad += 1
    elif a != b: print(f'  DIFFERS  {name} ({path})'); bad += 1
    else: print(f'  same     {name} ({path}, {a.count(chr(10)) + 1} lines)')
print('VERDICT: ' + ('every function\'s source is the same text as ' + base if not bad else f'{bad} function(s) differ'))
sys.exit(1 if bad else 0)
