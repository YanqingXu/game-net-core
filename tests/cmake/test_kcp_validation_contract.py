# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Configure actual CMake graphs; C4 must not leak into the production package."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--installed-prefix', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='kcp-c4-') as temp:
        def configure(name, udp=False, kcp=False, c4=False, success=True):
            build = Path(temp)/name
            query = build/'.cmake/api/v1/query'; query.mkdir(parents=True)
            (query/'codemodel-v2').touch()
            command = ['cmake', '-S', str(ROOT), '-B', str(build), '-DGAMENET_BUILD_TESTING=OFF',
                       '-DGAMENET_ENABLE_UDP='+('ON' if udp else 'OFF'),
                       '-DGAMENET_ENABLE_KCP='+('ON' if kcp else 'OFF'),
                       '-DGAMENET_BUILD_KCP_VALIDATION='+('ON' if c4 else 'OFF')]
            command += ['-A', 'x64'] if os.name == 'nt' else ['-G', 'Ninja']
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace', timeout=120)
            assert (result.returncode == 0) == success, result.stdout
            if not success:
                assert 'requires explicit KCP and UDP ON' in result.stdout, result.stdout
                return None
            reply = build/'.cmake/api/v1/reply'
            index = json.loads(next(reply.glob('index-*.json')).read_text())
            model = json.loads((reply/index['reply']['codemodel-v2']['jsonFile']).read_text())
            graphs = []
            for config in model['configurations']:
                graph = {t['name']: json.loads((reply/t['jsonFile']).read_text()) for t in config['targets']}
                graphs.append(graph)
            return graphs
        for graph in configure('default'):
            assert 'gamenet_kcp_validation' not in graph
        for graph in configure('kcp-only', True, True):
            assert 'gamenet_kcp_validation' not in graph
        configure('no-udp', c4=True, success=False)
        configure('no-kcp', udp=True, c4=True, success=False)
        for graph in configure('c4', True, True, True):
            tool = graph['gamenet_kcp_validation']
            assert tool['type'] == 'EXECUTABLE' and 'install' not in tool
            assert any(d['id'] == graph['gamenet_kcp']['id'] for d in tool['dependencies'])
            for name, target in graph.items():
                if name != 'gamenet_kcp_validation' and target['type'] != 'UTILITY':
                    assert all(d['id'] != tool['id'] for d in target.get('dependencies', [])), name
    cmake = (ROOT/'benchmarks/kcp_validation/CMakeLists.txt').read_text()
    assert 'install(' not in cmake and 'EXPORT_NAME' not in cmake
    if args.installed_prefix:
        prefix = args.installed_prefix.resolve()
        assert prefix.is_dir() and list(prefix.rglob('GameNetCoreConfig.cmake'))
        for path in prefix.rglob('*'):
            assert 'kcp_validation' not in path.name.lower(), path
            if path.suffix == '.cmake':
                assert 'gamenet_kcp_validation' not in path.read_text(encoding='utf-8'), path
    print('C4 default-OFF, explicit prerequisites, graph/install/export isolation passed')


if __name__ == '__main__':
    main()
