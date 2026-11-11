import unittest
from FlakeReport import failed_cases


class FlakeReportTests(unittest.TestCase):
    def test_failed_cases_of_a_ctest_log(self):
        log = '\n'.join([
            'desktop\tTest\t2026-11-11T07:40:00Z 643/687 Test #614: DeveloperBrowserIntegration-typescript .....   Passed   23.76 sec',
            'desktop\tTest\t2026-11-11T07:40:01Z 644/687 Test #615: DeveloperBrowserIntegration-jasmine ........***Failed   18.97 sec',
            'desktop\tTest\t2026-11-11T07:40:02Z  64/687 Test #652: JourneyOfflineMissing ......................***Not Run   0.00 sec',
            'desktop\tTest\t2026-11-11T07:40:03Z 503/505 Test #543: TestResourceMeasurement ....................***Timeout  15.34 sec',
            'desktop\tTest\t2026-11-11T07:40:04Z  49/650 Test #160: Scenario: A window that ignores the close request ...***Failed   16.65 sec',
            'desktop\tTest\t2026-11-11T07:40:05Z \t615 - DeveloperBrowserIntegration-jasmine (Failed)      web-integration',
        ])
        self.assertEqual(failed_cases(log), [
            ('DeveloperBrowserIntegration-jasmine', 'Failed'),
            ('TestResourceMeasurement', 'Timeout'),
            ('Scenario: A window that ignores the close request', 'Failed')])

    def test_a_log_without_ctest_has_no_case(self):
        self.assertEqual(failed_cases('ninja: build stopped: subcommand failed.\n'), [])


if __name__ == '__main__':
    unittest.main()
