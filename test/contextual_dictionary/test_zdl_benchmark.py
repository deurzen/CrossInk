import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
CONTEXTUAL = ROOT / "scripts" / "dictionary" / "contextual"
BENCHMARK_PATH = ROOT / "test" / "data" / "contextual" / "zdl-model-benchmark.json"


class ZdlBenchmarkTest(unittest.TestCase):
    def test_transformer_manifest_and_overlay_pin_registry_digest(self):
        manifest = json.loads((CONTEXTUAL / "zdl-dist-model.json").read_text(encoding="utf-8"))
        model = manifest["model"]
        self.assertEqual(model["distribution"], "de-zdl-dist")
        self.assertEqual(model["version"], "4.0.0")
        self.assertEqual(len(model["sha256"]), 64)
        lock = (CONTEXTUAL / "requirements-zdl-dist.lock").read_text(encoding="utf-8")
        self.assertIn(f"{model['url']}#sha256={model['sha256']}", lock)
        self.assertIn("spacy-transformers==1.3.9", lock)
        self.assertIn("torch==2.13.0", lock)

    def test_checked_in_benchmark_supports_static_selection(self):
        report = json.loads(BENCHMARK_PATH.read_text(encoding="utf-8"))
        self.assertEqual(report["schemaVersion"], 1)
        self.assertEqual(report["repeats"], 5)
        models = {model["distribution"]: model for model in report["models"]}
        self.assertEqual(set(models), {"de-zdl-lg", "de-zdl-dist"})
        static = models["de-zdl-lg"]
        transformer = models["de-zdl-dist"]
        self.assertEqual(static["accuracy"], transformer["accuracy"])
        self.assertEqual(static["accuracy"]["contextCorrect"], 13)
        self.assertLess(static["peakRssBytes"], transformer["peakRssBytes"])
        self.assertLess(static["loadSeconds"], transformer["loadSeconds"])
        self.assertLess(
            static["warmCorpusMeanSeconds"],
            transformer["warmCorpusMeanSeconds"],
        )


if __name__ == "__main__":
    unittest.main()
