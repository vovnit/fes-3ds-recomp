"""Offline benchmark comparison checks; no disc or console required."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('scenario', Path(__file__).resolve().parents[1] / 'tools/p3fes_scenario.py')
scenario = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scenario)

class ScenarioTests(unittest.TestCase):
    def test_matched_windows_only(self):
        with tempfile.TemporaryDirectory() as folder:
            a, b = Path(folder)/'a', Path(folder)/'b'
            a.write_text('frame 30 ms_30frames 300\nframe 60 ms_30frames 600\n')
            b.write_text('frame 30 ms_30frames 150\nframe 90 ms_30frames 9000\n')
            result = scenario.compare(a,b)
            self.assertEqual(result['matched_windows'], 1)
            self.assertEqual(result['median_paired_change_percent'], -50)
            self.assertEqual(result['unmatched_after'], 1)

    def test_restarted_log_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            p = Path(folder)/'perf'
            p.write_text('frame 30 ms_30frames 300\nframe 30 ms_30frames 300\n')
            with self.assertRaises(ValueError):
                scenario.windows(p)

if __name__ == '__main__':
    unittest.main()
