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
    INT_VARIABLE_NAME = "GraphCount"

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
        int_type = unreal.BlueprintEditorLibrary.get_basic_type_by_name(
            unreal.Name("int")
        )
        self.assertTrue(
            unreal.BlueprintEditorLibrary.add_member_variable(
                self.blueprint,
                unreal.Name(self.INT_VARIABLE_NAME),
                int_type,
            )
        )
        self.graph_id = self._event_graph_id()
        self.variable_id = self._variable_id()
        self.int_variable_id = self._variable_id(self.INT_VARIABLE_NAME)

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

    def _variable_id(self, variable_name=None):
        variable_name = variable_name or self.VARIABLE_NAME
        variables = self._inspect(
            [
                {
                    "op": "variables",
                    "detail": "detailed",
                    "name_pattern": variable_name,
                }
            ]
        )[0]["items"]
        self.assertEqual(len(variables), 1, variables)
        return variables[0]["variable_id"]

    def _variable_record(self, variable_name):
        variables = self._inspect(
            [
                {
                    "op": "variables",
                    "detail": "detailed",
                    "name_pattern": variable_name,
                }
            ]
        )[0]["items"]
        self.assertEqual(len(variables), 1, variables)
        return variables[0]

    def _variable_snapshot(self, variable_name):
        return json.dumps(
            self._variable_record(variable_name),
            sort_keys=True,
            separators=(",", ":"),
        )

    def _component_records(self):
        return self._inspect(
            [{"op": "components", "detail": "detailed", "limit": 500}]
        )[0]["items"]

    def _component_record(self, component_name):
        records = [
            record
            for record in self._component_records()
            if record["name"] == component_name
        ]
        self.assertEqual(len(records), 1, records)
        return records[0]

    def _component_snapshot(self):
        return json.dumps(
            self._component_records(), sort_keys=True, separators=(",", ":")
        )

    def _add_component(self, component_name, component_class, parent_name=""):
        result = call_action(
            "blueprint_actions",
            "ue_add_component_to_blueprint",
            asset_path=self.asset_path,
            component_class_path=component_class,
            component_name=component_name,
            parent_component_name=parent_name,
        )
        self.assertSuccess(result)
        return self._component_record(component_name)

    def _assert_component_rejected_unchanged(
        self, action_name, expected_code="INVALID_INPUT", **kwargs
    ):
        before = self._component_snapshot()
        result = call_action(
            "blueprint_actions",
            action_name,
            asset_path=self.asset_path,
            **kwargs,
        )
        self.assertFalse(result.get("success"), result)
        self.assertEqual(result["errors"][0]["code"], expected_code, result)
        self.assertEqual(self._component_snapshot(), before)
        return result

    def _add_member_variable(self, variable_name, pin_type):
        self.assertTrue(
            unreal.BlueprintEditorLibrary.add_member_variable(
                self.blueprint,
                unreal.Name(variable_name),
                pin_type,
            )
        )
        return self._variable_id(variable_name)

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

    def _graph_snapshot(self):
        nodes = self._inspect(
            [{"op": "nodes", "detail": "detailed", "limit": 500}]
        )[0]["items"]
        return json.dumps(nodes, sort_keys=True)

    def _connection_ids(self):
        connections = self._inspect(
            [{"op": "connections", "detail": "compact", "limit": 500}]
        )[0]["items"]
        return {connection["id"] for connection in connections}

    def _assert_rejected_call_unchanged(
        self, action_name, expected_code="INVALID_INPUT", **kwargs
    ):
        before = self._graph_snapshot()
        result = call_action(
            "blueprint_actions",
            action_name,
            asset_path=self.asset_path,
            **kwargs,
        )
        self.assertFalse(result.get("success"), result)
        self.assertEqual(result["errors"][0]["code"], expected_code, result)
        self.assertEqual(self._graph_snapshot(), before)
        return result

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

    def test_rename_blueprint_variable(self):
        original_id = self.variable_id
        notify_function = self.call(
            "blueprint_actions",
            "ue_create_blueprint_function",
            asset_path=self.asset_path,
            function_name="OnRep_GraphEnabled",
            inputs=[],
            outputs=[],
        )
        self.assertSuccess(notify_function)
        replication = call_action(
            "blueprint_actions",
            "ue_set_blueprint_variable_replication",
            asset_path=self.asset_path,
            variable_id=original_id,
            mode="rep_notify",
            notify_function_name="OnRep_GraphEnabled",
        )
        self.assertSuccess(replication)
        renamed = call_action(
            "blueprint_actions",
            "ue_rename_blueprint_variable",
            asset_path=self.asset_path,
            variable_id=original_id,
            new_name="RenamedEnabled",
        )
        self.assertSuccess(renamed)
        self.assertEqual(renamed["data"]["variable_id"], original_id)
        self.assertEqual(renamed["data"]["before"]["name"], self.VARIABLE_NAME)
        self.assertEqual(renamed["data"]["after"]["name"], "RenamedEnabled")
        renamed_record = self._variable_record("RenamedEnabled")
        self.assertEqual(renamed_record["variable_id"], original_id)
        self.assertEqual(
            renamed_record["rep_notify_function"], "OnRep_GraphEnabled"
        )

        before_collision = self._variable_snapshot("RenamedEnabled")
        collision = call_action(
            "blueprint_actions",
            "ue_rename_blueprint_variable",
            asset_path=self.asset_path,
            variable_id=original_id,
            new_name=self.INT_VARIABLE_NAME,
        )
        self.assertFalse(collision.get("success"), collision)
        self.assertEqual(collision["errors"][0]["code"], "CONFLICT")
        self.assertEqual(
            self._variable_snapshot("RenamedEnabled"), before_collision
        )

        malformed = json.loads(
            unreal.MCPythonHelper.rename_blueprint_variable(
                self.blueprint,
                json.dumps(
                    {
                        "variable_id": original_id,
                        "new_name": "MustNotPersist",
                        "unknown": True,
                    }
                ),
            )
        )
        self.assertFalse(malformed.get("success"), malformed)
        self.assertEqual(malformed["errors"][0]["code"], "INVALID_INPUT")
        self.assertEqual(
            self._variable_snapshot("RenamedEnabled"), before_collision
        )

        transaction_id = self._begin_workflow("Rename Blueprint variable")
        try:
            workflow_rename = call_action(
                "workflow_actions",
                "ue_execute_step",
                transaction_id=transaction_id,
                action_module="UnrealMCPython.blueprint_actions",
                action_name="ue_rename_blueprint_variable",
                params={
                    "asset_path": self.asset_path,
                    "variable_id": original_id,
                    "new_name": "WorkflowRenamed",
                },
            )
            self.assertSuccess(workflow_rename)
            self._variable_record("WorkflowRenamed")
            self._rollback_workflow(transaction_id)
            self._variable_record("RenamedEnabled")
        finally:
            context = call_action(
                "workflow_actions", "ue_get_editor_context", asset_paths=[]
            )["workflow_transaction"]
            if context["active"]:
                self._rollback_workflow(transaction_id)

        unreal.BlueprintEditorLibrary.compile_blueprint(self.blueprint)
        child_name = f"Blueprint2VariableChild_{uuid.uuid4().hex[:10]}"
        child_factory = unreal.BlueprintFactory()
        child_factory.set_editor_property(
            "parent_class", self.blueprint.generated_class()
        )
        child = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            child_name,
            BLUEPRINT2_TEST_ROOT,
            unreal.Blueprint,
            child_factory,
        )
        self.assertIsNotNone(child)
        child_path = f"{BLUEPRINT2_TEST_ROOT}/{child_name}.{child_name}"
        self._created_assets.append(child_path)
        inherited = call_action(
            "blueprint_actions",
            "ue_rename_blueprint_variable",
            asset_path=child_path,
            variable_id=original_id,
            new_name="InheritedMustNotRename",
        )
        self.assertFalse(inherited.get("success"), inherited)
        self.assertEqual(inherited["errors"][0]["code"], "PRECONDITION_FAILED")
        self._variable_record("RenamedEnabled")

        dispatcher = self.call(
            "blueprint_actions",
            "ue_add_event_dispatcher",
            asset_path=self.asset_path,
            dispatcher_name="BeforeRenameDispatcher",
            parameters=[],
        )
        self.assertSuccess(dispatcher)
        dispatcher_id = dispatcher["data"]["dispatcher_id"]
        dispatcher_renamed = call_action(
            "blueprint_actions",
            "ue_rename_blueprint_variable",
            asset_path=self.asset_path,
            variable_id=dispatcher_id,
            new_name="AfterRenameDispatcher",
        )
        self.assertSuccess(dispatcher_renamed)
        dispatchers = self._inspect(
            [{"op": "dispatchers", "detail": "detailed"}]
        )[0]["items"]
        self.assertNotIn(
            "BeforeRenameDispatcher",
            {item["name"] for item in dispatchers},
        )
        self.assertIn(
            "AfterRenameDispatcher",
            {item["name"] for item in dispatchers},
        )

    def test_add_variable_rejects_loaded_child_collision(self):
        unreal.BlueprintEditorLibrary.compile_blueprint(self.blueprint)
        child_name = f"Blueprint2ChildCollision_{uuid.uuid4().hex[:10]}"
        child_factory = unreal.BlueprintFactory()
        child_factory.set_editor_property(
            "parent_class", self.blueprint.generated_class()
        )
        child = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            child_name,
            BLUEPRINT2_TEST_ROOT,
            unreal.Blueprint,
            child_factory,
        )
        self.assertIsNotNone(child)
        child_path = f"{BLUEPRINT2_TEST_ROOT}/{child_name}.{child_name}"
        self._created_assets.append(child_path)
        self.assertTrue(
            unreal.BlueprintEditorLibrary.add_member_variable(
                child,
                unreal.Name("ChildOwnedName"),
                unreal.BlueprintEditorLibrary.get_basic_type_by_name(
                    unreal.Name("bool")
                ),
            )
        )

        rejected = call_action(
            "blueprint_actions",
            "ue_add_variable",
            asset_path=self.asset_path,
            variable_name="ChildOwnedName",
            variable_type="bool",
        )

        self.assertFalse(rejected.get("success"), rejected)
        self.assertEqual(rejected["errors"][0]["code"], "CONFLICT")
        self.assertEqual(rejected["errors"][0]["path"], "variable_name")
        variables = self._inspect(
            [
                {
                    "op": "variables",
                    "detail": "detailed",
                    "name_pattern": "ChildOwnedName",
                }
            ]
        )[0]["items"]
        self.assertEqual(variables, [])

    def test_remove_blueprint_variable(self):
        removed_id = self.int_variable_id
        removed = call_action(
            "blueprint_actions",
            "ue_remove_blueprint_variable",
            asset_path=self.asset_path,
            variable_id=removed_id,
        )
        self.assertSuccess(removed)
        self.assertEqual(removed["data"]["variable_id"], removed_id)
        remaining = self._inspect(
            [
                {
                    "op": "variables",
                    "detail": "detailed",
                    "name_pattern": self.INT_VARIABLE_NAME,
                }
            ]
        )[0]["items"]
        self.assertEqual(remaining, [])

        before = self._variable_snapshot(self.VARIABLE_NAME)
        malformed = json.loads(
            unreal.MCPythonHelper.remove_blueprint_variable(
                self.blueprint,
                json.dumps(
                    {
                        "variable_id": self.variable_id,
                        "new_name": "not-allowed",
                    }
                ),
            )
        )
        self.assertFalse(malformed.get("success"), malformed)
        self.assertEqual(malformed["errors"][0]["code"], "INVALID_INPUT")
        self.assertEqual(self._variable_snapshot(self.VARIABLE_NAME), before)

    def test_set_blueprint_variable_default(self):
        library = unreal.BlueprintEditorLibrary

        def basic(name):
            return library.get_basic_type_by_name(unreal.Name(name))

        int_type = basic("int")
        cases = [
            ("DefaultBool", basic("bool"), False, "false"),
            ("DefaultByte", basic("byte"), 200, -1),
            (self.INT_VARIABLE_NAME, int_type, 42, 1.25),
            ("DefaultInt64", basic("int64"), 9007199254740991, 1.25),
            ("DefaultReal", basic("real"), 2.5, {"bad": True}),
            ("DefaultString", basic("string"), "hello", False),
            ("DefaultName", basic("name"), "PlayerStart", False),
            ("DefaultText", basic("text"), "Hello text", False),
            (
                "DefaultVector",
                library.get_struct_type(
                    unreal.load_object(None, "/Script/CoreUObject.Vector")
                ),
                {"X": 1.0, "Y": 2.0, "Z": 3.0},
                {"Q": 1.0},
            ),
            (
                "DefaultHardObject",
                library.get_object_reference_type(unreal.Actor),
                "/Script/Engine.Default__Actor",
                "/Script/Engine.Default__Texture2D",
            ),
            (
                "DefaultClassPath",
                library.get_class_reference_type(unreal.Actor),
                "/Script/Engine.Character",
                "/Script/Engine.Texture2D",
            ),
            (
                "DefaultNumbers",
                library.get_array_type(int_type),
                [3, 1, 2],
                [1, "two", 3],
            ),
            (
                "DefaultTags",
                library.get_set_type(basic("name")),
                ["Player", "Enemy"],
                ["Player", False],
            ),
            (
                "DefaultFlags",
                library.get_map_type(basic("string"), basic("bool")),
                [{"key": "Enabled", "value": True}],
                [{"key": "Enabled", "value": "yes"}],
            ),
        ]

        action_cases = []
        for variable_name, pin_type, requested, invalid in cases:
            variable_id = (
                self.int_variable_id
                if variable_name == self.INT_VARIABLE_NAME
                else self._add_member_variable(variable_name, pin_type)
            )
            action_cases.append(
                (variable_id, variable_name, requested, invalid)
            )

        for variable_id, variable_name, requested, _ in action_cases:
            with self.subTest(variable_name=variable_name):
                changed = call_action(
                    "blueprint_actions",
                    "ue_set_blueprint_variable_default",
                    asset_path=self.asset_path,
                    variable_id=variable_id,
                    default=requested,
                )
                self.assertSuccess(changed)
                self.assertEqual(changed["data"]["after"], requested)

        for variable_id, variable_name, _, invalid in action_cases:
            with self.subTest(variable_name=variable_name, invalid=invalid):
                before = self._variable_snapshot(variable_name)
                rejected = call_action(
                    "blueprint_actions",
                    "ue_set_blueprint_variable_default",
                    asset_path=self.asset_path,
                    variable_id=variable_id,
                    default=invalid,
                )
                self.assertFalse(rejected.get("success"), rejected)
                self.assertEqual(
                    rejected["errors"][0]["code"], "INVALID_INPUT"
                )
                self.assertEqual(
                    self._variable_snapshot(variable_name), before
                )

    def test_set_blueprint_variable_metadata(self):
        patch = {
            "category": "Scoring",
            "tooltip": "Current score",
            "visible": True,
            "instance_editable": True,
            "expose_on_spawn": True,
            "save_game": True,
            "cinematic": True,
        }
        changed = call_action(
            "blueprint_actions",
            "ue_set_blueprint_variable_metadata",
            asset_path=self.asset_path,
            variable_id=self.variable_id,
            metadata=patch,
        )
        self.assertSuccess(changed)
        for key, value in patch.items():
            self.assertEqual(changed["data"]["after"][key], value)

        visibility = call_action(
            "blueprint_actions",
            "ue_set_blueprint_variable_metadata",
            asset_path=self.asset_path,
            variable_id=self.variable_id,
            metadata={"visible": False},
        )
        self.assertSuccess(visibility)
        self.assertFalse(visibility["data"]["after"]["visible"])
        self.assertEqual(
            visibility["data"]["after"]["category"], "Scoring"
        )
        self.assertTrue(visibility["data"]["after"]["save_game"])

        before = self._variable_snapshot(self.VARIABLE_NAME)
        for metadata in (
            {"unknown": True},
            {"save_game": "yes"},
            {"expose_on_spawn": True, "instance_editable": False},
        ):
            with self.subTest(metadata=metadata):
                rejected = call_action(
                    "blueprint_actions",
                    "ue_set_blueprint_variable_metadata",
                    asset_path=self.asset_path,
                    variable_id=self.variable_id,
                    metadata=metadata,
                )
                self.assertFalse(rejected.get("success"), rejected)
                self.assertEqual(
                    self._variable_snapshot(self.VARIABLE_NAME), before
                )

    def test_set_blueprint_variable_replication(self):
        replicated = call_action(
            "blueprint_actions",
            "ue_set_blueprint_variable_replication",
            asset_path=self.asset_path,
            variable_id=self.variable_id,
            mode="replicated",
            condition="owner_only",
        )
        self.assertSuccess(replicated)
        self.assertEqual(replicated["data"]["after"]["mode"], "replicated")
        self.assertEqual(
            replicated["data"]["after"]["condition"], "owner_only"
        )

        notify_function = self.call(
            "blueprint_actions",
            "ue_create_blueprint_function",
            asset_path=self.asset_path,
            function_name="OnRep_GraphEnabled",
            inputs=[],
            outputs=[],
        )
        self.assertSuccess(notify_function)
        rep_notify = call_action(
            "blueprint_actions",
            "ue_set_blueprint_variable_replication",
            asset_path=self.asset_path,
            variable_id=self.variable_id,
            mode="rep_notify",
            notify_function_name="OnRep_GraphEnabled",
            condition="skip_owner",
        )
        self.assertSuccess(rep_notify)
        self.assertEqual(rep_notify["data"]["after"]["mode"], "rep_notify")
        self.assertEqual(
            rep_notify["data"]["after"]["notify_function_name"],
            "OnRep_GraphEnabled",
        )
        self.assertEqual(
            rep_notify["data"]["after"]["condition"], "skip_owner"
        )

        before = self._variable_snapshot(self.VARIABLE_NAME)
        invalid_requests = (
            {
                "mode": "replicated",
                "notify_function_name": "OnRep_GraphEnabled",
            },
            {"mode": "rep_notify", "notify_function_name": "MissingNotify"},
            {"mode": "replicated", "condition": "dynamic"},
            {"mode": "none", "condition": "owner_only"},
        )
        for params in invalid_requests:
            with self.subTest(params=params):
                rejected = call_action(
                    "blueprint_actions",
                    "ue_set_blueprint_variable_replication",
                    asset_path=self.asset_path,
                    variable_id=self.variable_id,
                    **params,
                )
                self.assertFalse(rejected.get("success"), rejected)
                self.assertEqual(
                    self._variable_snapshot(self.VARIABLE_NAME), before
                )

        cleared = call_action(
            "blueprint_actions",
            "ue_set_blueprint_variable_replication",
            asset_path=self.asset_path,
            variable_id=self.variable_id,
            mode="none",
        )
        self.assertSuccess(cleared)
        self.assertEqual(cleared["data"]["after"]["mode"], "none")
        self.assertEqual(cleared["data"]["after"]["condition"], "none")
        self.assertEqual(
            cleared["data"]["after"]["notify_function_name"], ""
        )

        unsupported_name = f"Blueprint2Object_{uuid.uuid4().hex[:10]}"
        unsupported_factory = unreal.BlueprintFactory()
        unsupported_factory.set_editor_property("parent_class", unreal.Object)
        unsupported = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            unsupported_name,
            BLUEPRINT2_TEST_ROOT,
            unreal.Blueprint,
            unsupported_factory,
        )
        self.assertIsNotNone(unsupported)
        unsupported_path = (
            f"{BLUEPRINT2_TEST_ROOT}/{unsupported_name}.{unsupported_name}"
        )
        self._created_assets.append(unsupported_path)
        self.assertTrue(
            unreal.BlueprintEditorLibrary.add_member_variable(
                unsupported,
                unreal.Name("UnsupportedReplication"),
                unreal.BlueprintEditorLibrary.get_basic_type_by_name(
                    unreal.Name("bool")
                ),
            )
        )
        unsupported_variables = call_action(
            "blueprint_actions",
            "ue_inspect_blueprint",
            asset_path=unsupported_path,
            queries=[{"op": "variables", "detail": "detailed"}],
        )
        self.assertSuccess(unsupported_variables)
        unsupported_id = unsupported_variables["data"]["results"][0][
            "items"
        ][0]["variable_id"]
        rejected_none = call_action(
            "blueprint_actions",
            "ue_set_blueprint_variable_replication",
            asset_path=unsupported_path,
            variable_id=unsupported_id,
            mode="none",
        )
        self.assertFalse(rejected_none.get("success"), rejected_none)
        self.assertEqual(
            rejected_none["errors"][0]["code"], "UE_VERSION_UNSUPPORTED"
        )

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

    def test_set_blueprint_node_properties(self):
        branch = self._add_common_node(
            {"type": "Branch", "pos_x": 160, "pos_y": 120}
        )
        branch_id = branch["data"]["node_id"]
        condition = self._pin(branch_id, "Condition")
        result = call_action(
            "blueprint_actions",
            "ue_set_blueprint_node_properties",
            asset_path=self.asset_path,
            node_id=branch_id,
            properties={
                "comment": "Validated comment",
                "comment_bubble_visible": True,
                "enabled_state": "disabled",
                "position": {"x": 640, "y": 360},
                "pin_defaults": {condition["pin_id"]: False},
            },
        )
        self.assertSuccess(result)
        self.assertEqual(result["changes"][0]["kind"], "update")
        self.assertEqual(result["changes"][0]["target_id"], branch_id)
        self.assertEqual(result["data"]["node_id"], branch_id)
        self.assertEqual(
            result["data"]["properties"],
            {
                "comment": "Validated comment",
                "comment_bubble_visible": True,
                "enabled_state": "disabled",
                "position": {"x": 640, "y": 360},
                "pin_defaults": {condition["pin_id"]: False},
            },
        )
        self.assertEqual(
            self._inspect_node(branch_id)["position"], {"x": 640, "y": 360}
        )
        self.assertEqual(
            self._inspect_node(branch_id)["comment"], "Validated comment"
        )
        self.assertEqual(self._pin(branch_id, "Condition")["default"], "false")

        sequence = self._add_common_node(
            {"type": "Sequence", "output_count": 2}
        )
        select = self._add_common_node(
            {
                "type": "Select",
                "option_count": 2,
                "value_type": {"kind": "int"},
            }
        )
        switches = (
            (
                self._add_common_node(
                    {"type": "Switch", "switch_kind": "int", "cases": [1, 2]}
                ),
                [5, 6, 7],
            ),
            (
                self._add_common_node(
                    {
                        "type": "Switch",
                        "switch_kind": "string",
                        "cases": ["Idle"],
                    }
                ),
                ["Running", "Stopped"],
            ),
            (
                self._add_common_node(
                    {
                        "type": "Switch",
                        "switch_kind": "name",
                        "cases": ["North"],
                    }
                ),
                ["East", "West"],
            ),
        )

        for node, property_name, value in (
            (sequence, "output_count", 4),
            (select, "option_count", 4),
        ):
            with self.subTest(property_name=property_name):
                node_id = node["data"]["node_id"]
                changed = call_action(
                    "blueprint_actions",
                    "ue_set_blueprint_node_properties",
                    asset_path=self.asset_path,
                    node_id=node_id,
                    properties={property_name: value},
                )
                self.assertSuccess(changed)
                pins = self._inspect_pins(node_id)
                self.assertEqual(
                    sorted(changed["data"]["pin_ids"]),
                    sorted(pin["pin_id"] for pin in pins),
                )
                if property_name == "output_count":
                    relevant = [
                        pin for pin in pins if pin["direction"] == "output"
                    ]
                else:
                    relevant = [
                        pin
                        for pin in pins
                        if pin["direction"] == "input" and pin["name"] != "Index"
                    ]
                self.assertEqual(len(relevant), value, relevant)

        sequence_id = sequence["data"]["node_id"]
        last_output = self._pin(sequence_id, "then_3")
        consumer = self._add_common_node({"type": "Branch"})
        consumer_execute = self._pin(consumer["data"]["node_id"], "execute")
        linked = call_action(
            "blueprint_actions",
            "ue_connect_blueprint_pins",
            asset_path=self.asset_path,
            graph_name="EventGraph",
            source_node=sequence_id,
            source_pin=last_output["pin_id"],
            target_node=consumer["data"]["node_id"],
            target_pin=consumer_execute["pin_id"],
        )
        self.assertSuccess(linked)
        reduced = call_action(
            "blueprint_actions",
            "ue_set_blueprint_node_properties",
            asset_path=self.asset_path,
            node_id=sequence_id,
            properties={"output_count": 2},
        )
        self.assertSuccess(reduced)
        self.assertEqual(
            self._pin(consumer["data"]["node_id"], "execute")["link_count"],
            0,
        )

        for switch, cases in switches:
            with self.subTest(cases=cases):
                node_id = switch["data"]["node_id"]
                changed = call_action(
                    "blueprint_actions",
                    "ue_set_blueprint_node_properties",
                    asset_path=self.asset_path,
                    node_id=node_id,
                    properties={"cases": cases},
                )
                self.assertSuccess(changed)
                self.assertEqual(changed["data"]["properties"]["cases"], cases)
                pins = self._inspect_pins(node_id)
                output_names = {
                    pin["name"] for pin in pins if pin["direction"] == "output"
                }
                self.assertTrue(
                    {str(case) for case in cases} <= output_names,
                    output_names,
                )
                self.assertEqual(
                    sorted(changed["data"]["pin_ids"]),
                    sorted(pin["pin_id"] for pin in pins),
                )

        before_workflow = self._graph_snapshot()
        transaction_id = self._begin_workflow("Set Blueprint node properties")
        try:
            changed = call_action(
                "workflow_actions",
                "ue_execute_step",
                transaction_id=transaction_id,
                action_module="UnrealMCPython.blueprint_actions",
                action_name="ue_set_blueprint_node_properties",
                params={
                    "asset_path": self.asset_path,
                    "node_id": branch_id,
                    "properties": {"comment": "Temporary workflow comment"},
                },
            )
            self.assertSuccess(changed)
            self.assertEqual(
                self._inspect_node(branch_id)["comment"],
                "Temporary workflow comment",
            )
            self._rollback_workflow(transaction_id)
            self.assertEqual(self._graph_snapshot(), before_workflow)
        finally:
            context = call_action(
                "workflow_actions", "ue_get_editor_context", asset_paths=[]
            )["workflow_transaction"]
            if context["active"]:
                self._rollback_workflow(transaction_id)

    def test_set_blueprint_node_object_array_default(self):
        container_function = self.call(
            "blueprint_actions",
            "ue_create_blueprint_function",
            asset_path=self.asset_path,
            function_name="ContainerDefaults",
            outputs=[
                {
                    "name": "Actors",
                    "type": {
                        "kind": "array",
                        "item": {
                            "kind": "object",
                            "class_path": "/Script/Engine.Actor",
                        },
                    },
                }
            ],
        )
        self.assertSuccess(container_function)
        function_id = container_function["data"]["function_id"]
        function_nodes = self._inspect(
            [{"op": "nodes", "detail": "detailed", "limit": 500}]
        )[0]["items"]
        result_nodes = [
            node
            for node in function_nodes
            if node["graph_id"] == function_id
            and node["class_path"]
            == "/Script/BlueprintGraph.K2Node_FunctionResult"
        ]
        self.assertEqual(len(result_nodes), 1, result_nodes)
        result_node_id = result_nodes[0]["node_id"]
        actors = self._pin(result_node_id, "Actors")

        changed = call_action(
            "blueprint_actions",
            "ue_set_blueprint_node_properties",
            asset_path=self.asset_path,
            node_id=result_node_id,
            properties={
                "pin_defaults": {
                    actors["pin_id"]: [
                        "/Script/Engine.Default__Actor"
                    ]
                }
            },
        )

        self.assertSuccess(changed)
        self.assertEqual(
            self._pin(result_node_id, "Actors")["default"],
            "(/Script/Engine.Default__Actor)",
        )

    def test_rejected_blueprint_node_properties_do_not_mutate(self):
        branch = self._add_common_node(
            {"type": "Branch", "pos_x": 160, "pos_y": 120}
        )
        other = self._add_common_node(
            {"type": "Branch", "pos_x": 480, "pos_y": 120}
        )
        branch_id = branch["data"]["node_id"]
        condition_id = self._pin(branch_id, "Condition")["pin_id"]
        then_id = self._pin(branch_id, "then")["pin_id"]
        other_condition_id = self._pin(
            other["data"]["node_id"], "Condition"
        )["pin_id"]

        allowed = {
            "comment",
            "comment_bubble_visible",
            "enabled_state",
            "position",
            "pin_defaults",
        }
        for properties in (
            {"comment": "must not persist", "unknown_property": 1},
            {"NodeComment": "read-only reflection spelling"},
            {"class_path": "/Script/BlueprintGraph.K2Node_Select"},
            {"output_count": "not-an-integer"},
        ):
            with self.subTest(properties=properties):
                rejected = self._assert_rejected_call_unchanged(
                    "ue_set_blueprint_node_properties",
                    node_id=branch_id,
                    properties=properties,
                )
                self.assertEqual(
                    set(rejected["errors"][0]["details"]["allowed_properties"]),
                    allowed,
                )

        for pin_defaults in (
            {condition_id: "definitely-not-a-bool"},
            {then_id: "not-an-exec-default"},
            {other_condition_id: True},
            {"pin:11111111-1111-4111-8111-111111111111": True},
        ):
            with self.subTest(pin_defaults=pin_defaults):
                self._assert_rejected_call_unchanged(
                    "ue_set_blueprint_node_properties",
                    node_id=branch_id,
                    properties={
                        "comment": "must not persist",
                        "pin_defaults": pin_defaults,
                    },
                )

        self._assert_rejected_call_unchanged(
            "ue_set_blueprint_node_properties",
            node_id=branch_id,
            properties={},
        )

        reroute = self._add_common_node({"type": "Reroute"})
        reroute_input = next(
            pin
            for pin in self._inspect_pins(reroute["data"]["node_id"])
            if pin["direction"] == "input"
        )
        self._assert_rejected_call_unchanged(
            "ue_set_blueprint_node_properties",
            node_id=reroute["data"]["node_id"],
            properties={"pin_defaults": {reroute_input["pin_id"]: "ignored"}},
        )

        array_call = call_action(
            "blueprint_actions",
            "ue_add_reflected_blueprint_node",
            asset_path=self.asset_path,
            graph_id=self.graph_id,
            member_kind="function",
            member_path="/Script/Engine.KismetMathLibrary:MaxOfIntArray",
            position={"x": 640, "y": 320},
        )
        self.assertSuccess(array_call)
        array_pin = self._pin(array_call["data"]["node_id"], "IntArray")
        self._assert_rejected_call_unchanged(
            "ue_set_blueprint_node_properties",
            node_id=array_call["data"]["node_id"],
            properties={"pin_defaults": {array_pin["pin_id"]: []}},
        )

        switch = self._add_common_node(
            {"type": "Switch", "switch_kind": "int", "cases": [0, 1]}
        )
        self._assert_rejected_call_unchanged(
            "ue_set_blueprint_node_properties",
            node_id=switch["data"]["node_id"],
            properties={"cases": list(range(65))},
        )

    def test_enum_select_option_count_is_rejected_without_mutation(self):
        select = self._add_common_node(
            {
                "type": "Select",
                "option_count": 2,
                "value_type": {"kind": "int"},
            }
        )
        enum_literal = call_action(
            "blueprint_actions",
            "ue_add_reflected_blueprint_node",
            asset_path=self.asset_path,
            graph_id=self.graph_id,
            member_kind="enum_literal",
            member_path="/Script/Engine.ECollisionChannel",
            position={"x": 320, "y": 160},
        )
        self.assertSuccess(enum_literal)
        enum_output = self._pin(enum_literal["data"]["node_id"], "ReturnValue")
        index_pin = self._pin(select["data"]["node_id"], "Index")
        connected = call_action(
            "blueprint_actions",
            "ue_connect_blueprint_pins",
            asset_path=self.asset_path,
            graph_name="EventGraph",
            source_node=enum_literal["data"]["node_id"],
            source_pin=enum_output["pin_id"],
            target_node=select["data"]["node_id"],
            target_pin=index_pin["pin_id"],
        )
        self.assertSuccess(connected)
        typed_index = self._pin(select["data"]["node_id"], "Index")
        self.assertEqual(
            typed_index["type"]["class_path"],
            "/Script/Engine.ECollisionChannel",
        )
        self._assert_rejected_call_unchanged(
            "ue_set_blueprint_node_properties",
            node_id=select["data"]["node_id"],
            properties={"option_count": 3},
        )

    def test_connect_blueprint_pins_by_names_and_stable_ids(self):
        for stable in (False, True):
            with self.subTest(stable=stable):
                branch = self._add_common_node({"type": "Branch"})
                sequence = self._add_common_node(
                    {"type": "Sequence", "output_count": 2}
                )
                source_pin = self._pin(branch["data"]["node_id"], "then")
                target_pin = self._pin(sequence["data"]["node_id"], "execute")
                source_node_arg = (
                    branch["data"]["node_id"] if stable else branch["node_name"]
                )
                source_pin_arg = source_pin["pin_id"] if stable else "then"
                target_node_arg = (
                    sequence["data"]["node_id"]
                    if stable
                    else sequence["node_name"]
                )
                target_pin_arg = target_pin["pin_id"] if stable else "execute"

                connected = call_action(
                    "blueprint_actions",
                    "ue_connect_blueprint_pins",
                    asset_path=self.asset_path,
                    graph_name="EventGraph",
                    source_node=source_node_arg,
                    source_pin=source_pin_arg,
                    target_node=target_node_arg,
                    target_pin=target_pin_arg,
                )
                self.assertSuccess(connected)
                self.assertIn("message", connected)
                self.assertEqual(connected["source_node"], source_node_arg)
                self.assertEqual(connected["source_pin"], source_pin_arg)
                self.assertEqual(connected["target_node"], target_node_arg)
                self.assertEqual(connected["target_pin"], target_pin_arg)
                self.assertEqual(
                    connected["data"]["source_pin_id"], source_pin["pin_id"]
                )
                self.assertEqual(
                    connected["data"]["target_pin_id"], target_pin["pin_id"]
                )
                self.assertEqual(
                    connected["data"]["inserted_conversion_node_ids"], []
                )
                self.assertEqual(connected["changes"][0]["kind"], "create")
                self.assertEqual(
                    self._pin(branch["data"]["node_id"], "then")["link_count"],
                    1,
                )

    def test_legacy_connect_failure_retains_message(self):
        branch = self._add_common_node({"type": "Branch"})
        sequence = self._add_common_node(
            {"type": "Sequence", "output_count": 2}
        )

        rejected = self._assert_rejected_call_unchanged(
            "ue_connect_blueprint_pins",
            graph_name="EventGraph",
            source_node=branch["node_name"],
            source_pin="then",
            target_node=sequence["node_name"],
            target_pin="then_0",
        )

        self.assertEqual(
            rejected["message"], rejected["errors"][0]["message"]
        )

    def test_connection_validation_and_schema_conversion(self):
        first = self._add_common_node({"type": "Branch"})
        second = self._add_common_node({"type": "Branch"})
        first_condition = self._pin(first["data"]["node_id"], "Condition")
        second_condition = self._pin(second["data"]["node_id"], "Condition")
        first_then = self._pin(first["data"]["node_id"], "then")

        same_direction = self._assert_rejected_call_unchanged(
            "ue_connect_blueprint_pins",
            graph_name="EventGraph",
            source_node=first["data"]["node_id"],
            source_pin=first_condition["pin_id"],
            target_node=second["data"]["node_id"],
            target_pin=second_condition["pin_id"],
        )
        self.assertIn("direction", same_direction["errors"][0]["message"].lower())

        incompatible = self._assert_rejected_call_unchanged(
            "ue_connect_blueprint_pins",
            graph_name="EventGraph",
            source_node=first["data"]["node_id"],
            source_pin=first_then["pin_id"],
            target_node=second["data"]["node_id"],
            target_pin=second_condition["pin_id"],
        )
        details = incompatible["errors"][0]["details"]
        self.assertIn("source_type", details)
        self.assertIn("target_type", details)

        second_then = self._pin(second["data"]["node_id"], "then")
        sequence = self._add_common_node(
            {"type": "Sequence", "output_count": 2}
        )
        execute = self._pin(sequence["data"]["node_id"], "execute")
        first_link = call_action(
            "blueprint_actions",
            "ue_connect_blueprint_pins",
            asset_path=self.asset_path,
            graph_name="EventGraph",
            source_node=first["data"]["node_id"],
            source_pin=first_then["pin_id"],
            target_node=sequence["data"]["node_id"],
            target_pin=execute["pin_id"],
        )
        self.assertSuccess(first_link)
        before_replacement = self._connection_ids()
        replacement = call_action(
            "blueprint_actions",
            "ue_connect_blueprint_pins",
            asset_path=self.asset_path,
            graph_name="EventGraph",
            source_node=second["data"]["node_id"],
            source_pin=second_then["pin_id"],
            target_node=sequence["data"]["node_id"],
            target_pin=execute["pin_id"],
        )
        self.assertSuccess(replacement)
        after_replacement = self._connection_ids()
        expected_replacement_changes = {
            *(('delete', connection_id) for connection_id in before_replacement - after_replacement),
            *(('create', connection_id) for connection_id in after_replacement - before_replacement),
        }
        self.assertEqual(
            {
                (change["kind"], change["target_id"])
                for change in replacement["changes"]
            },
            expected_replacement_changes,
        )
        self.assertEqual(
            [change["target_id"] for change in replacement["changes"]],
            sorted(change["target_id"] for change in replacement["changes"]),
        )

        int_get = self._add_common_node(
            {"type": "VariableGet", "variable_id": self.int_variable_id}
        )
        print_string = call_action(
            "blueprint_actions",
            "ue_add_reflected_blueprint_node",
            asset_path=self.asset_path,
            graph_id=self.graph_id,
            member_kind="function",
            member_path="/Script/Engine.KismetSystemLibrary:PrintString",
            position={"x": 720, "y": 320},
        )
        self.assertSuccess(print_string)
        return_pin = self._pin(
            int_get["data"]["node_id"], self.INT_VARIABLE_NAME
        )
        string_pin = self._pin(print_string["data"]["node_id"], "InString")
        before_count = self._node_count()
        before_connections = self._connection_ids()

        converted = call_action(
            "blueprint_actions",
            "ue_connect_blueprint_pins",
            asset_path=self.asset_path,
            graph_name="EventGraph",
            source_node=int_get["data"]["node_id"],
            source_pin=return_pin["pin_id"],
            target_node=print_string["data"]["node_id"],
            target_pin=string_pin["pin_id"],
        )
        self.assertSuccess(converted)
        inserted = converted["data"]["inserted_conversion_node_ids"]
        self.assertGreaterEqual(len(inserted), 1, converted)
        self.assertTrue(all(node_id.startswith("node:") for node_id in inserted))
        self.assertEqual(self._node_count(), before_count + len(inserted))
        for node_id in inserted:
            self._inspect_node(node_id)
        after_connections = self._connection_ids()
        self.assertEqual(
            {
                (change["kind"], change["target_id"])
                for change in converted["changes"]
            },
            {
                *(('delete', connection_id) for connection_id in before_connections - after_connections),
                *(('create', connection_id) for connection_id in after_connections - before_connections),
            },
        )
        self.assertEqual(
            [change["target_id"] for change in converted["changes"]],
            sorted(change["target_id"] for change in converted["changes"]),
        )
        requested_connection_id = (
            f"connection:{return_pin['pin_id']}->{string_pin['pin_id']}"
        )
        self.assertNotIn(requested_connection_id, after_connections)

    def test_disconnect_blueprint_pins(self):
        branch = self._add_common_node({"type": "Branch"})
        sequence = self._add_common_node(
            {"type": "Sequence", "output_count": 2}
        )
        source = self._pin(branch["data"]["node_id"], "then")
        target = self._pin(sequence["data"]["node_id"], "execute")
        connected = call_action(
            "blueprint_actions",
            "ue_connect_blueprint_pins",
            asset_path=self.asset_path,
            graph_name="EventGraph",
            source_node=branch["data"]["node_id"],
            source_pin=source["pin_id"],
            target_node=sequence["data"]["node_id"],
            target_pin=target["pin_id"],
        )
        self.assertSuccess(connected)

        self._assert_rejected_call_unchanged(
            "ue_disconnect_blueprint_pins",
            pin_id=source["pin_id"],
            source_pin_id=None,
        )

        disconnected = call_action(
            "blueprint_actions",
            "ue_disconnect_blueprint_pins",
            asset_path=self.asset_path,
            source_pin_id=source["pin_id"],
            target_pin_id=target["pin_id"],
        )
        self.assertSuccess(disconnected)
        self.assertEqual(len(disconnected["changes"]), 1)
        self.assertEqual(disconnected["changes"][0]["kind"], "delete")
        self.assertEqual(self._pin(branch["data"]["node_id"], "then")["link_count"], 0)
        self._assert_rejected_call_unchanged(
            "ue_disconnect_blueprint_pins",
            expected_code="PRECONDITION_FAILED",
            source_pin_id=source["pin_id"],
            target_pin_id=target["pin_id"],
        )

        operator = self._add_common_node(
            {
                "type": "Operator",
                "operator": "add",
                "operand_type": {"kind": "int"},
            }
        )
        selects = [
            self._add_common_node(
                {
                    "type": "Select",
                    "option_count": 2,
                    "value_type": {"kind": "int"},
                }
            )
            for _ in range(2)
        ]
        output = self._pin(operator["data"]["node_id"], "ReturnValue")
        option_pins = [
            self._pin(select["data"]["node_id"], "Option 0")
            for select in selects
        ]
        for select, option in zip(selects, option_pins):
            linked = call_action(
                "blueprint_actions",
                "ue_connect_blueprint_pins",
                asset_path=self.asset_path,
                graph_name="EventGraph",
                source_node=operator["data"]["node_id"],
                source_pin=output["pin_id"],
                target_node=select["data"]["node_id"],
                target_pin=option["pin_id"],
            )
            self.assertSuccess(linked)
        self.assertEqual(
            self._pin(operator["data"]["node_id"], "ReturnValue")["link_count"],
            2,
        )

        broken_all = call_action(
            "blueprint_actions",
            "ue_disconnect_blueprint_pins",
            asset_path=self.asset_path,
            pin_id=output["pin_id"],
        )
        self.assertSuccess(broken_all)
        self.assertEqual(len(broken_all["changes"]), 2)
        self.assertTrue(
            all(change["kind"] == "delete" for change in broken_all["changes"])
        )
        self.assertEqual(
            self._pin(operator["data"]["node_id"], "ReturnValue")["link_count"],
            0,
        )

        self._assert_rejected_call_unchanged("ue_disconnect_blueprint_pins")
        self._assert_rejected_call_unchanged(
            "ue_disconnect_blueprint_pins",
            pin_id=output["pin_id"],
            source_pin_id=source["pin_id"],
            target_pin_id=target["pin_id"],
        )

        reconnected = call_action(
            "blueprint_actions",
            "ue_connect_blueprint_pins",
            asset_path=self.asset_path,
            graph_name="EventGraph",
            source_node=branch["data"]["node_id"],
            source_pin=source["pin_id"],
            target_node=sequence["data"]["node_id"],
            target_pin=target["pin_id"],
        )
        self.assertSuccess(reconnected)
        before_workflow = self._graph_snapshot()
        transaction_id = self._begin_workflow("Disconnect Blueprint pins")
        try:
            removed = call_action(
                "workflow_actions",
                "ue_execute_step",
                transaction_id=transaction_id,
                action_module="UnrealMCPython.blueprint_actions",
                action_name="ue_disconnect_blueprint_pins",
                params={
                    "asset_path": self.asset_path,
                    "source_pin_id": source["pin_id"],
                    "target_pin_id": target["pin_id"],
                },
            )
            self.assertSuccess(removed)
            self.assertEqual(
                self._pin(branch["data"]["node_id"], "then")["link_count"],
                0,
            )
            self._rollback_workflow(transaction_id)
            self.assertEqual(self._graph_snapshot(), before_workflow)
        finally:
            context = call_action(
                "workflow_actions", "ue_get_editor_context", asset_paths=[]
            )["workflow_transaction"]
            if context["active"]:
                self._rollback_workflow(transaction_id)

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

    def test_rename_blueprint_component(self):
        camera = self._add_component(
            "RenameCamera", "/Script/Engine.CameraComponent"
        )
        other = self._add_component(
            "RenameCollision", "/Script/Engine.BoxComponent"
        )
        getter = self._add_common_node(
            {"type": "VariableGet", "variable_name": "RenameCamera"}
        )

        renamed = call_action(
            "blueprint_actions",
            "ue_rename_blueprint_component",
            asset_path=self.asset_path,
            component_id=camera["component_id"],
            new_name="RenamedCamera",
        )
        self.assertSuccess(renamed)
        self.assertEqual(renamed["data"]["component_id"], camera["component_id"])
        self.assertEqual(
            self._component_record("RenamedCamera")["component_id"],
            camera["component_id"],
        )
        inspected_getter = self._inspect_node(getter["data"]["node_id"])
        self.assertIn(
            "RenamedCamera", [pin["name"] for pin in inspected_getter["pins"]]
        )
        compiled = call_action(
            "blueprint_actions",
            "ue_compile_blueprint",
            asset_path=self.asset_path,
        )
        self.assertSuccess(compiled)

        self._assert_component_rejected_unchanged(
            "ue_rename_blueprint_component",
            expected_code="CONFLICT",
            component_id=camera["component_id"],
            new_name=other["name"],
        )
        self._assert_component_rejected_unchanged(
            "ue_rename_blueprint_component",
            expected_code="CONFLICT",
            component_id=camera["component_id"],
            new_name=self.VARIABLE_NAME,
        )

        unreal.BlueprintEditorLibrary.compile_blueprint(self.blueprint)
        child_name = f"Blueprint2ComponentChild_{uuid.uuid4().hex[:10]}"
        child_factory = unreal.BlueprintFactory()
        child_factory.set_editor_property(
            "parent_class", self.blueprint.generated_class()
        )
        child_blueprint = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            child_name,
            BLUEPRINT2_TEST_ROOT,
            unreal.Blueprint,
            child_factory,
        )
        self.assertIsNotNone(child_blueprint)
        child_path = f"{BLUEPRINT2_TEST_ROOT}/{child_name}.{child_name}"
        self._created_assets.append(child_path)
        self.assertTrue(
            unreal.BlueprintEditorLibrary.add_member_variable(
                child_blueprint,
                unreal.Name("ChildComponentConflict"),
                unreal.BlueprintEditorLibrary.get_basic_type_by_name(
                    unreal.Name("bool")
                ),
            )
        )
        self._assert_component_rejected_unchanged(
            "ue_rename_blueprint_component",
            expected_code="CONFLICT",
            component_id=camera["component_id"],
            new_name="ChildComponentConflict",
        )
        self._assert_component_rejected_unchanged(
            "ue_rename_blueprint_component",
            expected_code="PRECONDITION_FAILED",
            component_id="component:aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
            new_name="MissingComponent",
        )

    def test_reparent_blueprint_component(self):
        parent = self._add_component(
            "HierarchyParent", "/Script/Engine.SceneComponent"
        )
        child = self._add_component(
            "HierarchyChild",
            "/Script/Engine.SceneComponent",
            parent_name="HierarchyParent",
        )
        grandchild = self._add_component(
            "HierarchyGrandchild",
            "/Script/Engine.SceneComponent",
            parent_name="HierarchyChild",
        )

        self._assert_component_rejected_unchanged(
            "ue_reparent_blueprint_component",
            expected_code="CONFLICT",
            component_id=parent["component_id"],
            parent_component_id=grandchild["component_id"],
        )
        self._assert_component_rejected_unchanged(
            "ue_reparent_blueprint_component",
            expected_code="CONFLICT",
            component_id=child["component_id"],
            parent_component_id=child["component_id"],
        )
        self._assert_component_rejected_unchanged(
            "ue_reparent_blueprint_component",
            expected_code="PRECONDITION_FAILED",
            component_id=child["component_id"],
            parent_component_id=(
                "component:aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
            ),
        )

        rooted = call_action(
            "blueprint_actions",
            "ue_reparent_blueprint_component",
            asset_path=self.asset_path,
            component_id=child["component_id"],
            parent_component_id=None,
        )
        self.assertSuccess(rooted)
        self.assertEqual(self._component_record("HierarchyChild")["parent_id"], "")
        self.assertEqual(
            self._component_record("HierarchyGrandchild")["parent_id"],
            child["component_id"],
        )

        before_workflow = self._component_snapshot()
        transaction_id = self._begin_workflow("Reparent Blueprint component")
        try:
            moved = call_action(
                "workflow_actions",
                "ue_execute_step",
                transaction_id=transaction_id,
                action_module="UnrealMCPython.blueprint_actions",
                action_name="ue_reparent_blueprint_component",
                params={
                    "asset_path": self.asset_path,
                    "component_id": grandchild["component_id"],
                    "parent_component_id": parent["component_id"],
                },
            )
            self.assertSuccess(moved)
            self.assertEqual(
                self._component_record("HierarchyGrandchild")["parent_id"],
                parent["component_id"],
            )
            self._rollback_workflow(transaction_id)
            self.assertEqual(self._component_snapshot(), before_workflow)
        finally:
            context = call_action(
                "workflow_actions", "ue_get_editor_context", asset_paths=[]
            )["workflow_transaction"]
            if context["active"]:
                self._rollback_workflow(transaction_id)

    def test_reorder_blueprint_component(self):
        parent = self._add_component(
            "OrderParent", "/Script/Engine.SceneComponent"
        )
        children = [
            self._add_component(
                name,
                "/Script/Engine.SceneComponent",
                parent_name="OrderParent",
            )
            for name in ("OrderA", "OrderB", "OrderC")
        ]

        def indexes():
            return {
                name: self._component_record(name)["sibling_index"]
                for name in ("OrderA", "OrderB", "OrderC")
            }

        self.assertEqual(indexes(), {"OrderA": 0, "OrderB": 1, "OrderC": 2})
        for component, requested, expected in (
            (children[2], 0, {"OrderC": 0, "OrderA": 1, "OrderB": 2}),
            (children[2], 1, {"OrderA": 0, "OrderC": 1, "OrderB": 2}),
            (children[2], 2, {"OrderA": 0, "OrderB": 1, "OrderC": 2}),
        ):
            moved = call_action(
                "blueprint_actions",
                "ue_reorder_blueprint_component",
                asset_path=self.asset_path,
                component_id=component["component_id"],
                sibling_index=requested,
            )
            self.assertSuccess(moved)
            self.assertEqual(indexes(), expected)

        self._assert_component_rejected_unchanged(
            "ue_reorder_blueprint_component",
            component_id=children[0]["component_id"],
            sibling_index=3,
        )
        self._assert_component_rejected_unchanged(
            "ue_reorder_blueprint_component",
            expected_code="PRECONDITION_FAILED",
            component_id="component:aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
            sibling_index=0,
        )

        movement = self._add_component(
            "OrderMovement",
            "/Script/Engine.RotatingMovementComponent",
            parent_name="OrderParent",
        )
        rooted = call_action(
            "blueprint_actions",
            "ue_reparent_blueprint_component",
            asset_path=self.asset_path,
            component_id=movement["component_id"],
            parent_component_id=None,
        )
        self.assertSuccess(rooted)
        before_root_reorder = self._component_snapshot()
        root_reorder = call_action(
            "blueprint_actions",
            "ue_reorder_blueprint_component",
            asset_path=self.asset_path,
            component_id=movement["component_id"],
            sibling_index=0,
        )
        if not root_reorder.get("success"):
            self.assertEqual(
                root_reorder["errors"][0]["code"], "UE_VERSION_UNSUPPORTED"
            )
            self.assertEqual(
                root_reorder["errors"][0]["details"]["capability"],
                "root_component_reorder",
            )
            self.assertEqual(self._component_snapshot(), before_root_reorder)
        else:
            self.assertEqual(
                self._component_record("OrderMovement")["sibling_index"], 0
            )

    def test_set_blueprint_component_transform(self):
        scene = self._add_component(
            "TransformScene", "/Script/Engine.SceneComponent"
        )
        initial = self._component_record("TransformScene")["defaults"]

        for transform in (
            {"location": [10.5, -20.25, 30.75]},
            {"rotation": [15, 25, 35]},
            {"scale": [2, 3, 4]},
        ):
            changed = call_action(
                "blueprint_actions",
                "ue_set_blueprint_component_transform",
                asset_path=self.asset_path,
                component_id=scene["component_id"],
                transform=transform,
            )
            self.assertSuccess(changed)

        defaults = self._component_record("TransformScene")["defaults"]
        self.assertEqual(
            defaults["relative_location"], {"x": 10.5, "y": -20.25, "z": 30.75}
        )
        for field, expected in (("pitch", 15), ("yaw", 25), ("roll", 35)):
            self.assertAlmostEqual(
                defaults["relative_rotation"][field], expected, places=6
            )
        self.assertEqual(
            defaults["relative_scale"], {"x": 2, "y": 3, "z": 4}
        )
        self.assertNotEqual(defaults, initial)

        self._assert_component_rejected_unchanged(
            "ue_set_blueprint_component_transform",
            component_id=scene["component_id"],
            transform={"location": [0, 0, 1_000_000_001]},
        )
        self._assert_component_rejected_unchanged(
            "ue_set_blueprint_component_transform",
            expected_code="PRECONDITION_FAILED",
            component_id="component:aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
            transform={"location": [0, 0, 0]},
        )
        movement = self._add_component(
            "TransformMovement", "/Script/Engine.RotatingMovementComponent"
        )
        self._assert_component_rejected_unchanged(
            "ue_set_blueprint_component_transform",
            expected_code="PRECONDITION_FAILED",
            component_id=movement["component_id"],
            transform={"location": [0, 0, 0]},
        )

    def test_legacy_graph_mutation_workflow_rollbacks_continued(self):
        branch = self._add_common_node(
            {"type": "Branch", "pos_x": 160, "pos_y": 120}
        )
        sequence = self._add_common_node(
            {"type": "Sequence", "output_count": 2, "pos_x": 480, "pos_y": 120}
        )

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
