# deploy.py - STUB until its port (job E2-E5).
from __future__ import annotations

import cli

from typing import List


def scan(dir: str) -> List[str]:
    cli.fail(3, "deploy.scan is not ported yet")
    return []


def deploy(conf_file: str, args: List[str]) -> None:
    cli.fail(3, "deploy.deploy is not ported yet")


def undeploy(conf_file: str, args: List[str]) -> None:
    cli.fail(3, "deploy.undeploy is not ported yet")
