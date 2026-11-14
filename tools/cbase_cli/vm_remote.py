# vm_remote.py - the scripts SSM runs on a VM (the port of vm_remote.c). The deploy is tools/cbase.d/vm_remote.sh
# with the app's values in a header and its two units spliced in; the NIC interrupts' setup is vm_irq_setup.sh
# with its program and unit spliced in, its check a file as it is; the undeploy and the data list are literal
# scripts. The text is the shell's, byte for byte: a port must not change what runs on the instance, and
# vm_remote.sh itself stays bash, read from its file at run time.
from __future__ import annotations

from typing import Dict, List

import conf as confmod
import vm

conf = confmod.conf


def _add_lines(out: List[str], text: str) -> None:
    """The lines of TEXT, each ended by a newline (awk's print of each line of the unit)."""
    for line in vm.lines_of(text):
        out.append(line)
        out.append("\n")


def _splice(script: str, parts: Dict[str, str]) -> str:
    """The file SCRIPT (under cbase.d/) from its second line on (the shebang is dropped), each line that is
    a marker of PARTS replaced by the lines of its text (awk's splice in vm.sh's vm_remote_deploy)."""
    text = vm.read(vm.file(script))
    nl = text.find("\n")
    rest = text[nl + 1:] if nl >= 0 else ""   # NR == 1 is the shebang, dropped
    out: List[str] = []
    for line in vm.lines_of(rest):
        if line in parts:
            _add_lines(out, parts[line])
        else:
            out.append(line)
            out.append("\n")
    return "".join(out)


def deploy(port: str, bucket: str, key: str, sha: str, region: str, state_bucket: str) -> str:
    """The deploy script of the app in conf on the VM, on PORT, with the artifact at BUCKET/KEY (sha256 SHA),
    REGION, and STATE_BUCKET (the backup bucket's default): the values in its header, then
    tools/cbase.d/vm_remote.sh with its unit (vm.unit) and socket (vm.socket) spliced in."""
    app = conf.app
    data = vm.data_dir()
    settings = vm.settings()
    # The header: the heredoc of vm.sh's vm_remote_deploy, with its values in as they are (every one is a
    # name, digits, hex, a path or a bucket and region name this tool checked).
    head = ("#!/bin/bash\n"
            "# The deploy of cbase app %s on its VM, rendered by ./cbase deploy (tools/cbase.d/vm.sh) from\n"
            "# vm_remote.sh, and run as root by SSM.\n"
            "app=%s\n"
            "port=%s\n"
            "user=cbase-%s\n"
            "unit=cbase-%s.service\n"
            "sock=cbase-%s.socket\n"
            "home=\"${CBASE_VM_ROOT:-}/srv/cbase/%s\"\n"
            "data=%s\n"
            "bucket=%s\n"
            "key=%s\n"
            "sha=%s\n"
            "region=%s\n"
            "settings=%s\n"
            "replaces=%s\n"
            % (app, app, port, app, app, app, app, data, bucket, key, sha, region, settings, conf.vm_replaces))
    unit = vm.unit(port, region, vm.backup_bucket(state_bucket))
    socket = vm.socket(port)
    return head + _splice("vm_remote.sh", {"@CBASE_SERVICE@": unit, "@CBASE_SOCKET@": socket})


def undeploy(delete_data: bool) -> str:
    """The script that stops and removes the app's service; DELETE_DATA also deletes its folder and user."""
    app = conf.app
    # The first heredoc is vm.sh's unquoted one: $app is its value, and \$ is a literal $.
    s = ("#!/bin/bash\n"
         "# The undeploy of cbase app %s on its VM, rendered by ./cbase undeploy (tools/cbase.d/vm.sh).\n"
         "set -euo pipefail\n"
         "R=\"${CBASE_VM_ROOT:-}\"\n"
         "app=%s\n"
         "unit=cbase-%s.service\n"
         "sock=cbase-%s.socket\n"
         "home=\"$R/srv/cbase/%s\"\n"
         "say() { echo \"CBASE: $*\"; }\n"
         "if [ -f \"$R/etc/systemd/system/$unit\" ]; then\n"
         "  # The socket first: with it listening, a connection would start the service again.\n"
         "  systemctl disable --now --quiet \"$sock\" 2> /dev/null || true\n"
         "  systemctl disable --now --quiet \"$unit\" || true\n"
         "  rm -f \"$R/etc/systemd/system/$unit\" \"$R/etc/systemd/system/$sock\"\n"
         "  systemctl daemon-reload\n"
         "  say \"$unit and $sock stopped, disabled and removed\"\n"
         "else\n"
         "  say \"$unit is not installed\"\n"
         "fi\n"
         "rm -f \"$home/serve\" \"$home/serve.new\" \"$R/etc/cbase/$app.env\" \"$R/etc/cbase/$app.loaded\"\n"
         % (app, app, app, app, app))
    if delete_data:
        s += ("rm -rf \"$home\"\n"
              "user=\"cbase-$app\"\n"
              "if id -u \"$user\" > /dev/null 2>&1; then userdel \"$user\" || true; fi\n"
              "say \"deleted /srv/cbase/$app and its data (--delete-data)\"\n")
    else:
        s += "if [ -d \"$home/data\" ]; then say \"kept /srv/cbase/$app/data (./cbase undeploy --delete-data deletes it)\"; fi\n"
    return s


def data() -> str:
    """The script that names each app whose data is on the instance (group-destroy)."""
    return ("#!/bin/bash\n"
            "set -euo pipefail\n"
            "R=\"${CBASE_VM_ROOT:-}\"\n"
            "for d in \"$R\"/srv/cbase/*/data; do\n"
            "  [ -d \"$d\" ] || continue\n"
            "  a=\"${d%/data}\"\n"
            "  echo \"CBASE: data ${a##*/}\"\n"
            "done\n")


def irq_setup(mode: str) -> str:
    """The script that sets the VM's NIC interrupts for CBASE_VM_IRQ = MODE (split or irqbalance): the mode in
    its header, then tools/cbase.d/vm_irq_setup.sh with the program (vm_irq.sh, whole) and its unit
    (vm_irq.service) spliced in."""
    head = ("#!/bin/bash\n"
            "# The NIC interrupts' setup of cbase app %s's VM (CBASE_VM_IRQ = %s), rendered by ./cbase deploy\n"
            "# (tools/cbase_cli/vm_remote.py) from vm_irq_setup.sh, and run as root by SSM.\n"
            "irq=%s\n" % (conf.app, mode, mode))
    return head + _splice("vm_irq_setup.sh", {"@CBASE_IRQ_SH@": vm.read(vm.file("vm_irq.sh")),
                                              "@CBASE_IRQ_UNIT@": vm.read(vm.file("vm_irq.service"))})


def irq_check() -> str:
    """The read-only check of the VM's NIC interrupts, tools/cbase.d/vm_irq_check.sh as it is."""
    return vm.read(vm.file("vm_irq_check.sh"))
