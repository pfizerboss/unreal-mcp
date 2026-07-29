"""In-editor acceptance coverage for Blueprint palette actions."""

import uuid

import unreal

from UnrealMCPython.tests.base import MCPTestCase
from UnrealMCPython.tests.blueprint2_support import (
    BLUEPRINT2_TEST_ROOT,
    call_action,
)


class TestBlueprint2Palette(MCPTestCase):

    def setUp(self):
        self.ensure_test_dir()
        self._created_assets = []
        self.addCleanup(self._cleanup_created_assets)

        name = f"Blueprint2Palette_{uuid.uuid4().hex[:10]}"
        factory = unreal.BlueprintFactory()
        factory.set_editor_property("parent_class", unreal.Actor)
        self.blueprint = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name,
            BLUEPRINT2_TEST_ROOT,
            unreal.Blueprint,
            factory,
        )
        self.assertIsNotNone(self.blueprint)
        self.asset_path = f"{BLUEPRINT2_TEST_ROOT}/{name}.{name}"
        self._created_assets.append(self.asset_path)
        self.graph_id = self._event_graph_id()

    def tearDown(self):
        self._cleanup_created_assets()

    def _cleanup_created_assets(self):
        if not self._created_assets:
            return
        unreal.SystemLibrary.collect_garbage()
        created_assets = list(reversed(self._created_assets))
        self._created_assets = []
        for asset_path in created_assets:
            self.delete_asset(asset_path)

    def _event_graph_id(self):
        result = call_action(
            "blueprint_actions",
            "ue_inspect_blueprint",
            asset_path=self.asset_path,
            queries=[{"op": "events", "detail": "compact", "limit": 100}],
        )
        self.assertSuccess(result)
        graph_ids = {
            item["graph_id"]
            for item in result["data"]["results"][0]["items"]
            if item.get("graph_id", "").startswith("graph:")
        }
        self.assertEqual(len(graph_ids), 1, graph_ids)
        return next(iter(graph_ids))

    def test_palette_search_describe_spawn_and_suggest(self):
        searched = call_action(
            "blueprint_actions",
            "ue_search_blueprint_node_actions",
            asset_path=self.asset_path,
            graph_id=self.graph_id,
            query="Get Actor Location",
            filters={"action_kinds": ["function"]},
            limit=50,
        )
        self.assertSuccess(searched)
        self.assertTrue(searched["data"]["items"], searched)
        action = searched["data"]["items"][0]

        described = call_action(
            "blueprint_actions",
            "ue_describe_blueprint_node_action",
            action_id=action["action_id"],
        )
        self.assertSuccess(described)
        self.assertEqual(described["data"]["action_id"], action["action_id"])

        spawned = call_action(
            "blueprint_actions",
            "ue_add_blueprint_action_node",
            asset_path=self.asset_path,
            graph_id=self.graph_id,
            action_id=action["action_id"],
            position={"x": 320, "y": 160},
            bindings=[
                binding["binding_id"] for binding in action["bindings"]
            ],
        )
        self.assertSuccess(spawned)
        self.assertTrue(spawned["data"]["node_id"].startswith("node:"))
        self.assertTrue(spawned["data"]["pin_ids"], spawned)

        suggested = call_action(
            "blueprint_actions",
            "ue_suggest_blueprint_nodes_for_pin",
            asset_path=self.asset_path,
            graph_id=self.graph_id,
            pin_id=spawned["data"]["pin_ids"][0],
            query="",
            limit=50,
        )
        self.assertSuccess(suggested)
        self.assertEqual(
            suggested["data"]["source_pin_id"],
            spawned["data"]["pin_ids"][0],
        )
