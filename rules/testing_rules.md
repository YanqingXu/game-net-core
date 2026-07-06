# Testing Rules

- Unit tests cover isolated core components.
- Contract tests cover EventLoop, Channel, Poller, TimerQueue, and TCP lifecycle
  behavior expected by callers.
- Integration tests cover runnable networking flows such as echo server/client
  interaction.
- Tests should be deterministic and avoid sleeps unless timing behavior is the
  subject under test.
