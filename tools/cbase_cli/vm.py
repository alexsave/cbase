# vm.py - STUB until its port (job E5).
from __future__ import annotations

import cli

# The VM this run deploys to (vm.sh's VM_* variables): the ones deploy.py reads.
origin_addr = ""      # VM_ORIGIN_ADDR: ipv6 or dualstack, "" for none
dns = ""              # VM_DNS: its public DNS name, or the group's origin
port = ""             # VM_PORT: the port the app holds
origin_secret = ""    # VM_ORIGIN_SECRET: never printed


def deploy_find(group: str) -> None:
    cli.fail(3, "vm.deploy_find is not ported yet")


def deploy_build() -> None:
    cli.fail(3, "vm.deploy_build is not ported yet")


def deploy_join(group: str, dry: bool) -> None:
    cli.fail(3, "vm.deploy_join is not ported yet")


def ship(group: str) -> bool:
    cli.fail(3, "vm.ship is not ported yet")
    return False


def prune_artifacts(dry: bool) -> None:
    cli.fail(3, "vm.prune_artifacts is not ported yet")


def service() -> str:
    cli.fail(3, "vm.service is not ported yet")
    return ""


def undeploy(group: str, delete_data: bool) -> None:
    cli.fail(3, "vm.undeploy is not ported yet")
