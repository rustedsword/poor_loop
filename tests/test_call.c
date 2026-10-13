/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include "harness.h"

struct job {
	struct poor_loop_call call;
	int id;
	int runs;
	bool queued;
};

static void job_run(struct poor_loop *, struct poor_loop_call *call)
{
	struct job *job = container_of(call, struct job, call);

	job->runs++;
	job->queued = poor_loop_call_queued(call);
	tick_order = tick_order * 10 + job->id;
}

static void job_init(struct job *job, poor_loop_call_fn *fn, int id)
{
	*job = (struct job){ .call = POOR_LOOP_CALL_INIT(fn), .id = id };
}

static void stop_run(struct poor_loop *loop, struct poor_loop_call *call)
{
	job_run(loop, call);
	poor_loop_stop(loop);
}

static void requeue_run(struct poor_loop *loop, struct poor_loop_call *call)
{
	job_run(loop, call);
	poor_loop_call_queue(loop, call);
}

static int test_call_init(void)
{
	struct poor_loop_call lit = POOR_LOOP_CALL_INIT(job_run), call;

	CHECK(lit.fn == job_run);
	CHECK(!poor_loop_call_queued(&lit));
	memset(&call, 0xa5, sizeof(call));
	poor_loop_call_init(&call, job_run);
	CHECK(call.fn == job_run);
	CHECK(!poor_loop_call_queued(&call));
	return 0;
}

