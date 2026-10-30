"""Tabulate the reports of BuildStudy.py as Markdown (stdlib only)."""
import json
from pathlib import Path
import sys

ORDER = ['cold', 'warm', 'header', 'heavy-test', 'light-test']


def main(directory):
    reports = [json.loads(path.read_text()) for path in sorted(Path(directory).rglob('*-*.json'))]
    reports = [report for report in reports if 'scenarios' in report]
    for system in sorted({report['name'].split('-')[0] for report in reports}):
        mine = [report for report in reports if report['name'].startswith(system + '-')]
        print(f'\n### {system}: wall seconds per scenario (clean build of the target)\n')
        print('| variant | target | jobs | configure | ' + ' | '.join(ORDER) + ' | peak RSS MB (cold) |')
        print('|---|---|---|---|' + '---|' * len(ORDER) + '---|')
        for report in mine:
            cells = []
            for name in ORDER:
                scenario = report['scenarios'].get(name)
                if not scenario:
                    cells.append('-')
                    continue
                text = f"{scenario['wall_seconds']:.0f}" + ('' if not scenario['exit_code'] else ' FAIL')
                counts = scenario.get('sccache') or {}
                if counts.get('Compile requests') is not None:
                    text += f" ({counts['Cache hits']}/{counts['Compile requests']})"
                cells.append(text)
            cold = report['scenarios'].get('cold', {})
            rss = cold.get('sampled_peak_family_rss_bytes', 0) / 1e6
            variant = report['name'].split('-', 1)[1]
            print(f"| {variant} | {report['target']} | {report['jobs']} | {report.get('configure_seconds', 0):.0f} | "
                  + ' | '.join(cells) + f' | {rss:.0f} |')
        print('\nCells are wall seconds, then (sccache hits/requests). Cold: empty cache; warm: complete cache; '
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
