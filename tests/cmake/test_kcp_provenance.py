# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Bundled MIT bytes remain original and are never mislabeled as Apache-only."""
import hashlib,importlib.util,json,sys,tempfile
from pathlib import Path
def main():
    root=Path(__file__).resolve().parents[2];sys.path.insert(0,str(root/'tools'))
    from check_spdx_headers import verify_vendored_kcp
    from third_party_licenses import VENDORED_KCP,license_for_path,package_license
    from assemble_release import ReleaseFile,validate_project_metadata
    verify_vendored_kcp(root)
    assert all(hashlib.sha256((root/'third_party/kcp'/n).read_bytes()).hexdigest()==h for n,h in VENDORED_KCP.items())
    assert license_for_path('third_party/kcp/ikcp.c')=='MIT'
    assert license_for_path('third_party/kcp/upstream.json')=='Apache-2.0'
    assert license_for_path('lib/gamenet_kcp.lib')=='Apache-2.0 AND MIT'
    assert license_for_path('share/doc/GameNetCore/kcp/LICENSE')=='MIT'
    assert package_license([ReleaseFile('main.cpp',b'')])=='Apache-2.0'
    assert package_license([ReleaseFile('third_party/kcp/ikcp.h',b'')])=='Apache-2.0 AND MIT'
    try:validate_project_metadata([ReleaseFile('third_party/kcp/ikcp.c',b'modified')],'0.3.1')
    except ValueError as e:assert 'vendored KCP' in str(e)
    else:raise AssertionError('modified/missing MIT source accepted')
    print('Pinned MIT source, narrow header exemption and mixed-license SBOM contracts passed')
if __name__=='__main__':main()
