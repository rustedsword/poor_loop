/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <chioloop.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>

/*
 * The first line a client sends is its nick; every later line is a message.
 * Lines are stored without control characters and without the newline.
 */
struct client {
	struct chio_list link;
	struct chio_op recv_op;
	struct chio_op send_op;
	int fd;
	bool has_nick;
	uint64_t flood;
	size_t nick_len, in_len, out_len;
	char nick[32];
	char in[1024];
	char out[65536];
};

struct step {
	struct chio_op op;
	const char *name;
};

constexpr unsigned BUF_COUNT = 64;
constexpr unsigned BUF_SIZE = 4096;
constexpr int LISTEN_SLOT = 0;
constexpr uint64_t ACCEPT_RETRY_NS = 100'000'000;
constexpr uint64_t LINE_COST_NS = 200'000'000;
constexpr uint64_t FLOOD_BURST_NS = 10 * LINE_COST_NS;

static char bufs[BUF_COUNT][BUF_SIZE];
static struct io_uring_buf_ring *buf_ring;
static struct chio_list clients;
static struct sockaddr_in addr = { .sin_family = AF_INET };
static struct signalfd_siginfo siginfo;
static sigset_t signals;
static unsigned nclients;
static int one = 1, signal_fd, status;
static bool stopping;

static void on_accept(struct chio_loop *loop, struct chio_op *op,
		      const struct io_uring_cqe *cqe);

static struct chio_op accept_op = CHIO_OP_INIT(on_accept);

static struct io_uring_sqe *get_sqe(struct chio_loop *loop, struct chio_op *op)
{
	struct io_uring_sqe *sqe = chio_get_sqe_or_submit(loop, op);

	if (!sqe) {
		perror("chio_get_sqe_or_submit");
		exit(1);
	}
	return sqe;
}

static void reserve(struct chio_loop *loop, unsigned n)
{
	int ret = chio_check_sq_space_or_submit(loop, n);

	if (ret) {
		fprintf(stderr, "chio_check_sq_space_or_submit: %s\n",
			strerror(-ret));
		exit(1);
	}
}

