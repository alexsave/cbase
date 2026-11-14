# aws.py - STUB until its port (job E2-E5).
from __future__ import annotations

import cli

account = ""


def init() -> None:
    cli.fail(3, "aws.init is not ported yet")


def region() -> str:
    cli.fail(3, "aws.region is not ported yet")
    return ""


def group_find(kind: str, name: str) -> "str | None":
    """The group's id, or None when it is not found (the C returned a status and set *id)."""
    cli.fail(3, "aws.group_find is not ported yet")
    return None
