/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/mman.h>

#include "harness.h"

/* liburing 2.3 has no version check and none of the newer helpers */
#ifndef IO_URING_CHECK_VERSION
#define IO_URING_CHECK_VERSION(major, minor) 1
#endif

#if !IO_URING_CHECK_VERSION(2, 6)
#include <linux/futex.h>
#endif

static int test_prep_array(void)
{
	struct io_uring_sqe sqe = {};
	struct iovec iov[3] = {};
	int buf[4] = {}, bufs[3][4];
	char value[8] = {};

	io_uring_prep_read_array(&sqe, 0, buf, 0);
	CHECK_EQ(sqe.len, 16);
	io_uring_prep_write_array(&sqe, 0, buf, 0);
	CHECK_EQ(sqe.len, 16);
	io_uring_prep_read_fixed_array(&sqe, 0, buf, 0, 0);
	CHECK_EQ(sqe.len, 16);
	io_uring_prep_write_fixed_array(&sqe, 0, buf, 0, 0);
	CHECK_EQ(sqe.len, 16);

	io_uring_prep_readv_array(&sqe, 0, iov, 0);
	CHECK_EQ(sqe.len, 3);
	io_uring_prep_readv2_array(&sqe, 0, iov, 0, 0);
	CHECK_EQ(sqe.len, 3);
	io_uring_prep_writev_array(&sqe, 0, iov, 0);
	CHECK_EQ(sqe.len, 3);
	io_uring_prep_writev2_array(&sqe, 0, iov, 0, 0);
	CHECK_EQ(sqe.len, 3);

	io_uring_prep_send_array(&sqe, 0, buf, 0);
	CHECK_EQ(sqe.len, 16);
	io_uring_prep_send_zc_array(&sqe, 0, buf, 0, 0);
	CHECK_EQ(sqe.len, 16);
	io_uring_prep_send_zc_fixed_array(&sqe, 0, buf, 0, 0, 0);
	CHECK_EQ(sqe.len, 16);
	io_uring_prep_recv_array(&sqe, 0, buf, 0);
	CHECK_EQ(sqe.len, 16);
	io_uring_prep_recv_multishot_array(&sqe, 0, buf, 0);
	CHECK_EQ(sqe.len, 16);

	io_uring_prep_madvise_array(&sqe, buf, MADV_NORMAL);
	CHECK_EQ(sqe.len, 16);
	io_uring_prep_files_update_array(&sqe, buf, 0);
	CHECK_EQ(sqe.len, 4);
	io_uring_prep_provide_buffers_array(&sqe, bufs, 0, 0);
	CHECK_EQ(sqe.len, 16);
	CHECK_EQ(sqe.fd, 3);

	io_uring_prep_getxattr_array(&sqe, "user.test", value, "/");
	CHECK_EQ(sqe.len, 8);
	io_uring_prep_setxattr_array(&sqe, "user.test", value, "/", 0);
	CHECK_EQ(sqe.len, 8);
	io_uring_prep_fgetxattr_array(&sqe, 0, "user.test", value);
	CHECK_EQ(sqe.len, 8);
	io_uring_prep_fsetxattr_array(&sqe, 0, "user.test", value, 0);
	CHECK_EQ(sqe.len, 8);

#if !IO_URING_CHECK_VERSION(2, 4)
	struct sockaddr_in addr = {};

	io_uring_prep_sendto_array(&sqe, 0, buf, 0, (struct sockaddr *)&addr, sizeof(addr));
	CHECK_EQ(sqe.len, 16);
#endif
#if !IO_URING_CHECK_VERSION(2, 5)
	io_uring_prep_cmd_sock_array(&sqe, SOCKET_URING_OP_SETSOCKOPT, 0, SOL_SOCKET, SO_REUSEADDR, buf);
	CHECK_EQ(sqe.optlen, 16);
#endif
#if !IO_URING_CHECK_VERSION(2, 6)
	struct futex_waitv futex[2] = {};

	io_uring_prep_futex_waitv_array(&sqe, futex, 0);
	CHECK_EQ(sqe.len, 2);
#endif
#if !IO_URING_CHECK_VERSION(2, 7)
	io_uring_prep_madvise64_array(&sqe, buf, MADV_NORMAL);
	CHECK_EQ(sqe.off, 16);
#endif
#if !IO_URING_CHECK_VERSION(2, 10)
	struct epoll_event events[5] = {};

	io_uring_prep_readv_fixed_array(&sqe, 0, iov, 0, 0, 0);
	CHECK_EQ(sqe.len, 3);
	io_uring_prep_writev_fixed_array(&sqe, 0, iov, 0, 0, 0);
	CHECK_EQ(sqe.len, 3);
	io_uring_prep_epoll_wait_array(&sqe, 0, events, 0);
	CHECK_EQ(sqe.len, 5);
#endif
	return 0;
}

/* Only compiled: these need registered resources to run */
[[maybe_unused]] static void register_array(struct io_uring *ring, struct io_uring_buf_ring *br)
{
	struct io_uring_restriction res[1] = {};
	struct io_uring_cqe *cqes[4];
	struct iovec iov[2] = {};
	int fds[2] = { -1, -1 };
	__u64 tags[2] = {};
	char buf[8];

	io_uring_register_buffers_array(ring, iov);
	io_uring_register_buffers_tags_array(ring, iov, tags);
	io_uring_register_buffers_update_tag_array(ring, 0, iov, tags);
	io_uring_register_files_array(ring, fds);
	io_uring_register_files_tags_array(ring, fds, tags);
	io_uring_register_files_update_array(ring, 0, fds);
	io_uring_register_files_update_tag_array(ring, 0, fds, tags);
	io_uring_register_restrictions_array(ring, res);
	io_uring_buf_ring_add_array(br, buf, 0, io_uring_buf_ring_mask(4), 0);
	io_uring_peek_batch_cqe_array(ring, cqes);
#if !IO_URING_CHECK_VERSION(2, 9)
	struct io_uring_reg_wait reg[2] = {};

	io_uring_register_wait_reg_array(ring, reg);
#endif
}

static const struct test tests[] = {
	TEST(prep_array),
};

int main(int argc, char **argv)
{
	return run_tests(array_ptr(argv, argc), tests);
}
