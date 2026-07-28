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
        self._actor_event_node_name = event["node_name"]
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

    def _inspect(self, queries=(), *, cursor=""):
        return json.loads(
            execute_action(
                "UnrealMCPython.blueprint_actions",
                "ue_inspect_blueprint",
                {
                    "asset_path": self._actor_path,
                    "queries": list(queries),
                    "cursor": cursor,
                },
            )
        )

    def _add_custom_event(self, event_name):
        result = call_action(
            "blueprint_actions",
            "ue_add_blueprint_node",
            asset_path=self._actor_path,
            graph_name="EventGraph",
            node_json={"type": "CustomEvent", "event_name": event_name},
        )
        self.assertSuccess(result)

    def _add_connected_print_string(self):
        result = call_action(
            "blueprint_actions",
            "ue_add_blueprint_node",
            asset_path=self._actor_path,
            graph_name="EventGraph",
            node_json={
                "type": "CallFunction",
                "function_name": "PrintString",
                "target": "KismetSystemLibrary",
            },
        )
        self.assertSuccess(result)
        connected = call_action(
            "blueprint_actions",
            "ue_connect_blueprint_pins",
            asset_path=self._actor_path,
            graph_name="EventGraph",
            source_node=self._actor_event_node_name,
            source_pin="then",
            target_node=result["node_name"],
            target_pin="execute",
        )
        self.assertSuccess(connected)
        return result["node_name"]

    def _build_large_event_graph(self):
        nodes = [
            {
                "id": f"pagination_event_{index:03d}",
                "type": "CustomEvent",
                "event_name": f"PaginationEvent{index:03d}",
                "pos_x": (index % 10) * 300,
                "pos_y": (index // 10) * 180,
            }
            for index in range(120)
        ]
        result = call_action(
            "blueprint_actions",
            "ue_build_blueprint_graph",
            asset_path=self._actor_path,
            graph_name="EventGraph",
            graph_structure={"nodes": nodes, "connections": []},
        )
        self.assertSuccess(result)
        self.assertEqual(result["nodes_created"], 120)
        blueprint = unreal.EditorAssetLibrary.load_asset(self._actor_path)
        unreal.BlueprintEditorLibrary.compile_blueprint(blueprint)
        self.assertTrue(unreal.EditorAssetLibrary.save_loaded_asset(blueprint))

    def test_inspect_blueprint(self):
        result = self._inspect(
            [
                {"op": "variables", "name_pattern": "Brief*"},
                {
                    "op": "nodes",
                    "class_path": "/Script/BlueprintGraph.K2Node_CustomEvent",
                },
                {"op": "components", "name_pattern": "Brief*"},
                {"op": "events", "name_pattern": self.ACTOR_EVENT},
            ]
        )

        self.assertSuccess(result)
        self.assertEqual(result["data"]["asset_path"], self._actor_path)
        results = result["data"]["results"]
        self.assertEqual(
            [query_result["op"] for query_result in results],
            ["variables", "nodes", "components", "events"],
        )
        self.assertEqual(results[0]["items"][0]["name"], self.ACTOR_VARIABLE)
        self.assertEqual(results[1]["items"][0]["kind"], "custom_event")
        self.assertEqual(results[2]["items"][0]["name"], self.ACTOR_COMPONENT)
        event = results[3]["items"][0]
        self.assertTrue(event["name"].startswith("K2Node_CustomEvent_"))
        self.assertNotEqual(event["name"], self.ACTOR_EVENT)
        self.assertEqual(event["event_name"], self.ACTOR_EVENT)
        for query_result in results:
            self.assertEqual(query_result["detail"], "compact")
            self.assertEqual(query_result["returned_count"], len(query_result["items"]))
            for record in query_result["items"]:
                for identity_field in ("id", "id_kind", "stable", "kind", "name"):
                    self.assertIn(identity_field, record)

    def test_inspect_empty_queries_defaults_to_compact_overview(self):
        result = self._inspect()

        self.assertSuccess(result)
        query_result = result["data"]["results"][0]
        self.assertEqual(query_result["op"], "overview")
        self.assertEqual(query_result["detail"], "compact")
        self.assertEqual(query_result["returned_count"], 1)
        self.assertNotIn("nodes", query_result["items"][0])

    def test_inspect_each_supported_op(self):
        operations = [
            "overview",
            "variables",
            "variable_defaults",
            "components",
            "component_hierarchy",
            "functions",
            "macros",
            "events",
            "dispatchers",
            "interfaces",
            "nodes",
            "pins",
            "connections",
        ]

        result = self._inspect([{"op": operation, "limit": 1} for operation in operations])

        self.assertSuccess(result)
        results = result["data"]["results"]
        self.assertEqual([query_result["op"] for query_result in results], operations)
        for query_result in results:
            self.assertLessEqual(len(query_result["items"]), 1)
            self.assertIn("total_count", query_result)
            self.assertIn("returned_count", query_result)
            self.assertIn("next_cursor", query_result)
        overview = results[0]["items"][0]
        self.assertNotIn("nodes", overview)
        self.assertNotIn("pins", overview)

    def test_inspect_compact_omits_details(self):
        result = self._inspect(
            [
                {
                    "op": "nodes",
                    "class_path": "/Script/BlueprintGraph.K2Node_CustomEvent",
                    "limit": 1,
                    "detail": "compact",
                }
            ]
        )

        self.assertSuccess(result)
        node = result["data"]["results"][0]["items"][0]
        for detailed_field in (
            "title",
            "position",
            "pins",
            "default",
            "metadata",
            "linked_pin_ids",
        ):
            self.assertNotIn(detailed_field, node)
        self.assertIn("pin_count", node)
        self.assertIn("type", node)

    def test_inspect_detailed_includes_requested_details(self):
        result = self._inspect(
            [
                {
                    "op": "nodes",
                    "class_path": "/Script/BlueprintGraph.K2Node_CustomEvent",
                    "limit": 1,
                    "detail": "detailed",
                }
            ]
        )

        self.assertSuccess(result)
        node = result["data"]["results"][0]["items"][0]
        self.assertIn("title", node)
        self.assertEqual(set(node["position"]), {"x", "y"})
        self.assertGreater(len(node["pins"]), 0)
        for pin in node["pins"]:
            self.assertIn("id", pin)
            self.assertIn("type", pin)
            self.assertIn("default", pin)
            self.assertIn("linked_pin_ids", pin)
            self.assertIn("linked_node_ids", pin)

    def test_inspect_detailed_function_uses_source_metadata(self):
        result = self._inspect(
            [
                {
                    "op": "functions",
                    "name_pattern": self.ACTOR_FUNCTION,
                    "detail": "detailed",
                }
            ]
        )

        self.assertSuccess(result)
        function = result["data"]["results"][0]["items"][0]
        metadata = function["metadata"]
        required = {
            "pure",
            "const",
            "static",
            "access",
            "category",
            "description",
            "inputs",
            "outputs",
        }
        self.assertTrue(required <= set(metadata))
        self.assertIn(metadata["access"], {"public", "protected", "private"})
        for parameter in [*metadata["inputs"], *metadata["outputs"]]:
            self.assertIn("name", parameter)
            self.assertIsInstance(parameter["type"], dict)

    def test_inspect_detailed_pins_include_linked_node_ids(self):
        print_node_name = self._add_connected_print_string()
        nodes = self._inspect(
            [
                {
                    "op": "nodes",
                    "name_pattern": print_node_name,
                    "limit": 1,
                }
            ]
        )
        self.assertSuccess(nodes)
        print_node_id = nodes["data"]["results"][0]["items"][0]["node_id"]

        result = self._inspect(
            [
                {
                    "op": "pins",
                    "node_id": print_node_id,
                    "detail": "detailed",
                }
            ]
        )

        self.assertSuccess(result)
        linked_pins = [
            pin
            for pin in result["data"]["results"][0]["items"]
            if pin["link_count"]
        ]
        self.assertEqual(len(linked_pins), 1)
        self.assertEqual(len(linked_pins[0]["linked_pin_ids"]), 1)
        self.assertEqual(len(linked_pins[0]["linked_node_ids"]), 1)

    def test_inspect_rejects_malformed_filter_constraints(self):
        cases = (
            (
                {"op": "nodes", "graph_id": "not-a-stable-id"},
                "queries[0].graph_id",
            ),
            (
                {"op": "nodes", "member_id": "node:NOT-A-GUID"},
                "queries[0].member_id",
            ),
            (
                {"op": "pins", "node_id": "node:not-a-guid"},
                "queries[0].node_id",
            ),
            (
                {
                    "op": "nodes",
                    "class_path": "/Game/BP_Invalid.BP_Invalid_C",
                },
                "queries[0].class_path",
            ),
        )

        for query, path in cases:
            with self.subTest(path=path):
                result = self._inspect([query])
                self.assertFalse(result["success"])
                self.assertEqual(result["errors"][0]["code"], "INVALID_INPUT")
                self.assertEqual(result["errors"][0]["path"], path)

    def test_inspect_filters_by_graph_member_node_and_kind(self):
        event_result = self._inspect(
            [{"op": "events", "name_pattern": self.ACTOR_EVENT, "limit": 1}]
        )
        self.assertSuccess(event_result)
        event = event_result["data"]["results"][0]["items"][0]
        graph_id = event["graph_id"]
        node_id = event["node_id"]

        result = self._inspect(
            [
                {"op": "nodes", "graph_id": graph_id},
                {"op": "nodes", "member_id": graph_id},
                {"op": "pins", "node_id": node_id},
                {"op": "nodes", "kind": "custom_event"},
            ]
        )

        self.assertSuccess(result)
        graph_nodes, member_nodes, node_pins, event_nodes = result["data"][
            "results"
        ]
        self.assertTrue(graph_nodes["items"])
        self.assertTrue(member_nodes["items"])
        self.assertTrue(
            all(item["graph_id"] == graph_id for item in member_nodes["items"])
        )
        self.assertTrue(node_pins["items"])
        self.assertTrue(
            all(item["kind"] == "custom_event" for item in event_nodes["items"])
        )

    def test_inspect_details_are_independent_per_query(self):
        query = {
            "op": "nodes",
            "class_path": "/Script/BlueprintGraph.K2Node_CustomEvent",
            "limit": 1,
        }
        result = self._inspect(
            [
                {**query, "detail": "compact"},
                {**query, "detail": "detailed"},
            ]
        )

        self.assertSuccess(result)
        compact_result, detailed_result = result["data"]["results"]
        self.assertEqual(compact_result["detail"], "compact")
        self.assertNotIn("pins", compact_result["items"][0])
        self.assertEqual(detailed_result["detail"], "detailed")
        self.assertIn("pins", detailed_result["items"][0])

    def test_inspect_paginates_each_query_independently(self):
        self._build_large_event_graph()
        queries = [{"op": "nodes"}, {"op": "pins", "limit": 1}]

        first = self._inspect(queries)

        self.assertSuccess(first)
        first_results = first["data"]["results"]
        self.assertEqual(len(first_results[0]["items"]), 100)
        self.assertEqual(len(first_results[1]["items"]), 1)
        node_cursor = first_results[0]["next_cursor"]
        pin_cursor = first_results[1]["next_cursor"]
        self.assertTrue(node_cursor)
        self.assertTrue(pin_cursor)
        self.assertNotEqual(node_cursor, pin_cursor)

        second = self._inspect(
            [
                {**queries[0], "cursor": node_cursor},
                {**queries[1], "cursor": pin_cursor},
            ]
        )

        self.assertSuccess(second)
        second_results = second["data"]["results"]
        self.assertTrue(second_results[0]["items"])
        self.assertEqual(len(second_results[1]["items"]), 1)
        first_node_ids = {item["id"] for item in first_results[0]["items"]}
        first_pin_ids = {item["id"] for item in first_results[1]["items"]}
        self.assertTrue(first_node_ids.isdisjoint(
            item["id"] for item in second_results[0]["items"]
        ))
        self.assertTrue(first_pin_ids.isdisjoint(
            item["id"] for item in second_results[1]["items"]
        ))

    def test_inspect_rejects_stale_cursor(self):
        self._add_custom_event("StaleCursorEventA")
        self._add_custom_event("StaleCursorEventB")
        query = {
            "op": "nodes",
            "class_path": "/Script/BlueprintGraph.K2Node_CustomEvent",
            "limit": 1,
        }
        first = self._inspect([query])
        self.assertSuccess(first)
        first_result = first["data"]["results"][0]
        self.assertTrue(first_result["next_cursor"])
        removed = call_action(
            "blueprint_actions",
            "ue_remove_blueprint_node",
            asset_path=self._actor_path,
            graph_name="EventGraph",
            node_name=first_result["items"][0]["name"],
        )
        self.assertSuccess(removed)

        stale = self._inspect(
            [{**query, "cursor": first_result["next_cursor"]}]
        )

        self.assertFalse(stale["success"])
        self.assertEqual(stale["errors"][0]["code"], "INVALID_INPUT")
        self.assertEqual(stale["errors"][0]["path"], "queries[0].cursor")

    def test_inspect_rejects_query_mismatched_cursor(self):
        first = self._inspect([{"op": "nodes", "limit": 1}])
        self.assertSuccess(first)
        cursor = first["data"]["results"][0]["next_cursor"]
        self.assertTrue(cursor)

        mismatched = self._inspect(
            [{"op": "pins", "limit": 1, "cursor": cursor}]
        )

        self.assertFalse(mismatched["success"])
        self.assertEqual(mismatched["errors"][0]["code"], "INVALID_INPUT")
        self.assertEqual(mismatched["errors"][0]["path"], "queries[0].cursor")

    def test_inspect_top_level_cursor_requires_one_query(self):
        first = self._inspect([{"op": "nodes", "limit": 1}])
        self.assertSuccess(first)
        cursor = first["data"]["results"][0]["next_cursor"]
        self.assertTrue(cursor)

        rejected = self._inspect(
            [{"op": "nodes", "limit": 1}, {"op": "pins", "limit": 1}],
            cursor=cursor,
        )

        self.assertFalse(rejected["success"])
        self.assertEqual(rejected["errors"][0]["code"], "INVALID_INPUT")
        self.assertEqual(rejected["errors"][0]["path"], "cursor")

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

    def test_list_callable_functions_only_targets_blueprint_owned_graphs(self):
        local_result = call_action(
            "blueprint_actions",
            "ue_list_callable_functions",
            asset_path=self._actor_path,
            filter=self.ACTOR_FUNCTION,
        )
        self.assertSuccess(local_result)
        local = next(
            item
            for item in local_result["functions"]
            if item["function_name"] == self.ACTOR_FUNCTION
        )
        self.assertTrue(local["targetable"])
        self.assertEqual(local["function_id"], local["stable_id"])
        self.assertRegex(
            local["stable_id"],
            r"^(?:graph:[0-9a-f-]{36}|fallback:graph:[0-9a-f]{40})$",
        )
        self.assertIn(
            local["id_kind"],
            ("graph_guid", "qualified_name_fallback"),
        )
        self.assertEqual(
            local["stable"], local["stable_id"].startswith("graph:")
        )
        if not local["stable"]:
            for field in ("owner_id", "name", "type_path"):
                self.assertTrue(local[field], field)

        native_result = call_action(
            "blueprint_actions",
            "ue_list_callable_functions",
            asset_path=self._actor_path,
            filter="K2_GetActorLocation",
        )
        self.assertSuccess(native_result)
        native = next(
            item
            for item in native_result["functions"]
            if item["function_name"] == "K2_GetActorLocation"
        )
        self.assertFalse(native["targetable"])
        self.assertFalse(native["stable"])
        self.assertEqual(native["id_kind"], "unavailable")
        self.assertNotIn("stable_id", native)
        self.assertNotIn("function_id", native)

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
