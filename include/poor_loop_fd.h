/* SPDX-License-Identifier: MIT */
#ifndef POOR_LOOP_FD_H
#define POOR_LOOP_FD_H

#include "poor_loop_group.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Poor Loop Fd.
 *
 * A group that holds a file descriptor and closes it when the counter reaches zero,
 * so the file is closed as soon as it is no longer used.
 *
 * It is a regular group with an 'fd' member: attach ops and timers to it with
 * poor_loop_group_attach() and use poor_loop_group_xxx functions with it.
 * Define POOR_LOOP_FD_DONE() instead of POOR_LOOP_GROUP_DONE().
 *
 * Before 'done' is called, the fd is closed with an untracked IOSQE_ASYNC SQE and set
 * to -1. If the ring fails, the fd is closed synchronously with close().
 *
 * Since the close is asynchronous you should not rely on the file being really closed,
 * e.g. on its locks being released or on the peer seeing EOF. Use poor_loop_drain()
 * before poor_loop_exit() to wait for the close.
 *
 * Example:
 *
 *   poor_loop_fd_declare(job_file);
 *
 *   struct job {
 *           job_file file;
 *           struct poor_loop_op read;
 *           char buf[64];
 *   };
 *
 *   poor_loop_group_attach(job_file, struct job, file, read, read_done);
 *
 *   POOR_LOOP_GROUP_OP(read_done, loop, file, op, cqe)
 *   {
 *           printf("read: %d\n", cqe->res);
 *   }
 *
 *   POOR_LOOP_FD_DONE(job_file, loop, file)
 *   {
 *           free(container_of(file, struct job, file));
 *   }
 *
 *   void job_start(struct poor_loop *loop, struct job *job, int fd)
 *   {
 *           poor_loop_fd_init(&job->file, fd);
 *           poor_loop_op_init(&job->read, read_done);
 *           auto sqe = poor_loop_group_get_sqe(loop, &job->file, &job->read);
 *           io_uring_prep_read_array(sqe, fd, job->buf, 0);
 *   }
 */

/* Declare an fd group type. Like poor_loop_group_declare(), it can go in a header. */
#define poor_loop_fd_declare(name) h_fd_declare(name)

#define POOR_LOOP_FD_INIT(descriptor) { .group = {}, .fd = (descriptor) }

#define poor_loop_fd_init(g, descriptor) ((void)(*(g) = (typeof_unqual(*(g)))POOR_LOOP_FD_INIT(descriptor)))

/*
 * Define the done function for the fd group.
 *
 * When it is called the fd is closed and 'fd' is -1, but the file below may still be open:
 * it is safe to free group's memory, but do not rely on the file being closed.
 */
#define POOR_LOOP_FD_DONE(name, loop_arg, file_arg) h_fd_define_done(name, loop_arg, file_arg)

/*
 * Close 'fd' with an untracked SQE, so the result is ignored. 'sqe_flags' are IOSQE_* flags, e.g. IOSQE_ASYNC.
 * If the SQ is full, submit and wait; if that fails, close synchronously.
 */
void poor_loop_close_fd_or_submit(struct poor_loop *loop, int fd, unsigned sqe_flags);

/* Implementation details, subject to change. */

#define h_fd_declare(name) \
	typedef struct name { \
		struct poor_loop_group group; \
		int fd; \
	} name

#define h_fd_define_done(name, loop_arg, file_arg) \
	static void h_fd_done_##name(struct poor_loop *, struct name *); \
	h_group_define_done(name, h_loop, h_file) \
	{ \
		assert(h_file->fd >= 0); \
		poor_loop_close_fd_or_submit(h_loop, h_file->fd, IOSQE_ASYNC); \
		h_file->fd = -1; \
		h_fd_done_##name(h_loop, h_file); \
	} \
	static void h_fd_done_##name([[maybe_unused]] struct poor_loop *loop_arg, [[maybe_unused]] struct name *file_arg)

#ifdef __cplusplus
}
#endif

#endif
