/* SPDX-License-Identifier: MIT */
#ifndef POOR_LOOP_URING_ARRAY_H
#define POOR_LOOP_URING_ARRAY_H

#include <liburing.h>
#include <poor_array.h>

#define io_uring_prep_read_array(sqe, fd, buf, offset) \
	io_uring_prep_read(sqe, fd, auto_arr(buf), ARRAY_SIZE_BYTES(buf), offset)
#define io_uring_prep_write_array(sqe, fd, buf, offset) \
	io_uring_prep_write(sqe, fd, auto_arr(buf), ARRAY_SIZE_BYTES(buf), offset)
#define io_uring_prep_read_fixed_array(sqe, fd, buf, offset, buf_index) \
	io_uring_prep_read_fixed(sqe, fd, auto_arr(buf), ARRAY_SIZE_BYTES(buf), offset, buf_index)
#define io_uring_prep_write_fixed_array(sqe, fd, buf, offset, buf_index) \
	io_uring_prep_write_fixed(sqe, fd, auto_arr(buf), ARRAY_SIZE_BYTES(buf), offset, buf_index)

#define io_uring_prep_readv_array(sqe, fd, iovecs, offset) \
	io_uring_prep_readv(sqe, fd, auto_arr(iovecs), ARRAY_SIZE(iovecs), offset)
#define io_uring_prep_readv2_array(sqe, fd, iovecs, offset, flags) \
	io_uring_prep_readv2(sqe, fd, auto_arr(iovecs), ARRAY_SIZE(iovecs), offset, flags)
#define io_uring_prep_readv_fixed_array(sqe, fd, iovecs, offset, flags, buf_index) \
	io_uring_prep_readv_fixed(sqe, fd, auto_arr(iovecs), ARRAY_SIZE(iovecs), offset, flags, buf_index)
#define io_uring_prep_writev_array(sqe, fd, iovecs, offset) \
	io_uring_prep_writev(sqe, fd, auto_arr(iovecs), ARRAY_SIZE(iovecs), offset)
#define io_uring_prep_writev2_array(sqe, fd, iovecs, offset, flags) \
	io_uring_prep_writev2(sqe, fd, auto_arr(iovecs), ARRAY_SIZE(iovecs), offset, flags)
#define io_uring_prep_writev_fixed_array(sqe, fd, iovecs, offset, flags, buf_index) \
	io_uring_prep_writev_fixed(sqe, fd, auto_arr(iovecs), ARRAY_SIZE(iovecs), offset, flags, buf_index)

#define io_uring_prep_send_array(sqe, sockfd, buf, flags) \
	io_uring_prep_send(sqe, sockfd, auto_arr(buf), ARRAY_SIZE_BYTES(buf), flags)
#define io_uring_prep_sendto_array(sqe, sockfd, buf, flags, addr, addrlen) \
	io_uring_prep_sendto(sqe, sockfd, auto_arr(buf), ARRAY_SIZE_BYTES(buf), flags, addr, addrlen)
#define io_uring_prep_send_zc_array(sqe, sockfd, buf, flags, zc_flags) \
	io_uring_prep_send_zc(sqe, sockfd, auto_arr(buf), ARRAY_SIZE_BYTES(buf), flags, zc_flags)
#define io_uring_prep_send_zc_fixed_array(sqe, sockfd, buf, flags, zc_flags, buf_index) \
	io_uring_prep_send_zc_fixed(sqe, sockfd, auto_arr(buf), ARRAY_SIZE_BYTES(buf), flags, zc_flags, buf_index)
#define io_uring_prep_recv_array(sqe, sockfd, buf, flags) \
	io_uring_prep_recv(sqe, sockfd, auto_arr(buf), ARRAY_SIZE_BYTES(buf), flags)
#define io_uring_prep_recv_multishot_array(sqe, sockfd, buf, flags) \
	io_uring_prep_recv_multishot(sqe, sockfd, auto_arr(buf), ARRAY_SIZE_BYTES(buf), flags)

#define io_uring_prep_madvise_array(sqe, addr, advice) \
	io_uring_prep_madvise(sqe, auto_arr(addr), ARRAY_SIZE_BYTES(addr), advice)
#define io_uring_prep_madvise64_array(sqe, addr, advice) \
	io_uring_prep_madvise64(sqe, auto_arr(addr), ARRAY_SIZE_BYTES(addr), advice)
