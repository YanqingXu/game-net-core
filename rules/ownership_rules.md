# Ownership Rules

- Public APIs should make ownership transfer explicit.
- Connection lifetime must be controlled by the core TCP lifecycle, not by
  detached callbacks.
- Callbacks must not assume raw pointers outlive the owning EventLoop or
  TcpConnection.
