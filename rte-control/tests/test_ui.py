#!/usr/bin/env python3
"""Check static GUI contract and run browser behavior with a deterministic DOM."""
from html.parser import HTMLParser
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]

class Page(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids = []
        self.inputs = []
        self.labels = []
        self.forms = []
        self.form_inputs = {}
        self.current_form = None
    def handle_starttag(self, tag, pairs):
        attrs = dict(pairs)
        if "id" in attrs:
            self.ids.append(attrs["id"])
        if tag == "input":
            self.inputs.append(attrs)
            self.form_inputs[self.current_form].append(attrs)
        if tag == "label":
            self.labels.append(attrs.get("for"))
        if tag == "form":
            self.forms.append(attrs.get("id"))
            self.current_form = attrs.get("id")
            self.form_inputs[self.current_form] = []

    def handle_endtag(self, tag):
        if tag == "form":
            self.current_form = None

class GuiTests(unittest.TestCase):
    def test_document_contract(self):
        page = Page()
        page.feed((ROOT / "www/index.html").read_text())
        self.assertEqual(len(page.ids), len(set(page.ids)))
        self.assertEqual(set(page.forms), {"rf-form", *(f"target-{i}-form" for i in range(1, 5))})
        self.assertEqual(len(page.inputs), 24)
        for i in range(1, 5):
            inputs = page.form_inputs[f"target-{i}-form"]
            self.assertEqual({item["name"] for item in inputs}, {
                "enabled", "range_m", "radial_velocity_mps", "gain_linear", "phase_offset_deg"})
            self.assertTrue(all(item["id"].startswith(f"target-{i}-") for item in inputs))
            self.assertIn(f"target-{i}-apply", page.ids)
            self.assertIn(f"target-{i}-state", page.ids)
        self.assertEqual({item["name"] for item in page.inputs}, {
            "enabled", "range_m", "radial_velocity_mps", "gain_linear", "phase_offset_deg",
            "carrier_mhz", "bandwidth_mhz", "tx_gain_db", "rx_gain_db"})
        for item in page.inputs:
            self.assertIn(item["id"], page.labels)
        self.assertNotIn("rf-sample-rate", page.ids)
        self.assertNotIn("apply-all", page.ids)
        self.assertNotIn("target-select", page.ids)

    def test_behavior(self):
        subprocess.run(["node", str(ROOT / "tests/test_ui.js")], check=True)

if __name__ == "__main__":
    unittest.main()
