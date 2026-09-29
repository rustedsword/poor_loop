/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <chioloop.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* Copies fixed file 'from' to 'to'. */
struct relay {
	struct chio_op read_op;
	struct chio_op write_op;
	int from, to;
	size_t len, off;
	char buf[4096];
};

enum { STDIN_SLOT, STDOUT_SLOT, SOCK_SLOT };

static struct relay up, down;
static struct sockaddr_in addr = { .sin_family = AF_INET };
static int status;

static struct io_uring_sqe *get_sqe(struct chio_loop *loop, struct chio_op *op)
{
	struct io_uring_sqe *sqe = chio_get_sqe_or_submit(loop, op);

	if (!sqe) {
		perror("chio_get_sqe_or_submit");
		exit(1);
	}
	return sqe;
}

static bool check(struct chio_loop *loop, const struct io_uring_cqe *cqe,
		  const char *what)
{
	if (cqe->res >= 0)
		return true;
	if (cqe->res != -ECANCELED)
		fprintf(stderr, "%s: %s\n", what, strerror(-cqe->res));
	status = 1;
	chio_loop_stop(loop);
	return false;
}

static int parse_port(const char *s)
{
	char *end;
	unsigned long port = strtoul(s, &end, 10);

	return *s && !*end && port && port <= 65535 ? (int)port : -1;
}

static void relay_read(struct chio_loop *loop, struct relay *r)
{
	struct io_uring_sqe *sqe = get_sqe(loop, &r->read_op);

	io_uring_prep_read(sqe, r->from, r->buf, sizeof(r->buf), -1);
	sqe->flags |= IOSQE_FIXED_FILE;
}

static void relay_write(struct chio_loop *loop, struct relay *r)
{
	struct io_uring_sqe *sqe = get_sqe(loop, &r->write_op);

	io_uring_prep_write(sqe, r->to, r->buf + r->off, r->len - r->off, -1);
	sqe->flags |= IOSQE_FIXED_FILE;
}

static void on_read(struct chio_loop *loop, struct chio_op *op,
		    const struct io_uring_cqe *cqe)
{
	struct relay *r = container_of(op, struct relay, read_op);

	if (!check(loop, cqe, "read"))
		return;
	if (!cqe->res) {
		if (r == &down) {
			fputs("server closed the connection\n", stderr);
			status = 1;
		}
		chio_loop_stop(loop);
		return;
	}
	r->len = cqe->res;
	r->off = 0;
	relay_write(loop, r);
}

static void on_write(struct chio_loop *loop, struct chio_op *op,
		     const struct io_uring_cqe *cqe)
{
	struct relay *r = container_of(op, struct relay, write_op);

	if (!check(loop, cqe, "write"))
		return;
	r->off += cqe->res;
	if (r->off < r->len)
		relay_write(loop, r);
	else
		relay_read(loop, r);
}

static void on_connect(struct chio_loop *loop, struct chio_op *,
		       const struct io_uring_cqe *cqe)
{
	if (!check(loop, cqe, "connect"))
		return;
	relay_write(loop, &up);
	relay_read(loop, &down);
}

static void on_socket(struct chio_loop *loop, struct chio_op *,
		      const struct io_uring_cqe *cqe)
{
	check(loop, cqe, "socket");
}

static void connect_start(struct chio_loop *loop)
{
	static struct chio_op socket_op = CHIO_OP_INIT(on_socket);
	static struct chio_op connect_op = CHIO_OP_INIT(on_connect);
	struct io_uring_sqe *sqe;
	int ret = chio_check_sq_space_or_submit(loop, 2);

	if (ret) {
		fprintf(stderr, "chio_check_sq_space_or_submit: %s\n",
			strerror(-ret));
		exit(1);
	}
	sqe = get_sqe(loop, &socket_op);
	io_uring_prep_socket_direct(sqe, AF_INET, SOCK_STREAM, 0, SOCK_SLOT, 0);
	sqe->flags |= IOSQE_IO_LINK;
	sqe = get_sqe(loop, &connect_op);
	io_uring_prep_connect(sqe, SOCK_SLOT, (struct sockaddr *)&addr,
			      sizeof(addr));
	sqe->flags |= IOSQE_FIXED_FILE;
}

int main(int argc, char **argv)
{
	struct io_uring_params params = {};
	struct chio_loop loop;
	int port = argc > 3 ? parse_port(argv[3]) : 7777;
	int ret;

	if (argc < 2 || !*argv[1] || port < 0) {
		fprintf(stderr, "usage: %s NICK [HOST [PORT]]\n", argv[0]);
		return 1;
	}
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, argc > 2 ? argv[2] : "127.0.0.1",
		      &addr.sin_addr) != 1) {
		fprintf(stderr, "HOST must be a numeric IPv4 address\n");
		return 1;
	}
	signal(SIGPIPE, SIG_IGN);
	ret = chio_loop_init(&loop, 16, &params);
	if (ret) {
		fprintf(stderr, "chio_loop_init: %s\n", strerror(-ret));
		return 1;
	}
	ret = io_uring_register_files(chio_loop_ring(&loop),
				      (const int[]){ 0, 1, -1 }, 3);
	if (ret) {
		fprintf(stderr, "io_uring_register_files: %s\n", strerror(-ret));
		return 1;
	}

	up = (struct relay){ .read_op = CHIO_OP_INIT(on_read),
			     .write_op = CHIO_OP_INIT(on_write),
			     .from = STDIN_SLOT, .to = SOCK_SLOT };
	up.len = snprintf(up.buf, sizeof(up.buf), "%.32s\n", argv[1]);
	down = (struct relay){ .read_op = CHIO_OP_INIT(on_read),
			       .write_op = CHIO_OP_INIT(on_write),
			       .from = SOCK_SLOT, .to = STDOUT_SLOT };
	connect_start(&loop);
	ret = chio_loop_run(&loop);
	return ret || status;
}
