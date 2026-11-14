# group.py - STUB until its port (job E2-E5).
from __future__ import annotations

import cli

from typing import List


class Opts:
    pass


def args(cmd: str, kind: str, argv: List[str]) -> Opts:
    cli.fail(3, "group.args is not ported yet")
    return Opts()


def init(kind: str, group: str, o: Opts) -> None:
    cli.fail(3, "group.init is not ported yet")


def destroy(kind: str, group: str, o: Opts) -> None:
    cli.fail(3, "group.destroy is not ported yet")


def template(kind: str, group: str, ipv4: bool) -> None:
    cli.fail(3, "group.template is not ported yet")
