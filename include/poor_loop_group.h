/* SPDX-License-Identifier: MIT */
#ifndef POOR_LOOP_GROUP_H
#define POOR_LOOP_GROUP_H

#include <assert.h>
#include <limits.h>
#include <poor_traits.h>

#include "poor_loop_op.h"
#include "poor_loop_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Poor Loop Group.
 *
 * Calls 'done' function after all ops and timers are completed.
 *
 * Each acquired op or started timer op increases counter by 1.
 * Each completed op or finished/disarmed timer reduces counter by 1.
 *
 * After the last op/timer callback returns and the counter reaches zero
 * the 'done' function will be called.
 *
 * You can free group's memory inside done function.
 *
 * You can use any standalone op or timer with the group (e.g. dynamically allocate them)
 * But they must be attached to the group and the proper callback is set.
 * Use only poor_loop_group_xxx functions to work with attached timers or ops.
 *
 * Attached timers/ops do not change their behavior. So they can be freed in their callbacks as before.
 * Also, after timer or op is completed you can stop using it with the group by setting non-group callback
 * and using functions from poor_loop_op.h
 *
 * Ops are only completed when IORING_CQE_F_MORE is not set.
 *
 * IMPORTANT:
 *   poor_loop_group_release() and poor_loop_group_timer_disarm() do not call 'done' if counter reaches zero,
 *   these functions return true instead. Then call poor_loop_group_call_done() yourself,
 *   after the last use of the group.
 *
 * Example:
 *
 *   poor_loop_group_declare(job_io);
 *
 *   struct job {
 *           job_io io;
 *           struct poor_loop_op read;
 *           int fd;
 *           char buf[64];
 *   };
 *
 *   poor_loop_group_attach(job_io, struct job, io, read, read_done);
 *
 *   POOR_LOOP_GROUP_OP(read_done, loop, io, op, cqe)
 *   {
 *           printf("read: %d\n", cqe->res);
 *   }
 *
 *   POOR_LOOP_GROUP_DONE(job_io, loop, io)
 *   {
 *           struct job *job = container_of(io, struct job, io);
 *
 *           close(job->fd);
 *           free(job);
 *   }
 *
 *   void job_start(struct poor_loop *loop, struct job *job)
 *   {
 *           poor_loop_group_init(&job->io);
 *           poor_loop_op_init(&job->read, read_done);
 *           auto sqe = poor_loop_group_get_sqe(loop, &job->io, &job->read);
 *           io_uring_prep_read_array(sqe, job->fd, job->buf, 0);
 *   }
 */
struct poor_loop_group {
	unsigned pending;
};

#define POOR_LOOP_GROUP_INIT { .group = {} }

/*
 * Declare a group type. It is only a type, so it can go in a header:
 *
 *   poor_loop_group_declare(job_io);
 *
 * You need to define attach lines, callbacks and POOR_LOOP_GROUP_DONE() in a single file
 */
#define poor_loop_group_declare(name) h_group_declare(name)

/*
 * Attach the op or timer 'member' of 'type' to the group at 'path' in the same
 * type. 'path' can also be a pointer to the group, for allocated ops:
 *
 *   struct job { job_io io; struct poor_loop_op read; };
 *   poor_loop_group_attach(job_io, struct job, io, read, read_done);
 *
 *   struct write { struct poor_loop_op op; job_io *io; };
 *   poor_loop_group_attach(job_io, struct write, io, op, write_done);
 *
 * This declares the static callback 'fn'. Define it with POOR_LOOP_GROUP_OP()
 * or POOR_LOOP_GROUP_TIMER(), and initialize the op or timer with it as usual.
 */
#define poor_loop_group_attach(name, type, path, member, fn) h_group_attach(name, type, path, member, fn)

/*
 * Define op callback to use with the group.
 */
#define POOR_LOOP_GROUP_OP(fn, loop_arg, group_arg, op_arg, cqe_arg) h_group_define_op(fn, loop_arg, group_arg, op_arg, cqe_arg)

/*
 * Define timer callback to use with the group.
 */
