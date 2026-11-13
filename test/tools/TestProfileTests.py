from pathlib import Path
from contextlib import redirect_stderr
import io
import tempfile
import unittest
from ProfileTests import analyze, write_report


class ProfileTests(unittest.TestCase):
    def test_parallel_wall_time_dependencies_and_locks(self):
        graph = {'tests': [
            {'name': 'prepare', 'properties': [{'name': 'FIXTURES_SETUP', 'value': ['ready']}]},
            {'name': 'first', 'properties': [{'name': 'FIXTURES_REQUIRED', 'value': ['ready']},
                                          {'name': 'RESOURCE_LOCK', 'value': ['project']}]},
            {'name': 'second', 'properties': [{'name': 'FIXTURES_REQUIRED', 'value': ['ready']},
                                           {'name': 'RESOURCE_LOCK', 'value': ['project']}]},
            {'name': 'build', 'properties': [{'name': 'DEPENDS', 'value': ['first']},
                                          {'name': 'RUN_SERIAL', 'value': True}]}]}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report = root / 'report.xml'
            report.write_text('<testsuite time="10"><testcase name="prepare" time="1"/>'
                              '<testcase name="first" time="4"/><testcase name="second" time="3"/>'
                              '<testcase name="build" time="2"/></testsuite>')
            result = analyze(graph, report, 2)
            self.assertEqual(result['wall_seconds'], 10)
            self.assertEqual(result['sum_seconds'], 10)
            self.assertEqual(result['dependency_path_seconds'], 7)
            self.assertEqual(result['lower_bound_seconds'], 7)
            self.assertEqual(result['serial_seconds'], 2)
            write_report(result, root / 'profile.txt')
            self.assertIn('"prepare" -> "second"', (root / 'profile.dot').read_text())
            self.assertIn('style=dashed', (root / 'profile.dot').read_text())
            self.assertEqual(result['slowest'][0], ('first', 4))

    def test_dependency_cycles_fail(self):
        graph = {'tests': [{'name': 'a', 'properties': [{'name': 'DEPENDS', 'value': ['a']}]}]}
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / 'report.xml'
            report.write_text('<testsuite time="0"><testcase name="a" time="0"/></testsuite>')
            with self.assertRaisesRegex(ValueError, 'Cyclic'):
                analyze(graph, report, 1)

    def test_phase_names_carry_no_timestamp_or_status(self):
        from ProfileTests import phase_durations
        lines = ('11964.000ms end Life preview and bridge durationMs=8123.500\n'
                 '12.5ms end suite spec status=passed durationMs=3.250\n'
                 'wf spec: 7.0ms end command build durationMs=1.000')
        self.assertEqual(phase_durations([lines, lines]),
                         [(8123.5, 'pass 1: Life preview and bridge'), (3.25, 'pass 1: suite spec'),
                          (1.0, 'pass 1: command build'), (8123.5, 'pass 2: Life preview and bridge'),
                          (3.25, 'pass 2: suite spec'), (1.0, 'pass 2: command build')])

    def test_ninja_durations_rank_compile_and_link_jobs(self):
        from ProfileTests import compile_profile
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / 'ninja-log'
            log.write_text('# ninja log v5\n0\t2000\t0\ta.cpp.o\t123\n500\t1500\t0\tapp\t234\n')
            self.assertEqual(compile_profile(log), [(2, 'a.cpp.o'), (1, 'app')])

    def test_malformed_phase_durations_are_reported_without_losing_other_phases(self):
        from ProfileTests import phase_durations
        lines = ('end valid durationMs=1.250\n'
                 'Developer integration: Developer integration: 20868.300ms end command embed ui/main.tsx --output '
                 'durationMs=4723.50020276.900ms begin command run unawaited\n'
                 'end truncated durationMs=\n'
                 'end invalid durationMs=nan\n'
                 'end suffix durationMs=12.500garbled\n'
                 'end negative durationMs=-3\n'
                 'end overflow durationMs=' + '9' * 400 + '\n'
                 'end next status=passed durationMs=2.000\n')
        diagnostics = io.StringIO()
        with redirect_stderr(diagnostics):
            self.assertEqual(phase_durations([lines]),
                             [(1.25, 'pass 1: valid'), (2, 'pass 1: next')])
        self.assertIn('4723.50020276.900', diagnostics.getvalue())
        self.assertEqual(diagnostics.getvalue().count('Ignoring malformed phase duration'), 6)


if __name__ == '__main__':
    unittest.main()
