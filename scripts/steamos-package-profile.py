#!/usr/bin/env python3
"""Plan an offline SteamOS recovery userspace from original ALPM metadata.

Developer tool, not an installer/support claim. Emits a Btrfs restore filter for
whole selected packages and their original database entries. Actual pacman -Dk
and -Qk, ELF execution and session tests remain mandatory after restoration.
Never downloads packages or uses repository URLs embedded in the recovery image.
"""
import argparse
import json
from pathlib import Path
import re


def sections(path):
    result = {}; key = None
    for line in path.read_text().splitlines():
        if line.startswith('%') and line.endswith('%'):
            key = line[1:-1]; result[key] = []
        elif line and key:
            result[key].append(line)
    return result


def safe_path(path):
    if not path or path.startswith('/') or any(x in ('', '.', '..') for x in path.split('/')) or '\0' in path or '\n' in path:
        raise ValueError('unsafe original package path')
    return path


def restore_pattern(paths):
    trie = {}
    for path in paths:
        node = trie
        for part in safe_path(path).split('/'):
            node = node.setdefault(part, {})
    def escape(text):
        return ''.join('\\' + c if c in '\\.^$|?*+()[]{}' else c for c in text)
    def children(node):
        if not node:
            return ''
        return '(|/(' + '|'.join(escape(k) + children(v) for k, v in sorted(node.items())) + '))'
    # Include ancestor directories: btrfs restore tests every traversed level.
    return '^/(|' + '|'.join(escape(k) + children(v) for k, v in sorted(trie.items())) + ')$'


def restore_patterns(paths, limit=100000):
    ordered = sorted(paths)
    def split(items):
        pattern = restore_pattern(items)
        if len(pattern.encode()) <= limit:
            return [pattern]
        if len(items) < 2:
            raise ValueError('individual package path exceeds restore limit')
        middle = len(items) // 2
        return split(items[:middle]) + split(items[middle:])
    return split(ordered)


def plan(database, seeds, overrides):
    packages = {}; providers = {}
    for directory in sorted(database.iterdir()):
        if not (directory / 'desc').is_file():
            continue
        metadata = sections(directory / 'desc')
        name = metadata['NAME'][0]
        if name in packages:
            raise ValueError('duplicate installed package: ' + name)
        if metadata.get('ARCH') not in (['aarch64'], ['any']):
            raise ValueError('non-ARM64 package metadata: ' + name)
        packages[name] = (directory, metadata)
        for provided in metadata.get('PROVIDES', []):
            plain = re.split('[<>=]', provided)[0]
            providers.setdefault(plain, []).append((name, provided))
    chosen = set(); pending = list(seeds); constraints = []
    while pending:
        name = pending.pop()
        if name in chosen:
            continue
        if name not in packages:
            raise ValueError('package missing from original image: ' + name)
        chosen.add(name)
        for dependency in packages[name][1].get('DEPENDS', []):
            constraints.append(dependency)
            plain = re.split('[<>=]', dependency)[0]
            if plain in packages:
                pending.append(plain)
                continue
            choices = [p for p, value in providers.get(plain, []) if '=' not in dependency or value == dependency]
            selected = overrides.get(plain)
            if selected is not None:
                if selected not in choices:
                    raise ValueError('override does not provide ' + dependency)
                choices = [selected]
            if len(choices) != 1:
                raise ValueError('missing/ambiguous provider for ' + dependency + ': ' + ', '.join(choices))
            pending.append(choices[0])
    paths = set(); dbdirs = []
    for name in sorted(chosen):
        directory, metadata = packages[name]
        dbdirs.append(directory.name)
        for path in sections(directory / 'files').get('FILES', []):
            paths.add(safe_path(path.rstrip('/')))
        for file in directory.iterdir():
            if file.is_symlink() or not file.is_file():
                raise ValueError('unexpected package metadata entry')
            paths.add(safe_path('usr/lib/holo/pacmandb/local/' + directory.name + '/' + file.name))
    for file in database.iterdir():
        if file.is_file() and not file.is_symlink():
            paths.add(safe_path('usr/lib/holo/pacmandb/local/' + file.name))
    patterns = restore_patterns(paths)
    result = dict(label='Proposed SteamOS ARM64 developer userspace; source/runtime qualification pending',
                  packages=sorted(chosen), database_directories=dbdirs,
                  owned_paths=len(paths), restore_passes=len(patterns),
                  regex_bytes=sum(len(p.encode()) for p in patterns),
                  installed_bytes=sum(int(packages[n][1].get('SIZE', ['0'])[0]) for n in chosen),
                  dependencies=sorted(set(constraints)),
                  dependency_constraints_verified=False, package_files_verified=False,
                  source_identity_verified=False,
                  desktop_verified=False, steam_client_verified=False,
                  qualification='Run original pacman -Dk/-Qk on the restored root. Version constraints are retained, not inferred satisfied by this planner.')
    return result, patterns


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--database', type=Path, required=True)
    parser.add_argument('--package', action='append', required=True)
    parser.add_argument('--provider', action='append', default=[], help='explicit virtual-name=package-name')
    parser.add_argument('--output', type=Path, required=True, help='new report JSON; .regex saved alongside')
    args = parser.parse_args()
    result, patterns = plan(args.database, args.package, dict(v.split('=', 1) for v in args.provider))
    with args.output.open('x') as output:
        json.dump(result, output, indent=2); output.write('\n')
    with args.output.with_suffix('.filters.json').open('x') as output:
        json.dump(patterns, output); output.write('\n')
    print(f"PLANNED packages={len(result['packages'])} bytes={result['installed_bytes']} filter={result['regex_bytes']}; runtime qualification pending")
