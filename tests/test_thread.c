/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <sys/mman.h>

#include "harness.h"

/* liburing 2.3 has no version check and none of the newer helpers */
#ifndef IO_URING_CHECK_VERSION
#define IO_URING_CHECK_VERSION(major, minor) 1
#endif

constexpr unsigned TURNS = 16;

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
	TEST(thread_loops_in_turn),
	TEST(thread_no_mmap_in_turn),
};

int main(int argc, char **argv)
{
	return run_tests(array_ptr(argv, argc), tests);
}
