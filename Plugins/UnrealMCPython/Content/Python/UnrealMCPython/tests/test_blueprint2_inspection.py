"""In-editor tests for bounded Universal Blueprint 2 inspection."""

import unittest
import uuid

import unreal

from UnrealMCPython.tests.base import MCPTestCase, TEST_ROOT
from UnrealMCPython.tests.blueprint2_support import call_action


class TestBlueprint2Inspection(MCPTestCase):

    def setUp(self):
        self.ensure_test_dir()
        self._created_assets = []
        name = f"Blueprint2Brief_{uuid.uuid4().hex[:10]}"
        factory = unreal.BlueprintFactory()
        factory.set_editor_property("parent_class", unreal.Actor)
        blueprint = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, TEST_ROOT, unreal.Blueprint, factory
        )
        if blueprint is None:
            self.skipTest("Actor Blueprint fixture could not be created")
        self._actor_path = f"{TEST_ROOT}/{name}.{name}"
        self._created_assets.append(self._actor_path)

    def tearDown(self):
        for asset_path in reversed(self._created_assets):
            self.delete_asset(asset_path)

    def _brief(self, asset_path):
        return call_action(
            "blueprint_actions", "ue_get_blueprint_brief", asset_path=asset_path
        )

    def test_get_blueprint_brief(self):
        result = self._brief(self._actor_path)
        self.assertSuccess(result)
        data = result["data"]
        self.assertEqual(data["asset_path"], self._actor_path)
        for field in (
            "blueprint_class_path",
            "parent_class_path",
            "generated_class_path",
            "skeleton_class_path",
            "compile_status",
            "interfaces",
            "top_level_components",
            "graphs",
            "counts",
            "capabilities",
        ):
            self.assertIn(field, data)
        self.assertTrue(data["parent_class_path"].endswith(".Actor"))
        self.assertIn("EventGraph", data["graphs"])
        for count_name in (
            "variables",
            "components",
            "functions",
            "macros",
            "events",
            "dispatchers",
            "interfaces",
            "graphs",
            "nodes",
        ):
            self.assertIn(count_name, data["counts"])
            self.assertGreaterEqual(data["counts"][count_name], 0)
        self.assertEqual(data["capabilities"]["api_version"], 2)
        self.assertNotIn("nodes", data)
        self.assertNotIn("pins", data)

    def test_get_blueprint_brief_accepts_blueprint_subclass(self):
        if not hasattr(unreal, "WidgetBlueprint"):
            self.skipTest("UMG is not enabled")
        name = f"Blueprint2Widget_{uuid.uuid4().hex[:10]}"
        created = call_action(
            "umg_actions", "ue_create_widget_blueprint", name=name, path=TEST_ROOT
        )
        if not created.get("success"):
            self.skipTest(f"Widget Blueprint fixture unavailable: {created}")
        widget_path = f"{TEST_ROOT}/{name}.{name}"
        self._created_assets.append(widget_path)

        result = self._brief(widget_path)

        self.assertSuccess(result)
        self.assertIn("WidgetBlueprint", result["data"]["blueprint_class_path"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
