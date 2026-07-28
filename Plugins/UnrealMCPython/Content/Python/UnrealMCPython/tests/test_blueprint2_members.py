"""In-editor tests for Universal Blueprint 2 function authoring."""

import unittest
import uuid

import unreal

from UnrealMCPython.tests.base import MCPTestCase
from UnrealMCPython.tests.blueprint2_support import (
    BLUEPRINT2_TEST_ROOT,
    call_action,
)


ACTOR_TYPE = {"kind": "object", "class_path": "/Script/Engine.Actor"}
DOUBLE_TYPE = {"kind": "real", "precision": "double"}
BOOL_TYPE = {"kind": "bool"}
INT_TYPE = {"kind": "int"}
TEXT_TYPE = {"kind": "text"}
INT_ARRAY_TYPE = {"kind": "array", "item": INT_TYPE}
INT_BOOL_MAP_TYPE = {"kind": "map", "key": INT_TYPE, "value": BOOL_TYPE}


class TestBlueprint2Members(MCPTestCase):

    FUNCTION_NAME = "ComputeScore"

    def setUp(self):
        unreal.EditorAssetLibrary.make_directory(BLUEPRINT2_TEST_ROOT)
        self._created_assets = []
        self.addCleanup(self._cleanup_created_assets)

        name = f"Blueprint2Members_{uuid.uuid4().hex[:10]}"
        factory = unreal.BlueprintFactory()
        factory.set_editor_property("parent_class", unreal.Actor)
        self.blueprint = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, BLUEPRINT2_TEST_ROOT, unreal.Blueprint, factory
        )
        self.assertIsNotNone(
            self.blueprint, "Actor Blueprint fixture could not be created"
        )
        self.asset_path = f"{BLUEPRINT2_TEST_ROOT}/{name}.{name}"
        self._created_assets.append(self.asset_path)

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
            BLUEPRINT2_TEST_ROOT, recursive=True, include_folder=False
        )
        if remaining:
            raise AssertionError(
                f"Blueprint 2 member tests left assets behind: {remaining}"
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
                "Failed to remove empty Blueprint 2 test root: "
                f"{BLUEPRINT2_TEST_ROOT}"
            )

    def _seed_function(self, name=None):
        graph = unreal.BlueprintEditorLibrary.add_function_graph(
            self.blueprint, name or self.FUNCTION_NAME
        )
        self.assertIsNotNone(graph)
        inspected = self._inspect_function(name or self.FUNCTION_NAME)
        self.assertIsNotNone(inspected)
        return graph, inspected

    def _inspect_functions(self, asset_path=None, name_pattern=None):
        query = {"op": "functions", "detail": "detailed"}
        if name_pattern:
            query["name_pattern"] = name_pattern
        result = call_action(
            "blueprint_actions",
            "ue_inspect_blueprint",
            asset_path=asset_path or self.asset_path,
            queries=[query],
        )
        self.assertSuccess(result)
        return result["data"]["results"][0]["items"]

    def _inspect_function(self, name, asset_path=None):
        return next(
            (
                item
                for item in self._inspect_functions(asset_path, name)
                if item["name"] == name
            ),
            None,
        )

    def _initial_inputs(self):
        return [
            {"name": "TargetActor", "type": ACTOR_TYPE},
            {"name": "Weight", "type": DOUBLE_TYPE, "default": 1.5},
        ]

    def _initial_outputs(self):
        return [
            {"name": "Accepted", "type": BOOL_TYPE},
            {"name": "Score", "type": DOUBLE_TYPE},
        ]

    def _replacement_inputs(self):
        return [
            {"name": "Multiplier", "type": DOUBLE_TYPE, "default": 2.25},
            {"name": "SourceActor", "type": ACTOR_TYPE},
        ]

    def _replacement_outputs(self):
        return [
            {"name": "FinalScore", "type": DOUBLE_TYPE},
            {"name": "WasAccepted", "type": BOOL_TYPE},
        ]

    def _assert_signature(self, function, inputs, outputs, **metadata):
        actual = function["metadata"]
        self.assertEqual(
            [parameter["name"] for parameter in actual["inputs"]],
            [parameter["name"] for parameter in inputs],
        )
        self.assertEqual(
            [parameter["name"] for parameter in actual["outputs"]],
            [parameter["name"] for parameter in outputs],
        )
        for actual_parameter, expected_parameter in zip(
            actual["inputs"] + actual["outputs"], inputs + outputs
        ):
            self.assertEqual(actual_parameter["type"], expected_parameter["type"])
            if "default" in expected_parameter:
                self.assertEqual(
                    actual_parameter["default"], expected_parameter["default"]
                )
        for key, expected in metadata.items():
            self.assertEqual(actual[key], expected, key)

    def _assert_change(self, result, operation):
        self.assertSuccess(result)
        self.assertEqual(len(result["changes"]), 1)
        self.assertEqual(result["changes"][0]["kind"], operation)
        self.assertEqual(len(result["next_actions"]), 1)
        self.assertEqual(
            result["next_actions"][0]["action"], "compile_blueprint"
        )

    def _compile_without_errors(self):
        compiled = self.call(
            "blueprint_actions", "ue_compile_blueprint", asset_path=self.asset_path
        )
        self.assertSuccess(compiled)
        self.assertNotEqual(compiled["status"], "Error")

    def _assert_rejected(self, result, code, path):
        self.assertFalse(result["success"], result)
        self.assertEqual(result["errors"][0]["code"], code)
        self.assertEqual(result["errors"][0]["path"], path)

    def test_create_blueprint_function(self):
        created = self.call(
            "blueprint_actions",
            "ue_create_blueprint_function",
            asset_path=self.asset_path,
            function_name=self.FUNCTION_NAME,
            inputs=self._initial_inputs(),
            outputs=self._initial_outputs(),
            pure=True,
            const=True,
            access="public",
            category="Scoring",
            description="Computes a weighted score.",
        )

        self._assert_change(created, "create")
        function = self._inspect_function(self.FUNCTION_NAME)
        self.assertIsNotNone(function)
        self.assertEqual(function["function_id"], created["data"]["function_id"])
        self.assertTrue(function["function_id"].startswith("graph:"))
        self._assert_signature(
            function,
            self._initial_inputs(),
            self._initial_outputs(),
            pure=True,
            const=True,
            access="public",
            category="Scoring",
            description="Computes a weighted score.",
        )
        self._compile_without_errors()
        self.assertIsNotNone(self._inspect_function(self.FUNCTION_NAME))

    def test_set_blueprint_function_signature(self):
        _, before = self._seed_function()
        function_id = before["function_id"]
        initialized = self.call(
            "blueprint_actions",
            "ue_set_blueprint_function_signature",
            asset_path=self.asset_path,
            function_id=function_id,
            inputs=self._initial_inputs(),
            outputs=self._initial_outputs(),
            pure=False,
            const=False,
            access="protected",
            category="Before",
            description="Initial signature.",
        )
        self._assert_change(initialized, "update")
        initial = self._inspect_function(self.FUNCTION_NAME)
        self.assertEqual(initial["function_id"], function_id)
        self._assert_signature(
            initial,
            self._initial_inputs(),
            self._initial_outputs(),
            pure=False,
            const=False,
            access="protected",
            category="Before",
            description="Initial signature.",
        )

        updated = self.call(
            "blueprint_actions",
            "ue_set_blueprint_function_signature",
            asset_path=self.asset_path,
            function_id=function_id,
            inputs=self._replacement_inputs(),
            outputs=self._replacement_outputs(),
            pure=True,
            const=True,
            access="private",
            category="After",
            description="Replacement signature.",
        )

        self._assert_change(updated, "update")
        replacement = self._inspect_function(self.FUNCTION_NAME)
        self.assertEqual(replacement["function_id"], function_id)
        self._assert_signature(
            replacement,
            self._replacement_inputs(),
            self._replacement_outputs(),
            pure=True,
            const=True,
            access="private",
            category="After",
            description="Replacement signature.",
        )
        self._compile_without_errors()

    def test_invalid_signature_replacement_is_atomic(self):
        _, before = self._seed_function()
        function_id = before["function_id"]
        initialized = self.call(
            "blueprint_actions",
            "ue_set_blueprint_function_signature",
            asset_path=self.asset_path,
            function_id=function_id,
            inputs=self._initial_inputs(),
            outputs=self._initial_outputs(),
            pure=True,
            const=True,
            access="protected",
            category="Stable",
            description="Must survive a rejected replacement.",
        )
        self._assert_change(initialized, "update")

        rejected = self.call(
            "blueprint_actions",
            "ue_set_blueprint_function_signature",
            asset_path=self.asset_path,
            function_id=function_id,
            inputs=self._replacement_inputs(),
            outputs=[
                {"name": "Multiplier", "type": BOOL_TYPE},
                {"name": "Other", "type": DOUBLE_TYPE},
            ],
            pure=False,
            const=False,
            access="private",
            category="Rejected",
            description="Must not be applied.",
        )

        self._assert_rejected(rejected, "INVALID_INPUT", "outputs[0].name")
        unchanged = self._inspect_function(self.FUNCTION_NAME)
        self.assertEqual(unchanged["function_id"], function_id)
        self._assert_signature(
            unchanged,
            self._initial_inputs(),
            self._initial_outputs(),
            pure=True,
            const=True,
            access="protected",
            category="Stable",
            description="Must survive a rejected replacement.",
        )

    def test_signature_round_trips_canonical_types_and_defaults(self):
        inputs = [
            {
                "name": "ObjectValue",
                "type": ACTOR_TYPE,
                "default": "/Script/Engine.Default__Actor",
            },
            {"name": "TextValue", "type": TEXT_TYPE, "default": "Hello text"},
            {"name": "Numbers", "type": INT_ARRAY_TYPE, "default": [3, 1, 2]},
            {
                "name": "Flags",
                "type": INT_BOOL_MAP_TYPE,
                "default": [{"key": 7, "value": True}],
            },
        ]
        created = self.call(
            "blueprint_actions",
            "ue_create_blueprint_function",
            asset_path=self.asset_path,
            function_name=self.FUNCTION_NAME,
            inputs=inputs,
            outputs=[],
        )
        self._assert_change(created, "create")

        before_compile = self._inspect_function(self.FUNCTION_NAME)
        self._assert_signature(
            before_compile,
            inputs,
            [],
            pure=False,
            const=False,
            access="public",
            category="",
            description="",
        )
        self._compile_without_errors()
        after_compile = self._inspect_function(self.FUNCTION_NAME)
        self._assert_signature(
            after_compile,
            inputs,
            [],
            pure=False,
            const=False,
            access="public",
            category="",
            description="",
        )

    def test_rename_blueprint_function(self):
        _, before = self._seed_function()
        function_id = before["function_id"]
        renamed = self.call(
            "blueprint_actions",
            "ue_rename_blueprint_function",
            asset_path=self.asset_path,
            function_id=function_id,
            new_name="ComputeFinalScore",
        )

        self._assert_change(renamed, "update")
        self.assertIsNone(self._inspect_function(self.FUNCTION_NAME))
        after = self._inspect_function("ComputeFinalScore")
        self.assertIsNotNone(after)
        self.assertEqual(after["function_id"], function_id)
        self.assertEqual(renamed["data"]["function_id"], function_id)
        self._compile_without_errors()

    def test_delete_blueprint_function(self):
        _, before = self._seed_function()
        function_id = before["function_id"]
        deleted = self.call(
            "blueprint_actions",
            "ue_delete_blueprint_function",
            asset_path=self.asset_path,
            function_id=function_id,
        )

        self._assert_change(deleted, "delete")
        self.assertEqual(deleted["data"]["function_id"], function_id)
        self.assertIsNone(self._inspect_function(self.FUNCTION_NAME))

    def test_duplicate_member_name_is_rejected(self):
        self._seed_function()
        duplicate = self.call(
            "blueprint_actions",
            "ue_create_blueprint_function",
            asset_path=self.asset_path,
            function_name=self.FUNCTION_NAME,
        )
        self._assert_rejected(duplicate, "CONFLICT", "function_name")

    def test_event_graph_name_is_rejected_without_renaming_the_graph(self):
        event_graph = unreal.BlueprintEditorLibrary.find_event_graph(self.blueprint)
        self.assertIsNotNone(event_graph)
        graph_name = event_graph.get_name()
        graph_path = event_graph.get_path_name()

        rejected = self.call(
            "blueprint_actions",
            "ue_create_blueprint_function",
            asset_path=self.asset_path,
            function_name=graph_name,
        )

        self._assert_rejected(rejected, "CONFLICT", "function_name")
        unchanged = unreal.BlueprintEditorLibrary.find_event_graph(self.blueprint)
        self.assertIsNotNone(unchanged)
        self.assertEqual(unchanged.get_name(), graph_name)
        self.assertEqual(unchanged.get_path_name(), graph_path)
        self.assertIsNone(self._inspect_function(graph_name))

    def test_inherited_function_name_is_rejected(self):
        inherited_name = "K2_GetActorLocation"
        self.assertIsNone(self._inspect_function(inherited_name))

        rejected = self.call(
            "blueprint_actions",
            "ue_create_blueprint_function",
            asset_path=self.asset_path,
            function_name=inherited_name,
        )

        self._assert_rejected(rejected, "CONFLICT", "function_name")
        self.assertIsNone(self._inspect_function(inherited_name))

    @unittest.skipUnless(
        hasattr(unreal, "BlueprintMacroFactory"),
        "Blueprint macro factory is unavailable",
    )
    def test_macro_library_target_is_rejected_without_creating_a_graph(self):
        name = f"Blueprint2MacroMembers_{uuid.uuid4().hex[:10]}"
        macro_library = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name,
            BLUEPRINT2_TEST_ROOT,
            unreal.Blueprint,
            unreal.BlueprintMacroFactory(),
        )
        self.assertIsNotNone(macro_library)
        macro_path = f"{BLUEPRINT2_TEST_ROOT}/{name}.{name}"
        self._created_assets.append(macro_path)
        before = self._inspect_functions(asset_path=macro_path)

        rejected = self.call(
            "blueprint_actions",
            "ue_create_blueprint_function",
            asset_path=macro_path,
            function_name=self.FUNCTION_NAME,
        )

        self._assert_rejected(rejected, "PRECONDITION_FAILED", "asset_path")
        self.assertEqual(self._inspect_functions(asset_path=macro_path), before)

    def test_duplicate_parameter_name_is_rejected(self):
        duplicate = self.call(
            "blueprint_actions",
            "ue_create_blueprint_function",
            asset_path=self.asset_path,
            function_name=self.FUNCTION_NAME,
            inputs=[
                {"name": "Score", "type": DOUBLE_TYPE},
                {"name": "Score", "type": BOOL_TYPE},
            ],
        )
        self._assert_rejected(duplicate, "INVALID_INPUT", "inputs[1].name")

    def test_empty_parameter_name_reports_the_exact_path(self):
        invalid = self.call(
            "blueprint_actions",
            "ue_create_blueprint_function",
            asset_path=self.asset_path,
            function_name=self.FUNCTION_NAME,
            inputs=[{"name": "", "type": BOOL_TYPE}],
        )
        self._assert_rejected(invalid, "INVALID_INPUT", "inputs[0].name")

    def test_parameter_validation_reports_fully_qualified_paths(self):
        invalid_parameters = [
            ({"type": BOOL_TYPE}, "inputs[0].name"),
            ({"name": 7, "type": BOOL_TYPE}, "inputs[0].name"),
            (
                {"name": "Value", "type": BOOL_TYPE, "extra": True},
                "inputs[0].extra",
            ),
        ]
        for parameter, expected_path in invalid_parameters:
            with self.subTest(expected_path=expected_path):
                invalid = self.call(
                    "blueprint_actions",
                    "ue_create_blueprint_function",
                    asset_path=self.asset_path,
                    function_name=self.FUNCTION_NAME,
                    inputs=[parameter],
                )
                self._assert_rejected(invalid, "INVALID_INPUT", expected_path)

    def test_native_member_names_match_the_public_ascii_contract(self):
        for function_name in ("1Score", "Score-Value", "Счёт"):
            with self.subTest(function_name=function_name):
                invalid = self.call(
                    "blueprint_actions",
                    "ue_create_blueprint_function",
                    asset_path=self.asset_path,
                    function_name=function_name,
                )
                self._assert_rejected(invalid, "INVALID_INPUT", "function_name")

    def test_optional_fallback_qualifiers_reject_wrong_json_types(self):
        _, before = self._seed_function()
        function_id = before["function_id"]
        for field in (
            "function_owner_id",
            "function_name",
            "function_type_path",
        ):
            with self.subTest(field=field):
                rejected = self.call(
                    "blueprint_actions",
                    "ue_set_blueprint_function_signature",
                    asset_path=self.asset_path,
                    function_id=function_id,
                    inputs=self._initial_inputs(),
                    outputs=self._initial_outputs(),
                    pure=False,
                    const=False,
                    access="public",
                    category="",
                    description="",
                    **{field: 7},
                )
                self._assert_rejected(rejected, "INVALID_INPUT", field)
        self.assertEqual(
            self._inspect_function(self.FUNCTION_NAME)["function_id"], function_id
        )

    def test_optional_fallback_function_name_uses_the_public_name_contract(self):
        _, before = self._seed_function()
        function_id = before["function_id"]
        invalid_names = ("1Score", "Score-Value", "Счёт", "A" * 101)
        actions = (
            (
                "ue_rename_blueprint_function",
                {"new_name": "ComputeFinalScore"},
            ),
            (
                "ue_set_blueprint_function_signature",
                {
                    "inputs": self._initial_inputs(),
                    "outputs": self._initial_outputs(),
                    "pure": False,
                    "const": False,
                    "access": "public",
                    "category": "",
                    "description": "",
                },
            ),
            ("ue_delete_blueprint_function", {}),
        )
        for action, action_args in actions:
            for function_name in invalid_names:
                with self.subTest(action=action, function_name=function_name):
                    rejected = self.call(
                        "blueprint_actions",
                        action,
                        asset_path=self.asset_path,
                        function_id=function_id,
                        function_name=function_name,
                        **action_args,
                    )
                    self._assert_rejected(
                        rejected, "INVALID_INPUT", "function_name"
                    )
        self.assertEqual(
            self._inspect_function(self.FUNCTION_NAME)["function_id"], function_id
        )

    def test_invalid_access_is_rejected(self):
        invalid = self.call(
            "blueprint_actions",
            "ue_create_blueprint_function",
            asset_path=self.asset_path,
            function_name=self.FUNCTION_NAME,
            access="internal",
        )
        self._assert_rejected(invalid, "INVALID_INPUT", "access")

    @unittest.skipUnless(
        hasattr(unreal, "BlueprintInterfaceFactory"),
        "Blueprint interface factory is unavailable",
    )
    def test_interface_graph_target_is_rejected(self):
        name = f"Blueprint2InterfaceMembers_{uuid.uuid4().hex[:10]}"
        interface = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name,
            BLUEPRINT2_TEST_ROOT,
            unreal.Blueprint,
            unreal.BlueprintInterfaceFactory(),
        )
        self.assertIsNotNone(interface)
        interface_path = f"{BLUEPRINT2_TEST_ROOT}/{name}.{name}"
        self._created_assets.append(interface_path)
        graph = unreal.BlueprintEditorLibrary.add_function_graph(
            interface, self.FUNCTION_NAME
        )
        self.assertIsNotNone(graph)
        function = self._inspect_function(self.FUNCTION_NAME, interface_path)
        rejected = self.call(
            "blueprint_actions",
            "ue_delete_blueprint_function",
            asset_path=interface_path,
            function_id=function["function_id"],
        )
        self._assert_rejected(rejected, "PRECONDITION_FAILED", "function_id")

    def test_unstable_id_requires_explicit_fallback(self):
        fallback_id = "fallback:graph:" + "1" * 40
        rejected = self.call(
            "blueprint_actions",
            "ue_delete_blueprint_function",
            asset_path=self.asset_path,
            function_id=fallback_id,
            function_name=self.FUNCTION_NAME,
            function_owner_id=self.asset_path,
            function_type_path="/Script/BlueprintGraph.EdGraphSchema_K2",
        )
        self._assert_rejected(rejected, "INVALID_INPUT", "function_id")

if __name__ == "__main__":
    unittest.main()
