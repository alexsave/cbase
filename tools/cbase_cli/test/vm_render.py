# vm_render.py - the harness for the VM renders (vm_render_test.sh): loads a cbase.conf the way the tool
# does, renders one of the VM texts and prints it to stdout; the test compares it with the golden files.
#
#   vm_render.py CONF unit PORT REGION STATE_BUCKET
#   vm_render.py CONF socket PORT
#   vm_render.py CONF deploy PORT BUCKET KEY SHA REGION STATE_BUCKET
#   vm_render.py CONF undeploy 0|1
#   vm_render.py CONF data
#   vm_render.py CONF boot
#   vm_render.py CONF artifacts ACCOUNT REGION STATE_BUCKET
#   vm_render.py CONF fit TYPE MIB
#   vm_render.py CONF b64 TEXT
from __future__ import annotations

import os
import re
import sys
from typing import List

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import cli  # noqa: E402
import conf as confmod  # noqa: E402
import vm  # noqa: E402
import vm_boot  # noqa: E402
import vm_remote  # noqa: E402


def _need(argv: List[str], n: int, what: str) -> None:
    if len(argv) < n:
        cli.fail(cli.EXIT_USAGE, "vm_render: %s wants more arguments" % what)


def _atoll(s: str) -> int:
    """C's atoll: blanks, a sign, digits, then whatever follows is ignored; 0 when there are none."""
    m = re.match(r"[ \t\n\v\f\r]*([+-]?[0-9]+)", s, re.A)
    return int(m.group(1)) if m else 0


def main(argv: List[str]) -> int:
    if len(argv) < 3:
        cli.fail(cli.EXIT_USAGE, "vm_render CONF MODE ARGS...")
    confmod.load(argv[1])
    mode = argv[2]
    if mode == "unit":
        _need(argv, 6, mode)
        out = vm.unit(argv[3], argv[4], vm.backup_bucket(argv[5]))
    elif mode == "socket":
        _need(argv, 4, mode)
        out = vm.socket(argv[3])
    elif mode == "deploy":
        _need(argv, 9, mode)
        out = vm_remote.deploy(argv[3], argv[4], argv[5], argv[6], argv[7], argv[8])
    elif mode == "undeploy":
        _need(argv, 4, mode)
        out = vm_remote.undeploy(argv[3] == "1")
    elif mode == "data":
        out = vm_remote.data()
    elif mode == "boot":
        out = vm_boot.userdata()
    elif mode == "artifacts":
        _need(argv, 6, mode)
        bucket, prefix = vm.artifacts(argv[3], argv[4], argv[5])
        cli.out("%s %s" % (bucket, prefix))
        return 0
    elif mode == "b64":
        _need(argv, 4, mode)
        cli.out(vm.b64(argv[3]))
        return 0
    elif mode == "fit":
        _need(argv, 5, mode)
        limit, what = vm.fit(argv[3], _atoll(argv[4]))
        cli.out("%d\n%s\n" % (limit, what))
        return 0
    else:
        cli.fail(cli.EXIT_USAGE, "vm_render: no mode %s" % mode)
        return 2
    cli.out(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
