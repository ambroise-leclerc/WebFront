import contextlib
import io
import json
import unittest
from unittest.mock import patch
import datetime
from FlakeReport import attempts_to_read, failed_cases, main, within_days


class FlakeReportTests(unittest.TestCase):
    def test_failed_cases_of_a_ctest_log(self):
        log = '\n'.join([
            'desktop\tTest\t2026-11-11T07:40:00Z 643/687 Test #614: DeveloperBrowserIntegration-typescript .....   Passed   23.76 sec',
            'desktop\tTest\t2026-11-11T07:40:01Z 644/687 Test #615: DeveloperBrowserIntegration-jasmine ........***Failed   18.97 sec',
            'desktop\tTest\t2026-11-11T07:40:02Z  64/687 Test #652: JourneyOfflineMissing ......................***Not Run   0.00 sec',
            'desktop\tTest\t2026-11-11T07:40:03Z 503/505 Test #543: TestResourceMeasurement ....................***Timeout  15.34 sec',
            'desktop\tTest\t2026-11-11T07:40:04Z  49/650 Test #160: Scenario: A window that ignores the close request ...***Failed   16.65 sec',
            'desktop\tTest\t2026-11-11T07:40:05Z \t615 - DeveloperBrowserIntegration-jasmine (Failed)      web-integration',
            'desktop	Test	2026-11-11T07:40:06Z     Test #612: WebKitGTKAdapter-choose-file ..........***Failed    2.10 sec',
        ])
        self.assertEqual(failed_cases(log), [
            ('DeveloperBrowserIntegration-jasmine', 'Failed'),
            ('TestResourceMeasurement', 'Timeout'),
            ('Scenario: A window that ignores the close request', 'Failed'),
            ('WebKitGTKAdapter-choose-file', 'Failed')])

    def test_the_failed_attempts_of_a_run_that_passed_once_run_again_are_read(self):
        self.assertEqual(attempts_to_read({"conclusion": "success", "attempt": 3}), [1, 2])
        self.assertEqual(attempts_to_read({"conclusion": "failure", "attempt": 2}), [1, 2])
        self.assertEqual(attempts_to_read({"conclusion": "failure", "attempt": 1}), [1])
        self.assertEqual(attempts_to_read({"conclusion": "success", "attempt": 1}), [])
        self.assertEqual(attempts_to_read({"conclusion": "failure"}), [1])
        self.assertEqual(attempts_to_read({"conclusion": "timed_out", "attempt": 1}), [1])
        self.assertEqual(attempts_to_read({"conclusion": "timed_out", "attempt": 2}), [1, 2])

    def test_a_log_without_ctest_has_no_case(self):
        self.assertEqual(failed_cases('ninja: build stopped: subcommand failed.\n'), [])

    def test_logs_of_failed_attempts_are_read_even_after_a_successful_rerun(self):
        run = {"databaseId": 123, "conclusion": "success", "headBranch": "develop", "attempt": 3}

        def github(*arguments):
            if arguments[:2] == ('run', 'list'):
                return json.dumps([run])
            if arguments[0] == 'api':
                return json.dumps({"jobs": [{"id": 456, "name": "Windows", "conclusion": "failure", "steps": []}]})
            if arguments[:2] == ('run', 'view'):
                # Without --attempt, gh returns the latest attempt's successful log, even for an older job ID.
                if '--attempt' not in arguments:
                    return 'Test #1: IntermittentCase ........   Passed  1.00 sec'
                return 'Test #1: IntermittentCase ........***Failed  1.00 sec'
            self.fail(f'Unexpected GitHub command: {arguments}')

        output = io.StringIO()
        with patch('FlakeReport.gh', side_effect=github) as github_call, \
                patch('sys.argv', ['FlakeReport.py']), contextlib.redirect_stdout(output):
            self.assertEqual(main(), 0)

        for attempt in (1, 2):
            github_call.assert_any_call('run', 'view', '--job', '456', '--attempt', str(attempt), '--log')
            self.assertIn(f'Windows Failed (develop, run 123, attempt {attempt} of 3)', output.getvalue())
        self.assertIn('  2  IntermittentCase', output.getvalue())
        self.assertNotIn('Failed jobs without a failed CTest case', output.getvalue())
        self.assertIn('Reruns needed before green, per job', output.getvalue())
        self.assertIn('  2  Windows', output.getvalue())

    def test_a_job_that_timed_out_and_passed_once_run_again_counts_as_a_rerun(self):
        run = {"databaseId": 7, "conclusion": "success", "headBranch": "develop", "attempt": 2}

        def github(*arguments):
            if arguments[:2] == ('run', 'list'):
                return json.dumps([run])
            if arguments[0] == 'api':
                return json.dumps({"jobs": [{"id": 8, "name": "Windows", "conclusion": "timed_out", "steps": []},
                                            {"id": 9, "name": "Linux", "conclusion": "success", "steps": []}]})
            return 'no CTest here'

        output = io.StringIO()
        with patch('FlakeReport.gh', side_effect=github), patch('sys.argv', ['FlakeReport.py']), contextlib.redirect_stdout(output):
            self.assertEqual(main(), 0)
        self.assertIn('  1  Windows', output.getvalue())
        self.assertNotIn('Linux', output.getvalue().split('Reruns needed')[1])

    def test_a_listing_that_hits_its_limit_says_so_and_a_window_is_asked_of_github(self):
        runs = [{"databaseId": n, "conclusion": "success", "headBranch": "develop", "attempt": 1} for n in range(3)]
        listings = []

        def github(*arguments):
            if arguments[:2] == ('run', 'list'):
                listings.append(arguments)
                return json.dumps(runs)
            self.fail(f'Unexpected GitHub command: {arguments}')

        output = io.StringIO()
        with patch('FlakeReport.gh', side_effect=github), patch('sys.argv', ['FlakeReport.py', '--days', '7', '--limit', '3']), \
                contextlib.redirect_stdout(output):
            self.assertEqual(main(), 0)
        self.assertIn('WARNING: the listing hit its limit of 3 runs', output.getvalue())
        self.assertIn('--created', listings[0])
        self.assertTrue(listings[0][listings[0].index('--created') + 1].startswith('>='))

        output = io.StringIO()
        with patch('FlakeReport.gh', side_effect=github), patch('sys.argv', ['FlakeReport.py', '--days', '7']), \
                contextlib.redirect_stdout(output):
            self.assertEqual(main(), 0)
        self.assertNotIn('WARNING', output.getvalue())
        self.assertEqual(listings[1][listings[1].index('--limit') + 1], '1000')

    def test_a_run_that_timed_out_is_read_and_counted_as_failed(self):
        run = {"databaseId": 11, "conclusion": "timed_out", "headBranch": "develop", "attempt": 1}

        def github(*arguments):
            if arguments[:2] == ('run', 'list'):
                return json.dumps([run])
            if arguments[0] == 'api':
                return json.dumps({"jobs": [{"id": 12, "name": "Windows", "conclusion": "timed_out", "steps": []}]})
            return 'Test #1: SlowCase ........***Timeout  60.00 sec'

        output = io.StringIO()
        with patch('FlakeReport.gh', side_effect=github), patch('sys.argv', ['FlakeReport.py']), contextlib.redirect_stdout(output):
            self.assertEqual(main(), 0)
        self.assertIn('1 completed runs of BuildAndTest.yml, 1 failed', output.getvalue())
        self.assertIn('  1  SlowCase', output.getvalue())

    def test_runs_older_than_the_window_are_left_out(self):
        now = datetime.datetime(2026, 11, 20, tzinfo=datetime.timezone.utc)
        self.assertTrue(within_days({"createdAt": "2026-11-14T00:00:00Z"}, 7, now))
        self.assertFalse(within_days({"createdAt": "2026-11-12T00:00:00Z"}, 7, now))
        self.assertTrue(within_days({"createdAt": "2020-01-01T00:00:00Z"}, None, now))
        self.assertTrue(within_days({}, 7, now))


if __name__ == '__main__':
    unittest.main()
