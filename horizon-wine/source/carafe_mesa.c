#include <stdbool.h>
#include <stdio.h>
#include <string.h>

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

/* Mesa's debug messages (os_log_message, behind _debug_printf), which on the
 * Switch go to a stderr nothing reads. mesa-switch's Switch winsys says there
 * why a GPU submission failed when it latches the failure ("durable GPU
 * submission error latched"), after which its command buffer takes no more
 * and nvc0 writes past its end: Vampire: The Masquerade - Redemption faulted
 * on a thread's stack beside it. Into the runtime's log as [MESA] lines, the
 * first 256 and from then on every time their number doubles. Weak, in case a
 * Mesa has no os_log_message of its own to wrap. */
void __real_os_log_message(const char* message) __attribute__((weak));

void __wrap_os_log_message(const char* message) {
    static unsigned int count;
    unsigned int n;
    size_t len;

    /* Mesa's own puts it on stderr, which the runtime takes into the log too
     * ([NATIVE-STDERR]): once is enough. */
    if (wine_nx_runtime_trace == NULL) {
        if (__real_os_log_message != NULL) __real_os_log_message(message);
        return;
    }
    if (message == NULL) return;
    n = __atomic_add_fetch(&count, 1, __ATOMIC_RELAXED);
    if (n > 256 && (n & (n - 1))) return;
    len = strlen(message);
    while (len && (message[len - 1] == '\n' || message[len - 1] == '\r')) len--;
    if (len) {
        char line[640];

        snprintf(line, sizeof(line), "[MESA] %.*s%s", (int)(len > 600 ? 600 : len), message,
            n > 256 ? " (from here every time their number doubles)" : "");
        wine_nx_runtime_trace(line);
    }
}
