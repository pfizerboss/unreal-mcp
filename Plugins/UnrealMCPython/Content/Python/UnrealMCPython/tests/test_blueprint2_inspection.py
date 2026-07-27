"""In-editor tests for bounded Universal Blueprint 2 inspection."""

import json
import unittest
import uuid

import unreal

from UnrealMCPython.tests.base import MCPTestCase
from UnrealMCPython.tests.blueprint2_support import (
    BLUEPRINT2_TEST_ROOT,
    call_action,
)
from UnrealMCPython.mcp_unreal_actions import execute_action


class TestBlueprint2Inspection(MCPTestCase):

    ACTOR_VARIABLE = "BriefEnabled"
    ACTOR_COMPONENT = "BriefLight"
    ACTOR_FUNCTION = "BriefFunction"
    ACTOR_EVENT = "BriefCustomEvent"
    INTERFACE_FUNCTION = "BriefInterfaceFunction"
    MACRO_GRAPH = "BriefMacro"

    def setUp(self):
        self.ensure_test_dir()
        self._created_assets = []
        self._interface_path = None
        self._macro_path = None
        self.addCleanup(self._cleanup_created_assets)

        name = f"Blueprint2Brief_{uuid.uuid4().hex[:10]}"
        factory = unreal.BlueprintFactory()
        factory.set_editor_property("parent_class", unreal.Actor)
        blueprint = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, BLUEPRINT2_TEST_ROOT, unreal.Blueprint, factory
        )
        self.assertIsNotNone(
            blueprint, "Actor Blueprint fixture could not be created"
        )
        self._actor_path = f"{BLUEPRINT2_TEST_ROOT}/{name}.{name}"
        self._created_assets.append(self._actor_path)

        function_graph = unreal.BlueprintEditorLibrary.add_function_graph(
            blueprint, self.ACTOR_FUNCTION
        )
        self.assertIsNotNone(function_graph)
        bool_type = unreal.BlueprintEditorLibrary.get_basic_type_by_name(
            unreal.Name("bool")
        )
        self.assertTrue(
            unreal.BlueprintEditorLibrary.add_member_variable(
                blueprint, unreal.Name(self.ACTOR_VARIABLE), bool_type
            )
        )
        component = call_action(
            "blueprint_actions",
            "ue_add_component_to_blueprint",
            asset_path=self._actor_path,
            component_class_path="/Script/Engine.PointLightComponent",
            component_name=self.ACTOR_COMPONENT,
        )
        self.assertSuccess(component)
        event = call_action(
            "blueprint_actions",
            "ue_add_blueprint_node",
            asset_path=self._actor_path,
            graph_name="EventGraph",
            node_json={"type": "CustomEvent", "event_name": self.ACTOR_EVENT},
        )
        self.assertSuccess(event)
        unreal.BlueprintEditorLibrary.compile_blueprint(blueprint)
        self.assertTrue(unreal.EditorAssetLibrary.save_loaded_asset(blueprint))

        if hasattr(unreal, "BlueprintInterfaceFactory"):
            interface_name = f"Blueprint2Interface_{uuid.uuid4().hex[:10]}"
            interface = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
                interface_name,
                BLUEPRINT2_TEST_ROOT,
                unreal.Blueprint,
                unreal.BlueprintInterfaceFactory(),
            )
            self.assertIsNotNone(interface)
            self._interface_path = (
                f"{BLUEPRINT2_TEST_ROOT}/{interface_name}.{interface_name}"
            )
            self._created_assets.append(self._interface_path)
            interface_graph = unreal.BlueprintEditorLibrary.add_function_graph(
                interface, self.INTERFACE_FUNCTION
            )
            self.assertIsNotNone(interface_graph)
            unreal.BlueprintEditorLibrary.compile_blueprint(interface)
            self.assertTrue(unreal.EditorAssetLibrary.save_loaded_asset(interface))

        if hasattr(unreal, "BlueprintMacroFactory"):
            macro_name = f"Blueprint2Macro_{uuid.uuid4().hex[:10]}"
            macro = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
                macro_name,
                BLUEPRINT2_TEST_ROOT,
                unreal.Blueprint,
                unreal.BlueprintMacroFactory(),
            )
            self.assertIsNotNone(macro)
            self._macro_path = (
                f"{BLUEPRINT2_TEST_ROOT}/{macro_name}.{macro_name}"
            )
            self._created_assets.append(self._macro_path)
            self.assertTrue(unreal.EditorAssetLibrary.save_loaded_asset(macro))

    def tearDown(self):
        self._cleanup_created_assets()

    def _cleanup_created_assets(self):
        if not self._created_assets:
            return
        unreal.SystemLibrary.collect_garbage()
        created_assets = list(reversed(self._created_assets))
        self._created_assets = []
        for asset_path in created_assets:
            with self.subTest(cleanup_asset=asset_path):
                self.delete_asset(asset_path)
                self.assertFalse(
                    unreal.EditorAssetLibrary.does_asset_exist(asset_path)
                )

    @classmethod
    def tearDownClass(cls):
        if not unreal.EditorAssetLibrary.does_directory_exist(
            BLUEPRINT2_TEST_ROOT
        ):
            return
        remaining = unreal.EditorAssetLibrary.list_assets(
            BLUEPRINT2_TEST_ROOT, recursive=True, include_folder=False
        )
        if remaining:
            raise AssertionError(
                f"Blueprint 2 inspection tests left assets behind: {remaining}"
            )
        deleted = unreal.EditorAssetLibrary.delete_directory(
            BLUEPRINT2_TEST_ROOT
        )
        if (
            not deleted
            and unreal.EditorAssetLibrary.does_directory_exist(
                BLUEPRINT2_TEST_ROOT
            )
        ):
            raise AssertionError(
                "Failed to remove empty test root: "
                f"{BLUEPRINT2_TEST_ROOT}"
            )

    def _brief(self, asset_path):
        return json.loads(
            execute_action(
                "UnrealMCPython.blueprint_actions",
                "ue_get_blueprint_brief",
                {"asset_path": asset_path},
            )
        )

    def test_get_blueprint_brief(self):
        result = self._brief(self._actor_path)
        self.assertSuccess(result)
        data = result["data"]
        self.assertEqual(data["asset_path"], self._actor_path)
        self.assertEqual(data["blueprint_class_path"], "/Script/Engine.Blueprint")
        self.assertEqual(data["parent_class_path"], "/Script/Engine.Actor")
        actor_name = self._actor_path.split(".")[-1]
        self.assertTrue(data["generated_class_path"].endswith(f"{actor_name}_C"))
        self.assertIn("SKEL_", data["skeleton_class_path"])
        self.assertEqual(data["compile_status"], "UpToDate")
        self.assertEqual(data["interfaces"], [])
        self.assertEqual(data["top_level_components"], ["DefaultSceneRoot"])
        self.assertCountEqual(
            data["graphs"],
            ["UserConstructionScript", self.ACTOR_FUNCTION, "EventGraph"],
        )
        self.assertEqual(
            data["counts"],
            {
                "variables": 1,
                "components": 2,
                "functions": 2,
                "macros": 0,
                "events": 1,
                "dispatchers": 0,
                "interfaces": 0,
                "graphs": 3,
                "nodes": 6,
            },
        )
        self.assertEqual(data["capabilities"]["api_version"], 2)
        self.assertTrue(data["capabilities"]["k2_schema"])
        self.assertTrue(data["capabilities"]["has_scs"])
        self.assertNotIn("nodes", data)
        self.assertNotIn("pins", data)

        components = call_action(
            "blueprint_actions",
            "ue_list_blueprint_components",
            asset_path=self._actor_path,
        )
        self.assertSuccess(components)
        self.assertCountEqual(
            [item["variable_name"] for item in components["components"]],
            ["DefaultSceneRoot", self.ACTOR_COMPONENT],
        )

    def test_get_blueprint_brief_accepts_interface_blueprint(self):
        if self._interface_path is None:
            self.skipTest("Blueprint interfaces are not exposed in this UE version")

        result = self._brief(self._interface_path)

        self.assertSuccess(result)
        data = result["data"]
        self.assertEqual(data["parent_class_path"], "/Script/CoreUObject.Interface")
        self.assertEqual(data["graphs"], [self.INTERFACE_FUNCTION])
        self.assertEqual(
            data["counts"],
            {
                "variables": 0,
                "components": 0,
                "functions": 1,
                "macros": 0,
                "events": 0,
                "dispatchers": 0,
                "interfaces": 0,
                "graphs": 1,
                "nodes": 1,
            },
        )
        self.assertTrue(data["capabilities"]["k2_schema"])
        self.assertFalse(data["capabilities"]["has_scs"])
        self.assertNotIn("nodes", data)
        self.assertNotIn("pins", data)

    def test_get_blueprint_brief_accepts_empty_macro_library(self):
        if self._macro_path is None:
            self.skipTest(
                "Blueprint macro libraries are not exposed in this UE version"
            )

        result = self._brief(self._macro_path)

        self.assertSuccess(result)
        data = result["data"]
        self.assertEqual(data["parent_class_path"], "/Script/Engine.Actor")
        self.assertEqual(data["graphs"], [])
        self.assertEqual(
            data["counts"],
            {
                "variables": 0,
                "components": 0,
                "functions": 0,
                "macros": 0,
                "events": 0,
                "dispatchers": 0,
                "interfaces": 0,
                "graphs": 0,
                "nodes": 0,
            },
        )
        self.assertFalse(data["capabilities"]["k2_schema"])
        self.assertFalse(data["capabilities"]["has_scs"])
        self.assertNotIn("nodes", data)
        self.assertNotIn("pins", data)

    def test_get_blueprint_brief_counts_created_macro_when_supported(self):
        if self._macro_path is None:
            self.skipTest(
                "Blueprint macro libraries are not exposed in this UE version"
            )
        if not hasattr(unreal.BlueprintEditorLibrary, "add_macro_graph"):
            self.skipTest(
                "No safe macro graph creation API is exposed in this UE version"
            )

        macro = unreal.EditorAssetLibrary.load_asset(self._macro_path)
        self.assertIsNotNone(macro)
        macro_graph = unreal.BlueprintEditorLibrary.add_macro_graph(
            macro, self.MACRO_GRAPH
        )
        self.assertIsNotNone(macro_graph)
        unreal.BlueprintEditorLibrary.compile_blueprint(macro)
        self.assertTrue(unreal.EditorAssetLibrary.save_loaded_asset(macro))

        result = self._brief(self._macro_path)

        self.assertSuccess(result)
        data = result["data"]
        self.assertIn(self.MACRO_GRAPH, data["graphs"])
        self.assertGreater(data["counts"]["macros"], 0)
        self.assertGreater(data["counts"]["graphs"], 0)
        self.assertGreater(data["counts"]["nodes"], 0)
        self.assertNotIn("nodes", data)
        self.assertNotIn("pins", data)

    def test_get_blueprint_brief_accepts_blueprint_subclass(self):
        if not hasattr(unreal, "WidgetBlueprint"):
            self.skipTest("UMG is not enabled")
        name = f"Blueprint2Widget_{uuid.uuid4().hex[:10]}"
        created = call_action(
            "umg_actions",
            "ue_create_widget_blueprint",
            name=name,
            path=BLUEPRINT2_TEST_ROOT,
        )
        self.assertSuccess(created)
        widget_path = f"{BLUEPRINT2_TEST_ROOT}/{name}.{name}"
        self._created_assets.append(widget_path)

        result = self._brief(widget_path)

        self.assertSuccess(result)
        self.assertIn("WidgetBlueprint", result["data"]["blueprint_class_path"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
