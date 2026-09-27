/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <chioloop.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


constexpr uint64_t NS_PER_US = 1'000;
constexpr uint64_t NS_PER_MS = 1'000'000;
constexpr uint64_t NS_PER_SEC = 1'000'000'000;

struct stats {
	int64_t min, p50, p99, max;
};

static int cmp_i64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;

	return x < y ? -1 : x > y;
}

static struct stats summarize(unsigned count, int64_t (*values)[count])
{
	qsort(*values, count, sizeof(**values), cmp_i64);
	return (struct stats){
		.min = (*values)[0],
		.p50 = (*values)[count / 2],
		.p99 = (*values)[count * 99 / 100],
		.max = (*values)[count - 1],
	};
}

static void print_stats(const char *what, struct stats s)
{
	printf("  %-26s: min=%" PRId64 "us p50=%" PRId64 "us p99=%" PRId64
	       "us max=%" PRId64 "us\n", what, s.min / 1000, s.p50 / 1000,
	       s.p99 / 1000, s.max / 1000);
}

static void nothing(struct chio_loop *, struct chio_timer *)
{
}

static void run(struct chio_loop *loop)
{
	int ret = chio_loop_run(loop);

	if (ret) {
		fprintf(stderr, "chio_loop_run: %s\n", strerror(-ret));
		exit(1);
	}
}

static uint64_t arm_cost(struct chio_loop *loop, struct chio_timer *spare,
			 uint64_t deadline, unsigned rounds)
{
	uint64_t start = chio_now();

	for (unsigned i = 0; i < rounds; i++) {
		chio_timer_arm(loop, spare, deadline);
		chio_timer_disarm(spare);
	}
	return (chio_now() - start) / rounds;
}

static void bench_insert(struct chio_loop *loop, unsigned max_timers,
			 unsigned rounds)
{
	struct chio_timer *timers = calloc(max_timers, sizeof(*timers));
	struct chio_timer spare = CHIO_TIMER_INIT(nothing);

	puts("arm cost, nanoseconds per chio_timer_arm:");
	for (unsigned count = 1; count <= max_timers; count *= 8) {
		uint64_t base = chio_now() + 10 * NS_PER_SEC;
		uint64_t tail, head, median;

		for (unsigned i = 0; i < count; i++) {
			chio_timer_init(&timers[i], nothing);
			chio_timer_arm(loop, &timers[i], base + i * NS_PER_MS);
		}
		tail = arm_cost(loop, &spare, base + count * NS_PER_MS, rounds);
		head = arm_cost(loop, &spare, 1, rounds);
		median = arm_cost(loop, &spare,
				  base + count / 2 * NS_PER_MS + 1, rounds);
		printf("  armed=%-4u tail=%" PRIu64 "ns head=%" PRIu64
		       "ns median=%" PRIu64 "ns\n", count, tail, head, median);
		for (unsigned i = 0; i < count; i++)
			chio_timer_disarm(&timers[i]);
	}
	free(timers);
}

struct probe {
	struct chio_timer timer;
	uint64_t fired;
};

static void probe_fire(struct chio_loop *loop, struct chio_timer *timer)
{
	chio_container_of(timer, struct probe, timer)->fired = chio_now();
	chio_loop_stop(loop);
}

static void bench_loop_timer(struct chio_loop *loop, uint64_t delay,
			     unsigned count, int64_t (*errors)[count])
{
	for (unsigned i = 0; i < count; i++) {
		struct probe probe = { .timer = CHIO_TIMER_INIT(probe_fire) };
		uint64_t deadline = chio_now() + delay;

		chio_timer_arm(loop, &probe.timer, deadline);
		run(loop);
		(*errors)[i] = (int64_t)(probe.fired - deadline);
	}
}

static void bench_uring_abs(struct io_uring *ring, uint64_t delay,
			    unsigned count, int64_t (*errors)[count])
{
	struct io_uring_cqe *cqe;

	for (unsigned i = 0; i < count; i++) {
		uint64_t deadline = chio_now() + delay;
		struct __kernel_timespec ts = {
			.tv_sec = deadline / NS_PER_SEC,
			.tv_nsec = deadline % NS_PER_SEC,
		};

		io_uring_prep_timeout(io_uring_get_sqe(ring), &ts, 0,
				      IORING_TIMEOUT_ABS);
		io_uring_submit_and_wait(ring, 1);
		(*errors)[i] = (int64_t)(chio_now() - deadline);
		if (!io_uring_peek_cqe(ring, &cqe))
			io_uring_cqe_seen(ring, cqe);
	}
}

