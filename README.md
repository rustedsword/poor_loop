# chioloop

Minimal completion-callback event loop on top of liburing.

## Build

Requires Linux, a C23 compiler, Meson 1.4 and liburing 2.3 or newer.

```sh
meson setup build
meson compile -C build
meson test -C build
```

Every test runs against four ring setups: `default`, `coop`
(`COOP_TASKRUN`), `defer` (`SINGLE_ISSUER | DEFER_TASKRUN`) and `sqpoll`;
select one with e.g. `meson test -C build --suite sqpoll`.

## Usage

```c
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
	io_uring_prep_read(chio_get_sqe(loop, op), 0, buf, sizeof(buf), -1);
}

int main(void)
{
	struct chio_op op = CHIO_OP_INIT(on_read);
	struct io_uring_params params = {};
	struct chio_loop loop;

	if (chio_loop_init(&loop, 64, &params))
		return 1;
	io_uring_prep_read(chio_get_sqe(&loop, &op), 0, buf, sizeof(buf), -1);
	chio_loop_run(&loop);
	chio_loop_exit(&loop);
	return 0;
}
```

## Semantics

- `struct chio_op` is two words: the `complete` callback, the `pending`
  flag and 7 bytes of `data` that the loop never touches. Embed it in your
  own state, initialize it with `CHIO_OP_INIT(fn)` or `chio_op_init(op, fn)`
  and use `container_of` in the callback.
- `chio_get_sqe()` binds a fresh SQE to the op and marks it pending. Prepare
  the SQE with any liburing helper, but keep `user_data` intact.
- `pending` is cleared right before `complete` runs for the final CQE, the
  one without `IORING_CQE_F_MORE`, so the callback may re-arm or free the op.
  Multishot requests and zero-copy notifications keep the op pending.
- An op has at most one request in flight: `chio_get_sqe()` on a pending op
  fails with `EBUSY`. When the SQ is full it submits queued SQEs first, so
  reserve space with `io_uring_sq_space_left()` before building a linked
  chain. On failure it returns `nullptr` and sets `errno`; `EAGAIN` and
  `ENOMEM` are transient.
- `chio_loop_run_once(loop, wait)` submits, waits for a CQE if `wait` is set,
  then dispatches the CQEs that were ready. It returns the number of CQEs
  reaped or `-errno`. `EINTR` is not an error, and neither is a submission
  that ran out of memory (`EAGAIN`, `ENOMEM`): the SQEs stay queued.
- `chio_loop_run()` runs until `chio_loop_stop()` is called. A stop requested
  outside of `chio_loop_run()` ends the next run.
- To cancel, target the op itself:
  `io_uring_prep_cancel(chio_get_sqe(loop, &cancel), &op, 0)`. The op stays
  pending until its own final CQE, usually `-ECANCELED`.
- Callbacks must not call `chio_loop_run*()` or `chio_loop_exit()`.
- CQEs with zero `user_data` are ignored, which leaves room for raw liburing
  SQEs. Don't set `IOSQE_CQE_SKIP_SUCCESS` on SQEs that belong to an op.
- `chio_loop_exit()` abandons pending ops without calling them.
- A loop is not thread-safe; use one per thread.
