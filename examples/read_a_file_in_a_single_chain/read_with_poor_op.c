/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poor_loop.h>
#include <poor_mman.h>
#include <poor_stdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Read a file with a single submit, then write it to stdout: open it into a
 * direct descriptor, read it and close it in one linked chain. The read and
 * the close refer to the slot before the file is open. The read is hard
 * linked, since a short read breaks a normal link and would cancel the close.
 *
 * Reads of regular files are only short at EOF, so one read gets the whole
 * file, up to MAX_READ bytes, the most a read returns. The buffer is reserved
 * address space, so only the pages the read fills take memory. The read runs
 * in an io-wq worker, since copying a cached file and faulting in the buffer
 * would stall the loop otherwise.
 *
 * Usage: read_with_poor_op [FILE]
 */
constexpr int SLOT = 0;
constexpr size_t MAX_READ = 0x7ffff000;

static const char *path;
static char (*buf)[MAX_READ];
static unsigned len, written;
static int status;

static bool check(const struct io_uring_cqe *cqe, const char *name, const char *what)
{
	if (cqe->res >= 0)
		return true;
	if (cqe->res != -ECANCELED)
		printerrln(name, ": ", what, ": ", strerror(-cqe->res));
	status = 1;
	return false;
}

static void write_rest(struct poor_loop *loop, struct poor_loop_op *op)
{
	auto unsent = arrview_cfront(written, arrview_first(len, buf));

	io_uring_prep_write_array(poor_loop_get_sqe(loop, op), STDOUT_FILENO, unsent, -1);
}

static void on_write(struct poor_loop *loop, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	if (check(cqe, "stdout", "write") && (written += cqe->res) < len)
		write_rest(loop, op);
	else
		poor_loop_stop(loop);
}

static void on_open(struct poor_loop *, struct poor_loop_op *, const struct io_uring_cqe *cqe)
{
	check(cqe, path, "open");
}

static void on_read(struct poor_loop *, struct poor_loop_op *, const struct io_uring_cqe *cqe)
{
	if (check(cqe, path, "read"))
		len = cqe->res;
}

static void on_close(struct poor_loop *loop, struct poor_loop_op *, const struct io_uring_cqe *cqe)
{
	static struct poor_loop_op write_op = POOR_LOOP_OP_INIT(on_write);

	if (check(cqe, path, "close") && len)
		write_rest(loop, &write_op);
	else
		poor_loop_stop(loop);
}

int main(int argc, char **argv)
{
	struct poor_loop_op open_op = POOR_LOOP_OP_INIT(on_open);
	struct poor_loop_op read_op = POOR_LOOP_OP_INIT(on_read);
	struct poor_loop_op close_op = POOR_LOOP_OP_INIT(on_close);
	struct io_uring_params params = {};
	struct io_uring_sqe *sqe;
	struct poor_loop loop;
	int ret;

	path = argc > 1 ? argv[1] : "/proc/uptime";
	if (mmap_array(buf, nullptr, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0) == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	ret = poor_loop_init(&loop, 8, &params);
	if (ret) {
		printerrln("poor_loop_init: ", strerror(-ret));
		return 1;
	}
	ret = io_uring_register_files_sparse(poor_loop_ring(&loop), 1);
	if (ret) {
		printerrln("io_uring_register_files_sparse: ", strerror(-ret));
		return 1;
	}
	sqe = poor_loop_get_sqe(&loop, &open_op);
	io_uring_prep_openat_direct(sqe, AT_FDCWD, path, O_RDONLY, 0, SLOT);
	sqe->flags |= IOSQE_IO_LINK;
	sqe = poor_loop_get_sqe(&loop, &read_op);
	io_uring_prep_read_array(sqe, SLOT, buf, 0);
	/* HARDLINK is needed here to let the close request run even if the read fails or reaches end of file. */
	sqe->flags |= IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK | IOSQE_ASYNC;
	io_uring_prep_close_direct(poor_loop_get_sqe(&loop, &close_op), SLOT);
	ret = poor_loop_run(&loop);
	poor_loop_exit(&loop);
	return ret || status;
}
