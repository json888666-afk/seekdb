#!/usr/bin/env python3
"""Check material format and evidence references, without predicting any score."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
MAX_BYTES = 128 * 1024


def reject_constant(text):
    raise ValueError('non-standard JSON number: '+text)


def unique_object(pairs):
    out = {}
    for key, value in pairs:
        if key in out:
            raise ValueError('duplicate JSON key: '+key)
        out[key] = value
    return out


def pointer(document, path):
    if path == '':
        return document
    if not isinstance(path, str) or not path.startswith('/'):
        raise ValueError('invalid RFC6901 pointer: '+repr(path))
    node = document
    for raw in path[1:].split('/'):
        if re.search(r'~(?![01])', raw):
            raise ValueError('invalid JSON pointer escape: '+path)
        key = raw.replace('~1', '/').replace('~0', '~')
        if isinstance(node, list):
            if not key.isdigit() or (len(key)>1 and key.startswith('0')):
                raise ValueError('invalid JSON array index: '+path)
            node = node[int(key)]
        else:
            node = node[key]
    return node


def validate(path):
    raw = path.read_bytes()
    if len(raw) > MAX_BYTES:
        raise ValueError(f'material is {len(raw)} bytes; maximum is {MAX_BYTES}')
    text = raw.decode('utf-8')
    data = json.loads(text, object_pairs_hook=unique_object, parse_constant=reject_constant)
    if not isinstance(data, dict):
        raise ValueError('top-level JSON must be an object')
    # Parsers can overflow valid exponent syntax such as 1e999 to infinity.
    json.dumps(data, allow_nan=False)
    if re.search(r'\b(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,})\b', text):
        raise ValueError('possible GitHub token in material')
    if re.search(r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----', text):
        raise ValueError('private key in material')

    all_ids = set()
    for name in ('user_prompts', 'conversations', 'experiments', 'evidence_units'):
        for record in data.get(name, []):
            ident = record.get('id')
            if ident:
                if ident in all_ids:
                    raise ValueError('duplicate record id: '+ident)
                all_ids.add(ident)

    seen = set()
    citations = 0
    for unit in data.get('evidence_units', []):
        # Local organization only. The official evaluator decides dimensions and scores.
        for kind in ('student_evidence', 'result_evidence'):
            for ref in unit.get(kind, []):
                node = pointer(data, ref['pointer'])
                quote = ref['quote']
                if not isinstance(node, str) or not isinstance(quote, str) or not quote or len(quote)>240:
                    raise ValueError('citation must point to text and quote 1..240 characters')
                if quote not in node:
                    raise ValueError('quote is not an exact substring: '+ref['pointer'])
                identity = (ref['pointer'], quote)
                if identity in seen:
                    raise ValueError('same evidence span is reused across units')
                seen.add(identity)
                record_path = ref['pointer'].rsplit('/',1)[0]
                record = pointer(data, record_path)
                source = record.get('source') if isinstance(record, dict) else None
                expected = {'user'} if kind=='student_evidence' else {'tool_result', 'user_reported_tool_output'}
                if source not in expected:
                    raise ValueError(f'{kind} has unexpected source {source!r}')
                citations += 1

    for item in data.get('code_state', {}).get('files', []):
        file = (ROOT / item['path']).resolve()
        file.relative_to(ROOT.resolve())
        content = file.read_bytes().replace(b'\r\n', b'\n')
        if hashlib.sha256(content).hexdigest() != item['sha256']:
            raise ValueError('source fingerprint changed: '+item['path'])

    return {
        'scope': 'local_material_format_and_reference_check_only',
        'passed': True, 'material_bytes': len(raw), 'max_bytes': MAX_BYTES,
        'material_sha256': hashlib.sha256(raw).hexdigest(), 'checked_citations': citations,
        'evidence_units': len(data.get('evidence_units', [])),
        'student_records': sum(x.get('source')=='user' for x in data.get('conversations', [])),
        'official_score': None,
        'note': 'Format and exact reference validity do not establish a score, rank, or completeness of learning evidence.',
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('path', type=Path, nargs='?', default=ROOT/'agent/conversation.json')
    args = parser.parse_args()
    try:
        print(json.dumps(validate(args.path), ensure_ascii=False, indent=2))
    except (OSError, ValueError, TypeError, KeyError, IndexError) as exc:
        print('Material check failed: '+str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