#define io_uring_prep_epoll_wait_array(sqe, fd, events, flags) \
	io_uring_prep_epoll_wait(sqe, fd, auto_arr(events), ARRAY_SIZE(events), flags)
#define io_uring_prep_files_update_array(sqe, fds, offset) \
	io_uring_prep_files_update(sqe, auto_arr(fds), ARRAY_SIZE(fds), offset)
#define io_uring_prep_futex_waitv_array(sqe, futex, flags) \
	io_uring_prep_futex_waitv(sqe, auto_arr(futex), ARRAY_SIZE(futex), flags)
#define io_uring_prep_provide_buffers_array(sqe, addr, bgid, bid) \
	io_uring_prep_provide_buffers(sqe, auto_arr(addr), ARRAY_ELEMENT_SIZE(addr), ARRAY_SIZE(addr), bgid, bid)
#define io_uring_prep_cmd_sock_array(sqe, cmd_op, fd, level, optname, optval) \
	io_uring_prep_cmd_sock(sqe, cmd_op, fd, level, optname, auto_arr(optval), ARRAY_SIZE_BYTES(optval))

#define io_uring_prep_getxattr_array(sqe, name, value, path) \
	io_uring_prep_getxattr(sqe, name, auto_arr(value), path, ARRAY_SIZE_BYTES(value))
#define io_uring_prep_setxattr_array(sqe, name, value, path, flags) \
	io_uring_prep_setxattr(sqe, name, auto_arr(value), path, flags, ARRAY_SIZE_BYTES(value))
#define io_uring_prep_fgetxattr_array(sqe, fd, name, value) \
	io_uring_prep_fgetxattr(sqe, fd, name, auto_arr(value), ARRAY_SIZE_BYTES(value))
#define io_uring_prep_fsetxattr_array(sqe, fd, name, value, flags) \
	io_uring_prep_fsetxattr(sqe, fd, name, auto_arr(value), flags, ARRAY_SIZE_BYTES(value))

#define h_uring_tags_nr(arrm, tags) \
	(static_assert_expr(constexpr_or(ARRAY_SIZE(arrm) == ARRAY_SIZE(tags), 1), "tags don't match the array size"), \
	 ARRAY_SIZE(arrm))

#define io_uring_register_buffers_array(ring, iovecs) \
	io_uring_register_buffers(ring, auto_arr(iovecs), ARRAY_SIZE(iovecs))
#define io_uring_register_buffers_tags_array(ring, iovecs, tags) \
	io_uring_register_buffers_tags(ring, auto_arr(iovecs), auto_arr(tags), h_uring_tags_nr(iovecs, tags))
#define io_uring_register_buffers_update_tag_array(ring, off, iovecs, tags) \
	io_uring_register_buffers_update_tag(ring, off, auto_arr(iovecs), auto_arr(tags), h_uring_tags_nr(iovecs, tags))
#define io_uring_register_files_array(ring, files) \
	io_uring_register_files(ring, auto_arr(files), ARRAY_SIZE(files))
#define io_uring_register_files_tags_array(ring, files, tags) \
	io_uring_register_files_tags(ring, auto_arr(files), auto_arr(tags), h_uring_tags_nr(files, tags))
#define io_uring_register_files_update_array(ring, off, files) \
	io_uring_register_files_update(ring, off, auto_arr(files), ARRAY_SIZE(files))
#define io_uring_register_files_update_tag_array(ring, off, files, tags) \
	io_uring_register_files_update_tag(ring, off, auto_arr(files), auto_arr(tags), h_uring_tags_nr(files, tags))
#define io_uring_register_restrictions_array(ring, res) \
	io_uring_register_restrictions(ring, auto_arr(res), ARRAY_SIZE(res))
#define io_uring_register_wait_reg_array(ring, reg) \
	io_uring_register_wait_reg(ring, auto_arr(reg), ARRAY_SIZE(reg))

#define io_uring_buf_ring_add_array(br, addr, bid, mask, buf_offset) \
	io_uring_buf_ring_add(br, auto_arr(addr), ARRAY_SIZE_BYTES(addr), bid, mask, buf_offset)
#define io_uring_peek_batch_cqe_array(ring, cqes) \
	io_uring_peek_batch_cqe(ring, auto_arr(cqes), ARRAY_SIZE(cqes))

#endif
