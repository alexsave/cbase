// aws_client.c - the `aws` the deploy tool's tests find first on PATH in CBASE_TEST_AWS=resident mode:
// it hands each call to the resident real AWS CLI (aws_resident.py) and ends as `aws` would have.
// Test code only. It sends argv, the whole environment and the working directory, with its stdin,
// stdout and stderr (SCM_RIGHTS) so the CLI's child writes where this process's caller reads; it
// reads the child's pid, forwards SIGTERM and SIGINT to it, and ends with the child's exit code, or
// by the signal that killed the child (so its parent sees what a killed `aws` would show).
//
// The socket is CBASE_TEST_AWS_SOCKET. The wire format is described in aws_resident.py.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

extern char **environ;

static volatile sig_atomic_t g_child;   // the child's pid once known, else 0

static void forward(int sig) {
    if (g_child > 0) {
        kill((pid_t)g_child, sig);
    } else {                            // nothing to forward to yet: die of it, as aws would
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

static void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// read_all: exactly n bytes, or 0 when the server ended first.
static int read_all(int fd, unsigned char *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        got += (size_t)r;
    }
    return 1;
}

static int died(void) {
    fputs("aws (cbase test client): the resident AWS CLI died during the call\n", stderr);
    return 255;
}

int main(int argc, char **argv) {
    const char *path = getenv("CBASE_TEST_AWS_SOCKET");
    if (!path || !*path) {
        fputs("aws (cbase test client): CBASE_TEST_AWS_SOCKET is not set\n", stderr);
        return 255;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = forward;
    sigemptyset(&sa.sa_mask);
    // A signal ignored at start (a background job's INT) stays ignored, as in a fresh aws.
    struct sigaction old;
    if (sigaction(SIGTERM, NULL, &old) == 0 && old.sa_handler != SIG_IGN) sigaction(SIGTERM, &sa, NULL);
    if (sigaction(SIGINT, NULL, &old) == 0 && old.sa_handler != SIG_IGN) sigaction(SIGINT, &sa, NULL);

    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) { perror("aws (cbase test client): getcwd"); return 255; }
    int envc = 0;
    size_t len = 4 + 4 + 4;             // length, argc, envc
    for (int i = 0; i < argc; i++) len += strlen(argv[i]) + 1;
    for (char **e = environ; e && *e; e++) { envc++; len += strlen(*e) + 1; }
    len += strlen(cwd) + 1;
    unsigned char *msg = malloc(len);
    if (!msg) return 255;
    put32(msg, (uint32_t)(len - 4));
    put32(msg + 4, (uint32_t)argc);
    put32(msg + 8, (uint32_t)envc);
    size_t off = 12;
    for (int i = 0; i < argc; i++) { size_t n = strlen(argv[i]) + 1; memcpy(msg + off, argv[i], n); off += n; }
    for (char **e = environ; e && *e; e++) { size_t n = strlen(*e) + 1; memcpy(msg + off, *e, n); off += n; }
    memcpy(msg + off, cwd, strlen(cwd) + 1);

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof a.sun_path) { fprintf(stderr, "aws (cbase test client): %s is too long for a socket path\n", path); return 255; }
    strcpy(a.sun_path, path);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "aws (cbase test client): the resident AWS CLI at %s is not running\n", path);
        return 255;
    }
    // SIGPIPE would end this process silently when the server is gone: the write error says it.
    signal(SIGPIPE, SIG_IGN);

    int fds[3] = {0, 1, 2};
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof fds)]; } ctl;
    memset(&ctl, 0, sizeof ctl);
    struct iovec iov = { msg, len };
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctl.b;
    mh.msg_controllen = CMSG_SPACE(sizeof fds);
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof fds);
    memcpy(CMSG_DATA(c), fds, sizeof fds);
    ssize_t w;
    do w = sendmsg(s, &mh, 0); while (w < 0 && errno == EINTR);
    if (w < 0) return died();
    size_t sent = (size_t)w;
    while (sent < len) {
        w = write(s, msg + sent, len - sent);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0) return died();
        sent += (size_t)w;
    }
    free(msg);

    unsigned char b[8];
    if (!read_all(s, b, 4)) return died();
    g_child = (sig_atomic_t)get32(b);
    if (!read_all(s, b, 8)) return died();
    uint32_t kind = get32(b), val = get32(b + 4);
    if (kind == 1) {
        sigset_t all;
        sigemptyset(&all);
        signal((int)val, SIG_DFL);
        sigprocmask(SIG_SETMASK, &all, NULL);
        raise((int)val);
        return 128 + (int)val;          // a signal that did not end us
    }
    return (int)val;
}
