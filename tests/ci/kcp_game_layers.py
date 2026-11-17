# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
"""Read actual dual-family C3 contract output, not a synthetic success count."""
import re

TEST = 'integration.kcp.test_kcp_game_layers'
EXPECTED = dict(stale_plan=1, stale_task=1, stale_logic=2, pending_refusal=1,
                task_refusal=1, byte_refusal=2, queue_refusal=1, pending_cancel=1,
                healthy_progress=1, zero_residue=1)

def verify_game_layers(root):
    cases = [t for t in root.findall('.//testcase') if t.get('name') == TEST]
    if len(cases) != 1:
        raise ValueError('C3 execution record missing/duplicated')
    records = []
    for line in re.findall(r'^KCP_C3 (.+)$', cases[0].findtext('system-out') or '', re.M):
        pairs = [token.split('=') for token in line.split()]
        if any(len(p) != 2 for p in pairs) or len({p[0] for p in pairs}) != len(pairs):
            raise ValueError('malformed C3 evidence')
        try:
            records.append({k: int(v) for k, v in pairs})
        except ValueError as error:
            raise ValueError('non-numeric C3 evidence') from error
    if records != [dict(family=4, **EXPECTED), dict(family=6, **EXPECTED)]:
        raise ValueError('dual-family C3 contract evidence incomplete')
    return records
