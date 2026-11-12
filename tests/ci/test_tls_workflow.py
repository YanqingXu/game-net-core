# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
from __future__ import annotations
import copy
from pathlib import Path
import re
import xml.etree.ElementTree as ET
from run_tls_contracts import CONTRACTS, RPC_C3, verify_inventory, verify_results

def rejects(callback, value) -> None:
    try: callback(value)
    except ValueError: return
    raise AssertionError("invalid TLS evidence accepted")

def main() -> None:
    root = Path(__file__).resolve().parents[2]
    workflow = (root / '.github/workflows/tls.yml').read_text(encoding='utf-8')
    assert set(re.findall(r'- lane: ([\w-]+)', workflow)) == {
        'linux-release', 'linux-asan-ubsan', 'linux-tsan', 'windows-release', 'windows-asan'}
    for fragment in ['run_tls_contracts.py', '--with-tls', 'if-no-files-found: error', 'if: always()',
                     'Real OpenSSL development files are required', 'vm.mmap_rnd_bits=28']:
        assert fragment in workflow, fragment
    assert 'continue-on-error: true' not in workflow
    cmake = (root / 'CMakeLists.txt').read_text(encoding='utf-8')
    assert 'option(GAMENET_ENABLE_TLS "Build TLS support" OFF)' in cmake
    assert re.search(r'if\(GAMENET_ENABLE_TLS\)\s+add_subdirectory\(src/tls\)\s+add_subdirectory\(src/rpc_tls\)\s+endif\(\)', cmake)
    target = (root / 'src/tls/CMakeLists.txt').read_text(encoding='utf-8')
    for fragment in ['find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL)', 'GameNetTlsTargets', 'GameNet::tls']:
        assert fragment in target
    config = (root / 'cmake/GameNetCoreConfig.cmake.in').read_text(encoding='utf-8')
    assert '"tls" IN_LIST GameNetCore_FIND_COMPONENTS OR "rpc_tls" IN_LIST GameNetCore_FIND_COMPONENTS' in config
    assert 'GameNetRpcTlsTargets.cmake' in config
    assert re.search(r'if\("rpc_tls" IN_LIST GameNetCore_FIND_COMPONENTS AND @GAMENET_ENABLE_TLS@\)', config)
    tests = (root / 'tests/CMakeLists.txt').read_text(encoding='utf-8')
    block = re.search(r'if\(GAMENET_ENABLE_TLS\)(.*?)endif\(\)', tests, re.S).group(1)
    for name in CONTRACTS:
        layer, module, source = name.split('.')
        assert f'{layer}/{module}/{source}.cpp' in block
    document = {'tests': [
        {'name': n, 'properties': [{'name': 'LABELS', 'value': ['tls'] if n in CONTRACTS else ['contract']}]}
        for n in [*[f'baseline.test.{i}' for i in range(133)], *sorted(CONTRACTS)]]}
    verify_inventory(document)
    invalid = copy.deepcopy(document); invalid['tests'].pop(); rejects(verify_inventory, invalid)
    invalid = copy.deepcopy(document); invalid['tests'][-1]['name'] = 'wrong.tls.replacement'; rejects(verify_inventory, invalid)
    invalid = copy.deepcopy(document); invalid['tests'][-1]['properties'].append({'name': 'DISABLED', 'value': True}); rejects(verify_inventory, invalid)
    suite = ET.Element('testsuite')
    for name in sorted(CONTRACTS): ET.SubElement(suite, 'testcase', {'name': name, 'status': 'run'})
    verify_results(suite, CONTRACTS)
    rpc_suite = ET.Element('testsuite')
    for name in sorted(RPC_C3): ET.SubElement(rpc_suite, 'testcase', {'name': name, 'status': 'run'})
    verify_results(rpc_suite, RPC_C3)
    invalid = copy.deepcopy(rpc_suite); invalid.remove(invalid[0])
    rejects(lambda x: verify_results(x, RPC_C3), invalid)
    for kind in ['failure', 'error', 'skipped']:
        invalid = copy.deepcopy(suite); ET.SubElement(invalid[0], kind)
        rejects(lambda x: verify_results(x, CONTRACTS), invalid)
    invalid = copy.deepcopy(suite); invalid[0].set('status', 'notrun')
    rejects(lambda x: verify_results(x, CONTRACTS), invalid)
    print('TLS optional build/package boundaries and exact execution rejection contracts passed')
if __name__ == '__main__': main()
