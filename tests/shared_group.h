/* SPDX-License-Identifier: MIT */
#ifndef POOR_LOOP_TEST_SHARED_GROUP_H
#define POOR_LOOP_TEST_SHARED_GROUP_H

#include <poor_loop_group.h>

poor_loop_group_declare(shared_io);

struct shared {
	shared_io io;
	struct poor_loop_op nop;
	struct poor_loop_timer tick;
	unsigned completed, calls;
};

void shared_init(struct shared *s);
void shared_finish(struct poor_loop *loop, struct shared *s);

#endif
