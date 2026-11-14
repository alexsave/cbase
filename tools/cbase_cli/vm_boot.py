# vm_boot.py - vm_boot.sh's user-data text (the port of vm_boot.c): the group's boot script (tools/cbase.d/
# vm_boot.sh), the instance's user-data. The script itself stays bash: it runs on the VM at every boot. The
# tool only reads it, to put its code into the group's template (group.sh's T_BOOT).
from __future__ import annotations

from typing import List

import vm


def _dropped(line: str) -> bool:
    """A line is dropped when it is empty or when its first non-blank character is '#'."""
    if line == "":
        return True
    return line.lstrip(" \t\f\v\r").startswith("#")


def userdata() -> str:
    """vm_boot.sh without its comment and blank lines, the shebang kept (awk 'NR == 1 || (!/^[[:space:]]*#/
    && !/^$/)'). The template step (the ${ escapes, the indent and the origin's address) is the group's."""
    text = vm.read(vm.file("vm_boot.sh"))
    out: List[str] = []
    nr = 0
    for line in vm.lines_of(text):
        nr += 1
        if nr == 1 or not _dropped(line):
            out.append(line)
            out.append("\n")
    return "".join(out)
