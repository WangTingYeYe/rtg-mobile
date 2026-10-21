#!/usr/bin/env python3
"""Check the public source tree, or the exact Git index with --staged.

Reject local artifacts, binary files, common credentials and personal paths.
Reports contain filenames and rule names only, never matched secret values.
This is a publication guard, not a comprehensive secret-detection service.
"""
import argparse
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
LOCAL_DIRS = {'game', 'build', 'run', '.local', '.claude', '.codex', '.venv',
              'venv', '__pycache__', 'romfs', 'exefs', 'save', 'saves',
              'node_modules', 'bin', 'obj', 'cmakefiles'}
TEXT_SUFFIXES = {'.c', '.h', '.s', '.inc', '.py', '.java', '.cs', '.csproj',
                 '.glsl', '.vert', '.frag', '.props', '.targets', '.md', '.txt', '.xml', '.json', '.tsv',
                 '.yml', '.yaml', '.sh', '.toml'}
TEXT_NAMES = {'license', 'license.txt', 'copying', 'makefile', '.gitignore',
              '.gitattributes', '.editorconfig', 'pre-commit'}
SENSITIVE_NAMES = {'.env', 'local.properties', 'prod.keys', 'title.keys',
                   'credentials', 'credentials.json', 'id_rsa', 'id_ed25519'}
PATTERNS = [
    ('private key', re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH |DSA |ENCRYPTED )?PRIVATE KEY-----')),
    ('GitHub token', re.compile(r'\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,})\b')),
    ('AWS access key', re.compile(r'\b(?:AKIA|ASIA)[A-Z0-9]{16}\b')),
    ('service token', re.compile(r'\b(?:sk-(?:proj-)?[A-Za-z0-9_-]{32,}|xox[baprs]-[A-Za-z0-9-]{20,})\b')),
    ('personal absolute path', re.compile(r'/(?:Users|home)/[A-Za-z0-9_.-]+/|[A-Za-z]:[\\/]Users[\\/][A-Za-z0-9_.-]+[\\/]')),
]


def issues(name, data, mode='100644'):
    path = PurePosixPath(name)
    found = []
    if mode not in ('100644', '100755'):
        found.append('symlink, submodule or unresolved merge entry')
    if any(part.lower() in LOCAL_DIRS or part.endswith('.dSYM') for part in path.parts[:-1]):
        found.append('local/generated directory')
    if path.name.lower() in SENSITIVE_NAMES or path.name.startswith('.env.') and path.name != '.env.example':
        found.append('credential/configuration file')
    if path.name.lower() not in TEXT_NAMES and path.suffix.lower() not in TEXT_SUFFIXES:
        found.append('not a supported source/text file')
    if len(data) > 2 * 1024 * 1024:
        found.append('file exceeds 2 MiB; review generated content')
    try:
        content = data.decode('utf-8-sig')
        if '\0' in content:
            found.append('binary content')
        for label, pattern in PATTERNS:
            matches = list(pattern.finditer(content))
            if label == 'personal absolute path':
                matches = [m for m in matches if re.split(r'[\\/]', m.group().rstrip('/\\'))[-1] not in ('user', 'username')]
            if matches:
                found.append(label)
    except UnicodeDecodeError:
        found.append('non-UTF-8/binary content')
    return found


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args])


def entries(root, staged):
    if staged:
        records = []
        for record in git(root, 'ls-files', '--stage', '-z').split(b'\0'):
            if not record:
                continue
            info, name = record.split(b'\t', 1)
            mode, oid, stage = info.decode().split()
            records.append((name.decode(), oid, mode if stage == '0' else 'conflict'))
        # Read the staged blobs, not potentially different working-tree bytes.
        blobs = b'' if not records else subprocess.check_output(
            ['git', '-C', str(root), 'cat-file', '--batch'],
            input=('\n'.join(r[1] for r in records) + '\n').encode())
        offset = 0
        for name, oid, mode in records:
            end = blobs.index(b'\n', offset)
            header = blobs[offset:end].split()
            if len(header) != 3 or header[1] != b'blob':
                raise ValueError(f'{name}: index entry is not a readable source blob')
            size = int(header[2]); offset = end + 1
            yield name, blobs[offset:offset + size], mode
            offset += size + 1
    else:
        names = set(git(root, 'ls-files', '--cached', '--others', '--exclude-standard', '-z').split(b'\0'))
        for raw in sorted(names - {b''}):
            name = raw.decode(); path = root / name
            if path.is_symlink():
                yield name, b'', '120000'
            elif path.is_file():
                yield name, path.read_bytes(), '100644'
            else:
                yield name, b'', 'missing'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--staged', action='store_true', help='Inspect every file in the Git index')
    parser.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args()
    total = size = failures = 0
    try:
        for name, data, mode in entries(args.root, args.staged):
            total += 1; size += len(data)
            for problem in issues(name, data, mode):
                failures += 1
                print(f'FAIL {name}: {problem}', file=sys.stderr)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'Cannot inspect repository: {error}', file=sys.stderr)
        return 1
    print(f'{"FAIL" if failures else "PASS"}: {total} source files, {size / 1048576:.2f} MiB, {failures} findings')
    return int(bool(failures))


if __name__ == '__main__':
    sys.exit(main())
