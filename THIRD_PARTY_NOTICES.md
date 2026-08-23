# Third-Party Notices

The game-net-core source and binary distributions do not bundle third-party
library source or binary dependencies.

The following build and system prerequisites may be used but are not included
in project distributions: CMake, Ninja, GCC and binutils, MSVC, the Windows
SDK, POSIX threads, and Windows Winsock. Each is governed by its own license.

The PacketFramer fuzz corpus under `tests/fuzz/corpus/packet_framer/` is
deterministically generated from project-authored byte strings by
`tests/fuzz/generate_packet_framer_corpus.py`; it is not an external corpus.

If a future distribution bundles additional third-party material, this file
and the release SBOM must be updated before that distribution is published.
