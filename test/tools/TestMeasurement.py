from pathlib import Path
import json
import sys
import tempfile
import unittest
from MeasureCommand import measure
from CompareCoverage import hits


class MeasurementTests(unittest.TestCase):
    def test_resources_and_failed_command_status_are_retained(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'resources.json'
            self.assertEqual(measure([sys.executable, '-c', 'import time; time.sleep(0.4); raise SystemExit(7)'], output, 0.05), 7)
            report = json.loads(output.read_text())
            self.assertEqual(report['exit_code'], 7)
            self.assertGreaterEqual(report['sampled_peak_processes'], 1)
            self.assertGreater(report['sampled_peak_family_rss_bytes'], 0)

    def test_coverage_checks_counters_not_just_percentages(self):
        with tempfile.TemporaryDirectory() as directory:
            first, second = Path(directory) / 'a', Path(directory) / 'b'
            first.write_text('SF:header.hpp\nDA:1,4\nFNDA:2,f\nBRDA:1,0,0,2\nend_of_record\n')
            second.write_text('SF:header.hpp\nFNDA:2,f\nBRDA:1,0,0,2\nDA:1,4\nend_of_record\n')
            self.assertEqual(hits(first), hits(second))
            second.write_text(second.read_text().replace('DA:1,4', 'DA:1,5'))
            self.assertNotEqual(hits(first), hits(second))

    def test_repeated_source_sections_aggregate_all_counters(self):
        with tempfile.TemporaryDirectory() as directory:
            first, second = Path(directory) / 'a', Path(directory) / 'b'
            first.write_text('SF:a.hpp\nDA:1,4\nFNDA:2,f\nBRDA:1,0,0,-\nend_of_record\n'
                             'SF:a.hpp\nDA:1,3\nFNDA:3,f\nBRDA:1,0,0,2\nend_of_record\n')
            second.write_text('SF:a.hpp\nDA:1,7\nFNDA:5,f\nBRDA:1,0,0,2\nend_of_record\n')
            self.assertEqual(hits(first), hits(second))
            first.write_text(first.read_text().replace('DA:1,4', 'DA:1,5'))
            self.assertNotEqual(hits(first), hits(second))


if __name__ == '__main__':
    unittest.main()
