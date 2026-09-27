# chioloop

Minimal completion-callback event loop on top of liburing.

## Build

Requires Linux, a C23 compiler, Meson 1.4 and liburing 2.3 or newer.

```sh
meson setup build
meson compile -C build
```

## Example

```c
#define _GNU_SOURCE

#include <chioloop.h>
#include <stdio.h>

static char buf[4096];

static void on_read(struct chio_loop *loop, struct chio_op *op,
		    const struct io_uring_cqe *cqe)
{
	if (cqe->res <= 0) {
		chio_loop_stop(loop);
		return;
	}
	fwrite(buf, 1, cqe->res, stdout);
	io_uring_prep_read(chio_get_sqe_or_submit(loop, op), 0, buf, sizeof(buf), -1);
}

int main(void)
{
	struct chio_op op = CHIO_OP_INIT(on_read);
	struct io_uring_params params = {};
	struct chio_loop loop;
	int ret;

	if (chio_loop_init(&loop, 64, &params))
		return 1;
	io_uring_prep_read(chio_get_sqe_or_submit(&loop, &op), 0, buf, sizeof(buf), -1);
	ret = chio_loop_run(&loop);
	chio_loop_exit(&loop);
	return ret != 0;
}
```