static void fd_close(struct chio_loop *loop, int fd)
{
	reserve(loop, 1);
	io_uring_prep_close(chio_get_untracked_sqe(loop), fd);
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

/*
 * Pending recv and send keep the socket alive, so closing the fd would not end
 * them; the shutdown does. The hard link runs close even if shutdown fails,
 * e.g. with ENOTCONN after a reset.
 */
static void client_drop(struct chio_loop *loop, struct client *c)
{
	struct io_uring_sqe *sqe;

	if (!chio_list_linked(&c->link))
		return;
	chio_list_remove(&c->link);
	reserve(loop, 2);
	sqe = chio_get_untracked_sqe(loop);
	io_uring_prep_shutdown(sqe, c->fd, SHUT_RDWR);
	sqe->flags |= IOSQE_IO_HARDLINK;
	io_uring_prep_close(chio_get_untracked_sqe(loop), c->fd);
}

static void on_drained(struct chio_loop *loop, struct chio_op *,
		       const struct io_uring_cqe *)
{
	chio_loop_stop(loop);
}

/*
 * Once all clients are freed and the accept is gone, only untracked requests
 * can be left. A drained NOP completes after all of them.
 */
static void shutdown_finish(struct chio_loop *loop)
{
	static struct chio_op drain_op = CHIO_OP_INIT(on_drained);
	struct io_uring_sqe *sqe;

	if (!stopping || nclients || accept_op.pending)
		return;
	sqe = get_sqe(loop, &drain_op);
	io_uring_prep_nop(sqe);
	sqe->flags |= IOSQE_IO_DRAIN;
}

/* Only called at the end of a final recv or send completion. */
static void client_release(struct chio_loop *loop, struct client *c)
{
	if (c->recv_op.pending || c->send_op.pending)
		return;
	free(c);
	nclients--;
	shutdown_finish(loop);
}

static void client_flush(struct chio_loop *loop, struct client *c)
{
	io_uring_prep_send(get_sqe(loop, &c->send_op), c->fd, c->out,
			   c->out_len, MSG_NOSIGNAL);
}

/*
 * A full out buffer drops the reader. Flood control is per sender, so enough
 * senders bursting in one loop iteration can still drop a healthy reader; any
 * bounded buffer has that limit.
 */
static void client_send(struct chio_loop *loop, struct client *c,
			const char *data, size_t len)
{
	if (c->out_len + len > sizeof(c->out)) {
		client_drop(loop, c);
		return;
	}
	memcpy(c->out + c->out_len, data, len);
	c->out_len += len;
	if (!c->send_op.pending)
		client_flush(loop, c);
}

/*
 * IRC-style flood control: every line costs LINE_COST_NS and a sender may run
 * at most FLOOD_BURST_NS ahead of now. Output only drains between loop
 * iterations, so this bounds what one sender can queue for each reader.
 */
static void client_line(struct chio_loop *loop, struct client *c)
{
	char msg[sizeof(c->nick) + 2 + sizeof(c->in) + 1];
	size_t n = c->in_len, len = c->nick_len + 2 + n + 1;
	uint64_t now = chio_now();
	struct client *o, *tmp;

	c->in_len = 0;
	if (!c->has_nick) {
		c->has_nick = true;
		c->nick_len = n < sizeof(c->nick) ? n : sizeof(c->nick);
		memcpy(c->nick, c->in, c->nick_len);
		return;
	}
	if (!n)
		return;
	c->flood = (c->flood > now ? c->flood : now) + LINE_COST_NS;
	if (c->flood > now + FLOOD_BURST_NS) {
		client_drop(loop, c);
		return;
	}
	memcpy(msg, c->nick, c->nick_len);
	memcpy(msg + c->nick_len, ": ", 2);
	memcpy(msg + c->nick_len + 2, c->in, n);
	msg[len - 1] = '\n';
	chio_list_for_each_safe(o, tmp, &clients, link)
		if (o != c)
			client_send(loop, o, msg, len);
}

/*
 * Control characters are dropped so nobody can fake a nick: C0 and DEL, and C1
 * (U+0080..U+009F, i.e. 0xc2 0x80..0x9f), which some terminals obey too. Long
 * lines are split, except the nick line, which is cut.
 */
static void client_input(struct chio_loop *loop, struct client *c,
			 const char *data, size_t len)
{
	unsigned char ch;

	for (; len && chio_list_linked(&c->link); data++, len--) {
		ch = *data;
		if (ch == '\n') {
			client_line(loop, c);
		} else if (ch >= 0x80 && ch <= 0x9f && c->in_len &&
			   (unsigned char)c->in[c->in_len - 1] == 0xc2) {
			c->in_len--;
		} else if (!iscntrl(ch)) {
			if (c->in_len == sizeof(c->in)) {
				if (!c->has_nick)
					continue;
				client_line(loop, c);
			}
			c->in[c->in_len++] = ch;
		}
	}
}

static void client_recv(struct chio_loop *loop, struct client *c)
{
	struct io_uring_sqe *sqe = get_sqe(loop, &c->recv_op);

	io_uring_prep_recv_multishot(sqe, c->fd, nullptr, 0, 0);
	sqe->flags |= IOSQE_BUFFER_SELECT;
	sqe->buf_group = 0;
}

static void buf_recycle(unsigned id)
{
	io_uring_buf_ring_add(buf_ring, bufs[id], BUF_SIZE, id,
			      io_uring_buf_ring_mask(BUF_COUNT), 0);
	io_uring_buf_ring_advance(buf_ring, 1);
}

static void on_send(struct chio_loop *loop, struct chio_op *op,
		    const struct io_uring_cqe *cqe)
{
	struct client *c = chio_container_of(op, struct client, send_op);

	if (cqe->res < 0 || !chio_list_linked(&c->link)) {
		client_drop(loop, c);
		client_release(loop, c);
		return;
	}
	c->out_len -= cqe->res;
	memmove(c->out, c->out + cqe->res, c->out_len);
	if (c->out_len)
		client_flush(loop, c);
}

static void on_recv(struct chio_loop *loop, struct chio_op *op,
		    const struct io_uring_cqe *cqe)
{
	struct client *c = chio_container_of(op, struct client, recv_op);
	unsigned id;

	if (cqe->flags & IORING_CQE_F_BUFFER) {
		id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
		if (cqe->res > 0 && chio_list_linked(&c->link))
			client_input(loop, c, bufs[id], cqe->res);
		buf_recycle(id);
	}
	if (op->pending)
		return;
	if ((cqe->res > 0 || cqe->res == -ENOBUFS) && chio_list_linked(&c->link)) {
		client_recv(loop, c);
		return;
	}
	client_drop(loop, c);
	client_release(loop, c);
}

static void accept_arm(struct chio_loop *loop, struct chio_op *op)
{
	struct io_uring_sqe *sqe = get_sqe(loop, op);

	io_uring_prep_multishot_accept(sqe, LISTEN_SLOT, nullptr, nullptr, 0);
	sqe->flags |= IOSQE_FIXED_FILE;
}

static void on_accept_retry(struct chio_loop *loop, struct chio_timer *)
{
	accept_arm(loop, &accept_op);
}

static struct chio_timer accept_timer = CHIO_TIMER_INIT(on_accept_retry);

/*
 * -ECANCELED means a setup step failed and already reported why. Other errors
 * are retried later: while out of fds, a queued connection makes accept fail
 * again at once.
 */
static void on_accept(struct chio_loop *loop, struct chio_op *op,
		      const struct io_uring_cqe *cqe)
{
	struct client *c;

	if (stopping) {
		if (cqe->res >= 0)
			fd_close(loop, cqe->res);
		if (!op->pending)
			shutdown_finish(loop);
		return;
	}
	if (cqe->res == -ECANCELED) {
		check(loop, cqe, "accept");
		return;
	}
	if (cqe->res < 0) {
		fprintf(stderr, "accept: %s\n", strerror(-cqe->res));
		if (!op->pending)
			chio_timer_arm(loop, &accept_timer,
				       chio_now() + ACCEPT_RETRY_NS);
		return;
	}
	c = malloc(sizeof(*c));
	if (c) {
		chio_op_init(&c->recv_op, on_recv);
		chio_op_init(&c->send_op, on_send);
		c->fd = cqe->res;
		c->has_nick = false;
		c->flood = 0;
		c->nick_len = c->in_len = c->out_len = 0;
		chio_list_append(&clients, &c->link);
		nclients++;
		client_recv(loop, c);
	} else {
		fputs("out of memory\n", stderr);
		fd_close(loop, cqe->res);
	}
	if (!op->pending)
		accept_arm(loop, op);
}

static void on_step(struct chio_loop *loop, struct chio_op *op,
		    const struct io_uring_cqe *cqe)
{
	check(loop, cqe, chio_container_of(op, struct step, op)->name);
}

static struct step steps[] = {
	{ CHIO_OP_INIT(on_step), "socket" },
	{ CHIO_OP_INIT(on_step), "setsockopt" },
	{ CHIO_OP_INIT(on_step), "bind" },
	{ CHIO_OP_INIT(on_step), "listen" },
};

/* The socket is a direct descriptor, so the linked SQEs can refer to it before it exists. */
static void listen_start(struct chio_loop *loop)
{
	struct io_uring_sqe *sqe;

	reserve(loop, 5);
	sqe = get_sqe(loop, &steps[0].op);
	io_uring_prep_socket_direct(sqe, AF_INET, SOCK_STREAM, 0, LISTEN_SLOT,
				    0);
	sqe->flags |= IOSQE_IO_LINK;
	sqe = get_sqe(loop, &steps[1].op);
	io_uring_prep_cmd_sock(sqe, SOCKET_URING_OP_SETSOCKOPT, LISTEN_SLOT,
			       SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sqe->flags |= IOSQE_FIXED_FILE | IOSQE_IO_LINK;
	sqe = get_sqe(loop, &steps[2].op);
	io_uring_prep_bind(sqe, LISTEN_SLOT, (struct sockaddr *)&addr,
			   sizeof(addr));
	sqe->flags |= IOSQE_FIXED_FILE | IOSQE_IO_LINK;
	sqe = get_sqe(loop, &steps[3].op);
	io_uring_prep_listen(sqe, LISTEN_SLOT, SOMAXCONN);
	sqe->flags |= IOSQE_FIXED_FILE | IOSQE_IO_LINK;
	accept_arm(loop, &accept_op);
}

/* Signals stay unblocked afterwards, so a second Ctrl-C kills the server. */
static void on_signal(struct chio_loop *loop, struct chio_op *,
		      const struct io_uring_cqe *cqe)
{
	struct client *c, *tmp;

	if (!check(loop, cqe, "signalfd"))
		return;
	sigprocmask(SIG_UNBLOCK, &signals, nullptr);
	stopping = true;
	chio_timer_disarm(&accept_timer);
	reserve(loop, 3);
	if (accept_op.pending)
		io_uring_prep_cancel(chio_get_untracked_sqe(loop), &accept_op, 0);
	io_uring_prep_close_direct(chio_get_untracked_sqe(loop), LISTEN_SLOT);
	io_uring_prep_close(chio_get_untracked_sqe(loop), signal_fd);
	chio_list_for_each_safe(c, tmp, &clients, link)
		client_drop(loop, c);
	shutdown_finish(loop);
}

int main(int argc, char **argv)
{
	struct chio_op signal_op = CHIO_OP_INIT(on_signal);
	struct io_uring_params params = {};
	struct chio_loop loop;
	int port = argc > 1 ? parse_port(argv[1]) : 7777;
	int ret;

	if (port < 0) {
		fprintf(stderr, "usage: %s [PORT]\n", argv[0]);
		return 1;
	}
	addr.sin_port = htons(port);
	sigemptyset(&signals);
	sigaddset(&signals, SIGINT);
	sigaddset(&signals, SIGTERM);
	sigprocmask(SIG_BLOCK, &signals, nullptr);
	signal_fd = signalfd(-1, &signals, SFD_CLOEXEC);
	if (signal_fd < 0) {
		perror("signalfd");
		return 1;
	}
	ret = chio_loop_init(&loop, 256, &params);
	if (ret) {
		fprintf(stderr, "chio_loop_init: %s\n", strerror(-ret));
		return 1;
	}
	buf_ring = io_uring_setup_buf_ring(chio_loop_ring(&loop), BUF_COUNT, 0,
					   0, &ret);
	if (!buf_ring) {
		fprintf(stderr, "io_uring_setup_buf_ring: %s\n", strerror(-ret));
		return 1;
	}
	for (unsigned i = 0; i < BUF_COUNT; i++)
		buf_recycle(i);
	ret = io_uring_register_files_sparse(chio_loop_ring(&loop), 1);
	if (ret) {
		fprintf(stderr, "io_uring_register_files_sparse: %s\n",
			strerror(-ret));
		return 1;
	}
	chio_list_init(&clients);
	listen_start(&loop);
	io_uring_prep_read(get_sqe(&loop, &signal_op), signal_fd, &siginfo,
			   sizeof(siginfo), -1);
	ret = chio_loop_run(&loop);
	if (ret || status)
		return 1;
	io_uring_free_buf_ring(chio_loop_ring(&loop), buf_ring, BUF_COUNT, 0);
	chio_loop_exit(&loop);
	return 0;
}
