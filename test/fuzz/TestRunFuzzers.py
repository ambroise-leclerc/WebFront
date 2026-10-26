import concurrent.futures
from pathlib import Path
import tempfile
import threading
import unittest
from RunFuzzers import DECODERS, run_all


class FuzzRunnerTests(unittest.TestCase):
    def test_failure_does_not_skip_other_decoders_and_resources_are_isolated(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'corpus/gzip').mkdir(parents=True)
            (root / 'corpus/gzip/seed').write_bytes(b'seed')
            commands = []
            lock = threading.Lock()
            barrier = threading.Barrier(2)
            def run(command, **kwargs):
                with lock:
                    commands.append(command)
                    index = len(commands)
                if index <= 2:
                    barrier.wait(timeout=5)
                return 1 if command[0].endswith('fuzz_http') else 0
            self.assertEqual(run_all(root, 2, 30, run), 1)
            self.assertEqual(len(commands), len(DECODERS))
            self.assertEqual(len({command[1] for command in commands}), len(DECODERS))
            self.assertEqual(len({command[-1] for command in commands}), len(DECODERS))
            self.assertEqual((root / 'corpus/gzip_differential/seed').read_bytes(), b'seed')
            self.assertEqual(len(list((root / 'artifacts').glob('*.log'))), len(DECODERS))

    def test_invalid_bound_is_rejected(self):
        with self.assertRaises(ValueError):
            run_all(Path('.'), 0, 30)


if __name__ == '__main__':
    unittest.main()
