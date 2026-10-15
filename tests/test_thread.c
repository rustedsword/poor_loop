/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <poor_loop_fd.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <sys/mman.h>

#include "harness.h"

/* liburing 2.3 has no version check and none of the newer helpers */
#ifndef IO_URING_CHECK_VERSION
#define IO_URING_CHECK_VERSION(major, minor) 1
#endif

constexpr unsigned ROUNDS = 100;
constexpr unsigned TURNS = 16;

poor_loop_fd_declare(pipe_end);

struct peer {
	struct poor_loop loop;
	pipe_end in, out;
	struct poor_loop_op read_op, write_op;
	unsigned msg, count, open;
	bool sender;
};

poor_loop_group_attach(pipe_end, struct peer, in, read_op, on_read);
poor_loop_group_attach(pipe_end, struct peer, out, write_op, on_write);

static struct peer *peer_of(struct poor_loop *loop)
{
	return container_of(loop, struct peer, loop);
}

static void start_read(struct peer *p)
{
	auto sqe = poor_loop_group_get_sqe(&p->loop, &p->in, &p->read_op);

	CHECK(sqe);
	io_uring_prep_read_array(sqe, p->in.fd, array_ptr(&p->msg), -1);
}

static void start_write(struct peer *p)
{
	auto sqe = poor_loop_group_get_sqe(&p->loop, &p->out, &p->write_op);

	CHECK(sqe);
	io_uring_prep_write_array(sqe, p->out.fd, array_ptr(&p->msg), -1);
}

/* A peer holds both ends while it talks, so they close after the last op */
static void hang_up(struct peer *p)
{
	if (poor_loop_group_release(&p->in))
		poor_loop_group_call_done(pipe_end, &p->loop, &p->in);
	if (poor_loop_group_release(&p->out))
		poor_loop_group_call_done(pipe_end, &p->loop, &p->out);
}

POOR_LOOP_GROUP_OP(on_read, loop, in, op, cqe)
{
	struct peer *p = peer_of(loop);

	if (!p->sender && !cqe->res) {
		hang_up(p);
		return;
	}
	CHECK_EQ(cqe->res, sizeof(p->msg));
	if (!p->sender) {
		CHECK_EQ(p->msg, ++p->count);
	} else if (p->msg == p->count && p->count < ROUNDS) {
		p->msg = ++p->count;
	} else {
		CHECK_EQ(p->msg, ROUNDS);
		hang_up(p);
		return;
	}
	start_write(p);
}

POOR_LOOP_GROUP_OP(on_write, loop, out, op, cqe)
{
	CHECK_EQ(cqe->res, sizeof(unsigned));
	start_read(peer_of(loop));
}

POOR_LOOP_FD_DONE(pipe_end, loop, end)
{
	if (!--peer_of(loop)->open)
		poor_loop_stop(loop);
}

static void peer_init(struct peer *p, int in, int out, bool sender)
{
	poor_loop_fd_init(&p->in, in);
	poor_loop_fd_init(&p->out, out);
	poor_loop_op_init(&p->read_op, on_read);
	poor_loop_op_init(&p->write_op, on_write);
	p->count = 0;
	p->open = 2;
	p->sender = sender;
}

