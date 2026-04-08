# Deferred timeout-stack-pointer risks

Two request/response paths currently enqueue pointers to stack-local objects and then
wait with a fixed timeout. If the worker thread services the queued request after the
caller has already timed out and returned, the worker will dereference dangling pointers.

## `nrf_firmware/src/prop_gfsk/link.c`

`prop_gfsk_link_set_config()` builds a request containing:

- `struct k_sem *done`
- `bool *result`

Both point to locals on the caller's stack. The request is placed in
`g_prop_gfsk_config_queue`, then the caller waits for up to
`PROP_GFSK_LINK_CONFIG_SET_TIMEOUT`.

If the wait times out first, the function returns while the queued request can still be
pending. When the link thread later handles that request in
`prop_gfsk_link_process_config_request()`, it will execute:

- `*request.result = ...`
- `k_sem_give(request.done)`

against freed stack storage.

## `nrf_firmware/src/app_control.c`

`app_control_set()` uses the same pattern:

- it queues pointers to a stack-local semaphore and result flag
- waits up to `APP_CONTROL_SET_TIMEOUT`
- returns on timeout without ensuring the queue item is gone

If the app-control thread processes that request later, it writes through those dead
stack pointers.

## Why this matters

The bug is timing-sensitive and may only show up when a worker thread is delayed, for
example during debugger halts, long blocking operations, or scheduler stalls. When it
does happen, the failure mode is memory corruption rather than a clean timeout.

## Likely fixes later

- Make the request object own its completion state for the full request lifetime.
- Or ensure timed-out requests are removed/cancelled before returning.
- Or move to a synchronous worker API that does not enqueue raw stack pointers.
