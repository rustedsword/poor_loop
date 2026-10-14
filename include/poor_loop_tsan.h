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

#ifdef __cplusplus
extern "C" {
#endif

/* From linux_syscall_hooks.h, which GCC doesn't install */
void __sanitizer_syscall_pre_impl_close(long fd);

#ifdef __cplusplus
}
#endif

/* TSan doesn't see closes in the ring and reports races when another thread gets the same fd */
#define poor_loop_tsan_close(fd) __sanitizer_syscall_pre_impl_close(fd)

#else

#define poor_loop_tsan_close(fd) ((void)0)

#endif

#endif
