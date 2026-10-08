/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include "harness.h"
#include "shared_group.h"

poor_loop_group_attach(shared_io, struct shared, io, nop, shared_nop_done);
poor_loop_group_attach(shared_io, struct shared, io, tick, shared_tick);

POOR_LOOP_GROUP_OP(shared_nop_done, loop, io, op, cqe)
{
	struct shared *s = container_of(io, struct shared, io);

	CHECK(op == &s->nop);
	CHECK_EQ(cqe->res, 0);
	s->completed++;
}

POOR_LOOP_GROUP_TIMER(shared_tick, loop, io, timer)
{
	struct shared *s = container_of(io, struct shared, io);

	CHECK(timer == &s->tick);
	s->completed++;
}

POOR_LOOP_GROUP_DONE(shared_io, loop, io)
{
	container_of(io, struct shared, io)->calls++;
	poor_loop_stop(loop);
}

void shared_finish(struct poor_loop *loop, struct shared *s)
{
	poor_loop_group_call_done(shared_io, loop, &s->io);
}

void shared_init(struct shared *s)
{
	poor_loop_group_init(&s->io);
	poor_loop_op_init(&s->nop, shared_nop_done);
	poor_loop_timer_init(&s->tick, shared_tick);
	s->completed = 0;
	s->calls = 0;
}
