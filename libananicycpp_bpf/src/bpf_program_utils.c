#include "ananicy_cpp.skel.h"
#include "bpf_program_utils.h"

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include <bpf/libbpf.h>

#include "process_utils.h"

static bool g_verbose = false;

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args) {
    if (!g_verbose)
        return 0;
    return vfprintf(stderr, format, args);
}

ananicy_cpp_bpf_t* initialize_bpf_program(bool verbose) {
    g_verbose = verbose;
    libbpf_set_print(libbpf_print_fn);

    ananicy_cpp_bpf_t* obj = ananicy_cpp_bpf__open();
    if (!obj) {
        fprintf(stderr, "failed to open BPF object\n");
        return NULL;
    }

    int32_t err = ananicy_cpp_bpf__load(obj);
    if (err) {
        fprintf(stderr, "failed to load BPF object: %d\n", err);
        ananicy_cpp_bpf__destroy(obj);
        return NULL;
    }

    err = ananicy_cpp_bpf__attach(obj);
    if (err) {
        fprintf(stderr, "failed to attach BPF programs\n");
        ananicy_cpp_bpf__destroy(obj);
        return NULL;
    }

    return obj;
}

void destroy_bpf_program(ananicy_cpp_bpf_t* obj, struct perf_buffer* pb) {
    perf_buffer__free(pb);
    ananicy_cpp_bpf__destroy(obj);
}

uint64_t bpf_program_take_dropped_events(ananicy_cpp_bpf_t* obj) {
    if (!obj || !obj->bss)
        return 0;
    return __atomic_exchange_n(&obj->bss->dropped_events, 0, __ATOMIC_RELAXED);
}

struct perf_buffer* bpf_program_init_events(ananicy_cpp_bpf_t* obj) {
    return perf_buffer__new(bpf_map__fd(obj->maps.events), 64,
                handle_event, handle_lost_events, NULL, NULL);
}
