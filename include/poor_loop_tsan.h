/* SPDX-License-Identifier: MIT */
#ifndef POOR_LOOP_TSAN_H
#define POOR_LOOP_TSAN_H

/*
 * Implementation details, subject to change.
 *
 * TSan hooks live in headers, so they follow the flags of the application,
 * even if the library is built without TSan. Without TSan they compile to nothing.
 */

#if defined(__SANITIZE_THREAD__)
#define POOR_LOOP_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define POOR_LOOP_TSAN 1
#endif
#endif

#ifdef POOR_LOOP_TSAN

#include <liburing.h>
#include <sanitizer/tsan_interface.h>
#include <stdint.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* From linux_syscall_hooks.h, which GCC doesn't install */
void __sanitizer_syscall_pre_impl_close(long fd);

static inline void h_tsan_sync_pages(void *start, size_t size, void (*sync)(void *))
{
	uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);

	for (uintptr_t p = (uintptr_t)start & -page; p < (uintptr_t)start + size; p += page)
		sync((void *)p);
}

/*
 * liburing maps rings with raw syscalls, so TSan doesn't see a new ring take the memory
 * of a closed one and reports races with the thread that used it. The unmap happens
 * before the map: release every page on exit and acquire it on init.
 */
static inline void h_tsan_sync_ring(const struct io_uring *ring, void (*sync)(void *))
{
	h_tsan_sync_pages(ring->sq.ring_ptr, ring->sq.ring_sz, sync);
	h_tsan_sync_pages(ring->cq.ring_ptr, ring->cq.ring_sz, sync);
	h_tsan_sync_pages(ring->sq.sqes,
			  ring->sq.ring_entries * sizeof(struct io_uring_sqe) << !!(ring->flags & IORING_SETUP_SQE128), sync);
	/* With IORING_SETUP_NO_MMAP liburing can put the rings after the SQEs and leave ring_sz zero */
	if (!ring->sq.ring_sz) {
		char *end = (char *)(ring->cq.cqes + (ring->cq.ring_entries << !!(ring->flags & IORING_SETUP_CQE32)));

		if (ring->sq.array)
			end = (char *)(ring->sq.array + ring->sq.ring_entries);
		h_tsan_sync_pages(ring->sq.ring_ptr, end - (char *)ring->sq.ring_ptr, sync);
	}
}

#ifdef __cplusplus
}
#endif

/* TSan doesn't see closes in the ring and reports races when another thread gets the same fd */
#define poor_loop_tsan_close(fd) __sanitizer_syscall_pre_impl_close(fd)
#define poor_loop_tsan_ring_acquire(ring) h_tsan_sync_ring(ring, __tsan_acquire)
#define poor_loop_tsan_ring_release(ring) h_tsan_sync_ring(ring, __tsan_release)

#else

#define poor_loop_tsan_close(fd) ((void)0)
#define poor_loop_tsan_ring_acquire(ring) ((void)0)
#define poor_loop_tsan_ring_release(ring) ((void)0)

#endif

#endif
