/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <poor_array.h>
#include <poor_loop.h>
#include <poor_stdio.h>
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
	qsort(*values, count, ARRAY_ELEMENT_SIZE(values), cmp_i64);
	return (struct stats){
		.min = *array_first_ref(values),
		.p50 = arr(values)[count / 2],
		.p99 = arr(values)[count * 99 / 100],
		.max = *array_last_ref(values),
	};
}

static void print_stats(const char *what, struct stats s)
{
	println("  ", fmt_w(what, -26), ": min=", s.min / 1000, "us p50=", s.p50 / 1000, "us p99=", s.p99 / 1000,
		"us max=", s.max / 1000, "us");
}

static void nothing(struct poor_loop *, struct poor_loop_timer *)
{
}

static void run_once(struct poor_loop *loop)
{
	int ret = poor_loop_run_once(loop);

	if (ret) {
		printerrln("poor_loop_run_once: ", strerror(-ret));
		exit(1);
	}
}

static uint64_t arm_cost(struct poor_loop *loop, struct poor_loop_timer *spare, uint64_t deadline, unsigned rounds)
{
	uint64_t start = poor_loop_now();

	for (unsigned i = 0; i < rounds; i++) {
		poor_loop_timer_arm(loop, spare, deadline);
		poor_loop_timer_disarm(spare);
	}
	return (poor_loop_now() - start) / rounds;
}

static void bench_insert(struct poor_loop *loop, unsigned max_timers, unsigned rounds)
{
	struct poor_loop_timer(*timers)[max_timers] = calloc_array(timers);
	struct poor_loop_timer spare = POOR_LOOP_TIMER_INIT(nothing);

	println("arm cost, nanoseconds per poor_loop_timer_arm:");
	for (unsigned count = 1; count <= max_timers; count *= 8) {
		uint64_t base = poor_loop_now() + 10 * NS_PER_SEC;
		make_arrview_first(armed, count, timers);
		uint64_t tail, head, median;

		foreach_array_index(armed, i) {
			poor_loop_timer_init(&arr(armed)[i], nothing);
			poor_loop_timer_arm(loop, &arr(armed)[i], base + i * NS_PER_MS);
		}
		tail = arm_cost(loop, &spare, base + count * NS_PER_MS, rounds);
		head = arm_cost(loop, &spare, 1, rounds);
		median = arm_cost(loop, &spare, base + count / 2 * NS_PER_MS + 1, rounds);
		println("  armed=", fmt_w(count, -4), " tail=", tail, "ns head=", head, "ns median=", median, "ns");
		foreach_array_ref(armed, timer)
			poor_loop_timer_disarm(timer);
	}
	free(timers);
}

struct probe {
	struct poor_loop_timer timer;
	uint64_t fired;
};

static void probe_fire(struct poor_loop *, struct poor_loop_timer *timer)
{
	container_of(timer, struct probe, timer)->fired = poor_loop_now();
}

static void bench_loop_timer(struct poor_loop *loop, uint64_t delay, unsigned count, int64_t (*errors)[count])
{
	foreach_array_ref(errors, error) {
		struct probe probe = { .timer = POOR_LOOP_TIMER_INIT(probe_fire) };
		uint64_t deadline = poor_loop_now() + delay;

		poor_loop_timer_arm(loop, &probe.timer, deadline);
		while (poor_loop_timer_armed(&probe.timer))
			run_once(loop);
		*error = (int64_t)(probe.fired - deadline);
	}
}

static void bench_uring_abs(struct io_uring *ring, uint64_t delay, unsigned count, int64_t (*errors)[count])
{
	struct io_uring_cqe *cqe;

	foreach_array_ref(errors, error) {
		uint64_t deadline = poor_loop_now() + delay;
		struct __kernel_timespec ts = {
			.tv_sec = deadline / NS_PER_SEC,
			.tv_nsec = deadline % NS_PER_SEC,
		};

		io_uring_prep_timeout(io_uring_get_sqe(ring), &ts, 0, IORING_TIMEOUT_ABS);
		io_uring_submit_and_wait(ring, 1);
		*error = (int64_t)(poor_loop_now() - deadline);
		if (!io_uring_peek_cqe(ring, &cqe))
			io_uring_cqe_seen(ring, cqe);
	}
}

static void bench_nanosleep(uint64_t delay, unsigned count, int64_t (*errors)[count])
{
	foreach_array_ref(errors, error) {
		uint64_t deadline = poor_loop_now() + delay;
		struct timespec ts = {
			.tv_sec = deadline / NS_PER_SEC,
			.tv_nsec = deadline % NS_PER_SEC,
		};

		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
		*error = (int64_t)(poor_loop_now() - deadline);
	}
}

static void bench_latency(struct poor_loop *loop, uint64_t delay, unsigned samples)
{
	int64_t (*errors)[samples] = calloc_array(errors);
	struct io_uring ring;

	io_uring_queue_init(64, &ring, 0);
	println("firing error at a ", delay / NS_PER_US, "us deadline, n=", samples, ":");
	bench_loop_timer(loop, delay, samples, errors);
	print_stats("poor_loop_timer (wait timeout)", summarize(samples, errors));
	bench_uring_abs(&ring, delay, samples, errors);
	print_stats("IORING_TIMEOUT_ABS", summarize(samples, errors));
	bench_nanosleep(delay, samples, errors);
	print_stats("clock_nanosleep ABS", summarize(samples, errors));
	io_uring_queue_exit(&ring);
	free(errors);
}

struct ordered {
	struct poor_loop_timer timer;
	unsigned id;
};

static unsigned fired, order;

static void note_order(struct poor_loop *, struct poor_loop_timer *timer)
{
	struct ordered *ordered = container_of(timer, struct ordered, timer);

	order = order * 10 + ordered->id;
	fired++;
}

static bool check_order(struct poor_loop *loop)
{
	struct ordered timers[] = {
		{ .timer = POOR_LOOP_TIMER_INIT(note_order), .id = 3 },
		{ .timer = POOR_LOOP_TIMER_INIT(note_order), .id = 1 },
		{ .timer = POOR_LOOP_TIMER_INIT(note_order), .id = 2 },
	};
	uint64_t base = poor_loop_now();
	uint64_t deadlines[] = {
		base + 30 * NS_PER_MS,
		base,
		base + 15 * NS_PER_MS,
	};

	fired = order = 0;
	foreach_array_index(timers, i)
		poor_loop_timer_arm(loop, &timers[i].timer, deadlines[i]);
	poor_loop_timer_arm(loop, &timers[1].timer, base + 5 * NS_PER_MS);
	while (fired < ARRAY_SIZE(timers))
		run_once(loop);
	println("deadline order: fired ", order, order == 123 ? " (ok)" : " (WRONG)");
	return order == 123;
}

int main(int argc, char **argv)
{
	unsigned max_timers = 512, rounds = 20'000, samples = 2'000;
	struct io_uring_params params = {};
	const char *what = "all";
	struct poor_loop loop;
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
	if (poor_loop_init(&loop, 64, &params))
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
	poor_loop_exit(&loop);
	return !ok;
}
