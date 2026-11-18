# aws_resident.py - a resident real AWS CLI for the deploy tool's tests (CBASE_TEST_AWS=resident).
# Test code only: it is started by tools/cbase.d/test/lib.sh and stopped with its script.
#
# The tests call `aws` thousands of times, and about nine tenths of their time is the CLI starting
# (importing awscli and botocore). This server imports awscli once, and serves each call from a fork
# of itself, so a call runs the real CLI with the clean process a fresh `aws` has (os.environ, the
# working directory, module state, signal handlers) at the price of a fork. tools/cbase.d/test/
# aws_client.c is the `aws` the tests find first on PATH: it sends its argv, its whole environment and
# its working directory, with its stdin, stdout and stderr as file descriptors (SCM_RIGHTS), and
# reports the child's exit code or the signal that killed it.
#
# It must run on the AWS CLI's own interpreter (lib.sh resolves `aws`'s #! line and runs this file
# with it, -I), because the CLI is imported from there.
#
#   aws_resident.py --socket PATH
#
# Wire format, a stream on the Unix socket. Client to server: u32 length N (little endian) and N bytes:
# u32 argc, u32 envc, argc NUL-ended arguments, envc NUL-ended KEY=VALUE strings, the NUL-ended
# working directory; the first bytes carry the three descriptors. Server to client: u32 pid of the
# child, then u32 kind (0 exit code, 1 killed by a signal) and u32 the code or the signal number.
# One request is served at a time: another client waits in the socket's backlog.
import argparse
import io
import locale
import os
import select
import signal
import socket
import struct
import sys
import warnings

_before = dict(os.environ)
import awscli.clidriver

# What importing awscli does to os.environ: awscli/__init__.py adds its own data directory to
# AWS_DATA_PATH (after the caller's entries, expanded), and the CLI cannot find its data without it.
# A fresh `aws` has done that to the caller's environment by the time it runs, so each child redoes
# it on the environment it was sent. Any other change the import makes is not known here: refuse.
_import_changes = {k for k in os.environ if _before.get(k) != os.environ[k]} | {k for k in _before if k not in os.environ}
if _import_changes != {"AWS_DATA_PATH"}:
    sys.exit("aws_resident: importing awscli changed %s of os.environ, not only AWS_DATA_PATH" % sorted(_import_changes))
_AWSCLI_DATA = os.path.join(os.path.dirname(os.path.abspath(awscli.__file__)), "data")


def awscli_import_environ():
    paths = []
    if "AWS_DATA_PATH" in os.environ:
        for path in os.environ["AWS_DATA_PATH"].split(os.pathsep):
            paths.append(os.path.expanduser(os.path.expandvars(path)))
    paths.append(_AWSCLI_DATA)
    os.environ["AWS_DATA_PATH"] = os.pathsep.join(paths)


def read_exact(conn, buf, n):
    while len(buf) < n:
        chunk = conn.recv(min(1 << 20, n - len(buf)))
        if not chunk:
            return None
        buf += chunk
    return buf


def parse(body):
    argc, envc = struct.unpack_from("<II", body, 0)
    parts = body[8:].split(b"\0")
    # argc + envc + 1 strings, then the empty tail after the last NUL
    if len(parts) != argc + envc + 2:
        raise ValueError("a request that does not match its counts")
    fs = os.fsdecode
    argv = [fs(p) for p in parts[:argc]]
    env = [fs(p) for p in parts[argc:argc + envc]]
    return argv, env, fs(parts[argc + envc])


def stdio_settings(env):
    """The encoding and error handler a fresh interpreter gives stdin, stdout and stderr under ENV."""
    ioenc = env.get("PYTHONIOENCODING")
    enc = errors = None
    if ioenc:
        enc, _, errors = ioenc.partition(":")
        enc = enc or None
        errors = errors or None
    utf8 = env.get("PYTHONUTF8")
    loc = env.get("LC_ALL") or env.get("LC_CTYPE") or env.get("LANG") or "C"
    c_locale = loc in ("C", "POSIX")
    if enc is None:
        if utf8 == "1" or (c_locale and utf8 != "0"):
            enc = "utf-8"
            errors_io = "surrogateescape"
        else:
            try:
                locale.setlocale(locale.LC_CTYPE, "")
            except locale.Error:
                pass
            enc = locale.nl_langinfo(locale.CODESET) or "utf-8"
            errors_io = "strict"
    else:
        errors_io = "strict"
    return enc, errors or errors_io, errors or "backslashreplace"


