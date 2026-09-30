"""Safety checks for the offline gate evidence reader (no daemon required)."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


READER = Path(__file__).resolve().parents[1] / "gate_snapshot"


class SnapshotTests(unittest.TestCase):
    def inspect(self, payload):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "shares.v3"
            source.write_bytes(payload)
            source.chmod(0o444)
            result = subprocess.run([str(READER), str(source)], capture_output=True,
                                    text=True, timeout=20)
            self.assertEqual(source.read_bytes(), payload, "source was modified")
            return result

    def test_genesis(self):
        result = self.inspect(b"")
        self.assertEqual(result.returncode, 0, result.stderr)
        evidence = json.loads(result.stdout)
        self.assertEqual(evidence["height"], 0)
        self.assertEqual(evidence["work"], 0)
        self.assertEqual(evidence["ledger"]["accounts"], [])
        self.assertEqual(len(evidence["entries"]), 1)
        self.assertTrue(evidence["entries"][0]["canonical"])

    def test_corrupt_input_fails_without_repairing_source(self):
        for payload in (b"\x80", b"\x80\x00", b"\x80\x00" + bytes(128), b"\xff\xff"):
            with self.subTest(payload=payload):
                result = self.inspect(payload)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")

    def test_missing_source(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "absent"
            result = subprocess.run([str(READER), str(source)], capture_output=True,
                                    timeout=20)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(source.exists())


if __name__ == "__main__":
    unittest.main()
