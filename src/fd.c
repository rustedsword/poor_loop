/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "internal.h"
#include "poor_loop_fd.h"

void poor_loop_close_fd_or_submit_impl(struct poor_loop *loop, int fd, unsigned sqe_flags)
{
	struct io_uring_sqe *sqe = poor_loop_get_untracked_sqe_or_submit(loop);

	if (uring_unlikely(!sqe)) {
		poor_loop_log(loop, "No SQE to close fd ", fd, ": ", strerror(errno), ": closing synchronously");
		close(fd);
		return;
	}
	io_uring_prep_close(sqe, fd);
	io_uring_sqe_set_flags(sqe, sqe_flags);
}
