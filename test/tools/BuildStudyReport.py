"""Tabulate the reports of BuildStudy.py as Markdown (stdlib only)."""
import json
import statistics
from pathlib import Path
import sys

ORDER = ['cold', 'warm', 'header', 'heavy-test', 'light-test']


def main(directory):
    reports = [json.loads(path.read_text()) for path in sorted(Path(directory).rglob('*-*.json'))]
    # A name may be reported by several runs (repetitions) and by the per-step JSON: keep the last full report of each.
    runs = {}
    for report in reports:
        if 'scenarios' in report:
            runs.setdefault(report['name'], {})[report.get('rep', 1)] = report
    reports = [{'reps': list(by_rep.values()), **next(iter(by_rep.values()))} for by_rep in runs.values()]
    for system in sorted({report['name'].split('-')[0] for report in reports}):
        mine = [report for report in reports if report['name'].startswith(system + '-')]
        print(f'\n### {system}: wall seconds per scenario (clean build of the target)\n')
        print('| variant | target | jobs | configure | ' + ' | '.join(ORDER) + ' | peak RSS MB (cold) |')
        print('|---|---|---|---|' + '---|' * len(ORDER) + '---|')
        for report in mine:
            cells = []
            for name in ORDER:
                scenarios = [rep['scenarios'][name] for rep in report['reps'] if name in rep['scenarios']]
                if not scenarios:
                    cells.append('-')
                    continue
                walls = sorted(scenario['wall_seconds'] for scenario in scenarios)
                text = f"{statistics.median(walls):.0f}" + (' FAIL' if any(scenario['exit_code'] for scenario in scenarios) else '')
                if len(walls) > 1:
                    text += f" [{walls[0]:.0f}-{walls[-1]:.0f}]"
                counts = scenarios[0].get('sccache') or {}
                if counts.get('Compile requests') is not None:
                    text += f" ({counts['Cache hits']}/{counts['Compile requests']})"
                cells.append(text)
            cold = report['scenarios'].get('cold', {})
            rss = cold.get('sampled_peak_family_rss_bytes', 0) / 1e6
            variant = report['name'].split('-', 1)[1]
            print(f"| {variant}{' x' + str(len(report['reps'])) if len(report['reps']) > 1 else ''} | {report['target']} | {report['jobs']} | {report.get('configure_seconds', 0):.0f} | "
                  + ' | '.join(cells) + f' | {rss:.0f} |')
        print('\nCells are the median wall seconds over the repetitions, [min-max] when repeated, then (sccache hits/requests of the first). Cold: empty cache; warm: complete cache; '
              'header/heavy-test/light-test: complete cache, one file changed.\n')
        for report in mine:
            cold = report['scenarios'].get('cold')
            if cold and cold.get('slowest_units') and report['name'].endswith(('-base-j4', '-unity-b4', '-pch')):
                print(f"<details><summary>{report['name']}: slowest units, cold</summary>\n")
                print('| unit | s |\n|---|---|')
                for unit in cold['slowest_units'][:10]:
                    print(f"| {unit['output']} | {unit['seconds']:.0f} |")
                print('\n</details>\n')


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else '.')
