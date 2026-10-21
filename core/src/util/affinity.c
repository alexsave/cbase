// affinity.c - see affinity.h.
#include "affinity.h"
#include "err.h"

#include <stdio.h>
#include <string.h>

#if defined(__linux__)
#include <errno.h>
#include <pthread.h>
#include <sched.h>

static _Thread_local bool t_saved;
static _Thread_local cpu_set_t t_mask;

bool cb_affinity_supported(void) { return true; }

int cb_affinity_pin(int cpu, char *err, size_t cap) {
    if (!t_saved) {
        if (pthread_getaffinity_np(pthread_self(), sizeof t_mask, &t_mask) != 0) {
            snprintf(err, cap, "the thread's CPUs cannot be read");
            return CB_E_INVALID;
        }
        t_saved = true;
    }
    cpu_set_t want;
    if (cpu < 0) {
        want = t_mask;
    } else {
        if (cpu >= CPU_SETSIZE || !CPU_ISSET(cpu, &t_mask)) {
            snprintf(err, cap, "CPU %d is not one this process may use", cpu);
            return CB_E_INVALID;
        }
        CPU_ZERO(&want);
        CPU_SET(cpu, &want);
    }
    const int rc = pthread_setaffinity_np(pthread_self(), sizeof want, &want);
    if (rc != 0) {
        snprintf(err, cap, "CPU %d: %s", cpu, strerror(rc));
        return CB_E_INVALID;
    }
    return CB_OK;
}

int cb_affinity_count(void) {
    cpu_set_t m;
    return pthread_getaffinity_np(pthread_self(), sizeof m, &m) == 0 ? CPU_COUNT(&m) : -1;
}

int cb_affinity_pick(int *loop_cpu, int *bg_cpu, char *err, size_t cap) {
    *loop_cpu = *bg_cpu = -1;
    cpu_set_t m;
    if (pthread_getaffinity_np(pthread_self(), sizeof m, &m) != 0) {
        snprintf(err, cap, "the thread's CPUs cannot be read");
        return CB_E_INVALID;
    }
    int got[2], n = 0;
    for (int i = 0; i < CPU_SETSIZE && n < 2; i++)
        if (CPU_ISSET(i, &m)) got[n++] = i;
    if (n < 2) {
        snprintf(err, cap, "this process may use %d CPU", n);
        return CB_E_INVALID;
    }
    *loop_cpu = got[0];
    *bg_cpu = got[1];
    return CB_OK;
}

#else

bool cb_affinity_supported(void) { return false; }

int cb_affinity_pin(int cpu, char *err, size_t cap) {
    snprintf(err, cap, "pinning a thread to CPU %d needs Linux", cpu);
    return CB_E_NOTIMPL;
}

int cb_affinity_count(void) { return -1; }

int cb_affinity_pick(int *loop_cpu, int *bg_cpu, char *err, size_t cap) {
    *loop_cpu = *bg_cpu = -1;
    snprintf(err, cap, "picking CPUs needs Linux");
    return CB_E_NOTIMPL;
}

#endif
