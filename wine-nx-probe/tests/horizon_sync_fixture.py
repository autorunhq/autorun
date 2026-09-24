"""Keep legacy wait-loop fixtures separate from the queued-wait stress tests."""
import re


def legacy_sync_support(source):
    types = []
    for name in ('horizon_sync_link', 'horizon_sync_waiter', 'horizon_select_signal_and_wait_op'):
        types.append(re.search(r'^struct ' + name + r'\n\{.*?\n\};', source, re.M | re.S).group())
    return '\n'.join(types) + r'''
static void horizon_server_sync_lock(const struct horizon_server_connection *c) {
    assert(!c->direct_reply); pthread_mutex_lock(&horizon_server_objects_mutex);
}
static int horizon_sync_queue_locked(struct horizon_sync_waiter *w) { (void)w; assert(0); return 0; }
static void horizon_sync_unqueue_locked(struct horizon_sync_waiter *w) { assert(!w->queued); }
static void horizon_sync_sleep_locked(struct horizon_sync_waiter *w, long long t) {
    (void)w; (void)t; assert(0);
}
'''