static void bench_nanosleep(uint64_t delay, unsigned count,
			    int64_t (*errors)[count])
{
	for (unsigned i = 0; i < count; i++) {
		uint64_t deadline = chio_now() + delay;
		struct timespec ts = {
			.tv_sec = deadline / NS_PER_SEC,
			.tv_nsec = deadline % NS_PER_SEC,
		};

		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
		(*errors)[i] = (int64_t)(chio_now() - deadline);
	}
}

static void bench_latency(struct chio_loop *loop, uint64_t delay,
			  unsigned samples)
{
	int64_t (*errors)[samples] = calloc(1, sizeof(*errors));
	struct io_uring ring;

	io_uring_queue_init(64, &ring, 0);
	printf("firing error at a %" PRIu64 "us deadline, n=%u:\n",
	       delay / NS_PER_US, samples);
	bench_loop_timer(loop, delay, samples, errors);
	print_stats("chio_timer (wait timeout)", summarize(samples, errors));
	bench_uring_abs(&ring, delay, samples, errors);
	print_stats("IORING_TIMEOUT_ABS", summarize(samples, errors));
	bench_nanosleep(delay, samples, errors);
	print_stats("clock_nanosleep ABS", summarize(samples, errors));
	io_uring_queue_exit(&ring);
	free(errors);
}

struct ordered {
	struct chio_timer timer;
	unsigned id;
};

static unsigned fired, order;

static void note_order(struct chio_loop *loop, struct chio_timer *timer)
{
	struct ordered *ordered = chio_container_of(timer, struct ordered,
						    timer);

	order = order * 10 + ordered->id;
	if (++fired == 3)
		chio_loop_stop(loop);
}

static bool check_order(struct chio_loop *loop)
{
	struct ordered timers[] = {
		{ .timer = CHIO_TIMER_INIT(note_order), .id = 3 },
		{ .timer = CHIO_TIMER_INIT(note_order), .id = 1 },
		{ .timer = CHIO_TIMER_INIT(note_order), .id = 2 },
	};
	uint64_t base = chio_now();
	uint64_t deadlines[] = {
		base + 30 * NS_PER_MS, base, base + 15 * NS_PER_MS,
	};

	fired = order = 0;
	for (unsigned i = 0; i < 3; i++)
		chio_timer_arm(loop, &timers[i].timer, deadlines[i]);
	chio_timer_arm(loop, &timers[1].timer, base + 5 * NS_PER_MS);
	run(loop);
	printf("deadline order: fired %u%s\n", order,
	       order == 123 ? " (ok)" : " (WRONG)");
	return order == 123;
}

int main(int argc, char **argv)
{
	unsigned max_timers = 512, rounds = 20'000, samples = 2'000;
	struct io_uring_params params = {};
	const char *what = "all";
	struct chio_loop loop;
	bool ok = true;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--max-timers") && i + 1 < argc)
			max_timers = strtoul(argv[++i], nullptr, 10);
		else if (!strcmp(argv[i], "--rounds") && i + 1 < argc)
			rounds = strtoul(argv[++i], nullptr, 10);
		else if (!strcmp(argv[i], "--samples") && i + 1 < argc)
			samples = strtoul(argv[++i], nullptr, 10);
		else
			what = argv[i];
	}
	setvbuf(stdout, nullptr, _IONBF, 0);
	if (chio_loop_init(&loop, 64, &params))
		return 1;
	if (!strcmp(what, "all") || !strcmp(what, "insert")) {
		ok = check_order(&loop);
		bench_insert(&loop, max_timers, rounds);
	}
	if (!strcmp(what, "all") || !strcmp(what, "latency")) {
		bench_latency(&loop, 100 * NS_PER_US, samples);
		bench_latency(&loop, NS_PER_MS, samples);
		bench_latency(&loop, 16 * NS_PER_MS, samples / 8 + 1);
	}
	chio_loop_exit(&loop);
	return !ok;
}
