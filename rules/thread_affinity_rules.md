# Thread Affinity Rules

- Each EventLoop owns the channels, timers, and active I/O callbacks bound to
  its loop thread.
- Cross-thread operations must be expressed through the loop wakeup/queue
  mechanism.
- Code that requires loop-thread execution should assert or document that
  precondition.
