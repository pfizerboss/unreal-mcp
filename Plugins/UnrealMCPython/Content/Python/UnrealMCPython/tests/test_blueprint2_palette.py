"""In-editor acceptance coverage for Blueprint palette actions."""

import json
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
        self.standard_macros = unreal.load_asset(
            "/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"
        )
        self.assertIsNotNone(self.standard_macros)
        self.graph_id = self._event_graph_id()

    def tearDown(self):
        self._cleanup_created_assets()

    @classmethod
    def tearDownClass(cls):
        if not unreal.EditorAssetLibrary.does_directory_exist(
            BLUEPRINT2_TEST_ROOT
        ):
            return
        remaining = unreal.EditorAssetLibrary.list_assets(
            BLUEPRINT2_TEST_ROOT,
            recursive=True,
            include_folder=False,
        )
        if remaining:
            raise AssertionError(
                f"Blueprint palette tests left assets behind: {remaining}"
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
                "Failed to remove empty Blueprint palette test root: "
                f"{BLUEPRINT2_TEST_ROOT}"
            )

    def _cleanup_created_assets(self):
        if not self._created_assets:
            return
        unreal.SystemLibrary.collect_garbage()
        created_assets = list(reversed(self._created_assets))
        self._created_assets = []
        for asset_path in created_assets:
            with self.subTest(cleanup_asset=asset_path):
                self.delete_asset(asset_path)

    def _inspect(self, queries):
        result = call_action(
            "blueprint_actions",
            "ue_inspect_blueprint",
            asset_path=self.asset_path,
            queries=queries,
        )
        self.assertSuccess(result)
        return result["data"]["results"]

    def _event_graph_id(self):
        events = self._inspect(
            [{"op": "events", "detail": "compact", "limit": 100}]
        )[0]
        graph_ids = {
            item["graph_id"]
            for item in events["items"]
            if item.get("graph_id", "").startswith("graph:")
        }
        self.assertEqual(len(graph_ids), 1, graph_ids)
        return next(iter(graph_ids))

    def _search(self, query="", *, filters=None, cursor="", limit=50):
        return call_action(
            "blueprint_actions",
            "ue_search_blueprint_node_actions",
            asset_path=self.asset_path,
            graph_id=self.graph_id,
            query=query,
            filters=filters or {},
            cursor=cursor,
            limit=limit,
        )

    def _search_all(self, query="", *, filters=None):
        items = []
        cursor = ""
        for _ in range(50):
            result = self._search(
                query,
                filters=filters,
                cursor=cursor,
                limit=200,
            )
            self.assertSuccess(result)
            items.extend(result["data"]["items"])
            cursor = result["data"]["next_cursor"]
            if not cursor:
                return items
        self.fail("Palette search exceeded 50 bounded pages.")

    def _find_action(self, query, predicate=lambda item: True):
        candidates = self._search_all(query)
        matches = [item for item in candidates if predicate(item)]
        diagnostics = [
            (
                item["title"],
                item["action_kind"],
                item["node_class_path"],
                item["owner_path"],
                item["member_path"],
            )
            for item in candidates
        ]
        self.assertTrue(matches, (query, diagnostics))
        return matches[0]

    def _spawn(self, action, x, y):
        return call_action(
            "blueprint_actions",
            "ue_add_blueprint_action_node",
            asset_path=self.asset_path,
            graph_id=self.graph_id,
            action_id=action["action_id"],
            position={"x": x, "y": y},
            bindings=[
                item["binding_id"] for item in action["bindings"]
            ],
        )

    def _suggest(self, pin_id, query="", *, cursor="", limit=50):
        return call_action(
            "blueprint_actions",
            "ue_suggest_blueprint_nodes_for_pin",
            asset_path=self.asset_path,
            graph_id=self.graph_id,
            pin_id=pin_id,
            query=query,
            cursor=cursor,
            limit=limit,
        )

    def _node(self, node_id):
        nodes = self._inspect(
            [
                {
                    "op": "nodes",
                    "member_id": node_id,
                    "detail": "detailed",
                }
            ]
        )[0]["items"]
        self.assertEqual(len(nodes), 1, nodes)
        return nodes[0]

    def _pins(self, node_id):
        return self._inspect(
            [
                {
                    "op": "pins",
                    "node_id": node_id,
                    "detail": "detailed",
                    "limit": 500,
                }
            ]
        )[0]["items"]

    def _snapshot(self):
        result = call_action(
            "blueprint_actions",
            "ue_snapshot_blueprint_graph",
            asset_path=self.asset_path,
            graph_ids=[self.graph_id],
        )
        self.assertSuccess(result)
        return result["data"]

    def _snapshot_text(self):
        return json.dumps(
            self._snapshot(), sort_keys=True, separators=(",", ":")
        )

    def _compile(self):
        result = call_action(
            "blueprint_actions",
            "ue_compile_blueprint",
            asset_path=self.asset_path,
        )
        self.assertSuccess(result)
        return result

    def _health(self):
        result = call_action(
            "blueprint_actions",
            "ue_get_blueprint_health",
            asset_path=self.asset_path,
            include_warnings=True,
        )
        self.assertSuccess(result)
        return result

    def _is_dirty(self):
        package_name = self.blueprint.get_outer().get_name()
        return any(
            package.get_name() == package_name
            for package in unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages()
        )

    def _begin_workflow(self, description):
        transaction_id = f"mcp_palette_{uuid.uuid4().hex}"
        begun = call_action(
            "workflow_actions",
            "ue_begin_transaction",
            transaction_id=transaction_id,
            description=description,
            total_steps=1,
            show_dialog=False,
        )
        self.assertSuccess(begun)
        return transaction_id

    def _rollback_workflow(self, transaction_id):
        result = call_action(
            "workflow_actions",
            "ue_rollback_transaction",
            transaction_id=transaction_id,
        )
        self.assertSuccess(result)

    def _execute_palette_step(self, transaction_id, action, x, y):
        return call_action(
            "workflow_actions",
            "ue_execute_step",
            transaction_id=transaction_id,
            action_module="UnrealMCPython.blueprint_actions",
            action_name="ue_add_blueprint_action_node",
            params={
                "asset_path": self.asset_path,
                "graph_id": self.graph_id,
                "action_id": action["action_id"],
                "position": {"x": x, "y": y},
                "bindings": [
                    item["binding_id"] for item in action["bindings"]
                ],
            },
        )

    def _close_active_workflow(self, transaction_id):
        context = call_action(
            "workflow_actions", "ue_get_editor_context", asset_paths=[]
        )["workflow_transaction"]
        if context["active"]:
            self._rollback_workflow(transaction_id)

    @staticmethod
    def _is_function(action):
        return (
            action["action_kind"] == "function"
            and action["member_path"].endswith(":K2_GetActorLocation")
            and not action["requires_binding"]
        )

    @staticmethod
    def _is_macro(action):
        return (
            action["action_kind"] == "macro"
            and action["node_class_path"].endswith("K2Node_MacroInstance")
            and not action["requires_binding"]
        )

    @staticmethod
    def _is_cast(action):
        return (
            action["action_kind"] == "cast"
            and action["node_class_path"].endswith("K2Node_DynamicCast")
            and action["owner_path"] == "/Script/Engine.Actor"
            and not action["requires_binding"]
        )

    @staticmethod
    def _is_delay(action):
        return (
            action["member_path"].endswith(
                ".KismetSystemLibrary:Delay"
            )
            and not action["requires_binding"]
        )

    def test_search_is_deterministic_paginated_and_context_bound(self):
        first = self._search("", limit=1)
        repeated = self._search("", limit=1)
        self.assertSuccess(first)
        self.assertSuccess(repeated)
        self.assertEqual(first["data"]["items"], repeated["data"]["items"])
        self.assertEqual(
            first["data"]["result_digest"],
            repeated["data"]["result_digest"],
        )
        self.assertEqual(first["data"]["returned_count"], 1)
        self.assertTrue(first["data"]["next_cursor"], first)

        second = self._search(
            "", cursor=first["data"]["next_cursor"], limit=1
        )
        self.assertSuccess(second)
        self.assertNotEqual(
            first["data"]["items"][0]["action_id"],
            second["data"]["items"][0]["action_id"],
        )

        rebound = self._search(
            "Actor", cursor=first["data"]["next_cursor"], limit=1
        )
        self.assertFalse(rebound["success"], rebound)
        self.assertEqual(rebound["errors"][0]["code"], "PRECONDITION_FAILED")

    def test_describe_is_read_only_and_rejects_tampering(self):
        action = self._find_action("Get Actor Location", self._is_function)
        before = self._snapshot_text()
        described = call_action(
            "blueprint_actions",
            "ue_describe_blueprint_node_action",
            action_id=action["action_id"],
        )
        self.assertSuccess(described)
        self.assertEqual(described["data"]["action_id"], action["action_id"])
        self.assertEqual(described["data"]["graph_id"], self.graph_id)
        self.assertEqual(self._snapshot_text(), before)

        action_id = action["action_id"]
        replacement = "0" if action_id[-1] != "0" else "1"
        tampered = call_action(
            "blueprint_actions",
            "ue_describe_blueprint_node_action",
            action_id=action_id[:-1] + replacement,
        )
        self.assertFalse(tampered["success"], tampered)
        self.assertEqual(tampered["errors"][0]["code"], "INVALID_INPUT")
        self.assertEqual(self._snapshot_text(), before)

    def test_spawn_function_event_macro_cast_and_latent_families(self):
        families = (
            (
                "function",
                self._find_action("Get Actor Location", self._is_function),
                (320, 160),
                "K2Node_CallFunction",
            ),
            (
                "event",
                self._find_action(
                    "End Play",
                    lambda item: item["action_kind"] == "event"
                    and item["member_path"].endswith(":ReceiveEndPlay"),
                ),
                (640, 160),
                "K2Node_Event",
            ),
            (
                "macro",
                self._find_action("For Each Loop", self._is_macro),
                (960, 160),
                "K2Node_MacroInstance",
            ),
            (
                "cast",
                self._find_action("Cast To Actor", self._is_cast),
                (1280, 160),
                "K2Node_DynamicCast",
            ),
            (
                "latent",
                self._find_action("Delay", self._is_delay),
                (1600, 160),
                "K2Node_CallFunction",
            ),
        )
        for family, action, position, class_suffix in families:
            with self.subTest(family=family):
                result = self._spawn(action, *position)
                self.assertSuccess(result)
                self.assertTrue(
                    result["data"]["class_path"].endswith(class_suffix),
                    result,
                )
                self.assertEqual(result["data"]["position"], {
                    "x": position[0],
                    "y": position[1],
                })

    def test_spawn_returns_inspectable_stable_nodes_and_pins(self):
        action = self._find_action("Get Actor Location", self._is_function)
        spawned = self._spawn(action, 320, 160)
        self.assertSuccess(spawned)
        data = spawned["data"]
        self.assertTrue(data["node_id"].startswith("node:"), data)
        self.assertTrue(data["pin_ids"], data)
        self.assertEqual(
            data["pin_ids"], [pin["id"] for pin in data["pins"]]
        )
        self.assertTrue(
            all(pin_id.startswith("pin:") for pin_id in data["pin_ids"])
        )

        inspected = self._node(data["node_id"])
        self.assertEqual(inspected["node_id"], data["node_id"])
        self.assertEqual(inspected["class_path"], data["class_path"])
        self.assertEqual(inspected["position"], data["position"])
        self.assertEqual(
            {pin["pin_id"] for pin in inspected["pins"]},
            set(data["pin_ids"]),
        )
        self.assertEqual(spawned["changes"][0]["target_id"], data["node_id"])

    def test_pin_suggestions_differ_by_pin_type_and_direction(self):
        delay = self._find_action("Delay", self._is_delay)
        delay_node = self._spawn(delay, 320, 160)
        self.assertSuccess(delay_node)
        pins = delay_node["data"]["pins"]
        exec_inputs = [
            pin
            for pin in pins
            if pin["direction"] == "input" and pin["type"]["kind"] == "exec"
        ]
        exec_outputs = [
            pin
            for pin in pins
            if pin["direction"] == "output" and pin["type"]["kind"] == "exec"
        ]
        data_inputs = [
            pin
            for pin in pins
            if pin["direction"] == "input" and pin["type"]["kind"] != "exec"
        ]
        self.assertTrue(exec_inputs, pins)
        self.assertTrue(exec_outputs, pins)
        self.assertTrue(data_inputs, pins)
        exec_input = exec_inputs[0]
        exec_output = exec_outputs[0]
        duration_input = data_inputs[0]

        input_actions = self._suggest(exec_input["id"], limit=100)
        output_actions = self._suggest(exec_output["id"], limit=100)
        typed_actions = self._suggest(duration_input["id"], limit=100)
        for result, source_pin in (
            (input_actions, exec_input),
            (output_actions, exec_output),
            (typed_actions, duration_input),
        ):
            self.assertSuccess(result)
            self.assertEqual(
                result["data"]["source_pin_id"], source_pin["id"]
            )
            self.assertTrue(result["data"]["items"], result)
        input_ids = {
            item["action_id"] for item in input_actions["data"]["items"]
        }
        output_ids = {
            item["action_id"] for item in output_actions["data"]["items"]
        }
        typed_ids = {
            item["action_id"] for item in typed_actions["data"]["items"]
        }
        self.assertNotEqual(input_ids, output_ids)
        self.assertNotEqual(typed_ids, output_ids)

        branches = self._suggest(exec_output["id"], "Branch", limit=200)
        self.assertSuccess(branches)
        branch = next(
            item
            for item in branches["data"]["items"]
            if item["node_class_path"].endswith("K2Node_IfThenElse")
        )
        branch_node = self._spawn(branch, 720, 160)
        self.assertSuccess(branch_node)
        branch_input = next(
            pin
            for pin in branch_node["data"]["pins"]
            if pin["direction"] == "input" and pin["type"] == exec_output["type"]
        )
        connected = call_action(
            "blueprint_actions",
            "ue_connect_blueprint_pins",
            asset_path=self.asset_path,
            graph_name="EventGraph",
            source_node=delay_node["data"]["node_id"],
            source_pin=exec_output["id"],
            target_node=branch_node["data"]["node_id"],
            target_pin=branch_input["id"],
        )
        self.assertSuccess(connected)

    def test_palette_spawn_requires_explicit_compile_and_never_saves(self):
        self._compile()
        self.assertEqual(self._health()["data"]["compile_status"], "UpToDate")
        self.assertTrue(self._is_dirty())

        action = self._find_action("Get Actor Location", self._is_function)
        spawned = self._spawn(action, 320, 160)
        self.assertSuccess(spawned)
        self.assertIsNot(spawned.get("saved"), True)
        self.assertIsNot(spawned["data"].get("saved"), True)
        self.assertTrue(self._is_dirty())
        self.assertEqual(
            [item["action"] for item in spawned["next_actions"]],
            ["compile_blueprint"],
        )

        self._compile()
        health = self._health()
        self.assertEqual(health["data"]["compile_status"], "UpToDate")
        self.assertTrue(health["data"]["healthy"], health)
        self.assertTrue(self._is_dirty())

    def test_palette_spawn_rolls_back_with_outer_workflow(self):
        action = self._find_action("Get Actor Location", self._is_function)
        before = self._snapshot_text()
        transaction_id = self._begin_workflow("Spawn Blueprint palette node")
        try:
            spawned = self._execute_palette_step(
                transaction_id, action, 320, 160
            )
            self.assertSuccess(spawned)
            self.assertNotEqual(self._snapshot_text(), before)
            self._rollback_workflow(transaction_id)
            self.assertEqual(self._snapshot_text(), before)
        finally:
            self._close_active_workflow(transaction_id)

    def test_palette_workflow_stress_12_of_12(self):
        families = (
            ("Get Actor Location", self._is_function),
            ("For Each Loop", self._is_macro),
            ("Cast To Actor", self._is_cast),
        )
        completed = 0
        for index in range(12):
            query, predicate = families[index % len(families)]
            action = self._find_action(query, predicate)
            before = self._snapshot_text()
            transaction_id = self._begin_workflow(
                f"Palette stress cycle {index + 1}"
            )
            try:
                spawned = self._execute_palette_step(
                    transaction_id,
                    action,
                    320 + (index % 4) * 320,
                    160 + (index // 4) * 240,
                )
                self.assertSuccess(spawned)
                self.assertNotEqual(self._snapshot_text(), before)
                self._rollback_workflow(transaction_id)
                self.assertEqual(self._snapshot_text(), before)
                completed += 1
            finally:
                self._close_active_workflow(transaction_id)
        print(f"Palette workflow stress: {completed}/12")
        self.assertEqual(completed, 12)

    def test_available_plugin_defined_action_round_trip(self):
        actions = [
            action
            for action in self._search_all("Ability")
            if (
                action["owner_path"].startswith("/Script/GameplayAbilities")
                or action["node_class_path"].startswith(
                    "/Script/GameplayAbilities"
                )
            )
        ]
        if not actions:
            self.skipTest(
                "Actor EventGraph has no compatible GameplayAbilities "
                "plugin-defined palette action."
            )

        failures = []
        for index, action in enumerate(actions):
            described = call_action(
                "blueprint_actions",
                "ue_describe_blueprint_node_action",
                action_id=action["action_id"],
            )
            self.assertSuccess(described)
            spawned = self._spawn(action, 320 + index * 80, 160)
            if spawned.get("success"):
                self.assertTrue(spawned["data"]["node_id"].startswith("node:"))
                self._node(spawned["data"]["node_id"])
                return
            failures.append(spawned)
        self.fail(
            "GameplayAbilities actions were palette-compatible but none "
            f"completed describe/spawn round-trip: {failures}"
        )
