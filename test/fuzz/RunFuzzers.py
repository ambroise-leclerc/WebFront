"""Run every decoder with bounded concurrency and independent findings/corpora."""
import argparse
import concurrent.futures
import os
from pathlib import Path
import shutil
import subprocess

DECODERS = ('messages', 'http', 'websocket', 'gzip', 'gzip_differential')


def run_all(root, jobs, seconds, run=subprocess.call):
    if jobs < 1 or seconds < 1:
        raise ValueError('jobs and seconds must be positive')
    artifacts = root / 'artifacts'
    artifacts.mkdir(parents=True, exist_ok=True)
    # Differential decoding shares the seeds, never a writable corpus, with gzip.
    shutil.copytree(root / 'corpus' / 'gzip', root / 'corpus' / 'gzip_differential', dirs_exist_ok=True)

    def decoder(name):
        command = [str(root / ('fuzz_' + name)), str(root / 'corpus' / name),
                   f'-max_total_time={seconds}', '-timeout=5', '-max_len=65536', '-rss_limit_mb=1024',
                   f'-artifact_prefix={artifacts / name}-']
        with (artifacts / (name + '.log')).open('w') as log:
            status = run(command, stdout=log, stderr=subprocess.STDOUT)
        print(f'{name}: exit {status}', flush=True)
        return status

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(decoder, DECODERS))
    return int(any(results))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--jobs', type=int, default=min(2, os.cpu_count() or 1),
                        help='one GiB RSS budget per worker; default at most two')
    parser.add_argument('--seconds', type=int, default=30)
    args = parser.parse_args()
    raise SystemExit(run_all(args.root.resolve(), args.jobs, args.seconds))