static int test_call(void)
{
	struct poor_loop loop;
	struct rec reader;
	struct tick tick;
	struct job job;
	char buf[8];
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	rec_init(&reader, rec_complete);
	tick_init(&tick, tick_fire, 1);
	job_init(&job, stop_run, 2);
	arm_read(&loop, &reader.op, fds[0], buf);
	poor_loop_timer_arm(&loop, &tick.timer, poor_loop_now() + 10'000'000'000);
	poor_loop_call_queue(&loop, &job.call);
	CHECK(poor_loop_call_queued(&job.call));
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(job.runs, 1);
	CHECK(!job.queued);
	CHECK(!poor_loop_call_queued(&job.call));
	CHECK_EQ(tick.fired, 0);
	CHECK_EQ(reader.calls, 0);
	CHECK(reader.op.pending);

	CHECK_EQ(write(fds[1], "x", 1), 1);
	drain(&loop, &reader.op);
	CHECK_EQ(reader.res, 1);
	CHECK_EQ(job.runs, 1);
	poor_loop_timer_disarm(&tick.timer);
	poor_loop_exit(&loop);
	close_pipe(&fds);
	return 0;
}

static int test_call_order(void)
{
	struct poor_loop loop;
	struct job jobs[3];

	loop_init(&loop, 8);
	foreach_array_index(jobs, i) {
		job_init(&jobs[i], job_run, i + 1);
		poor_loop_call_queue(&loop, &jobs[i].call);
	}
	poor_loop_call_queue(&loop, &jobs[0].call);
	tick_order = 0;
	CHECK_EQ(poor_loop_run_once(&loop), 0);
	CHECK_EQ(tick_order, 123);
	foreach_array_ref(jobs, job)
		CHECK_EQ(job->runs, 1);
	CHECK(!loop.calls);
	poor_loop_exit(&loop);
	return 0;
}

struct pair {
	struct job first;
	struct job second;
};

static void cancel_second(struct poor_loop *loop, struct poor_loop_call *call)
{
	struct pair *pair = container_of(call, struct pair, first.call);

	job_run(loop, call);
	poor_loop_call_cancel(loop, &pair->second.call);
}

static void queue_second(struct poor_loop *loop, struct poor_loop_call *call)
{
	struct pair *pair = container_of(call, struct pair, first.call);

	job_run(loop, call);
	poor_loop_call_queue(loop, &pair->second.call);
}

static void requeue_second(struct poor_loop *loop, struct poor_loop_call *call)
{
	struct pair *pair = container_of(call, struct pair, first.call);

	job_run(loop, call);
	poor_loop_call_cancel(loop, &pair->second.call);
	poor_loop_call_queue(loop, &pair->second.call);
}

static int test_call_cancel(void)
{
	struct poor_loop loop;
	struct job jobs[6];
	struct pair pair;

	loop_init(&loop, 8);
	foreach_array_index(jobs, i)
		job_init(&jobs[i], job_run, i + 1);
	poor_loop_call_queue(&loop, &jobs[0].call);
	poor_loop_call_cancel(&loop, &jobs[0].call);
	CHECK(!poor_loop_call_queued(&jobs[0].call));
	CHECK(!loop.calls);
	poor_loop_call_cancel(&loop, &jobs[0].call);
	for (unsigned i = 0; i < 5; i++)
		poor_loop_call_queue(&loop, &jobs[i].call);
	poor_loop_call_cancel(&loop, &jobs[0].call);
	poor_loop_call_cancel(&loop, &jobs[2].call);
	poor_loop_call_cancel(&loop, &jobs[4].call);
	poor_loop_call_queue(&loop, &jobs[5].call);
	tick_order = 0;
	CHECK_EQ(poor_loop_run_once(&loop), 0);
	CHECK_EQ(tick_order, 246);
	CHECK(!loop.calls);

	job_init(&pair.first, cancel_second, 7);
	job_init(&pair.second, job_run, 8);
	poor_loop_call_queue(&loop, &pair.first.call);
	poor_loop_call_queue(&loop, &pair.second.call);
	CHECK_EQ(poor_loop_run_once(&loop), 0);
	CHECK_EQ(pair.first.runs, 1);
	CHECK_EQ(pair.second.runs, 0);
	CHECK(!poor_loop_call_queued(&pair.second.call));
	CHECK(!loop.calls);

	poor_loop_call_queue(&loop, &pair.first.call);
	poor_loop_call_queue(&loop, &jobs[0].call);
	poor_loop_call_queue(&loop, &pair.second.call);
	tick_order = 0;
	CHECK_EQ(poor_loop_run_once(&loop), 0);
	CHECK_EQ(tick_order, 71);
	CHECK_EQ(pair.second.runs, 0);
	CHECK(!poor_loop_call_queued(&pair.second.call));
	CHECK(!loop.calls);
	poor_loop_exit(&loop);
	return 0;
}

static int test_call_cancel_requeue(void)
{
	struct poor_loop loop;
	struct pair pair;
	struct job job;

	loop_init(&loop, 8);
	job_init(&pair.first, requeue_second, 1);
	job_init(&pair.second, job_run, 2);
	job_init(&job, job_run, 3);
	poor_loop_call_queue(&loop, &pair.first.call);
	poor_loop_call_queue(&loop, &pair.second.call);
	poor_loop_call_queue(&loop, &job.call);
	tick_order = 0;
	CHECK_EQ(poor_loop_run_once(&loop), 0);
	CHECK_EQ(tick_order, 13);
	CHECK(poor_loop_call_queued(&pair.second.call));
	CHECK_EQ(poor_loop_run_once(&loop), 0);
	CHECK_EQ(tick_order, 132);
	CHECK(!loop.calls);
	poor_loop_exit(&loop);
	return 0;
}

static int test_call_requeue(void)
{
	struct poor_loop loop;
	struct pair pair;
	struct job job;

	loop_init(&loop, 8);
	job_init(&job, requeue_run, 1);
	job_init(&pair.first, queue_second, 2);
	job_init(&pair.second, job_run, 3);
	poor_loop_call_queue(&loop, &job.call);
	poor_loop_call_queue(&loop, &pair.first.call);
	tick_order = 0;
	CHECK_EQ(poor_loop_run_once(&loop), 0);
	CHECK_EQ(tick_order, 12);
	CHECK(poor_loop_call_queued(&job.call));
	CHECK(poor_loop_call_queued(&pair.second.call));
	CHECK_EQ(poor_loop_run_once(&loop), 0);
	CHECK_EQ(tick_order, 1213);
	CHECK_EQ(poor_loop_run_once(&loop), 0);
	CHECK_EQ(tick_order, 12131);
	CHECK_EQ(job.runs, 3);
	CHECK(!job.queued);
	CHECK(poor_loop_call_queued(&job.call));
	poor_loop_call_cancel(&loop, &job.call);
	poor_loop_exit(&loop);
	return 0;
}

struct sequence {
	struct rec rec;
	struct tick tick;
	struct job job;
};

static void sequence_complete(struct poor_loop *loop, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	struct sequence *seq = container_of(op, struct sequence, rec.op);

	rec_complete(loop, op, cqe);
	tick_order = tick_order * 10 + 1;
	poor_loop_call_queue(loop, &seq->job.call);
	poor_loop_timer_arm(loop, &seq->tick.timer, 0);
}

static int test_call_after_events(void)
{
	struct sequence seq;
	struct poor_loop loop;

	loop_init(&loop, 8);
	rec_init(&seq.rec, sequence_complete);
	tick_init(&seq.tick, tick_fire, 2);
	job_init(&seq.job, job_run, 3);
	arm_nop(&loop, &seq.rec.op);
	tick_order = 0;
	drain(&loop, &seq.rec.op);
	CHECK_EQ(tick_order, 123);
	CHECK_EQ(seq.rec.calls, 1);
	poor_loop_exit(&loop);
	return 0;
}

struct spin {
	struct job job;
	struct rec reader;
	int fd;
};

static void spin_run(struct poor_loop *loop, struct poor_loop_call *call)
{
	struct spin *spin = container_of(call, struct spin, job.call);

	requeue_run(loop, call);
	if (spin->job.runs == 1)
		CHECK_EQ(write(spin->fd, "x", 1), 1);
}

static int test_call_spin(void)
{
	struct poor_loop loop;
	struct spin spin;
	char buf[8];
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	job_init(&spin.job, spin_run, 1);
	rec_init(&spin.reader, stop_complete);
	spin.fd = fds[1];
	arm_read(&loop, &spin.reader.op, fds[0], buf);
	poor_loop_call_queue(&loop, &spin.job.call);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(spin.reader.calls, 1);
	CHECK_EQ(spin.reader.res, 1);
	CHECK(spin.job.runs >= 1);
	CHECK(poor_loop_call_queued(&spin.job.call));
	poor_loop_call_cancel(&loop, &spin.job.call);
	poor_loop_exit(&loop);
	close_pipe(&fds);
	return 0;
}

static void free_run(struct poor_loop *loop, struct poor_loop_call *call)
{
	job_run(loop, call);
	free(container_of(call, struct job, call));
}

static int test_call_free(void)
{
	struct poor_loop loop;
	struct job *jobs[2];

	loop_init(&loop, 8);
	foreach_array_index(jobs, i) {
		jobs[i] = malloc(sizeof(*jobs[i]));
		CHECK(jobs[i]);
		job_init(jobs[i], free_run, i + 1);
		poor_loop_call_queue(&loop, &jobs[i]->call);
	}
	tick_order = 0;
	CHECK_EQ(poor_loop_run_once(&loop), 0);
	CHECK_EQ(tick_order, 12);
	CHECK(!loop.calls);
	poor_loop_exit(&loop);
	return 0;
}

static const struct test tests[] = {
	TEST(call_init),
	TEST(call),
	TEST(call_order),
	TEST(call_cancel),
	TEST(call_cancel_requeue),
	TEST(call_requeue),
	TEST(call_after_events),
	TEST(call_spin),
	TEST(call_free),
};

int main(int argc, char **argv)
{
	return run_tests(array_ptr(argv, argc), tests);
}
