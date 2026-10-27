# Level 2 support contract

Edit `mtrace_student.c`; `make` links it with the provided `support.c`.
The supplied library forwards allocation calls and prints `complete=no` until
you implement statistics. A successful build is not assignment completion.

`mt_event(op, old_addr, result, requested, overflow)` runs once per outer API call
under a shared mutex. Implement pointer ownership, call counts, live/peak bytes,
and request histogram here. `old_addr` is an opaque integer identity, never a
readable allocation. Requested-size accounting fits this API; a usable-size
implementation needs to adapt the support code to capture usable size before
free/realloc. Do not call malloc_usable_size on an old, freed pointer.

`mt_report(fd, reason)` runs under the same mutex outside the signal handler.
Use `mt_text` and `mt_number` or your own allocation-free formatting. A snapshot
must fit your declared schema; include an end marker so a measurement driver
can wait for completion. Report incomplete accounting after metadata exhaustion.

The support serializes allocation calls, including forwarding, for consistent
statistics. This is intentionally simple and has contention overhead. Document
it; it is not a production tracing implementation. Runtime calls outside the
observer are included. Compare checkpoint deltas rather than assuming zero
baseline. Reporter setup/teardown calls on its creator are excluded using TLS.

## Bootstrap and recursion

- dlsym resolves all four originals; atomic publication prevents half-ready state.
- Recursive dlsym allocation uses a 1 MiB, max_align_t-aligned static arena.
- Bootstrap free is a no-op; realloc copies into another bootstrap block.
- Capacity/size overflow returns NULL/ENOMEM. Bootstrap allocations are excluded.
- The arena and metadata stay alive until process exit. No sbrk is used.
- Internal/reporting calls bypass instrumentation. free preserves errno.

## Signals and shutdown

SIGUSR1's handler only preserves errno and writes one byte to a nonblocking pipe.
A reporter thread takes the mutex and prints a snapshot. Multiple queued requests
may be coalesced; a full pipe drops new notifications, while pending ones remain.
One request while the workload is paused is sufficient. Wait for the report's
end marker before continuing the workload.

Normal exit joins the reporter and emits an exit report. Do not use this library
for fork/exec trees, asynchronous cancellation, programs owning SIGUSR1, static
executables, secure-execution binaries, or alternate allocators. SIGKILL/_exit
and signal-default termination may omit the exit report. Request a snapshot first.
MTRACE_INTERVAL is not implemented; it remains optional work.

The four wrappers do not cover aligned allocation APIs or direct mmap. Untracked
pointers passed to free must not reduce live totals. This limits interpretation
of live bytes for general programs.
