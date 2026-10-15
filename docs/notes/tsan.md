# ThreadSanitizer

TSan sees syscalls only through libc, but requests run in the kernel and
liburing makes its own syscalls. A loop per thread is fine while threads only
copy data through the kernel. Three things break when threads don't synchronize
in a way TSan sees, e.g. when they only talk through rings.

## A new ring gets the memory of a closed one

As of 2.15, liburing maps and unmaps rings with inline syscalls, even with
`--use-libc`, so TSan keeps the accesses to a closed ring. The next ring often
gets the same addresses, and if another thread creates it, its accesses to the
SQEs and the CQ are reported as races with the old thread.

The loop releases every ring page on exit and acquires them on init with
`__tsan_release()` and `__tsan_acquire()`. With `IORING_SETUP_NO_MMAP` liburing
can put the rings right after the SQEs and leave `ring_sz` zero, so the loop
covers them up to the end of the CQEs. Tests: `thread_loops_in_turn`,
`thread_no_mmap_in_turn`.

The proper fix is in liburing: use libc's `mmap()` and `munmap()` when built
with libc. A liburing patched to use its libc wrappers passes
`thread_loops_in_turn` without the workaround. That only helps builds with
`--use-libc`, builds without libc would need the same annotations.

## Another thread gets the number of an fd closed in the ring

TSan doesn't see `IORING_OP_CLOSE`, so it keeps the state of the fd and reports
a race when another thread gets the same number. The loop calls
`__sanitizer_syscall_pre_impl_close()` when it queues a close, and closes the
application queues itself need the same. This isn't a liburing bug. Test:
`fd_close_reuse`.

## A thread passes memory to another through the ring

A pointer can go through a pipe written with one ring and read with another, or
with `IORING_OP_MSG_RING`, which posts it as `user_data` of a CQE on the other
ring. TSan doesn't see the kernel order the threads, so the accesses to that
memory are reported as races. Copy the data, or call `__tsan_release()` on it
before queuing the request and `__tsan_acquire()` before the first access in
the other thread. With `MSG_RING` that is before anything reads through
`user_data`, including the code that dispatches the CQE.
