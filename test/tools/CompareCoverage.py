"""Compare per-file line, function and branch hits independently of lcov record ordering."""
import sys
from pathlib import Path


def hits(path):
    result = {}
    source = None
    for line in path.read_text(encoding='utf-8').splitlines():
        kind, _, value = line.partition(':')
        if kind == 'SF':
            source = value
        elif kind == 'DA':
            number, count, *_ = value.split(',')
            key = source, kind, number
            result[key] = result.get(key, 0) + int(count)
        elif kind == 'FNDA':
            count, name = value.split(',', 1)
            key = source, kind, name
            result[key] = result.get(key, 0) + int(count)
        elif kind == 'BRDA':
            number, block, branch, count = value.split(',')
            key = source, kind, number, block, branch
            previous = result.get(key, '-')
            result[key] = '-' if previous == count == '-' else int(0 if previous == '-' else previous) + int(0 if count == '-' else count)
    if not result:
        raise ValueError('Coverage contains no counters: ' + str(path))
    return result


if __name__ == '__main__':
    left, right = map(Path, sys.argv[1:])
    first, second = hits(left), hits(right)
    if first != second:
        raise SystemExit('JSON backends changed coverage counters')
    print(f'Identical coverage: {len(first)} line/function/branch counters')
