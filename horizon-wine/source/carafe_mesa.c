#include <stdbool.h>
#include <stdio.h>

bool __real_util_queue_init(void* queue, const char* name, unsigned max_jobs, unsigned num_threads, unsigned flags,
    void* context);
void wine_nx_runtime_trace(const char* msg) __attribute__((weak));

bool __wrap_util_queue_init(void* queue, const char* name, unsigned max_jobs, unsigned num_threads, unsigned flags,
    void* context) {
    bool made = __real_util_queue_init(queue, name, max_jobs, num_threads, flags, context);
    if (wine_nx_runtime_trace != NULL) {
        char line[160];
        snprintf(line, sizeof(line), "[NATIVE] Mesa queue %s: %u threads, %u jobs, flags %#x%s",
            name != NULL ? name : "?", num_threads, max_jobs, flags, made ? "" : ", not made");
        wine_nx_runtime_trace(line);
    }
    return made;
}
