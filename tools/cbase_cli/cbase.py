# cbase.py - the deploy tool's entry (README.md here): tools/cbase runs it as `python3 -I -B cbase.py ARGS`.
# It is written so that any Python 3 parses it, so an older interpreter gets this sentence and not a
# SyntaxError from a module that needs 3.9.
import sys

if sys.version_info < (3, 9):
    sys.stderr.write("cbase: the deploy tool needs Python 3.9 or newer, and %s is %d.%d\n"
                     % (sys.executable or "python3", sys.version_info[0], sys.version_info[1]))
    sys.exit(1)

import os  # noqa: E402

# -I leaves this directory off the import path: the tool's modules are its neighbours.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import main  # noqa: E402

main.main()
