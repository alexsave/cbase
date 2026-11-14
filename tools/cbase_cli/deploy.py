# deploy.py - STUB until its port (job E2-E5).
from __future__ import annotations

import cli
import stack
import static

from typing import List


def scan(dir: str) -> List[str]:
    """The build's files (static.scan), and their list written for the router's page list and the app
    shell check (stack.stk_files)."""
    files = static.scan(dir)
    stack.stk_files = cli.tmp_path("static-files")
    cli.write_file(stack.stk_files, "".join(f + "\n" for f in files))
    return files


def deploy(conf_file: str, args: List[str]) -> None:
    cli.fail(3, "deploy.deploy is not ported yet")


def undeploy(conf_file: str, args: List[str]) -> None:
    cli.fail(3, "deploy.undeploy is not ported yet")