#define POOR_LOOP_GROUP_TIMER(fn, loop_arg, group_arg, timer_arg) h_group_define_timer(fn, loop_arg, group_arg, timer_arg)

/*
 * Define the done function for the group.
 *
 * When it is called it is safe to free group's memory.
 */
#define POOR_LOOP_GROUP_DONE(name, loop_arg, group_arg) h_group_define_done(name, loop_arg, group_arg)

#define poor_loop_group_init(g) h_group_init(&(g)->group)
#define poor_loop_group_acquire(g) h_group_add(&(g)->group)

/* Drop a hold. Returns true if this drops the counter to zero; 'done' is not called. */
#define poor_loop_group_release(g) h_group_release(&(g)->group)

#define poor_loop_group_get_sqe(loop, g, op) h_group_count(&(g)->group, poor_loop_get_sqe(loop, op))
#define poor_loop_group_get_sqe_or_submit(loop, g, op) h_group_count(&(g)->group, poor_loop_get_sqe_or_submit(loop, op))
#define poor_loop_group_timer_arm(loop, g, timer, deadline) h_group_timer_arm(loop, &(g)->group, timer, deadline)

/*
 * Disarm an attached timer right away. Returns true if this drops the counter
 * to zero; 'done' is not called.
 */
#define poor_loop_group_timer_disarm(g, timer) h_group_timer_disarm(&(g)->group, timer)

/*
 * Call 'done' yourself after poor_loop_group_release() or poor_loop_group_timer_disarm() returned true.
 * Like the callbacks, it only works in the file with POOR_LOOP_GROUP_DONE(): export a wrapper for other files.
 */
#define poor_loop_group_call_done(name, loop, g) h_group_call_done_##name(loop, g)

/* Implementation details, subject to change. */

#define h_group_declare(name) \
	typedef struct name { \
		struct poor_loop_group group; \
	} name; \
	static_assert(sizeof(struct name) == sizeof(struct poor_loop_group))

#define h_group_attach(name, type, path, member, fn) \
	static_assert(h_group_is_ref(&((type *)nullptr)->path, name), \
		      #path " must be an unqualified " #name " or a pointer to one"); \
	static_assert(h_group_is(&((type *)nullptr)->member, struct poor_loop_op) || \
			      h_group_is(&((type *)nullptr)->member, struct poor_loop_timer), \
		      #member " must be a struct poor_loop_op or struct poor_loop_timer"); \
	h_group_declare_functions(name) \
	struct h_group_callback_##fn { \
		struct name *group; \
		h_group_kind(type, member) h_kind; \
	}; \
	static inline struct name *h_group_of_##fn(h_group_kind(type, member) h_ptr) \
	{ \
		return h_group_ref(name, ((type *)(void *)((char *)h_ptr - offsetof(type, member)))->path); \
	} \
	static inline void h_group_done_##fn(struct poor_loop *h_loop, struct name *h_group) \
	{ \
		h_group_release_##name(h_loop, h_group); \
	} \
	static h_group_fn(type, member) fn

#define h_group_define_op(fn, loop_arg, group_arg, op_arg, cqe_arg) \
	h_group_check_callback(fn, struct poor_loop_op); \
	static void fn##_body(struct poor_loop *, h_group_type(fn) *, struct poor_loop_op *, const struct io_uring_cqe *); \
	static void fn(struct poor_loop *h_loop, struct poor_loop_op *h_op, const struct io_uring_cqe *h_cqe) \
	{ \
		h_group_type(fn) *h_group = h_group_of_##fn(h_op); \
		bool h_last = !(h_cqe->flags & IORING_CQE_F_MORE); \
\
		fn##_body(h_loop, h_group, h_op, h_cqe); \
		if (h_last) \
			h_group_done_##fn(h_loop, h_group); \
	} \
	static void fn##_body([[maybe_unused]] struct poor_loop *loop_arg, [[maybe_unused]] h_group_type(fn) *group_arg, \
			      [[maybe_unused]] struct poor_loop_op *op_arg, [[maybe_unused]] const struct io_uring_cqe *cqe_arg)

