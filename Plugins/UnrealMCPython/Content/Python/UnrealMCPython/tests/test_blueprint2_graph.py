"""In-editor coverage for Blueprint 2 graph-node authoring."""

import json
import uuid

import unreal

from UnrealMCPython.tests.base import MCPTestCase
from UnrealMCPython.tests.blueprint2_support import (
    BLUEPRINT2_TEST_ROOT,
    call_action,
)


class TestBlueprint2Graph(MCPTestCase):

    VARIABLE_NAME = "GraphEnabled"

    def setUp(self):
        self.ensure_test_dir()
        self._created_assets = []
        self.addCleanup(self._cleanup_created_assets)

        name = f"Blueprint2Graph_{uuid.uuid4().hex[:10]}"
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

        bool_type = unreal.BlueprintEditorLibrary.get_basic_type_by_name(
            unreal.Name("bool")
        )
        self.assertTrue(
            unreal.BlueprintEditorLibrary.add_member_variable(
                self.blueprint,
                unreal.Name(self.VARIABLE_NAME),
                bool_type,
            )
        )
        self.graph_id = self._event_graph_id()
        self.variable_id = self._variable_id()

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
                f"Blueprint 2 graph tests left assets behind: {remaining}"
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
                "Failed to remove empty Blueprint 2 graph test root: "
                f"{BLUEPRINT2_TEST_ROOT}"
            )

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
        events = self._inspect([{"op": "events", "detail": "compact"}])[0]
        graph_ids = {
            item["graph_id"]
            for item in events["items"]
            if item["class_path"]
            == "/Script/BlueprintGraph.K2Node_Event"
        }
        self.assertEqual(len(graph_ids), 1, graph_ids)
        return next(iter(graph_ids))

    def _variable_id(self):
        variables = self._inspect(
            [
                {
                    "op": "variables",
                    "detail": "detailed",
                    "name_pattern": self.VARIABLE_NAME,
                }
            ]
        )[0]["items"]
        self.assertEqual(len(variables), 1, variables)
        return variables[0]["variable_id"]

    def _node_count(self):
        return self._inspect(
            [{"op": "nodes", "detail": "compact", "limit": 500}]
        )[0]["total_count"]

    def _inspect_node(self, node_id):
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

    def _inspect_pins(self, node_id):
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

    def _pin(self, node_id, pin_name):
        pins = [
            pin
            for pin in self._inspect_pins(node_id)
            if pin["name"] == pin_name
        ]
        self.assertEqual(len(pins), 1, pins)
        return pins[0]

    def _add_common_node(self, node_json):
        result = call_action(
            "blueprint_actions",
            "ue_add_blueprint_node",
            asset_path=self.asset_path,
            graph_name="EventGraph",
            node_json=node_json,
        )
        self.assertSuccess(result)
        return result

    def _begin_workflow(self, description):
        transaction_id = f"mcp_graph_{uuid.uuid4().hex}"
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
        rolled_back = call_action(
            "workflow_actions",
            "ue_rollback_transaction",
            transaction_id=transaction_id,
        )
        self.assertSuccess(rolled_back)

    def _assert_created_node(
        self,
        result,
        expected_class_path,
        expected_position,
        *,
        legacy=False,
    ):
        self.assertSuccess(result)
        data = result["data"]
        self.assertTrue(data["node_id"].startswith("node:"))
        self.assertEqual(data["graph_id"], self.graph_id)
        self.assertEqual(data["class_path"], expected_class_path)
        self.assertEqual(data["position"], expected_position)
        self.assertTrue(
            all(pin_id.startswith("pin:") for pin_id in data["pin_ids"])
        )
        self.assertEqual(result["changes"][0]["target_id"], data["node_id"])

        inspected = self._inspect_node(data["node_id"])
        self.assertEqual(inspected["node_id"], data["node_id"])
        self.assertEqual(inspected["graph_id"], self.graph_id)
        self.assertEqual(inspected["class_path"], expected_class_path)
        self.assertEqual(inspected["position"], expected_position)
        self.assertEqual(
            sorted(pin["id"] for pin in inspected["pins"]),
            sorted(data["pin_ids"]),
        )

        if legacy:
            for compatibility_field in (
                "node_name",
                "node_title",
                "pins",
                "message",
            ):
                self.assertIn(compatibility_field, result)
            self.assertEqual(
                sorted(pin["pin_id"] for pin in result["pins"]),
                sorted(data["pin_ids"]),
            )

    def test_add_blueprint_node_common_families(self):
        cases = (
            (
                {"type": "Branch"},
                "/Script/BlueprintGraph.K2Node_IfThenElse",
            ),
            (
                {"type": "Sequence", "output_count": 3},
                "/Script/BlueprintGraph.K2Node_ExecutionSequence",
            ),
            (
                {"type": "CastTo", "class_path": "/Script/Engine.Actor"},
                "/Script/BlueprintGraph.K2Node_DynamicCast",
            ),
            (
                {"type": "VariableGet", "variable_id": self.variable_id},
                "/Script/BlueprintGraph.K2Node_VariableGet",
            ),
            (
                {"type": "VariableSet", "variable_id": self.variable_id},
                "/Script/BlueprintGraph.K2Node_VariableSet",
            ),
            (
                {
                    "type": "Operator",
                    "operator": "add",
                    "operand_type": {
                        "kind": "real",
                        "precision": "double",
                    },
                },
                "/Script/BlueprintGraph.K2Node_PromotableOperator",
            ),
            (
                {
                    "type": "Operator",
                    "operator": "equal",
                    "operand_type": {"kind": "int"},
                },
                "/Script/BlueprintGraph.K2Node_PromotableOperator",
            ),
            (
                {
                    "type": "Event",
                    "function_path": "/Script/Engine.Actor:ReceiveBeginPlay",
                },
                "/Script/BlueprintGraph.K2Node_Event",
            ),
            (
                {
                    "type": "Select",
                    "option_count": 3,
                    "value_type": {"kind": "int"},
                },
                "/Script/BlueprintGraph.K2Node_Select",
            ),
            (
                {
                    "type": "Switch",
                    "switch_kind": "enum",
                    "enum_path": "/Script/Engine.ECollisionChannel",
                },
                "/Script/BlueprintGraph.K2Node_SwitchEnum",
            ),
            (
                {
                    "type": "Switch",
                    "switch_kind": "int",
                    "cases": [3, 4, 5],
                },
                "/Script/BlueprintGraph.K2Node_SwitchInteger",
            ),
            (
                {
                    "type": "Switch",
                    "switch_kind": "string",
                    "cases": ["Idle", "Running"],
                },
                "/Script/BlueprintGraph.K2Node_SwitchString",
            ),
            (
                {
                    "type": "Switch",
                    "switch_kind": "name",
                    "cases": ["North", "South"],
                },
                "/Script/BlueprintGraph.K2Node_SwitchName",
            ),
            (
                {"type": "Reroute"},
                "/Script/BlueprintGraph.K2Node_Knot",
            ),
            (
                {
                    "type": "Comment",
                    "text": "Validate input",
                    "size_x": 400,
                    "size_y": 180,
                },
                "/Script/UnrealEd.EdGraphNode_Comment",
            ),
            (
                {
                    "type": "MakeStruct",
                    "struct_path": "/Script/CoreUObject.Vector",
                },
                "/Script/BlueprintGraph.K2Node_MakeStruct",
            ),
            (
                {
                    "type": "BreakStruct",
                    "struct_path": "/Script/CoreUObject.Vector",
                },
                "/Script/BlueprintGraph.K2Node_BreakStruct",
            ),
        )

        for index, (node_json, class_path) in enumerate(cases):
            with self.subTest(node_type=node_json["type"], index=index):
                position = {"x": 160 + index * 32, "y": 96 + index * 24}
                request = dict(node_json)
                request["pos_x"] = position["x"]
                request["pos_y"] = position["y"]
                result = call_action(
                    "blueprint_actions",
                    "ue_add_blueprint_node",
                    asset_path=self.asset_path,
                    graph_name="EventGraph",
                    node_json=request,
                )
                self._assert_created_node(
                    result,
                    class_path,
                    position,
                    legacy=True,
                )
                pins = self._inspect_pins(result["data"]["node_id"])
                if node_json["type"] == "Sequence":
                    outputs = [
                        pin
                        for pin in pins
                        if pin["direction"] == "output"
                        and pin["type"]["kind"] == "exec"
                    ]
                    self.assertEqual(
                        len(outputs), node_json["output_count"], outputs
                    )
                elif node_json["type"] == "Select":
                    value_inputs = [
                        pin
                        for pin in pins
                        if pin["direction"] == "input"
                        and pin["name"] != "Index"
                    ]
                    self.assertEqual(
                        len(value_inputs), node_json["option_count"], pins
                    )
                    self.assertTrue(
                        all(pin["type"]["kind"] == "int" for pin in value_inputs),
                        value_inputs,
                    )
                elif node_json["type"] == "Switch" and node_json.get("cases"):
                    output_names = {
                        pin["name"]
                        for pin in pins
                        if pin["direction"] == "output"
                    }
                    self.assertTrue(
                        {str(case) for case in node_json["cases"]}
                        <= output_names,
                        output_names,
                    )
                elif node_json["type"] == "Comment":
                    self.assertEqual(result["data"]["comment"], node_json["text"])
                    self.assertEqual(
                        result["data"]["size"],
                        {"x": node_json["size_x"], "y": node_json["size_y"]},
                    )
                    self.assertEqual(
                        self._inspect_node(result["data"]["node_id"])["comment"],
                        node_json["text"],
                    )

    def test_operator_families_and_unmatched_type_rejection(self):
        expected_functions = {
            "add": "Add_IntInt",
            "subtract": "Subtract_IntInt",
            "multiply": "Multiply_IntInt",
            "divide": "Divide_IntInt",
            "equal": "EqualEqual_IntInt",
            "not_equal": "NotEqual_IntInt",
            "less": "Less_IntInt",
            "less_equal": "LessEqual_IntInt",
            "greater": "Greater_IntInt",
            "greater_equal": "GreaterEqual_IntInt",
        }
        for operator, function_name in expected_functions.items():
            with self.subTest(operator=operator):
                result = self._add_common_node(
                    {
                        "type": "Operator",
                        "operator": operator,
                        "operand_type": {"kind": "int"},
                    }
                )
                self.assertEqual(
                    result["data"]["reference_path"],
                    f"/Script/Engine.KismetMathLibrary:{function_name}",
                )

        before = self._node_count()
        rejected = call_action(
            "blueprint_actions",
            "ue_add_blueprint_node",
            asset_path=self.asset_path,
            graph_name="EventGraph",
            node_json={
                "type": "Operator",
                "operator": "add",
                "operand_type": {"kind": "text"},
            },
        )
        self.assertFalse(rejected.get("success"), rejected)
        self.assertEqual(self._node_count(), before)

    def test_add_reflected_blueprint_node(self):
        cases = (
            (
                "function",
                "/Script/Engine.Actor:K2_GetActorLocation",
                "/Script/BlueprintGraph.K2Node_CallFunction",
            ),
            (
                "property_get",
                "/Script/Engine.Actor:CustomTimeDilation",
                "/Script/BlueprintGraph.K2Node_VariableGet",
            ),
            (
                "property_set",
                "/Script/Engine.Actor:CustomTimeDilation",
                "/Script/BlueprintGraph.K2Node_VariableSet",
            ),
            (
                "cast_to",
                "/Script/Engine.Actor",
                "/Script/BlueprintGraph.K2Node_DynamicCast",
            ),
            (
                "enum_literal",
                "/Script/Engine.ECollisionChannel",
                "/Script/BlueprintGraph.K2Node_EnumLiteral",
            ),
            (
                "make_struct",
                "/Script/CoreUObject.Vector",
                "/Script/BlueprintGraph.K2Node_MakeStruct",
            ),
            (
                "break_struct",
                "/Script/CoreUObject.Vector",
                "/Script/BlueprintGraph.K2Node_BreakStruct",
            ),
        )

        for index, (member_kind, member_path, class_path) in enumerate(cases):
            with self.subTest(member_kind=member_kind):
                position = {"x": 480 + index * 40, "y": 240 + index * 32}
                result = call_action(
                    "blueprint_actions",
                    "ue_add_reflected_blueprint_node",
                    asset_path=self.asset_path,
                    graph_id=self.graph_id,
                    member_kind=member_kind,
                    member_path=member_path,
                    position=position,
                )
                self._assert_created_node(result, class_path, position)

    def test_invalid_reflected_paths_do_not_mutate_the_graph(self):
        cases = (
            ("function", "/Script/Engine.Actor:DefinitelyMissing"),
            ("function", "/Script/Engine.Actor:OnRep_ReplicatedMovement"),
            ("property_get", "/Script/Engine.Actor:K2_GetActorLocation"),
            ("cast_to", "/Script/CoreUObject.Vector"),
            ("enum_literal", "/Script/CoreUObject.Vector"),
            ("make_struct", "/Script/Engine.Actor"),
        )
        for member_kind, member_path in cases:
            with self.subTest(member_kind=member_kind):
                before = self._node_count()
                result = call_action(
                    "blueprint_actions",
                    "ue_add_reflected_blueprint_node",
                    asset_path=self.asset_path,
                    graph_id=self.graph_id,
                    member_kind=member_kind,
                    member_path=member_path,
                    position={"x": 0, "y": 0},
                )
                self.assertFalse(result.get("success"), result)
                self.assertEqual(self._node_count(), before)

    def test_latent_reflected_function_is_rejected_before_mutation(self):
        created = self.call(
            "blueprint_actions",
            "ue_create_blueprint_function",
            asset_path=self.asset_path,
            function_name="NoLatentCalls",
        )
        self.assertSuccess(created)
        before = self._node_count()

        rejected = call_action(
            "blueprint_actions",
            "ue_add_reflected_blueprint_node",
            asset_path=self.asset_path,
            graph_id=created["data"]["function_id"],
            member_kind="function",
            member_path="/Script/Engine.KismetSystemLibrary:Delay",
            position={"x": 240, "y": 160},
        )

        self.assertFalse(rejected.get("success"), rejected)
        self.assertEqual(self._node_count(), before)

    def test_reflected_node_creation_rolls_back_with_outer_workflow(self):
        before = self._node_count()
        transaction_id = self._begin_workflow("Add reflected Blueprint node")
        try:
            added = call_action(
                "workflow_actions",
                "ue_execute_step",
                transaction_id=transaction_id,
                action_module="UnrealMCPython.blueprint_actions",
                action_name="ue_add_reflected_blueprint_node",
                params={
                    "asset_path": self.asset_path,
                    "graph_id": self.graph_id,
                    "member_kind": "function",
                    "member_path": "/Script/Engine.Actor:K2_GetActorLocation",
                    "position": {"x": 720, "y": 320},
                },
            )
            self.assertSuccess(added)
            self.assertEqual(self._node_count(), before + 1)
            self._rollback_workflow(transaction_id)
            self.assertEqual(self._node_count(), before)
        finally:
            context = call_action(
                "workflow_actions", "ue_get_editor_context", asset_paths=[]
            )["workflow_transaction"]
            if context["active"]:
                self._rollback_workflow(transaction_id)

    def test_common_node_creation_rolls_back_with_outer_workflow(self):
        before = self._node_count()
        transaction_id = f"mcp_graph_{uuid.uuid4().hex}"
        try:
            begun = call_action(
                "workflow_actions",
                "ue_begin_transaction",
                transaction_id=transaction_id,
                description="Add Blueprint graph node",
                total_steps=1,
                show_dialog=False,
            )
            self.assertSuccess(begun)

            added = call_action(
                "workflow_actions",
                "ue_execute_step",
                transaction_id=transaction_id,
                action_module="UnrealMCPython.blueprint_actions",
                action_name="ue_add_blueprint_node",
                params={
                    "asset_path": self.asset_path,
                    "graph_name": "EventGraph",
                    "node_json": {
                        "type": "Branch",
                        "pos_x": 960,
                        "pos_y": 480,
                    },
                },
            )
            self.assertSuccess(added)
            self.assertEqual(self._node_count(), before + 1)

            rolled_back = call_action(
                "workflow_actions",
                "ue_rollback_transaction",
                transaction_id=transaction_id,
            )
            self.assertSuccess(rolled_back)
            self.assertEqual(self._node_count(), before)
        finally:
            context = call_action(
                "workflow_actions", "ue_get_editor_context", asset_paths=[]
            )["workflow_transaction"]
            if context["active"]:
                call_action(
                    "workflow_actions",
                    "ue_rollback_transaction",
                    transaction_id=transaction_id,
                )

    def test_failed_graph_build_is_atomic(self):
        retained = self._add_common_node(
            {"type": "Branch", "pos_x": 128, "pos_y": 96}
        )
        before = self._node_count()
        cases = (
            {
                "nodes": [
                    {"id": "valid", "type": "Branch"},
                    {"id": "invalid", "type": "DefinitelyUnknown"},
                ],
                "connections": [],
            },
            {"nodes": [42], "connections": []},
            {
                "nodes": [
                    {"id": "duplicate", "type": "Branch"},
                    {"id": "duplicate", "type": "Branch"},
                ],
                "connections": [],
            },
            {
                "nodes": [{"id": "valid", "type": "Branch"}],
                "connections": [42],
            },
            {"nodes": {}, "connections": []},
            {
                "nodes": [{"id": "valid", "type": "Branch"}],
                "connections": {},
            },
        )
        for graph_structure in cases:
            with self.subTest(graph_structure=graph_structure):
                result = call_action(
                    "blueprint_actions",
                    "ue_build_blueprint_graph",
                    asset_path=self.asset_path,
                    graph_name="EventGraph",
                    graph_structure=graph_structure,
                )

                self.assertFalse(result.get("success"), result)
                self.assertEqual(self._node_count(), before)
                self.assertEqual(
                    self._inspect_node(retained["data"]["node_id"])["node_id"],
                    retained["data"]["node_id"],
                )

    def test_legacy_graph_mutations_roll_back_with_outer_workflow(self):
        branch = self._add_common_node(
            {"type": "Branch", "pos_x": 160, "pos_y": 120}
        )
        sequence = self._add_common_node(
            {"type": "Sequence", "output_count": 2, "pos_x": 480, "pos_y": 120}
        )

        transaction_id = self._begin_workflow("Connect Blueprint pins")
        try:
            connected = call_action(
                "workflow_actions",
                "ue_execute_step",
                transaction_id=transaction_id,
                action_module="UnrealMCPython.blueprint_actions",
                action_name="ue_connect_blueprint_pins",
                params={
                    "asset_path": self.asset_path,
                    "graph_name": "EventGraph",
                    "source_node": branch["node_name"],
                    "source_pin": "then",
                    "target_node": sequence["node_name"],
                    "target_pin": "execute",
                },
            )
            self.assertSuccess(connected)
            self.assertEqual(
                self._pin(branch["data"]["node_id"], "then")["link_count"],
                1,
            )
            self._rollback_workflow(transaction_id)
            self.assertEqual(
                self._pin(branch["data"]["node_id"], "then")["link_count"],
                0,
            )
        finally:
            context = call_action(
                "workflow_actions", "ue_get_editor_context", asset_paths=[]
            )["workflow_transaction"]
            if context["active"]:
                self._rollback_workflow(transaction_id)

        transaction_id = self._begin_workflow("Move Blueprint node")
        try:
            moved = call_action(
                "workflow_actions",
                "ue_execute_step",
                transaction_id=transaction_id,
                action_module="UnrealMCPython.blueprint_actions",
                action_name="ue_set_blueprint_node_position",
                params={
                    "asset_path": self.asset_path,
                    "graph_name": "EventGraph",
                    "node_name": branch["node_name"],
                    "pos_x": 900,
                    "pos_y": 700,
                },
            )
            self.assertSuccess(moved)
            self.assertEqual(
                self._inspect_node(branch["data"]["node_id"])["position"],
                {"x": 900, "y": 700},
            )
            self._rollback_workflow(transaction_id)
            self.assertEqual(
                self._inspect_node(branch["data"]["node_id"])["position"],
                {"x": 160, "y": 120},
            )
        finally:
            context = call_action(
                "workflow_actions", "ue_get_editor_context", asset_paths=[]
            )["workflow_transaction"]
            if context["active"]:
                self._rollback_workflow(transaction_id)

        transaction_id = self._begin_workflow("Set Blueprint pin default")
        try:
            entered = json.loads(
                unreal.MCPythonHelper.begin_workflow_atomic_step(
                    transaction_id
                )
            )
            self.assertSuccess(entered)
            try:
                changed = unreal.MCPythonHelper.set_blueprint_node_pin_default(
                    self.blueprint,
                    "EventGraph",
                    branch["node_name"],
                    "Condition",
                    "false",
                )
                self.assertSuccess(json.loads(changed))
            finally:
                ended = json.loads(
                    unreal.MCPythonHelper.end_workflow_atomic_step(
                        transaction_id
                    )
                )
                self.assertSuccess(ended)
            self.assertEqual(
                self._pin(branch["data"]["node_id"], "Condition")["default"],
                "false",
            )
            self._rollback_workflow(transaction_id)
            self.assertEqual(
                self._pin(branch["data"]["node_id"], "Condition")["default"],
                "true",
            )
        finally:
            context = call_action(
                "workflow_actions", "ue_get_editor_context", asset_paths=[]
            )["workflow_transaction"]
            if context["active"]:
                self._rollback_workflow(transaction_id)

        before_build = self._node_count()
        transaction_id = self._begin_workflow("Build Blueprint graph")
        try:
            built = call_action(
                "workflow_actions",
                "ue_execute_step",
                transaction_id=transaction_id,
                action_module="UnrealMCPython.blueprint_actions",
                action_name="ue_build_blueprint_graph",
                params={
                    "asset_path": self.asset_path,
                    "graph_name": "EventGraph",
                    "graph_structure": {
                        "nodes": [{"id": "new", "type": "Branch"}],
                        "connections": [],
                    },
                },
            )
            self.assertSuccess(built)
            self.assertNotEqual(self._node_count(), before_build)
            self._rollback_workflow(transaction_id)
            self.assertEqual(self._node_count(), before_build)
            self._inspect_node(branch["data"]["node_id"])
        finally:
            context = call_action(
                "workflow_actions", "ue_get_editor_context", asset_paths=[]
            )["workflow_transaction"]
            if context["active"]:
                self._rollback_workflow(transaction_id)

        before_remove = self._node_count()
        transaction_id = self._begin_workflow("Remove Blueprint node")
        try:
            removed = call_action(
                "workflow_actions",
                "ue_execute_step",
                transaction_id=transaction_id,
                action_module="UnrealMCPython.blueprint_actions",
                action_name="ue_remove_blueprint_node",
                params={
                    "asset_path": self.asset_path,
                    "graph_name": "EventGraph",
                    "node_name": sequence["node_name"],
                },
            )
            self.assertSuccess(removed)
            self.assertEqual(self._node_count(), before_remove - 1)
            self._rollback_workflow(transaction_id)
            self.assertEqual(self._node_count(), before_remove)
            self._inspect_node(sequence["data"]["node_id"])
        finally:
            context = call_action(
                "workflow_actions", "ue_get_editor_context", asset_paths=[]
            )["workflow_transaction"]
            if context["active"]:
                self._rollback_workflow(transaction_id)
