# Draining the loop and cancelling requests

Before closing the ring, an application normally waits for all of its requests
to finish, so that the kernel no longer uses its buffers. The loop alone can't
tell when that happens: requests without an op, with zero `user_data` or
prepared directly on the ring, never go through it, and their CQEs are ignored.

## Waiting with IOSQE_IO_DRAIN

A request submitted with `IOSQE_IO_DRAIN` doesn't start until every request
submitted before it has completed. A NOP with this flag therefore completes
once everything submitted before it is done, whether the loop tracks it or not.
If nothing else was submitted while the NOP was pending, nothing is left in
flight and the ring can be closed.

## The problem with cancellation

The flag also works in the other direction: requests submitted after the NOP
don't start until the NOP completes, and cancellations are no exception. If a
request submitted before the NOP only finishes once it is cancelled, and the
cancellation is submitted after the NOP, neither of them ever completes.

So the NOP may only be submitted once every request in flight can finish
without anything else being submitted, which usually means after all
cancellations. An application can't always know that in advance, for example
when a completion leads to cancelling another request.

## Alternatives

- `IORING_REGISTER_SYNC_CANCEL` cancels without going through the SQ, so the
  NOP doesn't hold it back, but every cancellation becomes a separate syscall.
- The kernel doesn't report in-flight requests in any other way: fdinfo lists
  only poll-armed requests, and `IORING_REGISTER_QUERY` doesn't report them.
- Counting requests at the loop is totally stupid. And only kernel knows what
  is in flight anyway.
