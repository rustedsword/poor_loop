/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <poor_array.h>
#include <poor_loop.h>
#include <poor_loop_fd.h>
#include <poor_stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/uio.h>
#include <unistd.h>

/*
 * Print inotify events for DIR until the watch is removed, e.g. when DIR is
 * deleted. The inotify fd is an fd group with a read and a stdout write
 * attached. The next read starts only after the events are written, so a slow
 * stdout leaves them queued in the kernel instead of stalling the loop, and
 * the group closes the fd after the last write.
 *
 * A queue overflow can drop IN_IGNORED, so it ends the reads too, as a
 * failure. Errors go to stderr as untracked writes, which poor_loop_drain()
 * waits for.
 *
 * inotify has no FMODE_NOWAIT, so without IN_NONBLOCK io_uring can't read it
 * inline and hands every read to an io-wq worker.
 *
 * Usage: inotify_with_group_fd [DIR]
 */
constexpr uint32_t WATCH_MASK = IN_CREATE | IN_DELETE | IN_MOVE | IN_CLOSE_WRITE;
constexpr size_t BUF_SIZE = 4096;
constexpr uint64_t DRAIN_TIMEOUT_NS = 1'000'000'000;

poor_loop_fd_declare(watch_file);

struct watcher {
	watch_file file;
	struct poor_loop_op read_op, write_op;
	const char *path;
	struct iovec err[4];
	unsigned len, written;
	bool removed, failed;
	alignas(struct inotify_event) char buf[BUF_SIZE];
	char out[BUF_SIZE];
};

poor_loop_group_attach(watch_file, struct watcher, file, read_op, on_read);
poor_loop_group_attach(watch_file, struct watcher, file, write_op, on_write);

static const struct {
	uint32_t mask;
	const char *name;
} events[] = {
	{ IN_CREATE, "create" },     { IN_DELETE, "delete" },           { IN_MOVED_FROM, "moved_from" },
	{ IN_MOVED_TO, "moved_to" }, { IN_CLOSE_WRITE, "close_write" }, { IN_UNMOUNT, "unmount" },
	{ IN_IGNORED, "ignored" },
};

static struct watcher *watcher_of(watch_file *file)
{
	return container_of(file, struct watcher, file);
}

static const char *event_name(uint32_t mask)
{
	foreach_array_const_ref(events, e)
		if (mask & e->mask)
			return e->name;
	return "unknown";
}

/* Only the first error is reported, since the write of 'err' may still be in flight */
static void report(struct poor_loop *loop, struct watcher *w, const char *what, const char *msg)
{
	if (w->failed)
		return;
	w->failed = true;
	w->err[0] = (struct iovec){ (void *)what, strlen(what) };
	w->err[1] = (struct iovec){ ": ", 2 };
	w->err[2] = (struct iovec){ (void *)msg, strlen(msg) };
	w->err[3] = (struct iovec){ "\n", 1 };
	io_uring_prep_writev_array(poor_loop_get_untracked_sqe(loop), STDERR_FILENO, w->err, -1);
}

static void start_read(struct poor_loop *loop, struct watcher *w)
{
	io_uring_prep_read_array(poor_loop_group_get_sqe(loop, &w->file, &w->read_op), w->file.fd, w->buf, -1);
}

static void write_rest(struct poor_loop *loop, struct watcher *w)
{
	auto unwritten = arrview_cfront(w->written, arrview_first(w->len, w->out));

	io_uring_prep_write_array(poor_loop_group_get_sqe(loop, &w->file, &w->write_op), STDOUT_FILENO, unwritten, -1);
}

POOR_LOOP_GROUP_OP(on_read, loop, file, op, cqe)
{
	struct watcher *w = watcher_of(file);
	const struct inotify_event *ev;

	if (cqe->res < 0) {
		report(loop, w, "read", strerror(-cqe->res));
		return;
	}
	w->len = w->written = 0;
	for (int off = 0; off < cqe->res; off += sizeof(*ev) + ev->len) {
		ev = (const struct inotify_event *)&w->buf[off];
		if (ev->mask & IN_Q_OVERFLOW) {
			report(loop, w, w->path, "event queue overflow");
			break;
		}
		/* A line is shorter than the event it describes, so the lines of one read fit in 'out' */
		w->len += sprintln_array(arrview_cfront(w->len, w->out), event_name(ev->mask), ev->len ? ": " : "",
					 ev->len ? ev->name : "");
		assert(w->len < ARRAY_SIZE(w->out));
		w->removed |= ev->mask & IN_IGNORED;
	}
	if (w->len)
		write_rest(loop, w);
}

POOR_LOOP_GROUP_OP(on_write, loop, file, op, cqe)
{
	struct watcher *w = watcher_of(file);

	if (cqe->res < 0) {
		report(loop, w, "write", strerror(-cqe->res));
		return;
	}
	w->written += cqe->res;
	if (w->written < w->len)
		write_rest(loop, w);
	else if (!w->removed && !w->failed)
		start_read(loop, w);
}

POOR_LOOP_FD_DONE(watch_file, loop, file)
{
	poor_loop_stop(loop);
}

static void watch_start(struct poor_loop *loop, struct watcher *w)
{
	int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

	if (fd < 0) {
		report(loop, w, "inotify_init1", strerror(errno));
		poor_loop_stop(loop);
		return;
	}
	poor_loop_fd_init(&w->file, fd);
	if (inotify_add_watch(fd, w->path, WATCH_MASK) < 0) {
		report(loop, w, w->path, strerror(errno));
		poor_loop_group_call_done(watch_file, loop, &w->file);
		return;
	}
	start_read(loop, w);
}

int main(int argc, char **argv)
{
	struct watcher w = {
		.read_op = POOR_LOOP_OP_INIT(on_read),
		.write_op = POOR_LOOP_OP_INIT(on_write),
		.path = argc > 1 ? argv[1] : ".",
	};
	struct io_uring_params params = {};
	struct poor_loop loop;
	int ret;

	ret = poor_loop_init(&loop, 8, &params);
	if (ret) {
		printerrln("poor_loop_init: ", strerror(-ret));
		return 1;
	}
	watch_start(&loop, &w);
	ret = poor_loop_run(&loop);
	if (!ret)
		ret = poor_loop_drain(&loop, poor_loop_now() + DRAIN_TIMEOUT_NS);
	if (ret)
		printerrln("loop: ", strerror(-ret));
	poor_loop_exit(&loop);
	return ret || w.failed;
}