static void *talk(void *arg)
{
	struct peer *p = arg;

	loop_init(&p->loop, 8);
	poor_loop_group_acquire(&p->in);
	poor_loop_group_acquire(&p->out);
	if (p->sender) {
		p->msg = ++p->count;
		start_write(p);
	} else {
		start_read(p);
	}
	CHECK_EQ(poor_loop_run(&p->loop), 0);
	CHECK_EQ(poor_loop_drain(&p->loop, poor_loop_now() + 10'000'000'000), 0);
	poor_loop_exit(&p->loop);
	return nullptr;
}

/* The sender closes its ends after the last reply, which makes the other side read EOF */
static int test_thread_ping_pong(void)
{
	int request[2], reply[2];
	struct peer ping, pong;
	pthread_t thread;

	make_pipe(&request);
	make_pipe(&reply);
	peer_init(&ping, reply[0], request[1], true);
	peer_init(&pong, request[0], reply[1], false);
	CHECK_EQ(pthread_create(&thread, nullptr, talk, &pong), 0);
	talk(&ping);
	CHECK_EQ(pthread_join(thread, nullptr), 0);
	CHECK_EQ(ping.count, ROUNDS);
	CHECK_EQ(pong.count, ROUNDS);
	return 0;
}

/* Relaxed atomics give TSan no happens-before, like threads that only talk through rings */
static atomic_uint turn;

static void wait_turn(unsigned i)
{
	while (atomic_load_explicit(&turn, memory_order_relaxed) != i)
		sched_yield();
}

static void pass_turn(unsigned i)
{
	atomic_store_explicit(&turn, i, memory_order_relaxed);
}

static void take_turns(unsigned first)
{
	for (unsigned i = first; i < TURNS; i += 2) {
		struct poor_loop loop;
		struct rec rec;

		wait_turn(i);
		loop_init(&loop, 8);
		rec_init(&rec, rec_complete);
		arm_nop(&loop, &rec.op);
		drain(&loop, &rec.op);
		CHECK_EQ(rec.calls, 1);
		poor_loop_exit(&loop);
		pass_turn(i + 1);
	}
}

static void *odd_turns(void *)
{
	take_turns(1);
	return nullptr;
}

/* A new ring often gets the memory of the one closed before, here by another thread */
static int test_thread_loops_in_turn(void)
{
	pthread_t thread;

	pass_turn(0);
	CHECK_EQ(pthread_create(&thread, nullptr, odd_turns, nullptr), 0);
	take_turns(0);
	CHECK_EQ(pthread_join(thread, nullptr), 0);
	return 0;
}

#if !IO_URING_CHECK_VERSION(2, 5)
static char *no_mmap_mem;
static size_t no_mmap_size;

/*
 * poor_loop_init() gets the rings right after the SQEs only on huge pages, so give the
 * memory to liburing. Turn 1 starts a page later, so its SQEs go over the rings of turn 0.
 */
static int no_mmap_turn(unsigned i)
{
	size_t offset = i * (size_t)sysconf(_SC_PAGESIZE);
	struct io_uring_params p;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	int ret;

	setup_params(&p);
	ret = io_uring_queue_init_mem(8, &ring, &p, no_mmap_mem + offset, no_mmap_size - offset);
	if (ret == -EINVAL)
		return SKIP;
	CHECK(ret > 0);
	CHECK_EQ(ring.sq.ring_sz, 0);
	poor_loop_tsan_ring_acquire(&ring);
	io_uring_prep_nop(io_uring_get_sqe(&ring));
	CHECK(io_uring_submit_and_wait(&ring, 1) >= 0);
	CHECK_EQ(io_uring_wait_cqe(&ring, &cqe), 0);
	io_uring_cqe_seen(&ring, cqe);
	poor_loop_tsan_ring_release(&ring);
	io_uring_queue_exit(&ring);
	return 0;
}

static void *no_mmap_second_turn(void *)
{
	wait_turn(1);
	return (void *)(intptr_t)no_mmap_turn(1);
}

/* With IORING_SETUP_NO_MMAP liburing can put the rings after the SQEs and leave ring_sz zero */
static int test_thread_no_mmap_in_turn(void)
{
	pthread_t thread;
	void *second;
	int ret;

	no_mmap_size = 4 * (size_t)sysconf(_SC_PAGESIZE);
	no_mmap_mem = mmap(nullptr, no_mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	CHECK(no_mmap_mem != MAP_FAILED);
	pass_turn(0);
	CHECK_EQ(pthread_create(&thread, nullptr, no_mmap_second_turn, nullptr), 0);
	ret = no_mmap_turn(0);
	pass_turn(1);
	CHECK_EQ(pthread_join(thread, &second), 0);
	CHECK_EQ((intptr_t)second, ret);
	CHECK_EQ(munmap(no_mmap_mem, no_mmap_size), 0);
	return ret;
}
#else
static int test_thread_no_mmap_in_turn(void)
{
	return SKIP;
}
#endif

static const struct test tests[] = {
	TEST(thread_ping_pong),
	TEST(thread_loops_in_turn),
	TEST(thread_no_mmap_in_turn),
};

int main(int argc, char **argv)
{
	return run_tests(array_ptr(argv, argc), tests);
}
