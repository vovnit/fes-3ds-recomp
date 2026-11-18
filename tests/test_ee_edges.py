"""Validate branch instrumentation without game-derived fixtures."""
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1]/'tools/experiments/ee_edges.py'
spec = importlib.util.spec_from_file_location('ee_edges',path)
edges = importlib.util.module_from_spec(spec)
spec.loader.exec_module(edges)

class EdgeTests(unittest.TestCase):
    def test_preserves_delay_slot_order(self):
        source = 'L_00001000:\n{ const bool t = x == 0; x += 1; if (t) { goto L_00001010; } }\nL_00001010:\n{ const bool t = x == 2; if (t) { return; } }\n'
        patched, sites = edges.instrument(source)
        self.assertIn('x += 1; if (ee_edge(0, t))',patched)
        self.assertEqual([s['label'] for s in sites],['00001000','00001010'])
        self.assertIn('if (ee_edge(1, t))',patched)
        self.assertEqual(patched.count('const bool t'),2)

    def test_missing_label_rejected(self):
        with self.assertRaises(ValueError):
            edges.instrument('if (t) {}')

    def test_ambiguous_branch_rejected(self):
        with self.assertRaises(ValueError):
            edges.instrument('L_00001000:\nif (t) {} if (t) {}')

if __name__ == '__main__':
    unittest.main()
