# tools/cbase_cli: the deploy tool, in Python

`tools/cbase` (README.md "./cbase deploy tool") is a launcher for the Python program in this directory.
Python is the language of complex non-production work with logic, and deployment is that work: the tool links to none of cbase's code and runs only `aws`, `sha256sum` (or `shasum -a 256`) and `make -C <app> serve-binary`.
The C version (PRs #41 to #43) is replaced outright, with no fallback.

## The contract

Every line of stdout and stderr and every exit status is the contract, and the oracles hold it: `tools/cbase.d/test/cbase_test.sh`, `cbase_vm_test.sh`, `cbase_group_test.sh` (all black box, through the real AWS CLI against `tools/cbase.d/test/fake_aws.c`) and `tools/cbase_cli/test/vm_render_test.sh` (the VM renders against their golden files).
foolish and amgi pin cbase as a submodule and call `tools/cbase`, so the command line, the output and the exit codes do not change with the language.

## Layout

One module per job, named as the C files were, so each reads against the C it replaced in git history:

| module | job |
| --- | --- |
| `cbase.py` | the entry: the Python 3.9 floor, checked in syntax any Python 3 parses, then `main.main()` |
| `cli.py` | errors and exit statuses, the usage text, the one process runner, the run's temporary directory and its removal on exit and on SIGINT, SIGTERM and SIGHUP, the integer parser, splitting, cksum, the deploy flags |
| `conf.py` | cbase.conf read and checked, every error with its line number |
| `aws.py` | the one `aws` caller: throttling retry, error codes, masking, the bounded poll, the S3 conditional put |
| `state.py` | a group's state file in S3 |
| `stack.py` | the template render and the CloudFormation change-set flow |
| `static.py` | the build scan, the per-file headers, the uploads, the prune |
| `cert.py` | ACM, Route 53 and CloudFront aliases |
| `group.py` | group-init, group-destroy, group-template |
| `vm.py`, `vm_remote.py`, `vm_boot.py` | the VM side and the scripts sent over SSM |
| `deploy.py` | deploy and undeploy, step by step |
| `main.py` | the option and command dispatch |

The templates, `static_rules.tsv` and the VM scripts stay text in `tools/cbase.d/` and are read at run time from the path of this directory, so a moved checkout needs nothing rebuilt.

## The rules

- Standard library only, and Python 3.9 is the floor (macOS's `/usr/bin/python3` is 3.9.6): no `match`, no `X | Y` outside annotations, nothing added after 3.9. Every module starts with `from __future__ import annotations`.
- No JSON, read or written: every AWS call is `--query <JMESPath> --output text`, split on tabs and newlines, as before.
- The launcher runs `python3 -I -B`: `-I` keeps PYTHONPATH, the user's site packages and the current directory out of the import path, and `-B` writes no `__pycache__` into an app's submodule checkout.
- Text is `str`, decoded and encoded as UTF-8 with `surrogateescape` everywhere (stdio, files, child output), so bytes that are not UTF-8 pass through unchanged; files are opened with `newline=""`, so nothing is translated.
- Byte order: a sort that was `strcmp` (LC_ALL=C) sorts by the UTF-8 bytes (`cli.sort_bytes`).
- Integers from text go through `cli.parse_int` (`lib/num_parse.h`'s rule: `-?[0-9]+`, ASCII digits only, within a range), never bare `int()`, which takes blanks, `+`, `_` and non-ASCII digits.
- One process runner, `cli.run`: stdin from /dev/null, stdout and stderr captured or inherited, stdout and stderr flushed before the child starts, a program that cannot start is status 127 and a child killed by a signal is 128 + its number, as before.
- Signals: SIGPIPE is the default action (a closed reader ends the tool, as it ended the C one). SIGINT, SIGTERM and SIGHUP remove the run's temporary directory and then end the process by that same signal, so its status is 128 + the signal; a signal that was ignored when the run started stays ignored.
- No randomness and no clock in the logic: the throttling stagger is the app name's cksum and the state file's backoff is a fixed doubling, as before.