#define h_group_define_timer(fn, loop_arg, group_arg, timer_arg) \
	h_group_check_callback(fn, struct poor_loop_timer); \
	static void fn##_body(struct poor_loop *, h_group_type(fn) *, struct poor_loop_timer *); \
	static void fn(struct poor_loop *h_loop, struct poor_loop_timer *h_timer) \
	{ \
		h_group_type(fn) *h_group = h_group_of_##fn(h_timer); \
\
		fn##_body(h_loop, h_group, h_timer); \
		h_group_done_##fn(h_loop, h_group); \
	} \
	static void fn##_body([[maybe_unused]] struct poor_loop *loop_arg, [[maybe_unused]] h_group_type(fn) *group_arg, \
			      [[maybe_unused]] struct poor_loop_timer *timer_arg)

#define h_group_define_done(name, loop_arg, group_arg) \
	h_group_declare_functions(name) \
	[[maybe_unused]] static inline void h_group_release_##name(struct poor_loop *h_loop, struct name *h_group) \
	{ \
		if (h_group_release(&h_group->group)) \
			h_group_complete_##name(h_loop, h_group); \
	} \
	[[maybe_unused]] static inline void h_group_call_done_##name(struct poor_loop *h_loop, struct name *h_group) \
	{ \
		assert(!h_group->group.pending); \
		h_group_complete_##name(h_loop, h_group); \
	} \
	[[maybe_unused]] static void h_group_complete_##name([[maybe_unused]] struct poor_loop *loop_arg, \
							     [[maybe_unused]] struct name *group_arg)

#define h_group_is(ptr, type) _Generic((typeof(ptr))nullptr, type *: true, default: false)
#define h_group_is_ref(ptr, name) _Generic((typeof(ptr))nullptr, struct name *: true, struct name **: true, default: false)
#define h_group_ref(name, x) _Generic(&(x), struct name *: &(x), struct name **: (x), default: (struct name *)nullptr)
#define h_group_kind(type, member) \
	typeof(_Generic(&((type *)nullptr)->member, struct poor_loop_timer *: (struct poor_loop_timer *)nullptr, \
			default: (struct poor_loop_op *)nullptr))
#define h_group_fn(type, member) \
	typeof(*_Generic(&((type *)nullptr)->member, struct poor_loop_timer *: (poor_loop_timer_fn *)nullptr, \
			 default: (poor_loop_complete_fn *)nullptr))
#define h_group_callback(fn) ((struct h_group_callback_##fn *)nullptr)
#define h_group_type(fn) typeof_unqual(*h_group_callback(fn)->group)
#define h_group_check_callback(fn, type) \
	static_assert(h_group_is(h_group_callback(fn)->h_kind, type), #fn " must be attached to a " #type)
#define h_group_declare_functions(name) \
	static void h_group_complete_##name(struct poor_loop *, struct name *); \
	static void h_group_release_##name(struct poor_loop *, struct name *); \
	static void h_group_call_done_##name(struct poor_loop *, struct name *);

static inline void h_group_init(struct poor_loop_group *group)
{
	group->pending = 0;
}

static inline void h_group_add(struct poor_loop_group *group)
{
	assert(group->pending < UINT_MAX);
	group->pending++;
}

[[nodiscard]] static inline bool h_group_release(struct poor_loop_group *group)
{
	assert(group->pending);
	return !--group->pending;
}

[[nodiscard]] static inline struct io_uring_sqe *h_group_count(struct poor_loop_group *group, struct io_uring_sqe *sqe)
{
	if (sqe)
		h_group_add(group);
	return sqe;
}

static inline void h_group_timer_arm(struct poor_loop *loop, struct poor_loop_group *group, struct poor_loop_timer *timer,
				     uint64_t deadline)
{
	if (!poor_loop_timer_armed(timer))
		h_group_add(group);
	poor_loop_timer_arm(loop, timer, deadline);
}

[[nodiscard]] static inline bool h_group_timer_disarm(struct poor_loop_group *group, struct poor_loop_timer *timer)
{
	if (!poor_loop_timer_armed(timer))
		return false;
	poor_loop_timer_disarm(timer);
	return h_group_release(group);
}

#ifdef __cplusplus
}
#endif

#endif
