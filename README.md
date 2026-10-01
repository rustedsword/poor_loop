# poor_loop

Minimal event loop on top of liburing: completion callbacks plus software
timers. It does not hide the ring. Requests are prepared with plain liburing
helpers, and `poor_loop_ring()` exposes the ring for anything else, such as
registered files and buffers.

## Build

Requires Linux, a C23 compiler, Meson 1.4+, liburing 2.3+ and
[poor_base](https://github.com/rustedsword/poor_base).

```sh
meson setup build
meson compile -C build
```

Pass `-Dtests=true` to build the tests and run them with `meson test -C build`,
and `-Dexamples=true` to build the examples.

## Example

```c
#define _GNU_SOURCE

#include <poor_loop.h>
#include <unistd.h>

static char buf[4096];
static unsigned len, written;
static bool failed;

static void on_io(struct poor_loop *loop, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	if (cqe->res <= 0) {
		failed = cqe->res < 0 || written < len;
		poor_loop_stop(loop);
		return;
	}
	if (written < len)
		written += cqe->res;
	else {
		len = cqe->res;
		written = 0;
	}
	if (written < len) {
		auto unsent = arrview_cfront(written, arrview_first(len, buf));
		io_uring_prep_write_array(poor_loop_get_sqe(loop, op), STDOUT_FILENO, unsent, -1);
	} else {
		io_uring_prep_read_array(poor_loop_get_sqe(loop, op), STDIN_FILENO, buf, -1);
	}
}

int main(void)
{
	struct poor_loop_op op = POOR_LOOP_OP_INIT(on_io);
	struct io_uring_params params = {};
	struct poor_loop loop;
	int ret;

	if (poor_loop_init(&loop, 8, &params))
		return 1;
	io_uring_prep_read_array(poor_loop_get_sqe(&loop, &op), STDIN_FILENO, buf, -1);
	ret = poor_loop_run(&loop);
	poor_loop_exit(&loop);
	return ret || failed;
}
```
