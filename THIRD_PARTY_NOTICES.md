# Third-Party Notices

The optional `GameNet::tls` component links to an externally supplied OpenSSL 3
provider (Apache-2.0). Its default-OFF build has no OpenSSL dependency. This
repository does not bundle OpenSSL binaries, private keys or trust stores.
Provider source, license and notices: https://openssl-library.org/source/ and
https://github.com/openssl/openssl/blob/openssl-3.0/LICENSE.txt.

The game-net-core source and binary distributions do not bundle third-party
library source or binary dependencies.

Project-authored material in these distributions is licensed under
Apache-2.0 as stated by the top-level `LICENSE` and `NOTICE` files.

The following build and system prerequisites may be used but are not included
in project distributions: CMake, Ninja, GCC and binutils, MSVC, the Windows
SDK, POSIX threads, and Windows Winsock. Each is governed by its own license.

The PacketFramer fuzz corpus under `tests/fuzz/corpus/packet_framer/` is
deterministically generated from project-authored byte strings by
`tests/fuzz/generate_packet_framer_corpus.py`; it is not an external corpus.

If a future distribution bundles additional third-party material, this file
and the release SBOM must be updated before that distribution is published.
