"""Build-time study (#435): time one CMake configuration of the tests under the compiler-cache scenarios of a pull request.

Every scenario is a clean build of one target. The compiler cache (sccache, local disk) is empty for `cold`, complete for
`warm`, and complete except for the translation units that include the file a scenario changed. The changed file is restored
afterwards. Writes one JSON report (stdlib only)."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).parent))
from MeasureCommand import measure  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
SCENARIOS = [
    ('cold', None),
    ('warm', None),
    ('header', 'include/webfront/WebFront.hpp'),
    ('heavy-test', 'test/unit/WebFrontTests.cpp'),
    ('light-test', 'test/unit/MimeTypeTests.cpp'),
]


def run(command, **kwargs):
    print('+', ' '.join(map(str, command)), flush=True)
    return subprocess.run(command, check=True, **kwargs)


def sccache_counts():
    output = subprocess.run(['sccache', '--show-stats'], capture_output=True, text=True, check=True).stdout
    counts = {}
    for key in ('Compile requests', 'Cache hits', 'Cache misses'):
        match = re.search(r'^' + key + r'\s+(\d+)', output, re.MULTILINE)
        counts[key] = int(match.group(1)) if match else None
    return counts


def ninja_units(build):
    """(output, milliseconds) of the last build, from .ninja_log (the log keeps the last run of each output)."""
    units = []
    log = build / '.ninja_log'
    if not log.exists():
        return units
    for line in log.read_text().splitlines():
        fields = line.split('\t')
        if len(fields) >= 4 and fields[0].isdigit():
            units.append((fields[3], int(fields[1]) - int(fields[0])))
    return units


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--jobs', type=int, required=True)
    parser.add_argument('--target', default='tests')
    parser.add_argument('--config', default='Release')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--rep', type=int, default=1)
    parser.add_argument('--scenarios', default=','.join(name for name, _ in SCENARIOS))
    parser.add_argument('cmake_args', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    cmake_args = args.cmake_args[1:] if args.cmake_args[:1] == ['--'] else args.cmake_args
    build = args.build_dir.resolve()
    wanted = args.scenarios.split(',')
    results = {'name': args.name, 'rep': args.rep, 'jobs': args.jobs, 'target': args.target, 'config': args.config,
               'cmake_args': cmake_args, 'platform': sys.platform, 'scenarios': {}}

    def write():
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')

    shutil.rmtree(build, ignore_errors=True)
    configure = measure_command(['cmake', '-S', str(ROOT), '-B', str(build), '-G', 'Ninja', f'-DCMAKE_BUILD_TYPE={args.config}',
                                 '-DENABLE_TESTING=ON', '-DWEBFRONT_EMBED_CEF=OFF', '-DENABLE_CACHE=OFF',
                                 '-DCMAKE_CXX_COMPILER_LAUNCHER=sccache', '-DCMAKE_CXX_SCAN_FOR_MODULES=OFF'] + cmake_args,
                                build.parent / (args.name + f'-r{args.rep}-configure.json'))
    results['configure_seconds'] = configure['wall_seconds']
    if configure['exit_code']:
        results['error'] = 'configure failed'
        write()
        return 1
    subprocess.run(['sccache', '--stop-server'], capture_output=True)
    subprocess.run(['sccache', '--start-server'], capture_output=True)
    status = 0
    for name, changed in SCENARIOS:
        if name not in wanted:
            continue
        original = None
        if changed:
            path = ROOT / changed
            original = path.read_bytes()
            # A comment vanishes in preprocessing, and a preprocessor-keyed cache (GCC) would still hit: add a declaration.
            identifier = 'build_study_' + name.replace('-', '_')
            if path.suffix == '.hpp':
                addition = f'\nnamespace webfront {{ inline constexpr int {identifier} = 0; }}\n'
            else:
                addition = f'\nnamespace {{ [[maybe_unused]] constexpr int {identifier} = 0; }}\n'
            path.write_bytes(original + addition.encode())
        try:
            subprocess.run(['sccache', '--zero-stats'], capture_output=True, check=True)
            command = ['cmake', '--build', str(build), '--config', args.config, '--target', args.target,
                       '--parallel', str(args.jobs), '--clean-first']
            report = measure_command(command, build.parent / (args.name + f'-r{args.rep}-' + name + '.json'))
            counts = sccache_counts()
            units = sorted(ninja_units(build), key=lambda unit: -unit[1])
            report.update({'sccache': counts, 'units': len(units),
                           'unit_seconds_total': sum(ms for _, ms in units) / 1000,
                           'slowest_units': [{'output': out, 'seconds': ms / 1000} for out, ms in units[:15]]})
            results['scenarios'][name] = report
            write()
            if report['exit_code']:
                status = 1
                break
        finally:
            if original is not None:
                path.write_bytes(original)
    write()
    return status


def measure_command(command, output):
    code_report = {}
    exit_code = measure(command, output)
    code_report.update(json.loads(output.read_text()))
    code_report['exit_code'] = exit_code
    return code_report


if __name__ == '__main__':
    raise SystemExit(main())
