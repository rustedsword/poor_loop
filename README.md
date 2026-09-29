# poor_loop

Minimal completion-callback event loop on top of liburing.

## Build

Requires Linux, a C23 compiler, Meson 1.4, liburing 2.3 and
[poor_base](https://github.com/rustedsword/poor_base) 0.1 or newer. If
poor_base is not installed, the `subprojects/poor_base` submodule is used.

```sh
meson setup build
meson compile -C build
```

## Example

```c
#define _GNU_SOURCE

#include <poor_loop.h>
#include <stdio.h>

static char buf[4096];

static void on_read(struct poor_loop *loop, struct poor_loop_op *op,
		    const struct io_uring_cqe *cqe)
{
	if (cqe->res <= 0) {
		poor_loop_stop(loop);
		return;
	}
	fwrite(buf, 1, cqe->res, stdout);
	io_uring_prep_read(poor_loop_get_sqe_or_submit(loop, op), 0, buf, sizeof(buf), -1);
}

int main(void)
{
	struct poor_loop_op op = POOR_LOOP_OP_INIT(on_read);
	struct io_uring_params params = {};
	struct poor_loop loop;
	int ret;

	if (poor_loop_init(&loop, 64, &params))
		return 1;
	io_uring_prep_read(poor_loop_get_sqe_or_submit(&loop, &op), 0, buf, sizeof(buf), -1);
	ret = poor_loop_run(&loop);
	poor_loop_exit(&loop);
	return ret != 0;
}
```
