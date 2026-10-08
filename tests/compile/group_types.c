/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <poor_loop_group.h>

poor_loop_group_declare(io_group);
poor_loop_group_declare(empty_group);

struct owner {
	int value;
	io_group work;
	empty_group empty;
	struct poor_loop_op op;
	struct poor_loop_timer retry;
	const io_group *shared;
};

#if defined(BAD_ATTACH_GROUP)
poor_loop_group_attach(io_group, struct owner, value, op, on_op);
#elif defined(CONST_ATTACH_GROUP)
poor_loop_group_attach(io_group, struct owner, shared, op, on_op);
#elif defined(BAD_ATTACH_MEMBER)
poor_loop_group_attach(io_group, struct owner, work, value, on_op);
#else
poor_loop_group_attach(io_group, struct owner, work, op, on_op);
#endif
poor_loop_group_attach(io_group, struct owner, work, retry, on_timer);

#if defined(TIMER_WITH_OP)
POOR_LOOP_GROUP_TIMER(on_op, event_loop, group, timer)
{
	container_of(group, struct owner, work)->value++;
}
#else
POOR_LOOP_GROUP_OP(on_op, event_loop, group, op, completion)
{
	container_of(op, struct owner, op)->value = completion->res + group->group.pending;
}
#endif

#if defined(OP_WITH_TIMER)
POOR_LOOP_GROUP_OP(on_timer, event_loop, group, op, completion)
#else
POOR_LOOP_GROUP_TIMER(on_timer, event_loop, group, timer)
#endif
{
	container_of(group, struct owner, work)->value++;
}

POOR_LOOP_GROUP_DONE(io_group, event_loop, group)
{
	container_of(group, struct owner, work)->value++;
}

POOR_LOOP_GROUP_DONE(empty_group, event_loop, group)
{
	container_of(group, struct owner, empty)->value++;
}

/* The initializer works for static objects as well as automatic ones. */
static struct owner initial = {
	.work = POOR_LOOP_GROUP_INIT,
	.empty = POOR_LOOP_GROUP_INIT,
	.op = POOR_LOOP_OP_INIT(on_op),
	.retry = POOR_LOOP_TIMER_INIT(on_timer),
};

void check_types(struct poor_loop *loop, struct owner *owner)
{
	poor_loop_group_init(&owner->work);
	poor_loop_group_init(&owner->empty);
	poor_loop_group_acquire(&initial.empty);
#if defined(DISCARD_RELEASE)
	poor_loop_group_release(&initial.empty);
#else
	bool idle = poor_loop_group_release(&initial.empty);

	if (idle)
		poor_loop_group_call_done(empty_group, loop, &initial.empty);
#endif
#if defined(CALL_DONE_WRONG_GROUP)
	poor_loop_group_call_done(io_group, loop, &owner->empty);
#endif

#if defined(CONST_SQE_GROUP)
	const io_group *ops = &owner->work;
#else
	io_group *ops = &owner->work;
#endif
	auto sqe = poor_loop_group_get_sqe(loop, ops, &owner->op);
	io_uring_prep_nop(sqe);
	io_uring_prep_nop(poor_loop_group_get_sqe_or_submit(loop, ops, &owner->op));
#if defined(CONST_ARM_GROUP)
	const io_group *timers = &owner->work;
#else
	io_group *timers = &owner->work;
#endif
	poor_loop_group_timer_arm(loop, timers, &owner->retry, 0);
#if defined(DISCARD_DISARM)
	poor_loop_group_timer_disarm(&owner->work, &owner->retry);
#elif !defined(DISCARD_RELEASE)
	idle = poor_loop_group_timer_disarm(&owner->work, &owner->retry) && idle;
	(void)idle;
#endif
}

/* Group names may match identifiers used inside the generated helpers. */
poor_loop_group_declare(loop);
poor_loop_group_declare(ptr);
poor_loop_group_declare(group);
poor_loop_group_declare(timer);

struct shadow {
	loop l;
	ptr *p;
	group g;
	timer t;
	struct poor_loop_op op;
	struct poor_loop_timer tick;
};

poor_loop_group_attach(loop, struct shadow, l, op, on_shadow_op);
poor_loop_group_attach(ptr, struct shadow, p, tick, on_shadow_tick);

POOR_LOOP_GROUP_OP(on_shadow_op, loop, group, op, cqe) {}
POOR_LOOP_GROUP_TIMER(on_shadow_tick, loop, group, timer) {}
POOR_LOOP_GROUP_DONE(loop, loop, group) {}
POOR_LOOP_GROUP_DONE(ptr, loop, group) {}
POOR_LOOP_GROUP_DONE(group, loop, group) {}
POOR_LOOP_GROUP_DONE(timer, loop, group) {}

void check_shadow(struct poor_loop *l, struct shadow *s)
{
	*s = (struct shadow){
		.l = POOR_LOOP_GROUP_INIT,
		.g = POOR_LOOP_GROUP_INIT,
		.t = POOR_LOOP_GROUP_INIT,
		.op = POOR_LOOP_OP_INIT(on_shadow_op),
		.tick = POOR_LOOP_TIMER_INIT(on_shadow_tick),
	};
	io_uring_prep_nop(poor_loop_group_get_sqe(l, &s->l, &s->op));
	poor_loop_group_timer_arm(l, s->p, &s->tick, 0);
	poor_loop_group_acquire(&s->g);
	poor_loop_group_acquire(&s->t);
	bool idle = poor_loop_group_timer_disarm(s->p, &s->tick) && poor_loop_group_release(&s->g) &&
		    poor_loop_group_release(&s->t);
	(void)idle;
}
