// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2019 Facebook
#include <vmlinux.h>
#include <bpf/bpf_helpers.h>       /* most used helpers: SEC, __always_inline, etc */
#include <bpf/bpf_core_read.h>     /* for BPF CO-RE helpers */
#include <bpf/bpf_tracing.h>       /* for getting kprobe arguments */
#include "core_fixes.bpf.h"

/*
 * One event per process that may need a rule: it started (fork), loaded a new
 * program (exec), or renamed itself. User space only needs the pid; it reads
 * the name and command line from /proc itself.
 */
struct event {
    pid_t pid;
};

/*
 * Events that could not be written because the perf ring was full. The
 * kernel only reports such losses lazily (with the *next* event written to
 * that ring), which may never come on a quiet system; user space reads and
 * resets this counter directly and rescans /proc when it is not zero.
 */
__u64 dropped_events = 0;

struct {
    __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
    __uint(key_size, sizeof(u32));
    __uint(value_size, sizeof(u32));
} events SEC(".maps");

static __always_inline void output_event(void *ctx, pid_t pid) {
    struct event e = {.pid = pid};
    if (bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU, &e, sizeof(e)))
        __sync_fetch_and_add(&dropped_events, 1);
}

/*
 * Every exec is reported, also several in a row by the same pid (e.g.
 * "nice -n 5 game", "env FOO=1 game", launcher scripts): each one loads a
 * different program, and only the last one has the name the rules match.
 */
SEC("tp/sched/sched_process_exec")
int handle_exec(struct trace_event_raw_sched_process_exec* ctx)
{
    output_event(ctx, (pid_t)bpf_get_current_pid_tgid());
    return 0;
}

/*
 * The fork tracepoint runs in the context of the *parent*, so report the
 * newly created child. Otherwise a process that forks without exec'ing
 * afterwards (daemons such as wineserver) is never seen and keeps the
 * priority inherited from its parent.
 *
 * Only report new processes (thread-group leaders): the per-thread settings
 * are applied to every thread in /proc/<pid>/task, and threads created later
 * inherit them, so per-thread events would only add load.
 */
SEC("raw_tp/sched_process_fork")
int BPF_PROG(handle_fork, struct task_struct *parent, struct task_struct *child)
{
    pid_t pid = BPF_CORE_READ(child, pid);
    pid_t tgid = BPF_CORE_READ(child, tgid);
    if (pid != tgid) /* new thread, not a new process */
        return 0;

    output_event(ctx, pid);
    return 0;
}

/*
 * A process renamed itself with prctl(PR_SET_NAME). Wine/Proton does this
 * right after rewriting its argv in place, so the game only becomes
 * "game.exe" in /proc/<pid>/cmdline *after* its exec event was handled.
 * The netlink backend gets the same information from PROC_EVENT_COMM.
 *
 * Skip thread names (pthread_setname_np) and the rename done by exec itself,
 * which handle_exec already reports.
 */
SEC("raw_tp/task_rename")
int BPF_PROG(handle_rename, struct task_struct *task, const char *comm)
{
    pid_t pid = BPF_CORE_READ(task, pid);
    pid_t tgid = BPF_CORE_READ(task, tgid);
    if (pid != tgid)
        return 0;
    if (BPF_CORE_READ_BITFIELD_PROBED(task, in_execve))
        return 0;

    output_event(ctx, pid);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
