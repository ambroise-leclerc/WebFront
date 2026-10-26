"""Combine CTest's JSON dependency/lock graph and JUnit timings without summing parallel wall times."""
import argparse
import json
from pathlib import Path
import statistics
import re
import xml.etree.ElementTree as ET


def analyze(graph, report, workers):
    tests = {test['name']: {p['name']: p['value'] for p in test.get('properties', [])}
             for test in graph['tests']}
    xml = ET.parse(report).getroot()
    times = {case.attrib['name']: float(case.attrib.get('time', 0)) for case in xml.iter('testcase')}
    setters = {}
    for name, properties in tests.items():
        for fixture in properties.get('FIXTURES_SETUP', []):
            setters.setdefault(fixture, []).append(name)
    edges = {name: set(properties.get('DEPENDS', [])) for name, properties in tests.items()}
    locks = {}
    for name, properties in tests.items():
        for fixture in properties.get('FIXTURES_REQUIRED', []):
            edges[name].update(setters.get(fixture, []))
        for lock in properties.get('RESOURCE_LOCK', []):
            locks.setdefault(lock, []).append(name)
    # A fixture cleanup waits for every setup and consumer, even when there are no explicit DEPENDS edges.
    for name, properties in tests.items():
        for fixture in properties.get('FIXTURES_CLEANUP', []):
            edges[name].update(setters.get(fixture, []))
            edges[name].update(consumer for consumer, props in tests.items()
                               if fixture in props.get('FIXTURES_REQUIRED', []))
    costs = {}
    visiting = set()
    def cost(name):
        if name in visiting:
            raise ValueError('Cyclic CTest dependency: ' + name)
        if name not in costs:
            visiting.add(name)
            costs[name] = times.get(name, 0) + max((cost(parent) for parent in edges.get(name, [])), default=0)
            visiting.remove(name)
        return costs[name]
    critical = max((cost(name) for name in times), default=0)
    serial = sum(times.get(name, 0) for name, props in tests.items() if props.get('RUN_SERIAL'))
    lock_cost = max((sum(times.get(name, 0) for name in names) for names in locks.values()), default=0)
    # Lower bounds only: lock scheduling and fixture order can make the actual critical path longer.
    return {'wall_seconds': float(xml.attrib.get('time', 0)), 'sum_seconds': sum(times.values()),
            'dependency_path_seconds': critical, 'serial_seconds': serial,
            'lower_bound_seconds': max(critical, sum(times.values()) / workers, lock_cost, serial),
            'cases': len(times), 'slowest': sorted(times.items(), key=lambda x: x[1], reverse=True)[:25],
            'edges': edges, 'locks': locks}


def compile_profile(path):
    records = []
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        if line.startswith('#'):
            continue
        columns = line.split('\t')
        if len(columns) != 5:
            continue
        start, end, _, output, _ = columns
        seconds = (int(end) - int(start)) / 1000
        if seconds < 0:
            raise ValueError('Negative Ninja job duration: ' + output)
        records.append((seconds, output))
    return sorted(records, reverse=True)


def write_report(result, output):
    output.parent.mkdir(parents=True, exist_ok=True)
    lines = ['CTest profile', '', f"Cases: {result['cases']}"]
    for key in ('wall_seconds', 'sum_seconds', 'dependency_path_seconds', 'serial_seconds', 'lower_bound_seconds'):
        lines.append(f'{key}: {result[key]:.3f}')
    lines += ['', 'Slowest cases (seconds):'] + [f'{seconds:9.3f} {name}' for name, seconds in result['slowest']]
    output.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    dot = ['digraph ctest {']
    for name, dependencies in result['edges'].items():
        dot.append('  ' + json.dumps(name) + ';')
        for dependency in dependencies:
            dot.append(f'  {json.dumps(dependency)} -> {json.dumps(name)};')
    for lock, names in result['locks'].items():
        for name in names:
            dot.append(f'  {json.dumps("lock: " + lock)} -> {json.dumps(name)} [style=dashed];')
    output.with_suffix('.dot').write_text('\n'.join(dot + ['}']) + '\n', encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('graph', type=Path)
    parser.add_argument('reports', nargs='+', type=Path)
    parser.add_argument('--workers', type=int, default=4)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--log', type=Path)
    parser.add_argument('--ninja-log', type=Path)
    args = parser.parse_args()
    if args.workers < 1:
        parser.error('workers must be positive')
    results = [analyze(json.loads(args.graph.read_text()), report, args.workers) for report in args.reports]
    for index, result in enumerate(results, 1):
        write_report(result, args.output.with_name(f'{args.output.stem}-{index}.txt'))
    walls = [result['wall_seconds'] for result in results]
    args.output.write_text(f'Runs: {len(walls)}\nWall median: {statistics.median(walls):.3f} s\n'
                          f'Wall range: {min(walls):.3f}–{max(walls):.3f} s\n', encoding='utf-8')

    # JUnit keeps each pass independently and cannot be overwritten by a later `ctest --show-only`.
    transcripts = ['\n'.join(case.findtext('system-out', '') for case in ET.parse(report).getroot().iter('testcase'))
                   for report in args.reports]
    if args.log and args.log.exists() and not any('durationMs=' in text for text in transcripts):
        transcripts.append(args.log.read_text(encoding='utf-8', errors='replace'))
    if transcripts:
        phases = []
        for index, transcript in enumerate(transcripts, 1):
            for line in transcript.splitlines():
                match = re.search(r'(?:wf spec: |Developer integration: )?(.*?end .*?) durationMs=([\d.]+)', line)
                if match:
                    phases.append((float(match[2]), f'pass {index}: {match[1]}'))
        args.output.with_name(args.output.stem + '-phases.txt').write_text(
            '\n'.join(f'{duration:12.3f} ms {name}' for duration, name in sorted(phases, reverse=True)) + '\n', encoding='utf-8')

    if args.ninja_log and args.ninja_log.exists():
        records = compile_profile(args.ninja_log)
        args.output.with_name(args.output.stem + '-compilation.txt').write_text(
            'Ninja job durations (inclusive parallel work, not elapsed build time)\n' +
            '\n'.join(f'{duration:10.3f} s {output}' for duration, output in records[:40]) + '\n', encoding='utf-8')
