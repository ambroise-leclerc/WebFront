import contextlib
import io
import json
import unittest
from unittest.mock import patch
from FlakeReport import attempts_to_read, failed_cases, main


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


if __name__ == '__main__':
    unittest.main()
