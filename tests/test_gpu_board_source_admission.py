"""Offline descriptions and linked markers must never authorize power-on."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('board_blocker_audit', ROOT / 'scripts/check-ios16-gpu-board-source.py')
MOD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MOD)


class BoardSourceAdmission(unittest.TestCase):
    def test_current_tree_has_no_activation(self):
        report = MOD.audit()
        self.assertFalse(any(report['production_activation_calls'].values()))
        self.assertFalse(report['power_only_test_admissible'])
        self.assertFalse(report['hardware_access_performed'])

    def test_callsite_presence_is_not_authority(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / 'ios16').mkdir()
            (root / 'ios16/fake.cpp').write_text('ggbs_init(&s,&up,&io);\ngiulia_gpu_board_source_register(service,&s);\n')
            report = MOD.audit(root)
            self.assertTrue(all(report['production_activation_calls'].values()))
            self.assertFalse(report['upstream_native_contract_verified'])
            self.assertFalse(report['current_xnu_boot_handoff_verified'])
            self.assertFalse(report['power_only_test_admissible'])
            self.assertFalse(report['boot_image_generation_allowed_by_this_audit'])

    def test_json_true_and_ready_strings_are_not_inputs(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / 'ios16').mkdir()
            (root / 'ready.json').write_text('{"power_only_test_admissible":true,"ready":true}')
            (root / 'ios16/fake.cpp').write_text('const char *marker="GPU READY registered=1 granted=1";\n')
            report = MOD.audit(root)
            self.assertFalse(report['power_only_test_admissible'])
            self.assertTrue(report['blockers'])


if __name__ == '__main__':
    unittest.main()
