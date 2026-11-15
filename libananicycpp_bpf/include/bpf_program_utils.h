#ifndef BPF_PROGRAM_UTILS_H
#define BPF_PROGRAM_UTILS_H

#include <stdint.h>
#include <bpf/libbpf.h>

typedef struct ananicy_cpp_bpf ananicy_cpp_bpf_t;

#ifdef __cplusplus
extern "C" {
#endif

ananicy_cpp_bpf_t* initialize_bpf_program(bool verbose);
void destroy_bpf_program(ananicy_cpp_bpf_t* obj, struct perf_buffer* pb);

struct perf_buffer* bpf_program_init_events(ananicy_cpp_bpf_t* obj);
/* Returns and resets the number of events the BPF side failed to output. */
uint64_t bpf_program_take_dropped_events(ananicy_cpp_bpf_t* obj);

#ifdef __cplusplus
}
#endif

#endif // BPF_PROGRAM_UTILS_H
