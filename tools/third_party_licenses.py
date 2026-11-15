# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Exact KCP provenance and per-file SPDX classification; historical inputs stay unchanged."""
from pathlib import PurePosixPath
VENDORED_KCP = {
    'ikcp.c':'8654038a70d1bbe605aa94f066554ffb30bf28580a58769970221de3ba5027f0',
    'ikcp.h':'d67a0e67ba5701f1e882894b4415c4d3ad3d7d9f7231dd2ca68c6037c5217933',
    'LICENSE':'6587fd02a924b97dbd6758b0573ddec056fb494a84f3709c5aaa33ce1f7f029c',
}
def license_for_path(path):
    p=PurePosixPath(path)
    if path in {'third_party/kcp/'+n for n in VENDORED_KCP} or p.as_posix().endswith('/doc/GameNetCore/kcp/LICENSE'):
        return 'MIT'
    if p.name in {'libgamenet_kcp.a','gamenet_kcp.lib'}:return 'Apache-2.0 AND MIT'
    return 'Apache-2.0'
def package_license(files):
    return 'Apache-2.0 AND MIT' if any(license_for_path(f.path)!='Apache-2.0' for f in files) else 'Apache-2.0'
def copyright_for_path(path):
    license_id=license_for_path(path)
    if license_id=='MIT':return 'Copyright (c) 2017 Lin Wei (skywind3000 at gmail.com)'
    if license_id=='Apache-2.0 AND MIT':return 'Copyright 2026 Yanqing Xu; Copyright (c) 2017 Lin Wei (skywind3000 at gmail.com)'
    return 'Copyright 2026 Yanqing Xu'