def rebuild_stdio(env):
    enc, errors_io, errors_err = stdio_settings(env)
    sys.stdin = io.TextIOWrapper(io.BufferedReader(io.FileIO(0, "r", closefd=False)), encoding=enc, errors=errors_io)
    sys.stdout = io.TextIOWrapper(io.BufferedWriter(io.FileIO(1, "w", closefd=False)), encoding=enc, errors=errors_io,
                                  line_buffering=os.isatty(1))
    sys.stderr = io.TextIOWrapper(io.BufferedWriter(io.FileIO(2, "w", closefd=False)), encoding=enc, errors=errors_err,
                                  line_buffering=True, write_through=True)


def child(argv, env, cwd, fds):
    rc = 255
    try:
        for target, fd in enumerate(fds):
            os.dup2(fd, target)
        for fd in fds:
            if fd > 2:
                os.close(fd)
        os.chdir(cwd)
        # A fresh interpreter starts with the dispositions its parent gave it, and the client has
        # forwarded what it got: the server's own (a background job's ignored INT) are not the call's.
        signal.signal(signal.SIGINT, signal.default_int_handler)
        for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGQUIT):
            signal.signal(sig, signal.SIG_DFL)
        environ = dict(e.split("=", 1) for e in env)
        os.environ.clear()
        os.environ.update(environ)
        if os.environ.get("LC_CTYPE", "") == "UTF-8":
            os.environ["LC_CTYPE"] = "en_US.UTF-8"
        awscli_import_environ()
        rebuild_stdio(environ)
        sys.argv = argv
        try:
            rc = awscli.clidriver.main()
        except SystemExit as e:
            rc = e.code if isinstance(e.code, int) else (0 if e.code is None else 1)
        sys.stdout.flush()
        sys.stderr.flush()
    except BaseException:
        try:
            os.write(2, b"aws_resident: the child failed before the CLI ran\n")
        except OSError:
            pass
        rc = 255
    os._exit(rc & 0xFF)


def serve_one(conn):
    head, fds, _, _ = socket.recv_fds(conn, 65536, 3)
    if len(fds) != 3:
        for fd in fds:
            os.close(fd)
        return
    try:
        head = read_exact(conn, bytearray(head), 4)
        if head is None:
            return
        n = struct.unpack_from("<I", head, 0)[0]
        body = read_exact(conn, bytearray(head[4:]), n)
        if body is None:
            return
        argv, env, cwd = parse(bytes(body[:n]))
        with warnings.catch_warnings():   # 3.12+ warns that a process with threads forks
            warnings.simplefilter("ignore", DeprecationWarning)
            pid = os.fork()
        if pid == 0:
            conn.close()
            listener.close()
            child(argv, env, cwd, fds)
        conn.sendall(struct.pack("<I", pid))
        _, status = os.waitpid(pid, 0)
        if os.WIFSIGNALED(status):
            conn.sendall(struct.pack("<II", 1, os.WTERMSIG(status)))
        else:
            conn.sendall(struct.pack("<II", 0, os.WEXITSTATUS(status)))
    finally:
        for fd in fds:
            os.close(fd)


def main():
    global listener
    ap = argparse.ArgumentParser()
    ap.add_argument("--socket", required=True)
    path = ap.parse_args().socket
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(64)
    ino = os.stat(path).st_ino
    while True:
        # It ends when its socket is removed (the script's directory went), checked once a second.
        ready, _, _ = select.select([listener], [], [], 1.0)
        if not ready:
            try:
                if os.stat(path).st_ino != ino:
                    return
            except OSError:
                return
            continue
        conn, _ = listener.accept()
        try:
            serve_one(conn)
        except (OSError, ValueError, struct.error):
            pass
        finally:
            conn.close()


if __name__ == "__main__":
    main()
