"""Real in-editor acceptance for semantic Blueprint graph actions."""

import json
import uuid

import unreal

from UnrealMCPython.tests.base import MCPTestCase
from UnrealMCPython.tests.blueprint2_support import (
    BLUEPRINT2_TEST_ROOT,
    call_action,
)


class TestBlueprint2Semantic(MCPTestCase):
    created_assets = []

    def setUp(self):
        unreal.EditorAssetLibrary.make_directory(BLUEPRINT2_TEST_ROOT)
        self._created_assets = []
        self.addCleanup(self._cleanup_created_assets)

        name = f"Blueprint2Semantic_{uuid.uuid4().hex[:10]}"
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
        type(self).created_assets.append(self.asset_path)
        self.graph_id = self._event_graph_id()

    def tearDown(self):
        self._cleanup_created_assets()

    @classmethod
    def tearDownClass(cls):
        for asset_path in reversed(cls.created_assets):
            if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
                unreal.EditorAssetLibrary.delete_asset(asset_path)
        cls.created_assets = []
        if not unreal.EditorAssetLibrary.does_directory_exist(
            BLUEPRINT2_TEST_ROOT
        ):
            return
        leftovers = unreal.EditorAssetLibrary.list_assets(
            BLUEPRINT2_TEST_ROOT,
            recursive=True,
            include_folder=False,
        )
        if leftovers:
            raise AssertionError(f"Semantic test assets remain: {leftovers}")
        unreal.EditorAssetLibrary.delete_directory(BLUEPRINT2_TEST_ROOT)

    def _cleanup_created_assets(self):
        if not self._created_assets:
            return
        unreal.SystemLibrary.collect_garbage()
        assets = list(reversed(self._created_assets))
        self._created_assets = []
        for asset_path in assets:
            self.delete_asset(asset_path)
            if asset_path in type(self).created_assets:
                type(self).created_assets.remove(asset_path)

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

    @staticmethod
    def _pin_id(pin):
        return pin.get("pin_id", pin.get("id", ""))

    def _graph_nodes(self):
        return self._inspect(
            [
                {
                    "op": "nodes",
                    "graph_id": self.graph_id,
                    "detail": "detailed",
                    "limit": 500,
                }
            ]
        )[0]["items"]

    def _node(self, node_id):
        items = self._inspect(
            [
                {
                    "op": "nodes",
                    "member_id": node_id,
                    "detail": "detailed",
                }
            ]
        )[0]["items"]
        self.assertEqual(len(items), 1, items)
        return items[0]

    def _event_exec_output(self):
        for node in self._graph_nodes():
            for pin in node.get("pins", []):
                if (
                    pin["direction"] == "output"
                    and pin["type"]["kind"] == "exec"
                ):
                    return node["node_id"], pin
        self.fail("EventGraph exposes no stable execution output pin.")

    def _search_all(self, query=""):
        items = []
        cursor = ""
        for _ in range(50):
            result = call_action(
                "blueprint_actions",
                "ue_search_blueprint_node_actions",
                asset_path=self.asset_path,
                graph_id=self.graph_id,
                query=query,
                filters={},
                cursor=cursor,
                limit=200,
            )
            self.assertSuccess(result)
            items.extend(result["data"]["items"])
            cursor = result["data"]["next_cursor"]
            if not cursor:
                return items
        self.fail("Semantic palette search exceeded 50 bounded pages.")

    def _find_action(self, query, predicate):
        items = self._search_all(query)
        matches = [item for item in items if predicate(item)]
        self.assertTrue(matches, (query, items))
        return matches[0]

    @staticmethod
    def _is_print_string(action):
        return (
            action["member_path"].endswith(":PrintString")
            and not action["requires_binding"]
        )

    @staticmethod
    def _is_branch(action):
        return action["node_class_path"].endswith("K2Node_IfThenElse")

    def _print_string_action(self):
        return self._find_action("Print String", self._is_print_string)

    def _spawn(self, action, x=480, y=160):
        result = call_action(
            "blueprint_actions",
            "ue_add_blueprint_action_node",
            asset_path=self.asset_path,
            graph_id=self.graph_id,
            action_id=action["action_id"],
            position={"x": x, "y": y},
            bindings=[item["binding_id"] for item in action["bindings"]],
        )
        self.assertSuccess(result)
        self.assertIsNot(result["data"].get("saved"), True)
        return result

    def _suggest_pin(self, pin_id, query="", limit=200):
        result = call_action(
            "blueprint_actions",
            "ue_suggest_blueprint_nodes_for_pin",
            asset_path=self.asset_path,
            graph_id=self.graph_id,
            pin_id=pin_id,
            query=query,
            cursor="",
            limit=limit,
        )
        self.assertSuccess(result)
        return result

    def _suggest_connection(
        self,
        source_pin_id,
        target_pin_id,
        query="",
        allow_conversion=False,
        cursor="",
        limit=200,
    ):
        result = call_action(
            "blueprint_actions",
            "ue_suggest_blueprint_nodes_for_connection",
            asset_path=self.asset_path,
            graph_id=self.graph_id,
            source_pin_id=source_pin_id,
            target_pin_id=target_pin_id,
            query=query,
            filters={},
            allow_conversion=allow_conversion,
            cursor=cursor,
            limit=limit,
        )
        self.assertSuccess(result)
        return result

    def _find_connection_suggestion(
        self,
        source_pin_id,
        target_pin_id,
        query,
        action_predicate,
        allow_conversion=False,
    ):
        cursor = ""
        for _ in range(50):
            suggestion = self._suggest_connection(
                source_pin_id,
                target_pin_id,
                query,
                allow_conversion,
                cursor,
            )
            for action in suggestion["data"]["items"]:
                if action_predicate(action) and action["binding_pairs"]:
                    return suggestion, action, action["binding_pairs"][0]
            cursor = suggestion["data"]["next_cursor"]
            if not cursor:
                break
        self.fail(
            f"No connection action matched {query!r} across all cursor pages."
        )

    def _find_pin_suggestion(self, pin_id, query, action_predicate):
        suggestion = self._suggest_pin(pin_id, query)
        for action in suggestion["data"]["items"]:
            if not action_predicate(action):
                continue
            for binding in action["connection_bindings"]:
                return suggestion, action, binding
        self.fail((pin_id, query, suggestion))

    def _connected_spawn(self, pin_id, action, binding, x=560, y=160):
        result = call_action(
            "blueprint_actions",
            "ue_add_blueprint_connected_action_node",
            asset_path=self.asset_path,
            graph_id=self.graph_id,
            pin_id=pin_id,
            action_id=action["action_id"],
            connection_binding_id=binding["binding_id"],
            position={"x": x, "y": y},
            allow_conversion=binding["response"]["requires_conversion"],
            bindings=[item["binding_id"] for item in action["bindings"]],
        )
        self.assertSuccess(result)
        self.assertFalse(result["data"]["saved"])
        return result

    def _insert(self, source_pin_id, target_pin_id, action, pair):
        result = call_action(
            "blueprint_actions",
            "ue_insert_blueprint_action_node",
            asset_path=self.asset_path,
            graph_id=self.graph_id,
            source_pin_id=source_pin_id,
            target_pin_id=target_pin_id,
            action_id=action["action_id"],
            input_binding_id=pair["input_binding_id"],
            output_binding_id=pair["output_binding_id"],
            position={"x": 340, "y": 160},
            bindings=[item["binding_id"] for item in action["bindings"]],
        )
        self.assertSuccess(result)
        self.assertFalse(result["data"]["saved"])
        return result

    def _preview(self, node_id, action, allow_loss=False):
        return call_action(
            "blueprint_actions",
            "ue_preview_blueprint_action_replacement",
            asset_path=self.asset_path,
            graph_id=self.graph_id,
            node_id=node_id,
            action_id=action["action_id"],
            bindings=[item["binding_id"] for item in action["bindings"]],
            pin_mapping=[],
            allow_conversion=False,
            allow_loss=allow_loss,
        )

    def _replace(self, preview):
        result = call_action(
            "blueprint_actions",
            "ue_replace_blueprint_node_with_action",
            asset_path=self.asset_path,
            graph_id=self.graph_id,
            replacement_plan_id=preview["data"]["replacement_plan_id"],
            allow_loss=preview["data"]["allow_loss"],
        )
        self.assertSuccess(result)
        self.assertFalse(result["data"]["saved"])
        return result

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
        return json.dumps(self._snapshot(), sort_keys=True, separators=(",", ":"))

    def _compile_and_health(self):
        compiled = call_action(
            "blueprint_actions",
            "ue_compile_blueprint",
            asset_path=self.asset_path,
        )
        self.assertSuccess(compiled)
        health = call_action(
            "blueprint_actions",
            "ue_get_blueprint_health",
            asset_path=self.asset_path,
            include_warnings=True,
        )
        self.assertSuccess(health)
        self.assertTrue(health["data"]["healthy"], health)

    def _begin_workflow(self, description):
        transaction_id = f"mcp_semantic_{uuid.uuid4().hex}"
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

    def _execute_workflow_step(self, transaction_id, action_name, params):
        result = call_action(
            "workflow_actions",
            "ue_execute_step",
            transaction_id=transaction_id,
            action_module="UnrealMCPython.blueprint_actions",
            action_name=action_name,
            params=params,
        )
        self.assertSuccess(result)
        return result

    def _rollback_workflow(self, transaction_id):
        result = call_action(
            "workflow_actions",
            "ue_rollback_transaction",
            transaction_id=transaction_id,
        )
        self.assertSuccess(result)

    def _close_workflow(self, transaction_id):
        context = call_action(
            "workflow_actions", "ue_get_editor_context", asset_paths=[]
        )["workflow_transaction"]
        if context["active"]:
            self._rollback_workflow(transaction_id)

    def test_connected_spawn_and_exact_edge_insertion(self):
        _, event_output = self._event_exec_output()
        suggestion, action, binding = self._find_pin_suggestion(
            self._pin_id(event_output), "Print String", self._is_print_string
        )
        self.assertTrue(binding["binding_id"].startswith("binding:"))
        self.assertEqual(suggestion["data"]["source_pin_id"], self._pin_id(event_output))
        connected = self._connected_spawn(
            self._pin_id(event_output), action, binding
        )
        connected_node = self._node(connected["data"]["node_id"])
        self.assertEqual(connected_node["node_id"], connected["data"]["node_id"])
        connected_input = next(
            pin
            for pin in connected["data"]["pins"]
            if pin["direction"] == "input" and pin["type"]["kind"] == "exec"
        )

        _, sequence, pair = self._find_connection_suggestion(
            self._pin_id(event_output),
            connected_input["id"],
            "Sequence",
            lambda item: item["node_class_path"].endswith(
                "K2Node_ExecutionSequence"
            ),
        )
        inserted = self._insert(
            self._pin_id(event_output), connected_input["id"], sequence, pair
        )
        self.assertEqual(len(inserted["data"]["connections"]), 2)
        self._node(inserted["data"]["node_id"])
        snapshot = self._snapshot()
        snapshot_node_ids = {
            node["id"]
            for graph in snapshot["graphs"]
            for node in graph["nodes"]
        }
        self.assertIn(inserted["data"]["node_id"], snapshot_node_ids)

        scalar_target = self._spawn(self._print_string_action(), 900, 360)
        duration = next(
            pin
            for pin in scalar_target["data"]["pins"]
            if pin["direction"] == "input" and pin["type"]["kind"] == "real"
        )
        _, scalar_action, scalar_binding = self._find_pin_suggestion(
            duration["id"],
            "Random Float",
            lambda item: item["member_path"].endswith(":RandomFloat")
            and not item["requires_binding"],
        )
        self._connected_spawn(
            duration["id"], scalar_action, scalar_binding, x=720, y=360
        )
        self._compile_and_health()

    def test_strict_and_explicitly_lossy_replacement(self):
        print_action = self._print_string_action()
        target = self._spawn(print_action, 620, 180)
        string_pin = next(
            pin
            for pin in target["data"]["pins"]
            if pin["direction"] == "input" and pin["type"]["kind"] == "string"
        )
        changed = call_action(
            "blueprint_actions",
            "ue_set_blueprint_node_properties",
            asset_path=self.asset_path,
            node_id=target["data"]["node_id"],
            properties={
                "comment": "semantic editor replacement",
                "comment_bubble_visible": True,
                "enabled_state": "disabled",
                "position": {"x": 640, "y": 240},
                "pin_defaults": {string_pin["id"]: "preserved editor default"},
            },
        )
        self.assertSuccess(changed)
        strict = self._preview(target["data"]["node_id"], print_action)
        self.assertSuccess(strict)
        self.assertTrue(strict["data"]["applicable"], strict)
        self.assertEqual(strict["data"]["loss_count"], 0)
        replaced = self._replace(strict)
        new_node = self._node(replaced["data"]["new_node_id"])
        self.assertEqual(new_node["position"], {"x": 640, "y": 240})
        self.assertEqual(new_node["comment"], "semantic editor replacement")
        self.assertEqual(len(replaced["data"]["dropped_defaults"]), 0)
        self.assertTrue(replaced["data"]["preserved_defaults"])

        branch = self._spawn(
            self._find_action("Branch", self._is_branch), 980, 240
        )
        condition = next(
            pin
            for pin in branch["data"]["pins"]
            if pin["direction"] == "input" and pin["type"]["kind"] == "bool"
        )
        self.assertSuccess(
            call_action(
                "blueprint_actions",
                "ue_set_blueprint_node_properties",
                asset_path=self.asset_path,
                node_id=branch["data"]["node_id"],
                properties={"pin_defaults": {condition["id"]: True}},
            )
        )
        strict_loss = self._preview(branch["data"]["node_id"], print_action)
        lossy = self._preview(
            branch["data"]["node_id"], print_action, allow_loss=True
        )
        self.assertSuccess(strict_loss)
        self.assertSuccess(lossy)
        self.assertFalse(strict_loss["data"]["applicable"])
        self.assertTrue(lossy["data"]["applicable"])
        self.assertEqual(
            strict_loss["data"]["unmapped_defaults"],
            lossy["data"]["unmapped_defaults"],
        )
        lossy_result = self._replace(lossy)
        self.assertEqual(
            lossy_result["data"]["dropped_defaults"],
            lossy["data"]["unmapped_defaults"],
        )
        self.assertEqual(
            len(lossy_result["warnings"]),
            len(lossy_result["data"]["dropped_defaults"]),
        )
        self._compile_and_health()

    def test_plugin_defined_exec_insertion_when_available(self):
        _, event_output = self._event_exec_output()
        _, print_action, print_binding = self._find_pin_suggestion(
            self._pin_id(event_output), "Print String", self._is_print_string
        )
        target = self._connected_spawn(
            self._pin_id(event_output), print_action, print_binding
        )
        target_input = next(
            pin
            for pin in target["data"]["pins"]
            if pin["direction"] == "input" and pin["type"]["kind"] == "exec"
        )
        suggestions = self._suggest_connection(
            self._pin_id(event_output), target_input["id"], "Ability"
        )
        plugin_actions = [
            item
            for item in suggestions["data"]["items"]
            if item["node_class_path"].startswith("/Script/GameplayAbilities")
        ]
        if not plugin_actions:
            self.skipTest(
                "No GameplayAbilities action can bridge this Actor exec edge "
                "in the current enabled-plugin palette."
            )
        action = plugin_actions[0]
        self._insert(
            self._pin_id(event_output),
            target_input["id"],
            action,
            action["binding_pairs"][0],
        )
        self._compile_and_health()

    def test_semantic_workflow_stress_12_of_12(self):
        event_node_id, event_output = self._event_exec_output()
        event_pin_id = self._pin_id(event_output)
        completed = 0

        for index in range(4):
            before = self._snapshot_text()
            _, action, binding = self._find_pin_suggestion(
                event_pin_id, "Print String", self._is_print_string
            )
            transaction_id = self._begin_workflow(
                f"Semantic connected add stress {index + 1}"
            )
            try:
                self._execute_workflow_step(
                    transaction_id,
                    "ue_add_blueprint_connected_action_node",
                    {
                        "asset_path": self.asset_path,
                        "graph_id": self.graph_id,
                        "pin_id": event_pin_id,
                        "action_id": action["action_id"],
                        "connection_binding_id": binding["binding_id"],
                        "position": {"x": 480, "y": 160},
                        "allow_conversion": False,
                        "bindings": [],
                    },
                )
                self.assertNotEqual(self._snapshot_text(), before)
                self._rollback_workflow(transaction_id)
                self.assertEqual(self._snapshot_text(), before)
                completed += 1
            finally:
                self._close_workflow(transaction_id)

        _, print_action, print_binding = self._find_pin_suggestion(
            event_pin_id, "Print String", self._is_print_string
        )
        baseline = self._connected_spawn(
            event_pin_id, print_action, print_binding
        )
        baseline_input = next(
            pin
            for pin in baseline["data"]["pins"]
            if pin["direction"] == "input" and pin["type"]["kind"] == "exec"
        )

        for index in range(4):
            before = self._snapshot_text()
            _, action, pair = self._find_connection_suggestion(
                event_pin_id,
                baseline_input["id"],
                "Sequence",
                lambda item: item["node_class_path"].endswith(
                    "K2Node_ExecutionSequence"
                ),
            )
            transaction_id = self._begin_workflow(
                f"Semantic insertion stress {index + 1}"
            )
            try:
                self._execute_workflow_step(
                    transaction_id,
                    "ue_insert_blueprint_action_node",
                    {
                        "asset_path": self.asset_path,
                        "graph_id": self.graph_id,
                        "source_pin_id": event_pin_id,
                        "target_pin_id": baseline_input["id"],
                        "action_id": action["action_id"],
                        "input_binding_id": pair["input_binding_id"],
                        "output_binding_id": pair["output_binding_id"],
                        "position": {"x": 320, "y": 160},
                        "bindings": [],
                    },
                )
                self.assertNotEqual(self._snapshot_text(), before)
                self._rollback_workflow(transaction_id)
                self.assertEqual(self._snapshot_text(), before)
                completed += 1
            finally:
                self._close_workflow(transaction_id)

        for index in range(4):
            before = self._snapshot_text()
            action = self._print_string_action()
            preview = self._preview(baseline["data"]["node_id"], action)
            self.assertSuccess(preview)
            self.assertTrue(preview["data"]["applicable"], preview)
            transaction_id = self._begin_workflow(
                f"Semantic replacement stress {index + 1}"
            )
            try:
                self._execute_workflow_step(
                    transaction_id,
                    "ue_replace_blueprint_node_with_action",
                    {
                        "asset_path": self.asset_path,
                        "graph_id": self.graph_id,
                        "replacement_plan_id": preview["data"][
                            "replacement_plan_id"
                        ],
                        "allow_loss": False,
                    },
                )
                self.assertNotEqual(self._snapshot_text(), before)
                self._rollback_workflow(transaction_id)
                self.assertEqual(self._snapshot_text(), before)
                completed += 1
            finally:
                self._close_workflow(transaction_id)

        print(f"Semantic workflow stress: {completed}/12")
        self.assertEqual(completed, 12)
