/* SPDX-License-Identifier: MIT */
#ifndef POOR_LOOP_TEST_HARNESS_H
#define POOR_LOOP_TEST_HARNESS_H

#include <poor_loop.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SKIP 77
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

#define CHECK(cond) \
	do { \
		if (!(cond)) \
			fail(__FILE__, __LINE__, #cond); \
	} while (0)

#define CHECK_EQ(a, b) \
	do { \
		long long a_ = (long long)(a), b_ = (long long)(b); \
		if (a_ != b_) \
			fail_eq(__FILE__, __LINE__, #a, #b, a_, b_); \
	} while (0)

struct mode {
	const char *name;
	unsigned flags;
};

struct rec {
	struct poor_loop_op op;
	int calls;
	int more;
	int res;
	unsigned flags;
};

#define drain(loop, ...) \
	drain_ops(loop, \
		  ARRAY_SIZE(((struct poor_loop_op *[]){ __VA_ARGS__ })), \
		  &(struct poor_loop_op *[]){ __VA_ARGS__ })

struct tick {
	struct poor_loop_timer timer;
	int id;
	int fired;
	bool armed;
	uint64_t at;
};

struct test {
	const char *name;
	int (*fn)(void);
};

#define TEST(name) { #name, test_##name }

extern const struct mode *mode;
extern uint64_t tick_order;
extern const struct test tests[];
extern const size_t tests_count;

[[noreturn]] void fail(const char *file, int line, const char *expr);
[[noreturn]] void fail_eq(const char *file, int line, const char *a,
			  const char *b, long long va, long long vb);
void setup_params(struct io_uring_params *p);
void loop_init(struct poor_loop *loop, unsigned entries);
void enable_ring(struct poor_loop *loop);
void make_pipe(int (*fds)[2]);
void close_pipe(int (*fds)[2]);
struct io_uring_sqe *get_sqe(struct poor_loop *loop, struct poor_loop_op *op);
void arm_nop(struct poor_loop *loop, struct poor_loop_op *op);
void arm_read(struct poor_loop *loop, struct poor_loop_op *op, int fd,
	      unsigned len, char (*buf)[len]);
void drain_ops(struct poor_loop *loop, size_t count,
	       struct poor_loop_op *(*ops)[count]);
void rec_complete(struct poor_loop *loop, struct poor_loop_op *op,
		  const struct io_uring_cqe *cqe);
void rec_init(struct rec *rec, poor_loop_complete_fn *complete);
void stop_complete(struct poor_loop *loop, struct poor_loop_op *op,
		   const struct io_uring_cqe *cqe);
void tick_fire(struct poor_loop *loop, struct poor_loop_timer *timer);
void tick_init(struct tick *tick, poor_loop_timer_fn *fire, int id);
void stop_fire(struct poor_loop *loop, struct poor_loop_timer *timer);
void halt(struct poor_loop *loop, struct poor_loop_timer *timer);

#endif
