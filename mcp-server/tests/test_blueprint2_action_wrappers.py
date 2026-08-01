"""Offline contracts for the shared Universal Blueprint 2 core."""

import importlib.util
import json
from pathlib import Path
import sys
from types import ModuleType, SimpleNamespace

import pytest


ROOT = Path(__file__).parents[2]
ADAPTER_FILE = (
    ROOT
    / "Plugins"
    / "UnrealMCPython"
    / "Content"
    / "Python"
    / "UnrealMCPython"
    / "blueprint2.py"
)
PRIVATE = (
    ROOT
    / "Plugins"
    / "UnrealMCPython"
    / "Source"
    / "UnrealMCPython"
    / "Private"
)
CORE_HEADER = PRIVATE / "MCPythonBlueprint2Internal.h"
CORE_SOURCE = PRIVATE / "MCPythonBlueprint2Internal.cpp"
SHARED_HEADER = PRIVATE / "MCPythonHelperInternal.h"
WORKFLOW_SOURCE = PRIVATE / "MCPythonHelper_Workflow.cpp"
HELPER_SOURCE = PRIVATE / "MCPythonHelper.cpp"
HELPER_HEADER = (
    ROOT
    / "Plugins"
    / "UnrealMCPython"
    / "Source"
    / "UnrealMCPython"
    / "Public"
    / "MCPythonHelper.h"
)
INSPECTION_SOURCE = PRIVATE / "MCPythonHelper_BlueprintInspection.cpp"
GRAPH_SOURCE = PRIVATE / "MCPythonHelper_BlueprintGraph.cpp"
VARIABLE_SOURCE = PRIVATE / "MCPythonHelper_BlueprintVariables.cpp"
COMPONENT_SOURCE = PRIVATE / "MCPythonHelper_BlueprintComponents.cpp"
DIAGNOSTICS_SOURCE = PRIVATE / "MCPythonHelper_BlueprintDiagnostics.cpp"
PALETTE_SOURCE = PRIVATE / "MCPythonHelper_BlueprintPalette.cpp"
SEMANTIC_SOURCE = PRIVATE / "MCPythonHelper_BlueprintSemantic.cpp"
BUILD_SOURCE = PRIVATE.parent / "UnrealMCPython.Build.cs"
BLUEPRINT_ACTIONS = ADAPTER_FILE.with_name("blueprint_actions.py")
EDITOR_TESTS = (
    ROOT
    / "Plugins"
    / "UnrealMCPython"
    / "Content"
    / "Python"
    / "UnrealMCPython"
    / "tests"
)
EDITOR_TEST_BASE = EDITOR_TESTS / "base.py"
EDITOR_TEST_SUPPORT = EDITOR_TESTS / "blueprint2_support.py"
INSPECTION_EDITOR_TEST = EDITOR_TESTS / "test_blueprint2_inspection.py"
EDITOR_RUN_ALL = EDITOR_TESTS / "run_all.py"
SELF_HOSTED_WORKFLOW = ROOT / ".github" / "workflows" / "e2e-selfhosted.yml"
PLUGIN_PYTHON = ADAPTER_FILE.parents[1]


def test_palette_spawn_has_no_implicit_compile_save_or_python_escape():
    source = PALETTE_SOURCE.read_text(encoding="utf-8")

    assert "CompileBlueprint" not in source
    assert "SaveAsset" not in source
    assert "EditorAssetLibrary" not in source
    assert "execute_python" not in source
    assert "UBlueprintNodeSpawner::Invoke" in source or "->Invoke(" in source


def test_semantic_helpers_reuse_the_private_palette_interface():
    palette_source = PALETTE_SOURCE.read_text(encoding="utf-8")
    assert '#include "MCPythonBlueprintPaletteInternal.h"' in palette_source

    # The semantic implementation is introduced by the next native slice. Keep
    # its architectural guard active as soon as that source exists.
    if SEMANTIC_SOURCE.exists():
        semantic_source = SEMANTIC_SOURCE.read_text(encoding="utf-8")
        assert '#include "MCPythonBlueprintPaletteInternal.h"' in semantic_source
        assert "GetAllActions" not in semantic_source


def test_semantic_suggestion_is_read_only_and_connected_spawn_is_transactional():
    source = SEMANTIC_SOURCE.read_text(encoding="utf-8")
    assert "Semantic.Pairs.SetNum(" not in source
    assert "CandidateBindingCount > MaximumReturnedBindings" in source
    over_budget = source.index("CandidateBindingCount > MaximumReturnedBindings")
    budget_tail = source[over_budget : over_budget + 300]
    assert "++NextIndex" not in budget_tail
    assert "continue;" not in budget_tail
    assert "SerializeFinalTopologyEdges" in source
    assert "Values.Num() >= 511" not in source
    suggest = source[
        source.index("FString UMCPythonHelper::SuggestBlueprintNodesForConnection") :
        source.index("FString UMCPythonHelper::AddBlueprintConnectedActionNode")
    ]
    spawn = source[
        source.index("FString UMCPythonHelper::AddBlueprintConnectedActionNode") :
    ]

    for forbidden in (
        "FMutationScope",
        "->Invoke(",
        "TryCreateConnection",
        "MarkBlueprintAsModified",
        "BuildBlueprintGraphSnapshot",
    ):
        assert forbidden not in suggest

    for required in (
        "FMutationScope Scope",
        "BuildBlueprintGraphSnapshot",
        "ResolvePaletteActionToken",
        "ResolvePaletteTemplatePinBinding",
        "->Invoke(",
        "CanCreateConnection",
        "TryCreateConnection",
        "RollbackConnectedFailure",
        "MarkBlueprintAsModified",
    ):
        assert required in spawn

    assert "Scope.Rollback()" in source
    assert "Rollback.ResidualChanges.IsEmpty()" in source
    assert 'TEXT("before_digest")' in source
    assert 'TEXT("after_digest")' in source
    assert 'TEXT("affected_ids")' in source
    assert 'TEXT("residual_changes")' in source
    assert 'TEXT("saved"), false' in source

    assert spawn.index("BuildBlueprintGraphSnapshot") < spawn.index(
        "FMutationScope Scope"
    )
    assert spawn.index("CanCreateConnection") < spawn.index("FMutationScope Scope")
    assert spawn.index("VerifyConnectedSpawnTopology") < spawn.index(
        "MarkBlueprintAsModified"
    )
    assert spawn.index("ValidateStableConnectedSpawnResult") < spawn.index(
        "MarkBlueprintAsModified"
    )
    for forbidden in (
        "CompileBlueprint",
        "SavePackage(",
        "EditorAssetLibrary",
        "execute_python",
        "PlayInEditor",
    ):
        assert forbidden not in source


def test_replacement_preview_is_strictly_read_only():
    source = SEMANTIC_SOURCE.read_text(encoding="utf-8")
    body = source[source.index(
        "FString UMCPythonHelper::PreviewBlueprintActionReplacement"
    ):]

    for required in (
        "ResolvePaletteActionToken",
        "BuildCandidates",
        "GetBoundTemplateNode",
        "RegisterReplacementPlan",
        "NodeSnapshotDigest",
    ):
        assert required in body
    for forbidden in (
        "FMutationScope",
        "->Invoke(",
        "TryCreateConnection",
        "BreakSinglePinLink",
        "DestroyNode",
        "MarkBlueprintAsModified",
        "CompileBlueprint",
        "SavePackage(",
        "PlayInEditor",
    ):
        assert forbidden not in body


def _load_blueprint_actions(monkeypatch, helper):
    monkeypatch.setitem(
        sys.modules, "unreal", SimpleNamespace(MCPythonHelper=helper)
    )
    spec = importlib.util.spec_from_file_location(
        "_blueprint_actions_test", BLUEPRINT_ACTIONS
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def _load_variable_wrapper(monkeypatch):
    calls = []

    class Helper:
        pass

    package = ModuleType("UnrealMCPython")
    adapter = ModuleType("UnrealMCPython.blueprint2")

    def call_asset_helper(helper_name, asset_path, request=None):
        calls.append((helper_name, asset_path, request))
        return '{"success":true,"marker":"native"}'

    def call_json_helper(helper_name, request):
        calls.append((helper_name, request))
        return '{"success":true,"marker":"native"}'

    adapter.call_asset_helper = call_asset_helper
    adapter.call_json_helper = call_json_helper
    package.blueprint2 = adapter
    monkeypatch.setitem(sys.modules, "UnrealMCPython", package)
    monkeypatch.setitem(sys.modules, "UnrealMCPython.blueprint2", adapter)
    return _load_blueprint_actions(monkeypatch, Helper), calls


def _load(monkeypatch, asset=None, helper_result='{"success":true}'):
    calls = []

    class Blueprint:
        pass

    class DerivedBlueprint(Blueprint):
        pass

    loaded = DerivedBlueprint() if asset is None else asset

    class EditorAssetLibrary:
        @staticmethod
        def load_asset(asset_path):
            calls.append(("load", asset_path))
            return loaded

    class Helper:
        @staticmethod
        def get_blueprint_brief(blueprint):
            calls.append(("asset_helper_unary", blueprint))
            return helper_result

        @staticmethod
        def inspect_blueprint_v2(blueprint, request_json):
            calls.append(("asset_helper", blueprint, request_json))
            return helper_result

        @staticmethod
        def diff_blueprint_graphs_v2(request_json):
            calls.append(("json_helper", request_json))
            return helper_result

    fake_unreal = SimpleNamespace(
        Blueprint=Blueprint,
        EditorAssetLibrary=EditorAssetLibrary,
        MCPythonHelper=Helper,
    )
    monkeypatch.setitem(sys.modules, "unreal", fake_unreal)
    spec = importlib.util.spec_from_file_location("_blueprint2_test", ADAPTER_FILE)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module, calls, loaded, Blueprint


def test_rename_blueprint_variable(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)

    result = module.ue_rename_blueprint_variable(
        asset_path="/Game/BP.BP",
        variable_id="fallback:variable:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        new_name="FinalScore",
        allow_name_fallback=True,
        variable_name="Score",
        variable_owner_id="/Game/BP.BP",
        variable_type_path="category=int\ncontainer=none",
    )

    assert json.loads(result)["marker"] == "native"
    assert calls == [
        (
            "rename_blueprint_variable",
            "/Game/BP.BP",
            {
                "variable_id": "fallback:variable:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                "new_name": "FinalScore",
                "allow_name_fallback": True,
                "variable_name": "Score",
                "variable_owner_id": "/Game/BP.BP",
                "variable_type_path": "category=int\ncontainer=none",
            },
        )
    ]


def test_search_blueprint_node_actions_wrapper(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)

    result = module.ue_search_blueprint_node_actions(
        asset_path="/Game/BP.BP",
        graph_id="graph:11111111-1111-4111-8111-111111111111",
        query="Get Actor Location",
        filters={"action_kinds": ["function"], "pure_only": True},
        cursor="palette-cursor:page-two",
        limit=25,
    )

    assert json.loads(result)["marker"] == "native"
    assert calls == [
        (
            "search_blueprint_node_actions",
            "/Game/BP.BP",
            {
                "graph_id": "graph:11111111-1111-4111-8111-111111111111",
                "query": "Get Actor Location",
                "filters": {
                    "action_kinds": ["function"],
                    "pure_only": True,
                },
                "cursor": "palette-cursor:page-two",
                "limit": 25,
            },
        )
    ]


def test_describe_blueprint_node_action_wrapper(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)

    result = module.ue_describe_blueprint_node_action(
        action_id="action:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    )

    assert json.loads(result)["marker"] == "native"
    assert calls == [
        (
            "describe_blueprint_node_action",
            {"action_id": "action:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
        )
    ]


def test_add_blueprint_action_node_wrapper(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)

    result = module.ue_add_blueprint_action_node(
        asset_path="/Game/BP.BP",
        graph_id="graph:11111111-1111-4111-8111-111111111111",
        action_id="action:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        position={"x": 320, "y": 160},
        bindings=["binding:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"],
    )

    assert json.loads(result)["marker"] == "native"
    assert calls == [
        (
            "add_blueprint_action_node",
            "/Game/BP.BP",
            {
                "graph_id": "graph:11111111-1111-4111-8111-111111111111",
                "action_id": "action:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                "position": {"x": 320, "y": 160},
                "bindings": [
                    "binding:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                ],
            },
        )
    ]


def test_suggest_blueprint_nodes_for_pin_wrapper(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)

    result = module.ue_suggest_blueprint_nodes_for_pin(
        asset_path="/Game/BP.BP",
        graph_id="graph:11111111-1111-4111-8111-111111111111",
        pin_id="pin:22222222-2222-4222-8222-222222222222",
        query="Branch",
        cursor="palette-cursor:page-two",
        limit=10,
    )

    assert json.loads(result)["marker"] == "native"
    assert calls == [
        (
            "suggest_blueprint_nodes_for_pin",
            "/Game/BP.BP",
            {
                "graph_id": "graph:11111111-1111-4111-8111-111111111111",
                "pin_id": "pin:22222222-2222-4222-8222-222222222222",
                "query": "Branch",
                "cursor": "palette-cursor:page-two",
                "limit": 10,
            },
        )
    ]


def test_suggest_blueprint_nodes_for_connection_wrapper(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)
    filters = {"action_kinds": ["function"], "pure_only": True}

    result = module.ue_suggest_blueprint_nodes_for_connection(
        asset_path="/Game/BP.BP",
        graph_id="graph:11111111-1111-4111-8111-111111111111",
        source_pin_id="pin:22222222-2222-4222-8222-222222222222",
        target_pin_id="pin:33333333-3333-4333-8333-333333333333",
        query="Convert",
        filters=filters,
        allow_conversion=True,
        cursor="palette-cursor:" + "f" * 40,
        limit=25,
    )

    filters["action_kinds"].append("operator")
    assert json.loads(result)["marker"] == "native"
    assert calls == [
        (
            "suggest_blueprint_nodes_for_connection",
            "/Game/BP.BP",
            {
                "graph_id": "graph:11111111-1111-4111-8111-111111111111",
                "source_pin_id": (
                    "pin:22222222-2222-4222-8222-222222222222"
                ),
                "target_pin_id": (
                    "pin:33333333-3333-4333-8333-333333333333"
                ),
                "query": "Convert",
                "filters": {
                    "action_kinds": ["function"],
                    "pure_only": True,
                },
                "allow_conversion": True,
                "cursor": "palette-cursor:" + "f" * 40,
                "limit": 25,
            },
        )
    ]


def test_add_blueprint_connected_action_node_wrapper(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)
    position = {"x": 400, "y": 120}
    bindings = ["binding:" + "c" * 40]

    result = module.ue_add_blueprint_connected_action_node(
        asset_path="/Game/BP.BP",
        graph_id="graph:11111111-1111-4111-8111-111111111111",
        pin_id="pin:22222222-2222-4222-8222-222222222222",
        action_id="action:" + "a" * 40,
        connection_binding_id="binding:" + "b" * 40,
        position=position,
        allow_conversion=True,
        bindings=bindings,
    )

    position["x"] = -1
    bindings.append("binding:" + "d" * 40)
    assert json.loads(result)["marker"] == "native"
    assert calls[0] == (
        "add_blueprint_connected_action_node",
        "/Game/BP.BP",
        {
            "graph_id": "graph:11111111-1111-4111-8111-111111111111",
            "pin_id": "pin:22222222-2222-4222-8222-222222222222",
            "action_id": "action:" + "a" * 40,
            "connection_binding_id": "binding:" + "b" * 40,
            "position": {"x": 400, "y": 120},
            "allow_conversion": True,
            "bindings": ["binding:" + "c" * 40],
        },
    )


def test_insert_blueprint_action_node_wrapper(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)
    position = {"x": 600, "y": 80}
    bindings = ["binding:" + "c" * 40]

    result = module.ue_insert_blueprint_action_node(
        asset_path="/Game/BP.BP",
        graph_id="graph:11111111-1111-4111-8111-111111111111",
        source_pin_id="pin:22222222-2222-4222-8222-222222222222",
        target_pin_id="pin:33333333-3333-4333-8333-333333333333",
        action_id="action:" + "a" * 40,
        input_binding_id="binding:" + "b" * 40,
        output_binding_id="binding:" + "d" * 40,
        position=position,
        bindings=bindings,
    )

    position["x"] = -1
    bindings.append("binding:" + "e" * 40)
    assert json.loads(result)["marker"] == "native"
    assert calls[0] == (
        "insert_blueprint_action_node",
        "/Game/BP.BP",
        {
            "graph_id": "graph:11111111-1111-4111-8111-111111111111",
            "source_pin_id": (
                "pin:22222222-2222-4222-8222-222222222222"
            ),
            "target_pin_id": (
                "pin:33333333-3333-4333-8333-333333333333"
            ),
            "action_id": "action:" + "a" * 40,
            "input_binding_id": "binding:" + "b" * 40,
            "output_binding_id": "binding:" + "d" * 40,
            "position": {"x": 600, "y": 80},
            "bindings": ["binding:" + "c" * 40],
        },
    )


def test_preview_blueprint_action_replacement_wrapper(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)
    mapping = [
        {
            "old_pin_id": "pin:22222222-2222-4222-8222-222222222222",
            "new_binding_id": "binding:" + "b" * 40,
        }
    ]

    result = module.ue_preview_blueprint_action_replacement(
        asset_path="/Game/BP.BP",
        graph_id="graph:11111111-1111-4111-8111-111111111111",
        node_id="node:44444444-4444-4444-8444-444444444444",
        action_id="action:" + "a" * 40,
        bindings=["binding:" + "c" * 40],
        pin_mapping=mapping,
        allow_conversion=True,
        allow_loss=False,
    )

    mapping[0]["new_binding_id"] = "binding:" + "d" * 40
    assert json.loads(result)["marker"] == "native"
    assert calls[0][0:2] == (
        "preview_blueprint_action_replacement",
        "/Game/BP.BP",
    )
    assert calls[0][2]["pin_mapping"] == [
        {
            "old_pin_id": "pin:22222222-2222-4222-8222-222222222222",
            "new_binding_id": "binding:" + "b" * 40,
        }
    ]


def test_replace_blueprint_node_with_action_wrapper(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)

    result = module.ue_replace_blueprint_node_with_action(
        asset_path="/Game/BP.BP",
        graph_id="graph:11111111-1111-4111-8111-111111111111",
        replacement_plan_id="replacement-plan:" + "e" * 40,
        allow_loss=True,
    )

    assert json.loads(result)["marker"] == "native"
    assert calls == [
        (
            "replace_blueprint_node_with_action",
            "/Game/BP.BP",
            {
                "graph_id": "graph:11111111-1111-4111-8111-111111111111",
                "replacement_plan_id": "replacement-plan:" + "e" * 40,
                "allow_loss": True,
            },
        )
    ]


def test_remove_blueprint_variable(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)

    result = module.ue_remove_blueprint_variable(
        asset_path="/Game/BP.BP",
        variable_id="variable:44444444-4444-4444-8444-444444444444",
    )

    assert json.loads(result)["marker"] == "native"
    assert calls == [
        (
            "remove_blueprint_variable",
            "/Game/BP.BP",
            {
                "variable_id": "variable:44444444-4444-4444-8444-444444444444",
                "allow_name_fallback": False,
                "variable_name": "",
                "variable_owner_id": "",
                "variable_type_path": "",
            },
        )
    ]


def test_set_blueprint_variable_default(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)
    requested_default = [3, {"asset": "/Script/Engine.Default__Actor"}]
    before = json.loads(json.dumps(requested_default))

    result = module.ue_set_blueprint_variable_default(
        asset_path="/Game/BP.BP",
        variable_id="variable:44444444-4444-4444-8444-444444444444",
        default=requested_default,
    )

    assert json.loads(result)["marker"] == "native"
    assert requested_default == before
    assert calls == [
        (
            "set_blueprint_variable_default",
            "/Game/BP.BP",
            {
                "variable_id": "variable:44444444-4444-4444-8444-444444444444",
                "default": before,
            },
        )
    ]
    assert calls[0][2]["default"] is not requested_default


def test_set_blueprint_variable_metadata(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)
    metadata = {
        "category": "Scoring",
        "tooltip": "Current score",
        "visible": True,
        "instance_editable": True,
        "expose_on_spawn": True,
        "save_game": True,
        "cinematic": False,
    }
    before = dict(metadata)

    result = module.ue_set_blueprint_variable_metadata(
        asset_path="/Game/BP.BP",
        variable_id="variable:44444444-4444-4444-8444-444444444444",
        metadata=metadata,
    )

    assert json.loads(result)["marker"] == "native"
    assert metadata == before
    assert calls == [
        (
            "set_blueprint_variable_metadata",
            "/Game/BP.BP",
            {
                "variable_id": "variable:44444444-4444-4444-8444-444444444444",
                "metadata": before,
            },
        )
    ]
    assert calls[0][2]["metadata"] is not metadata


def test_set_blueprint_variable_replication(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)

    result = module.ue_set_blueprint_variable_replication(
        asset_path="/Game/BP.BP",
        variable_id="variable:44444444-4444-4444-8444-444444444444",
        mode="rep_notify",
        notify_function_name="OnRep_Score",
        condition="owner_only",
    )

    assert json.loads(result)["marker"] == "native"
    assert calls == [
        (
            "set_blueprint_variable_replication",
            "/Game/BP.BP",
            {
                "variable_id": "variable:44444444-4444-4444-8444-444444444444",
                "mode": "rep_notify",
                "notify_function_name": "OnRep_Score",
                "condition": "owner_only",
            },
        )
    ]


def test_rename_blueprint_component(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)

    result = module.ue_rename_blueprint_component(
        asset_path="/Game/BP.BP",
        component_id="component:55555555-5555-4555-8555-555555555555",
        new_name="PlayerMesh",
    )

    assert json.loads(result)["marker"] == "native"
    assert calls == [
        (
            "rename_blueprint_component",
            "/Game/BP.BP",
            {
                "component_id": "component:55555555-5555-4555-8555-555555555555",
                "new_name": "PlayerMesh",
            },
        )
    ]


def test_reparent_blueprint_component_supports_explicit_root(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)

    result = module.ue_reparent_blueprint_component(
        asset_path="/Game/BP.BP",
        component_id="component:55555555-5555-4555-8555-555555555555",
        parent_component_id=None,
    )

    assert json.loads(result)["marker"] == "native"
    assert calls == [
        (
            "reparent_blueprint_component",
            "/Game/BP.BP",
            {
                "component_id": "component:55555555-5555-4555-8555-555555555555",
                "parent_component_id": None,
            },
        )
    ]


def test_reorder_blueprint_component(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)

    result = module.ue_reorder_blueprint_component(
        asset_path="/Game/BP.BP",
        component_id="component:55555555-5555-4555-8555-555555555555",
        sibling_index=2,
    )

    assert json.loads(result)["marker"] == "native"
    assert calls == [
        (
            "reorder_blueprint_component",
            "/Game/BP.BP",
            {
                "component_id": "component:55555555-5555-4555-8555-555555555555",
                "sibling_index": 2,
            },
        )
    ]


def test_set_blueprint_component_transform_copies_input(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)
    transform = {"location": [10, 20, 30], "scale": [2, 2, 2]}
    before = json.loads(json.dumps(transform))

    result = module.ue_set_blueprint_component_transform(
        asset_path="/Game/BP.BP",
        component_id="component:55555555-5555-4555-8555-555555555555",
        transform=transform,
    )

    assert json.loads(result)["marker"] == "native"
    assert transform == before
    assert calls == [
        (
            "set_blueprint_component_transform",
            "/Game/BP.BP",
            {
                "component_id": "component:55555555-5555-4555-8555-555555555555",
                "transform": before,
            },
        )
    ]
    assert calls[0][2]["transform"] is not transform


def test_get_blueprint_health_calls_native_helper(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)

    result = module.ue_get_blueprint_health(
        asset_path="/Game/BP.BP",
        include_warnings=True,
    )

    assert json.loads(result)["marker"] == "native"
    assert calls == [
        (
            "get_blueprint_health",
            "/Game/BP.BP",
            None,
        )
    ]


def test_get_blueprint_health_can_hide_warning_records(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)
    native_result = {
        "success": True,
        "status": "succeeded",
        "summary": "health complete",
        "data": {
            "healthy": False,
            "issue_count": 2,
            "error_count": 1,
            "warning_count": 1,
            "issues": [
                {"code": "BP_COMPILE_WARNING", "severity": "warning"},
                {"code": "BP_DUPLICATE_MEMBER", "severity": "error"},
            ],
        },
        "warnings": [{"code": "BP_COMPILE_WARNING"}],
        "errors": [],
        "next_actions": [],
        "trace_id": "health-trace",
    }

    def call_asset_helper(helper_name, asset_path, request=None):
        calls.append((helper_name, asset_path, request))
        return json.dumps(native_result, separators=(",", ":"))

    monkeypatch.setattr(
        sys.modules["UnrealMCPython.blueprint2"],
        "call_asset_helper",
        call_asset_helper,
    )

    result = json.loads(
        module.ue_get_blueprint_health(
            asset_path="/Game/BP.BP",
            include_warnings=False,
        )
    )

    assert result["success"] is True, result
    assert result["data"]["healthy"] is False
    assert result["data"]["issues"] == [
        {"code": "BP_DUPLICATE_MEMBER", "severity": "error"}
    ]
    assert result["data"]["issue_count"] == 1
    assert result["data"]["error_count"] == 1
    assert result["data"]["warning_count"] == 0
    assert result["warnings"] == []


def test_create_blueprint_leaves_new_asset_dirty_and_unsaved(monkeypatch):
    created = []
    blueprint = SimpleNamespace()

    class Factory:
        def set_editor_property(self, name, value):
            assert name == "parent_class"
            assert value is not None

    class AssetTools:
        def create_asset(self, name, package, asset_class, factory):
            created.append((name, package, asset_class, factory))
            return blueprint

    unreal = SimpleNamespace(
        MCPythonHelper=SimpleNamespace(),
        EditorAssetLibrary=SimpleNamespace(
            does_asset_exist=lambda _path: False,
            save_loaded_asset=lambda _asset: pytest.fail(
                "create_blueprint must not save implicitly"
            ),
        ),
        load_class=lambda _outer, _path: object(),
        Blueprint=object(),
        BlueprintFactory=Factory,
        AssetToolsHelpers=SimpleNamespace(
            get_asset_tools=lambda: AssetTools()
        ),
    )
    monkeypatch.setitem(sys.modules, "unreal", unreal)
    spec = importlib.util.spec_from_file_location(
        "_blueprint_actions_create_test", BLUEPRINT_ACTIONS
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)

    result = json.loads(module.ue_create_blueprint("/Game/Tests/BP_Unsaved"))

    assert result["success"] is True, result
    assert result["asset_path"] == "/Game/Tests/BP_Unsaved"
    assert result["saved"] is False
    assert created and created[0][0:2] == ("BP_Unsaved", "/Game/Tests")


def test_snapshot_blueprint_graph_copies_graph_ids_and_calls_native_helper(
    monkeypatch,
):
    module, calls = _load_variable_wrapper(monkeypatch)
    graph_ids = [
        "graph:11111111-1111-4111-8111-111111111111",
        "graph:22222222-2222-4222-8222-222222222222",
    ]
    before = list(graph_ids)

    result = module.ue_snapshot_blueprint_graph(
        asset_path="/Game/BP.BP",
        graph_ids=graph_ids,
    )

    assert json.loads(result)["marker"] == "native"
    assert graph_ids == before
    assert calls == [
        (
            "snapshot_blueprint_graph",
            "/Game/BP.BP",
            {"graph_ids": before},
        )
    ]
    assert calls[0][2]["graph_ids"] is not graph_ids


def test_diff_blueprint_graphs_copies_snapshots_and_calls_json_helper(
    monkeypatch,
):
    module, calls = _load_variable_wrapper(monkeypatch)
    before_snapshot = {"snapshot_version": 1, "digest": "sha1:before"}
    after_snapshot = {"snapshot_version": 1, "digest": "sha1:after"}
    queries = [
        {
            "section": "nodes",
            "detail": "detailed",
            "limit": 25,
            "cursor": "",
        }
    ]
    original = json.loads(
        json.dumps([before_snapshot, after_snapshot, queries])
    )

    result = module.ue_diff_blueprint_graphs(
        before_snapshot=before_snapshot,
        after_snapshot=after_snapshot,
        queries=queries,
    )

    assert json.loads(result)["marker"] == "native"
    assert [before_snapshot, after_snapshot, queries] == original
    assert calls == [
        (
            "diff_blueprint_graphs",
            {
                "before_snapshot": original[0],
                "after_snapshot": original[1],
                "queries": original[2],
            },
        )
    ]
    assert calls[0][1]["before_snapshot"] is not before_snapshot
    assert calls[0][1]["after_snapshot"] is not after_snapshot
    assert calls[0][1]["queries"] is not queries


def test_load_blueprint_accepts_blueprint_subclasses(monkeypatch):
    module, calls, loaded, _ = _load(monkeypatch)

    assert module.load_blueprint("/Game/BP_Child.BP_Child") is loaded
    assert calls == [("load", "/Game/BP_Child.BP_Child")]


def test_call_asset_helper_copies_request_and_passes_compact_unicode_json(monkeypatch):
    passthrough = '{"success":true,"marker":"unchanged"}'
    module, calls, loaded, _ = _load(monkeypatch, helper_result=passthrough)
    request = {"query": "café", "nested": {"value": 1}}
    before = json.loads(json.dumps(request))

    result = module.call_asset_helper(
        "inspect_blueprint_v2", "/Game/BP.BP", request
    )

    assert result == passthrough
    assert request == before
    assert calls == [
        ("load", "/Game/BP.BP"),
        (
            "asset_helper",
            loaded,
            '{"query":"café","nested":{"value":1}}',
        ),
    ]


def test_call_asset_helper_without_request_uses_unary_reflected_signature(monkeypatch):
    passthrough = '{"success":true,"data":{"counts":{}}}'
    module, calls, loaded, _ = _load(monkeypatch, helper_result=passthrough)

    result = module.call_asset_helper("get_blueprint_brief", "/Game/BP.BP")

    assert result == passthrough
    assert calls == [
        ("load", "/Game/BP.BP"),
        ("asset_helper_unary", loaded),
    ]


def test_call_asset_helper_normalizes_asset_load_exceptions(monkeypatch):
    module, _, _, _ = _load(monkeypatch)

    def fail_load(_asset_path):
        raise RuntimeError("loader exploded")

    monkeypatch.setattr(module, "load_blueprint", fail_load)
    result_json = module.call_asset_helper(
        "get_blueprint_brief",
        "/Game/BP.BP",
    )
    result = json.loads(result_json)

    assert result["success"] is False
    assert result["errors"][0]["code"] == "INTERNAL_ERROR"
    assert result["errors"][0]["path"] == "asset_path"
    assert "traceback" not in result_json.lower()


def test_call_asset_helper_normalizes_missing_and_failing_helpers(monkeypatch):
    module, _, _, _ = _load(monkeypatch)

    missing_json = module.call_asset_helper(
        "missing_blueprint2_helper",
        "/Game/BP.BP",
    )
    missing = json.loads(missing_json)
    assert missing["errors"][0]["code"] == "UE_VERSION_UNSUPPORTED"
    assert "traceback" not in missing_json.lower()

    def fail_helper(_blueprint):
        raise RuntimeError("helper exploded")

    module.unreal.MCPythonHelper.get_blueprint_brief = fail_helper
    failed_json = module.call_asset_helper(
        "get_blueprint_brief",
        "/Game/BP.BP",
    )
    failed = json.loads(failed_json)
    assert failed["errors"][0]["code"] == "INTERNAL_ERROR"
    assert "traceback" not in failed_json.lower()


def test_blueprint_brief_executes_through_real_unreal_dispatcher(monkeypatch):
    class Blueprint:
        pass

    blueprint = Blueprint()
    fake_unreal = SimpleNamespace(
        Blueprint=Blueprint,
        EditorAssetLibrary=SimpleNamespace(load_asset=lambda _path: blueprint),
        MCPythonHelper=SimpleNamespace(
            get_blueprint_brief=lambda _blueprint: (
                '{"success":true,"status":"succeeded","summary":"ok",'
                '"data":{},"changes":[],"warnings":[],"errors":[],'
                '"next_actions":[],"trace_id":"test"}'
            )
        ),
    )
    monkeypatch.setitem(sys.modules, "unreal", fake_unreal)
    monkeypatch.syspath_prepend(str(PLUGIN_PYTHON))
    for module_name in (
        "UnrealMCPython.blueprint2",
        "UnrealMCPython.blueprint_actions",
        "UnrealMCPython.mcp_unreal_actions",
    ):
        monkeypatch.delitem(sys.modules, module_name, raising=False)
    package = sys.modules.get("UnrealMCPython")
    if package is not None:
        for attribute in ("blueprint2", "blueprint_actions", "mcp_unreal_actions"):
            monkeypatch.delattr(package, attribute, raising=False)

    dispatcher = __import__(
        "UnrealMCPython.mcp_unreal_actions",
        fromlist=["execute_action"],
    )
    result = json.loads(
        dispatcher.execute_action(
            "UnrealMCPython.blueprint_actions",
            "ue_get_blueprint_brief",
            {"asset_path": "/Game/BP.BP"},
        )
    )

    assert result["success"] is True
    assert result["status"] == "succeeded"


def test_inspect_blueprint(monkeypatch):
    calls = []

    class Blueprint:
        pass

    blueprint = Blueprint()

    def inspect_blueprint(asset, request_json):
        calls.append((asset, json.loads(request_json)))
        return (
            '{"success":true,"status":"succeeded","summary":"ok",'
            '"data":{"results":[]},"changes":[],"warnings":[],"errors":[],'
            '"next_actions":[],"trace_id":"test"}'
        )

    fake_unreal = SimpleNamespace(
        Blueprint=Blueprint,
        EditorAssetLibrary=SimpleNamespace(load_asset=lambda _path: blueprint),
        MCPythonHelper=SimpleNamespace(inspect_blueprint=inspect_blueprint),
    )
    monkeypatch.setitem(sys.modules, "unreal", fake_unreal)
    monkeypatch.syspath_prepend(str(PLUGIN_PYTHON))
    for module_name in (
        "UnrealMCPython.blueprint2",
        "UnrealMCPython.blueprint_actions",
        "UnrealMCPython.mcp_unreal_actions",
    ):
        monkeypatch.delitem(sys.modules, module_name, raising=False)
    package = sys.modules.get("UnrealMCPython")
    if package is not None:
        for attribute in ("blueprint2", "blueprint_actions", "mcp_unreal_actions"):
            monkeypatch.delattr(package, attribute, raising=False)

    dispatcher = __import__(
        "UnrealMCPython.mcp_unreal_actions",
        fromlist=["execute_action"],
    )
    queries = [{"op": "nodes", "limit": 7}]
    result = json.loads(
        dispatcher.execute_action(
            "UnrealMCPython.blueprint_actions",
            "ue_inspect_blueprint",
            {
                "asset_path": "/Game/BP.BP",
                "queries": queries,
                "cursor": "opaque-cursor",
            },
        )
    )

    assert result["success"] is True
    assert calls == [
        (
            blueprint,
            {
                "queries": [{"op": "nodes", "limit": 7}],
                "cursor": "opaque-cursor",
            },
        )
    ]
    assert queries == [{"op": "nodes", "limit": 7}]


@pytest.mark.parametrize(
    ("action", "params", "expected_request"),
    (
        (
            "create_blueprint_function",
            {
                "asset_path": "/Game/BP.BP",
                "function_name": "ComputeScore",
                "inputs": [{"name": "Actor", "type": {"kind": "object"}}],
                "outputs": [{"name": "Score", "type": {"kind": "int"}}],
                "pure": True,
                "const": True,
                "access": "private",
                "category": "Scoring",
                "description": "Computes a score.",
            },
            {
                "function_name": "ComputeScore",
                "inputs": [{"name": "Actor", "type": {"kind": "object"}}],
                "outputs": [{"name": "Score", "type": {"kind": "int"}}],
                "pure": True,
                "const": True,
                "access": "private",
                "category": "Scoring",
                "description": "Computes a score.",
            },
        ),
        (
            "rename_blueprint_function",
            {
                "asset_path": "/Game/BP.BP",
                "function_id": "graph:11111111-1111-4111-8111-111111111111",
                "new_name": "ComputeFinalScore",
                "allow_name_fallback": True,
                "function_name": "ComputeScore",
                "function_owner_id": "/Game/BP.BP",
                "function_type_path": "/Script/BlueprintGraph.EdGraphSchema_K2",
            },
            {
                "function_id": "graph:11111111-1111-4111-8111-111111111111",
                "new_name": "ComputeFinalScore",
                "allow_name_fallback": True,
                "function_name": "ComputeScore",
                "function_owner_id": "/Game/BP.BP",
                "function_type_path": "/Script/BlueprintGraph.EdGraphSchema_K2",
            },
        ),
        (
            "set_blueprint_function_signature",
            {
                "asset_path": "/Game/BP.BP",
                "function_id": "graph:11111111-1111-4111-8111-111111111111",
                "inputs": [{"name": "Value", "type": {"kind": "real"}}],
                "outputs": [{"name": "Result", "type": {"kind": "bool"}}],
                "pure": False,
                "const": True,
                "access": "protected",
                "category": "Scoring",
                "description": "Updated.",
                "allow_name_fallback": False,
                "function_name": "",
                "function_owner_id": "",
                "function_type_path": "",
            },
            {
                "function_id": "graph:11111111-1111-4111-8111-111111111111",
                "inputs": [{"name": "Value", "type": {"kind": "real"}}],
                "outputs": [{"name": "Result", "type": {"kind": "bool"}}],
                "pure": False,
                "const": True,
                "access": "protected",
                "category": "Scoring",
                "description": "Updated.",
                "allow_name_fallback": False,
                "function_name": "",
                "function_owner_id": "",
                "function_type_path": "",
            },
        ),
        (
            "delete_blueprint_function",
            {
                "asset_path": "/Game/BP.BP",
                "function_id": "graph:11111111-1111-4111-8111-111111111111",
                "allow_name_fallback": True,
                "function_name": "ComputeScore",
                "function_owner_id": "/Game/BP.BP",
                "function_type_path": "/Script/BlueprintGraph.EdGraphSchema_K2",
            },
            {
                "function_id": "graph:11111111-1111-4111-8111-111111111111",
                "allow_name_fallback": True,
                "function_name": "ComputeScore",
                "function_owner_id": "/Game/BP.BP",
                "function_type_path": "/Script/BlueprintGraph.EdGraphSchema_K2",
            },
        ),
    ),
)
def test_blueprint_function_wrappers_forward_exact_copied_requests(
    monkeypatch, action, params, expected_request
):
    calls = []

    class Blueprint:
        pass

    blueprint = Blueprint()

    def helper(asset, request_json):
        calls.append((asset, json.loads(request_json)))
        return '{"success":true}'

    fake_unreal = SimpleNamespace(
        Blueprint=Blueprint,
        EditorAssetLibrary=SimpleNamespace(load_asset=lambda _path: blueprint),
        MCPythonHelper=SimpleNamespace(**{action: helper}),
    )
    monkeypatch.setitem(sys.modules, "unreal", fake_unreal)
    monkeypatch.syspath_prepend(str(PLUGIN_PYTHON))
    for module_name in (
        "UnrealMCPython.blueprint2",
        "UnrealMCPython.blueprint_actions",
    ):
        monkeypatch.delitem(sys.modules, module_name, raising=False)
    package = sys.modules.get("UnrealMCPython")
    if package is not None:
        for attribute in ("blueprint2", "blueprint_actions"):
            monkeypatch.delattr(package, attribute, raising=False)

    actions = __import__(
        "UnrealMCPython.blueprint_actions", fromlist=[f"ue_{action}"]
    )
    original = json.loads(json.dumps(params))
    result = getattr(actions, f"ue_{action}")(**params)

    assert json.loads(result)["success"] is True
    assert params == original
    assert calls == [(blueprint, expected_request)]


@pytest.mark.parametrize(
    ("action", "params", "expected_request"),
    (
        (
            "create_blueprint_macro",
            {
                "asset_path": "/Game/BP.BP",
                "macro_name": "ClampScore",
                "inputs": [{"name": "Value", "type": {"kind": "int"}}],
                "outputs": [{"name": "Result", "type": {"kind": "int"}}],
                "pure": True,
                "category": "Scoring",
                "description": "Clamp a score.",
            },
            {
                "macro_name": "ClampScore",
                "inputs": [{"name": "Value", "type": {"kind": "int"}}],
                "outputs": [{"name": "Result", "type": {"kind": "int"}}],
                "pure": True,
                "category": "Scoring",
                "description": "Clamp a score.",
            },
        ),
        (
            "delete_blueprint_macro",
            {
                "asset_path": "/Game/BP.BP",
                "macro_id": "graph:11111111-1111-4111-8111-111111111111",
                "allow_name_fallback": True,
                "macro_name": "ClampScore",
                "macro_owner_id": "/Game/BP.BP",
                "macro_type_path": "/Script/BlueprintGraph.EdGraphSchema_K2",
            },
            {
                "macro_id": "graph:11111111-1111-4111-8111-111111111111",
                "allow_name_fallback": True,
                "macro_name": "ClampScore",
                "macro_owner_id": "/Game/BP.BP",
                "macro_type_path": "/Script/BlueprintGraph.EdGraphSchema_K2",
            },
        ),
        (
            "create_custom_event",
            {
                "asset_path": "/Game/BP.BP",
                "graph_id": "graph:11111111-1111-4111-8111-111111111111",
                "event_name": "OnScoreChanged",
                "parameters": [{"name": "Score", "type": {"kind": "int"}}],
                "pos_x": 160.0,
                "pos_y": 320.0,
            },
            {
                "graph_id": "graph:11111111-1111-4111-8111-111111111111",
                "event_name": "OnScoreChanged",
                "parameters": [{"name": "Score", "type": {"kind": "int"}}],
                "pos_x": 160.0,
                "pos_y": 320.0,
            },
        ),
        (
            "delete_custom_event",
            {
                "asset_path": "/Game/BP.BP",
                "event_id": "node:22222222-2222-4222-8222-222222222222",
                "allow_name_fallback": True,
                "event_name": "OnScoreChanged",
                "owner_graph_id": "graph:11111111-1111-4111-8111-111111111111",
                "event_type_path": "/Script/BlueprintGraph.K2Node_CustomEvent",
            },
            {
                "event_id": "node:22222222-2222-4222-8222-222222222222",
                "allow_name_fallback": True,
                "event_name": "OnScoreChanged",
                "owner_graph_id": "graph:11111111-1111-4111-8111-111111111111",
                "event_type_path": "/Script/BlueprintGraph.K2Node_CustomEvent",
            },
        ),
        (
            "add_event_dispatcher",
            {
                "asset_path": "/Game/BP.BP",
                "dispatcher_name": "ScoreChanged",
                "parameters": [{"name": "Score", "type": {"kind": "int"}}],
                "category": "Scoring",
                "description": "Emitted when the score changes.",
            },
            {
                "dispatcher_name": "ScoreChanged",
                "parameters": [{"name": "Score", "type": {"kind": "int"}}],
                "category": "Scoring",
                "description": "Emitted when the score changes.",
            },
        ),
        (
            "remove_event_dispatcher",
            {
                "asset_path": "/Game/BP.BP",
                "dispatcher_id": "variable:44444444-4444-4444-8444-444444444444",
                "allow_name_fallback": True,
                "dispatcher_name": "ScoreChanged",
                "dispatcher_owner_id": "/Game/BP.BP",
                "dispatcher_type_path": "mcdelegate",
            },
            {
                "dispatcher_id": "variable:44444444-4444-4444-8444-444444444444",
                "allow_name_fallback": True,
                "dispatcher_name": "ScoreChanged",
                "dispatcher_owner_id": "/Game/BP.BP",
                "dispatcher_type_path": "mcdelegate",
            },
        ),
        (
            "add_blueprint_interface",
            {
                "asset_path": "/Game/BP.BP",
                "interface_path": "/Game/BPI_Score.BPI_Score_C",
            },
            {"interface_path": "/Game/BPI_Score.BPI_Score_C"},
        ),
        (
            "remove_blueprint_interface",
            {
                "asset_path": "/Game/BP.BP",
                "interface_id": "interface:/Game/BPI_Score.BPI_Score_C",
            },
            {"interface_id": "interface:/Game/BPI_Score.BPI_Score_C"},
        ),
        (
            "add_reflected_blueprint_node",
            {
                "asset_path": "/Game/BP.BP",
                "graph_id": "graph:11111111-1111-4111-8111-111111111111",
                "member_kind": "function",
                "member_path": "/Script/Engine.Actor:K2_GetActorLocation",
                "position": {"x": 320, "y": 160},
            },
            {
                "graph_id": "graph:11111111-1111-4111-8111-111111111111",
                "member_kind": "function",
                "member_path": "/Script/Engine.Actor:K2_GetActorLocation",
                "position": {"x": 320, "y": 160},
            },
        ),
        (
            "set_blueprint_node_properties",
            {
                "asset_path": "/Game/BP.BP",
                "node_id": "node:22222222-2222-4222-8222-222222222222",
                "properties": {
                    "comment": "Validated comment",
                    "position": {"x": 480, "y": 240},
                    "pin_defaults": {
                        "pin:33333333-3333-4333-8333-333333333333": False,
                    },
                },
            },
            {
                "node_id": "node:22222222-2222-4222-8222-222222222222",
                "properties": {
                    "comment": "Validated comment",
                    "position": {"x": 480, "y": 240},
                    "pin_defaults": {
                        "pin:33333333-3333-4333-8333-333333333333": False,
                    },
                },
            },
        ),
        (
            "disconnect_blueprint_pins",
            {
                "asset_path": "/Game/BP.BP",
                "pin_id": "pin:33333333-3333-4333-8333-333333333333",
                "source_pin_id": "",
                "target_pin_id": "",
            },
            {
                "pin_id": "pin:33333333-3333-4333-8333-333333333333",
                "source_pin_id": "",
                "target_pin_id": "",
            },
        ),
        (
            "disconnect_blueprint_pins",
            {
                "asset_path": "/Game/BP.BP",
                "pin_id": "",
                "source_pin_id": "pin:33333333-3333-4333-8333-333333333333",
                "target_pin_id": "pin:44444444-4444-4444-8444-444444444444",
            },
            {
                "pin_id": "",
                "source_pin_id": "pin:33333333-3333-4333-8333-333333333333",
                "target_pin_id": "pin:44444444-4444-4444-8444-444444444444",
            },
        ),
    ),
)
def test_blueprint_member_wrappers_forward_exact_requests(
    monkeypatch, action, params, expected_request
):
    calls = []

    class Blueprint:
        pass

    blueprint = Blueprint()

    def helper(asset, request_json):
        calls.append((asset, json.loads(request_json)))
        return '{"success":true}'

    fake_unreal = SimpleNamespace(
        Blueprint=Blueprint,
        EditorAssetLibrary=SimpleNamespace(load_asset=lambda _path: blueprint),
        MCPythonHelper=SimpleNamespace(**{action: helper}),
    )
    monkeypatch.setitem(sys.modules, "unreal", fake_unreal)
    monkeypatch.syspath_prepend(str(PLUGIN_PYTHON))
    for module_name in (
        "UnrealMCPython.blueprint2",
        "UnrealMCPython.blueprint_actions",
    ):
        monkeypatch.delitem(sys.modules, module_name, raising=False)
    package = sys.modules.get("UnrealMCPython")
    if package is not None:
        for attribute in ("blueprint2", "blueprint_actions"):
            monkeypatch.delattr(package, attribute, raising=False)

    actions = __import__(
        "UnrealMCPython.blueprint_actions", fromlist=[f"ue_{action}"]
    )
    original = json.loads(json.dumps(params))
    result = getattr(actions, f"ue_{action}")(**params)

    assert json.loads(result)["success"] is True
    assert params == original
    assert calls == [(blueprint, expected_request)]


def test_legacy_connect_wrapper_forwards_stable_ids_without_rewriting(monkeypatch):
    calls = []

    class Blueprint:
        pass

    blueprint = Blueprint()

    class EditorAssetLibrary:
        @staticmethod
        def load_asset(asset_path):
            calls.append(("load", asset_path))
            return blueprint

    class Helper:
        @staticmethod
        def connect_blueprint_pins(asset, graph_name, *targets):
            calls.append(("connect", asset, graph_name, *targets))
            return '{"success":true}'

    fake_unreal = SimpleNamespace(
        Blueprint=Blueprint,
        EditorAssetLibrary=EditorAssetLibrary,
        MCPythonHelper=Helper,
    )
    monkeypatch.setitem(sys.modules, "unreal", fake_unreal)
    spec = importlib.util.spec_from_file_location(
        "_blueprint_actions_connect_test", BLUEPRINT_ACTIONS
    )
    actions = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(actions)

    targets = (
        "node:11111111-1111-4111-8111-111111111111",
        "pin:22222222-2222-4222-8222-222222222222",
        "node:33333333-3333-4333-8333-333333333333",
        "pin:44444444-4444-4444-8444-444444444444",
    )
    result = actions.ue_connect_blueprint_pins(
        asset_path="/Game/BP.BP",
        graph_name="EventGraph",
        source_node=targets[0],
        source_pin=targets[1],
        target_node=targets[2],
        target_pin=targets[3],
    )

    assert json.loads(result)["success"] is True
    assert calls == [
        ("load", "/Game/BP.BP"),
        ("connect", blueprint, "EventGraph", *targets),
    ]


def test_call_json_helper_copies_request_and_passes_result_through(monkeypatch):
    passthrough = '{"success":false,"errors":[{"code":"INVALID_INPUT"}]}'
    module, calls, _, _ = _load(monkeypatch, helper_result=passthrough)
    request = {"before": {"nodes": ["node:1"]}, "label": "Δ"}
    before = json.loads(json.dumps(request))

    result = module.call_json_helper("diff_blueprint_graphs_v2", request)

    assert result == passthrough
    assert request == before
    assert calls == [
        (
            "json_helper",
            '{"before":{"nodes":["node:1"]},"label":"Δ"}',
        )
    ]


def test_missing_or_non_blueprint_asset_returns_structured_precondition(monkeypatch):
    module, calls, _, _ = _load(monkeypatch, asset=object())

    result = json.loads(
        module.call_asset_helper("inspect_blueprint_v2", "/Game/NotBlueprint")
    )

    assert result["success"] is False
    assert result["status"] == "failed"
    assert result["errors"][0]["code"] == "PRECONDITION_FAILED"
    assert result["errors"][0]["path"] == "asset_path"
    assert result["errors"][0]["details"]["asset_path"] == "/Game/NotBlueprint"
    assert calls == [("load", "/Game/NotBlueprint")]


def test_target_request_is_explicit_and_stable(monkeypatch):
    module, _, _, _ = _load(monkeypatch)

    assert module.target_request(
        "node:abc",
        allow_name_fallback=True,
        owner_id="graph:def",
        name="CallThing",
        type_path="/Script/Engine.K2Node_CallFunction",
    ) == {
        "id": "node:abc",
        "allow_name_fallback": True,
        "owner_id": "graph:def",
        "name": "CallThing",
        "type_path": "/Script/Engine.K2Node_CallFunction",
    }


def test_internal_header_declares_stable_target_and_transaction_contracts():
    source = CORE_HEADER.read_text(encoding="utf-8")
    shared = SHARED_HEADER.read_text(encoding="utf-8")

    required = [
        "enum class ETargetKind : uint8 { Graph, Node, Pin, Variable, Component, Interface };",
        "struct FTargetRef",
        "FString Id;",
        "FString OwnerId;",
        "FString Name;",
        "FString TypePath;",
        "bool bAllowNameFallback = false;",
        "struct FResolvedTarget",
        "UObject* Object = nullptr;",
        "UEdGraph* Graph = nullptr;",
        "UEdGraphNode* Node = nullptr;",
        "UEdGraphPin* Pin = nullptr;",
        "FBPVariableDescription* Variable = nullptr;",
        "USCS_Node* Component = nullptr;",
        "FString IdKind;",
        "bool bStable = false;",
        "struct FPageRequest",
        "int32 Limit = 100;",
        "FString LastId;",
        "FString QueryDigest;",
        "struct FRollbackResult",
        "bool bSucceeded = false;",
        "bool bDeferredToWorkflow = false;",
        "TArray<TSharedPtr<FJsonValue>> ResidualChanges;",
        "class FMutationScope",
        "explicit FMutationScope(const FText& Description);",
        "bool IsValid() const;",
        "void Modify(UObject* Object);",
        "FRollbackResult Rollback();",
        "TUniquePtr<FScopedTransaction> LocalTransaction;",
        "int32 TransactionIndex = INDEX_NONE;",
        "FGuid TransactionGuid;",
        "bool bWorkflowOwned = false;",
    ]
    for declaration in required:
        assert declaration in source
    assert "const FGuid& GetEditorSessionId();" in shared
    assert "bool HasActiveWorkflowTransaction();" in shared


def test_graph_authoring_validates_before_mutation_and_uses_shared_transactions():
    source = GRAPH_SOURCE.read_text(encoding="utf-8")
    reflected = source[
        source.index("FString UMCPythonHelper::AddReflectedBlueprintNode") :
        source.index("FString UMCPythonHelper::AddBlueprintNode")
    ]
    mutation = reflected.index("FMutationScope Scope")
    for validation in (
        "IsExactK2Graph(Graph)",
        "CanUserKismetCallFunction(Function)",
        "CanFunctionBeUsedInGraph",
        "CanCreateNodeClass",
        "LoadExactObject<UClass>",
        "LoadExactObject<UEnum>",
        "LoadExactObject<UScriptStruct>",
    ):
        assert reflected.index(validation) < mutation
    assert reflected.count("Creator.Finalize();") == 7

    mutations = (
        "AddBlueprintNode",
        "SetBlueprintNodeProperties",
        "ConnectBlueprintPins",
        "DisconnectBlueprintPins",
        "RemoveBlueprintNode",
        "BuildBlueprintGraph",
        "SetBlueprintNodePosition",
        "SetBlueprintNodePinDefault",
    )
    starts = [source.index(f"FString UMCPythonHelper::{name}") for name in mutations]
    starts.append(len(source))
    for index, name in enumerate(mutations):
        body = source[starts[index] : starts[index + 1]]
        assert "FMutationScope Scope" in body, name
        assert "Scope.Modify(Blueprint);" in body, name
        assert "Scope.Modify(Graph);" in body, name

    properties = source[
        source.index("FString UMCPythonHelper::SetBlueprintNodeProperties") :
        source.index("FString UMCPythonHelper::ConnectBlueprintPins")
    ]
    assert properties.index("ResolveTarget") < properties.index("FMutationScope Scope")
    assert properties.index("NormalizeDefaultValue") < properties.index(
        "FMutationScope Scope"
    )
    assert properties.index("CanAddPin()") < properties.index(
        "FMutationScope Scope"
    )
    assert properties.index("CanRemoveOptionPinToNode()") < properties.index(
        "FMutationScope Scope"
    )
    assert properties.index("IsPinDefaultValid") < properties.index(
        "FMutationScope Scope"
    )
    assert "bDefaultValueIsReadOnly" in properties
    assert "bDefaultValueIsIgnored" in properties
    assert "MaxSwitchCases" in source
    assert "TrySetDefaultValue" in properties
    assert "TrySetDefaultObject" in properties
    assert "TrySetDefaultText" in properties

    connections = source[
        source.index("FString UMCPythonHelper::ConnectBlueprintPins") :
        source.index("FString UMCPythonHelper::DisconnectBlueprintPins")
    ]
    assert connections.index("CanCreateConnection") < connections.index(
        "FMutationScope Scope"
    )
    assert "TryCreateConnection" in connections
    assert "CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE" in connections
    assert "TSet<FGuid> BeforeNodeGuids" in connections
    assert "SnapshotConnections" in connections
    assert "DiffConnectionChanges" in connections

    disconnections = source[
        source.index("FString UMCPythonHelper::DisconnectBlueprintPins") :
        source.index("FString UMCPythonHelper::RemoveBlueprintNode")
    ]
    assert disconnections.index("ResolveStablePin") < disconnections.index(
        "FMutationScope Scope"
    )
    assert "BreakSinglePinLink" in disconnections
    assert "BreakPinLinks" in disconnections
    assert 'TryGetStringField(TEXT("pin_id")' in disconnections
    assert 'TryGetStringField(TEXT("source_pin_id")' in disconnections
    assert 'TryGetStringField(TEXT("target_pin_id")' in disconnections

    assert "CompileBlueprint(" not in source
    assert "SavePackage(" not in source


def test_variable_authoring_validates_before_mutation_and_never_compiles_or_saves():
    assert VARIABLE_SOURCE.exists(), "Task 12 native variable helper is missing"
    source = VARIABLE_SOURCE.read_text(encoding="utf-8")
    header = HELPER_HEADER.read_text(encoding="utf-8")
    actions = BLUEPRINT_ACTIONS.read_text(encoding="utf-8")

    for declaration in (
        "static FString AddBlueprintVariable(UBlueprint* Blueprint, const FString& RequestJson);",
        "static FString SetBlueprintVariableFlags(UBlueprint* Blueprint, const FString& RequestJson);",
        "static FString RenameBlueprintVariable(UBlueprint* Blueprint, const FString& RequestJson);",
        "static FString RemoveBlueprintVariable(UBlueprint* Blueprint, const FString& RequestJson);",
        "static FString SetBlueprintVariableDefault(UBlueprint* Blueprint, const FString& RequestJson);",
        "static FString SetBlueprintVariableMetadata(UBlueprint* Blueprint, const FString& RequestJson);",
        "static FString SetBlueprintVariableReplication(UBlueprint* Blueprint, const FString& RequestJson);",
    ):
        assert declaration in header

    starts = [
        source.index(f"FString UMCPythonHelper::{name}")
        for name in (
            "AddBlueprintVariable",
            "SetBlueprintVariableFlags",
            "RenameBlueprintVariable",
            "RemoveBlueprintVariable",
            "SetBlueprintVariableDefault",
            "SetBlueprintVariableMetadata",
            "SetBlueprintVariableReplication",
        )
    ]
    starts.append(len(source))
    for index in range(len(starts) - 1):
        body = source[starts[index] : starts[index + 1]]
        assert "FMutationScope Scope" in body

    default_body = source[
        source.index("FString UMCPythonHelper::SetBlueprintVariableDefault") :
        source.index("FString UMCPythonHelper::SetBlueprintVariableMetadata")
    ]
    assert default_body.index("NormalizeDefaultValue") < default_body.index(
        "FMutationScope Scope"
    )
    replication_body = source[
        source.index("FString UMCPythonHelper::SetBlueprintVariableReplication") :
    ]
    assert replication_body.index("ValidateRepNotifyFunction") < replication_body.index(
        "FMutationScope Scope"
    )
    for forbidden in (
        "CompileBlueprint(",
        "SavePackage(",
        "compile_blueprint(",
        "save_loaded_asset(",
    ):
        assert forbidden not in source

    add_wrapper = actions[
        actions.index("def ue_add_variable") : actions.index("def ue_set_variable_flags")
    ]
    flags_wrapper = actions[
        actions.index("def ue_set_variable_flags") : actions.index(
            "def _blueprint2_unsupported"
        )
    ]
    for body in (add_wrapper, flags_wrapper):
        assert "call_asset_helper" in body
        assert "compile_blueprint" not in body
        assert "save_loaded_asset" not in body


def test_component_authoring_is_transactional_safe_and_never_compiles_or_saves():
    assert COMPONENT_SOURCE.exists(), "Task 13 native component helper is missing"
    source = COMPONENT_SOURCE.read_text(encoding="utf-8")
    legacy_source = HELPER_SOURCE.read_text(encoding="utf-8")
    header = HELPER_HEADER.read_text(encoding="utf-8")
    actions = BLUEPRINT_ACTIONS.read_text(encoding="utf-8")

    for declaration in (
        "static FString RenameBlueprintComponent(UBlueprint* Blueprint, const FString& RequestJson);",
        "static FString ReparentBlueprintComponent(UBlueprint* Blueprint, const FString& RequestJson);",
        "static FString ReorderBlueprintComponent(UBlueprint* Blueprint, const FString& RequestJson);",
        "static FString SetBlueprintComponentTransform(UBlueprint* Blueprint, const FString& RequestJson);",
    ):
        assert declaration in header

    for name in (
        "AddComponentToBlueprint",
        "RemoveComponentFromBlueprint",
        "SetComponentProperty",
        "RenameBlueprintComponent",
        "ReparentBlueprintComponent",
        "ReorderBlueprintComponent",
        "SetBlueprintComponentTransform",
    ):
        body_start = source.index(f"FString UMCPythonHelper::{name}")
        next_start = source.find("FString UMCPythonHelper::", body_start + 1)
        body = source[body_start : next_start if next_start != -1 else len(source)]
        assert "FMutationScope Scope" in body, name

    property_body = source[
        source.index("FString UMCPythonHelper::SetComponentProperty") :
        source.index("FString UMCPythonHelper::RenameBlueprintComponent")
    ]
    mutation = property_body.index("FMutationScope Scope")
    import_text = property_body.index("ImportText_Direct")
    for validation in (
        "CPF_EditConst",
        "CPF_Transient",
        "FDelegateProperty",
        "FMulticastDelegateProperty",
    ):
        assert property_body.index(validation) < mutation
        assert property_body.index(validation) < import_text

    for moved_name in (
        "AddComponentToBlueprint",
        "RemoveComponentFromBlueprint",
        "SetComponentProperty",
    ):
        assert f"UMCPythonHelper::{moved_name}" not in legacy_source

    for forbidden in ("CompileBlueprint(", "SavePackage(", "save_asset("):
        assert forbidden not in source

    component_wrappers = (
        actions[
            actions.index("def ue_add_component_to_blueprint") :
            actions.index("def ue_remove_component_from_blueprint")
        ],
        actions[
            actions.index("def ue_remove_component_from_blueprint") :
            actions.index("def ue_set_component_property")
        ],
        actions[
            actions.index("def ue_set_component_property") :
            actions.index("# ─── Graph Auto-Layout")
        ],
        actions[
            actions.index("def ue_rename_blueprint_component") :
            actions.index("def ue_get_blueprint_health")
        ],
    )
    for body in component_wrappers:
        assert "save_asset(" not in body
        assert "compile_blueprint(" not in body


def test_compile_blueprint_uses_structured_diagnostics_without_saving():
    assert DIAGNOSTICS_SOURCE.exists(), "Task 14 diagnostic helper is missing"
    source = DIAGNOSTICS_SOURCE.read_text(encoding="utf-8")
    legacy_source = HELPER_SOURCE.read_text(encoding="utf-8")
    runner = EDITOR_RUN_ALL.read_text(encoding="utf-8")
    actions = BLUEPRINT_ACTIONS.read_text(encoding="utf-8")

    assert "UMCPythonHelper::CompileBlueprint" not in legacy_source
    assert "UMCPythonHelper::CompileBlueprint" in source
    for contract in (
        "FCompilerResultsLog",
        "bSilentMode",
        "bAnnotateMentionedNodes",
        "EBlueprintCompileOptions::None",
        "Results.Messages",
        "FEdGraphToken",
        "GetMessageTokens",
        "GetPin",
        "GetGraphObject",
        "DescribeGraphTarget",
        "DescribeNodeTarget",
        "DescribePinTarget",
        "BP_MISSING_REQUIRED_PIN",
        "BP_UNRESOLVED_MEMBER",
        "BP_TYPE_MISMATCH",
        "BP_DUPLICATE_MEMBER",
        "BP_POSSIBLE_NULL_ACCESS",
        "BP_COMPILE_ERROR",
        "BP_COMPILE_WARNING",
        "BS_UpToDateWithWarnings",
        "COMPILE_FAILED",
        'TEXT("diagnostics")',
        'TEXT("result_status")',
        'TEXT("error_count")',
        'TEXT("warning_count")',
        'TEXT("next_actions")',
    ):
        assert contract in source
    for forbidden in ("SavePackage(", "save_asset(", "FMutationScope"):
        assert forbidden not in source
    assert "UnrealMCPython.tests.test_blueprint2_diagnostics" in runner
    compile_wrapper = actions[
        actions.index("def ue_compile_blueprint") :
        actions.index("# ─── Component Management")
    ]
    assert "stable structured compiler diagnostics" in compile_wrapper


def test_compile_and_health_gate_unsupported_compiler_tokens_before_compile():
    source = DIAGNOSTICS_SOURCE.read_text(encoding="utf-8")
    core = CORE_SOURCE.read_text(encoding="utf-8")
    header = CORE_HEADER.read_text(encoding="utf-8")

    assert "bool SupportsCompilerTokens();" in header
    assert "bool SupportsCompilerTokens()" in core
    capabilities = core[core.index("TSharedRef<FJsonObject> BuildCapabilities") :]
    assert 'TEXT("supports_compiler_tokens")' in capabilities
    assert "SupportsCompilerTokens()" in capabilities

    compile_body = source[
        source.index("FString UMCPythonHelper::CompileBlueprint") :
        source.index("FString UMCPythonHelper::GetBlueprintHealth")
    ]
    health_body = source[source.index("FString UMCPythonHelper::GetBlueprintHealth") :]
    assert compile_body.index("if (!SupportsCompilerTokens())") < (
        compile_body.index("FKismetEditorUtilities::CompileBlueprint")
    )
    assert health_body.index("if (!SupportsCompilerTokens())") < (
        health_body.index("CompileBlueprint(Blueprint)")
    )
    assert "CompilerTokensUnsupportedResult()" in compile_body
    assert "CompilerTokensUnsupportedResult()" in health_body

    unsupported = source[
        source.index("FString CompilerTokensUnsupportedResult()") :
        source.index("void UE::MCPython::Blueprint2::CollectStructuralHealthIssues")
    ]
    for token in (
        'TEXT("UE_VERSION_UNSUPPORTED")',
        'TEXT("supports_compiler_tokens")',
        'TEXT("5.7")',
        "FEngineVersion::Current().ToString()",
    ):
        assert token in unsupported


def test_blueprint_health_compiles_once_and_checks_structural_invariants():
    source = DIAGNOSTICS_SOURCE.read_text(encoding="utf-8")
    header = HELPER_HEADER.read_text(encoding="utf-8")
    actions = BLUEPRINT_ACTIONS.read_text(encoding="utf-8")

    assert (
        "static FString GetBlueprintHealth(UBlueprint* Blueprint);" in header
    )
    health_body = source[source.index("FString UMCPythonHelper::GetBlueprintHealth") :]
    assert health_body.count("CompileBlueprint(Blueprint)") == 1
    assert "FKismetEditorUtilities::CompileBlueprint" not in health_body
    for contract in (
        "CollectStructuralHealthIssues",
        "DefaultValueSimpleValidation",
        "IsPinDefaultValid",
        "GetTargetFunction",
        "ResolveMember<FProperty>",
        "FindOverrideForFunction",
        "NormalizeHealthIssues",
        "GetAllNodes",
        "GetRootNodes",
        "GetChildNodes",
        "BP_MISSING_REQUIRED_PIN",
        "BP_UNRESOLVED_MEMBER",
        "BP_MISSING_INTERFACE_IMPLEMENTATION",
        "BP_DUPLICATE_MEMBER",
        "BP_INVALID_VARIABLE_DEFAULT",
        "BP_INVALID_OBJECT_DEFAULT",
        "BP_INVALID_CLASS_DEFAULT",
        "BP_SCS_CYCLE",
        "BP_SCS_ORPHAN",
        "BP_SCS_NODE_MISSING_FROM_ALL_NODES",
        "BP_SCS_DUPLICATE_GUID",
        "BP_SCS_MULTIPLE_PARENTS",
    ):
        assert contract in source
    for forbidden in ("FMutationScope", "SavePackage(", "save_asset("):
        assert forbidden not in health_body

    wrapper = actions[
        actions.index("def ue_get_blueprint_health") :
        actions.index("def ue_snapshot_blueprint_graph")
    ]
    assert 'call_asset_helper("get_blueprint_health", asset_path)' not in wrapper
    assert '"get_blueprint_health", asset_path' in wrapper
    assert 'issue.get("severity") != "warning"' in wrapper


def test_blueprint_helpers_compile_as_isolated_translation_units():
    build_source = BUILD_SOURCE.read_text(encoding="utf-8")

    assert "bUseUnity = false;" in build_source


def test_cpp_core_uses_persisted_guids_bounded_owners_and_guarded_undo():
    source = CORE_SOURCE.read_text(encoding="utf-8")
    workflow = WORKFLOW_SOURCE.read_text(encoding="utf-8")

    for token in (
        "Blueprint->GetAllGraphs",
        "GraphGuid",
        "NodeGuid",
        "PinId",
        "VarGuid",
        "VariableGuid",
        "FSHA1",
        "GetEditorSessionId()",
        "HasActiveWorkflowTransaction()",
        "GetQueueLength()",
        "GetUndoCount()",
        "UndoTransaction()",
        "ROLLBACK_FAILED",
        "FGuid::NewGuid()",
    ):
        assert token in source
    assert "const FGuid& UE::MCPython::GetEditorSessionId()" in workflow
    assert "bool UE::MCPython::HasActiveWorkflowTransaction()" in workflow


def test_workflow_fingerprints_read_saved_metadata_from_package_file():
    workflow = WORKFLOW_SOURCE.read_text(encoding="utf-8")

    for token in (
        '"UObject/PackageFileSummary.h"',
        "CreateFileReader",
        "FPackageFileSummary",
        "GetSavedHash()",
        "FileSize(*Filename)",
        "GetTimeStamp(*Filename)",
    ):
        assert token in workflow
    assert workflow.index("CreateFileReader") < workflow.index(
        "GetAssetPackageDataCopy"
    )


def test_graph_node_and_pin_ids_share_deterministic_fallback_helpers():
    header = CORE_HEADER.read_text(encoding="utf-8")
    core = CORE_SOURCE.read_text(encoding="utf-8")
    selected = HELPER_SOURCE.read_text(encoding="utf-8")
    inspection = INSPECTION_SOURCE.read_text(encoding="utf-8")

    for declaration in (
        "FString MakeGraphTargetId(UBlueprint* Blueprint, const UEdGraph* Graph);",
        "FString MakeNodeTargetId(UBlueprint* Blueprint, const UEdGraphNode* Node);",
        "FString MakePinTargetId(UBlueprint* Blueprint, const UEdGraphPin* Pin);",
    ):
        assert declaration in header

    for token in (
        "Graph->GraphGuid.IsValid()",
        "Node->NodeGuid.IsValid()",
        "Pin->PinId.IsValid()",
        "MakeTargetId(ETargetKind::Graph, Graph->GraphGuid)",
        "MakeTargetId(ETargetKind::Node, Node->NodeGuid)",
        "MakeTargetId(ETargetKind::Pin, Pin->PinId)",
        "Blueprint->GetPathName()",
        "Graph->GetName()",
        "Graph->GetSchema()->GetClass()->GetPathName()",
            "MakeGraphTargetId(Blueprint, Node->GetGraph())",
            "Node->GetClass()->GetPathName()",
            "MakeNodeTargetId(Blueprint, OwningNode)",
            "CanonicalPinType(Pin->PinType)",
            "PinValueType",
            "ContainerType",
            "TerminalSubCategoryObject",
            "MakeQualifiedFallbackId(",
    ):
        assert token in core

    for source in (selected, inspection):
        assert "DescribeGraphTarget(" in source
        assert "DescribeNodeTarget(" in source
        assert "DescribePinTarget(" in source


def test_self_hosted_workflow_builds_and_runs_native_blueprint2_gate():
    workflow = SELF_HOSTED_WORKFLOW.read_text(encoding="utf-8")

    for contract in (
        "Engine\\Build\\BatchFiles\\Build.bat",
        "Engine\\Build\\Build.version",
        "$buildVersion.MajorVersion -ne 5",
        "$buildVersion.MinorVersion -ne 7",
        "UnrealMCPSampleEditor Win64 Development",
        "Automation RunTests UnrealMCPython.Blueprint2",
        "UnrealMCPython.Blueprint2.TargetIds",
        "UnrealMCPython.Blueprint2.BriefCounts",
        "Result={Success} Name={TargetIds}",
        "Result={Success} Name={BriefCounts}",
        "$editorExit = $LASTEXITCODE",
    ):
        assert contract in workflow
    assert "Found 2 automation tests" not in workflow


def test_editor_runner_fails_closed_when_a_suite_cannot_load():
    runner = EDITOR_RUN_ALL.read_text(encoding="utf-8")

    assert "_load_errors = []" in runner
    assert "_load_errors.append" in runner
    assert "if _load_errors:" in runner
    assert "raise RuntimeError" in runner


def test_selected_node_wrapper_does_not_join_duplicate_names(monkeypatch):
    calls = []
    infos = [
        SimpleNamespace(
            node_name="DuplicateName",
            node_class="K2Node_CustomEvent",
            object_path="/Game/A.A:EventGraph.DuplicateName",
            stable_id="fallback:node:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            graph_id="fallback:graph:1111111111111111111111111111111111111111",
            owner_id="fallback:graph:1111111111111111111111111111111111111111",
            type_path="/Script/BlueprintGraph.K2Node_CustomEvent",
            graph_owner_id="/Game/A.A:EventGraph",
            graph_name="EventGraph",
            graph_type_path="/Script/BlueprintGraph.EdGraphSchema_K2",
        ),
        SimpleNamespace(
            node_name="DuplicateName",
            node_class="K2Node_CallFunction",
            object_path="/Game/B.B:OtherGraph.DuplicateName",
            stable_id="node:bbbbbbbb",
            graph_id="graph:22222222",
        ),
    ]

    class Helper:
        @staticmethod
        def get_selected_blueprint_node_infos():
            calls.append("infos")
            return infos

        @staticmethod
        def get_selected_blueprint_nodes():
            raise AssertionError("raw selected nodes must not be queried")

    module = _load_blueprint_actions(monkeypatch, Helper)
    result = json.loads(module.ue_get_selected_bp_nodes())

    assert result == {
        "success": True,
        "selected_nodes_count": 2,
        "selected_nodes": [
            {
                "name": "DuplicateName",
                "class": "K2Node_CustomEvent",
                "object_path": "/Game/A.A:EventGraph.DuplicateName",
                "stable_id": "fallback:node:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                "graph_id": "fallback:graph:1111111111111111111111111111111111111111",
                "id_kind": "qualified_name_fallback",
                "stable": False,
                "graph_id_kind": "qualified_name_fallback",
                "graph_stable": False,
                "owner_id": "fallback:graph:1111111111111111111111111111111111111111",
                "type_path": "/Script/BlueprintGraph.K2Node_CustomEvent",
                "graph_owner_id": "/Game/A.A:EventGraph",
                "graph_name": "EventGraph",
                "graph_type_path": "/Script/BlueprintGraph.EdGraphSchema_K2",
            },
            {
                "name": "DuplicateName",
                "class": "K2Node_CallFunction",
                "object_path": "/Game/B.B:OtherGraph.DuplicateName",
                "stable_id": "node:bbbbbbbb",
                "graph_id": "graph:22222222",
                "id_kind": "node_guid",
                "stable": True,
                "graph_id_kind": "graph_guid",
                "graph_stable": True,
            },
        ],
    }
    assert calls == ["infos"]


def test_compact_selected_node_links_use_stable_ids_with_duplicate_names(
    monkeypatch,
):
    link_to_first = SimpleNamespace(
        graph_id="graph:11111111",
        node_id="node:aaaaaaaa",
        pin_id="pin:aaaaaaaa",
        node_name="DuplicateName",
        node_title="First node",
        pin_name="Out",
    )
    linked_pin = SimpleNamespace(
        friendly_name="",
        pin_name="In",
        direction="input",
        stable_id="pin:bbbbbbbb",
        pin_id="pin:bbbbbbbb",
        graph_id="graph:22222222",
        node_id="node:bbbbbbbb",
        pin_type="exec",
        pin_sub_type="",
        default_value="",
        linked_to=[link_to_first],
    )
    infos = [
        SimpleNamespace(
            node_name="DuplicateName",
            node_title="First node",
            node_comment="",
            stable_id="node:aaaaaaaa",
            graph_id="graph:11111111",
            pins=[],
        ),
        SimpleNamespace(
            node_name="DuplicateName",
            node_title="Second node",
            node_comment="",
            stable_id="node:bbbbbbbb",
            graph_id="graph:22222222",
            pins=[linked_pin],
        ),
    ]

    class Helper:
        @staticmethod
        def get_selected_blueprint_node_infos():
            return infos

    module = _load_blueprint_actions(monkeypatch, Helper)
    result = json.loads(module.ue_get_selected_bp_node_infos())

    link = result["nodes"][1]["pins"][0]["linked"][0]
    assert link["node_id"] == "node:aaaaaaaa"
    assert link["node"] == 0
    assert result["nodes"][1]["id_kind"] == "node_guid"
    assert result["nodes"][1]["stable"] is True
    assert result["nodes"][1]["pins"][0]["id_kind"] == "pin_guid"
    assert result["nodes"][1]["pins"][0]["stable"] is True
    assert link["node_id_kind"] == "node_guid"
    assert link["pin_id_kind"] == "pin_guid"


def test_selected_fallback_records_expose_replay_qualification(monkeypatch):
    graph_id = "fallback:graph:1111111111111111111111111111111111111111"
    node_id = "fallback:node:2222222222222222222222222222222222222222"
    pin_id = "fallback:pin:3333333333333333333333333333333333333333"
    linked_node_id = "fallback:node:4444444444444444444444444444444444444444"
    linked_pin_id = "fallback:pin:5555555555555555555555555555555555555555"
    link = SimpleNamespace(
        graph_id=graph_id,
        graph_owner_id="/Game/BP.BP",
        graph_name="EventGraph",
        graph_type_path="/Script/BlueprintGraph.EdGraphSchema_K2",
        node_id=linked_node_id,
        node_owner_id=graph_id,
        node_name="LinkedNodeRaw",
        node_type_path="/Script/BlueprintGraph.K2Node_CallFunction",
        pin_id=linked_pin_id,
        owner_id=linked_node_id,
        name="LinkedPinRaw",
        type_path="category=exec\ndirection=1\nordinal=0",
        node_title="Linked Node",
        pin_name="Linked Pin Friendly",
    )
    pin = SimpleNamespace(
        friendly_name="Input Friendly",
        pin_name="InputRaw",
        direction="In",
        stable_id=pin_id,
        pin_id=pin_id,
        graph_id=graph_id,
        node_id=node_id,
        owner_id=node_id,
        type_path="category=exec\ndirection=0\nordinal=0",
        pin_type="exec",
        pin_sub_type="",
        default_value="",
        linked_to=[link],
    )
    node = SimpleNamespace(
        node_name="NodeRaw",
        node_title="Node Title",
        node_comment="",
        stable_id=node_id,
        graph_id=graph_id,
        owner_id=graph_id,
        type_path="/Script/BlueprintGraph.K2Node_CustomEvent",
        graph_owner_id="/Game/BP.BP",
        graph_name="EventGraph",
        graph_type_path="/Script/BlueprintGraph.EdGraphSchema_K2",
        pins=[pin],
    )

    class Helper:
        @staticmethod
        def get_selected_blueprint_node_infos():
            return [node]

    module = _load_blueprint_actions(monkeypatch, Helper)
    result = json.loads(module.ue_get_selected_bp_node_infos())
    node_record = result["nodes"][0]
    pin_record = node_record["pins"][0]
    link_record = pin_record["linked"][0]

    assert node_record["owner_id"] == graph_id
    assert node_record["name"] == "NodeRaw"
    assert node_record["type_path"] == node.type_path
    assert node_record["graph_owner_id"] == "/Game/BP.BP"
    assert pin_record["owner_id"] == node_id
    assert pin_record["name"] == "InputRaw"
    assert pin_record["display_name"] == "Input Friendly"
    assert pin_record["type_path"] == pin.type_path
    assert link_record["owner_id"] == linked_node_id
    assert link_record["name"] == "LinkedPinRaw"
    assert link_record["type_path"] == link.type_path
    assert link_record["node_owner_id"] == graph_id
    assert link_record["graph_owner_id"] == "/Game/BP.BP"


def test_editor_brief_fixture_has_exact_counts_and_strict_cleanup_contract():
    base = EDITOR_TEST_BASE.read_text(encoding="utf-8")
    support = EDITOR_TEST_SUPPORT.read_text(encoding="utf-8")
    inspection_test = INSPECTION_EDITOR_TEST.read_text(encoding="utf-8")

    assert '"/Game/__MCPTests/Blueprint2_' in support
    assert "inspect.isawaitable" not in support
    assert "asyncio.run" not in support
    assert "BLUEPRINT2_TEST_ROOT" in inspection_test
    assert "mcp_unreal_actions import execute_action" in inspection_test
    assert '"ue_get_blueprint_brief"' in inspection_test
    assert "from UnrealMCPython.tests.base import MCPTestCase, TEST_ROOT" not in (
        inspection_test
    )

    for fixture_name in (
        "BriefEnabled",
        "BriefLight",
        "BriefFunction",
        "BriefCustomEvent",
        "BriefInterfaceFunction",
        "BlueprintMacroFactory",
        "BlueprintInterfaceFactory",
    ):
        assert fixture_name in inspection_test
    for macro_contract in (
        "test_get_blueprint_brief_accepts_empty_macro_library",
        "test_get_blueprint_brief_counts_created_macro_when_supported",
        'hasattr(unreal.BlueprintEditorLibrary, "add_macro_graph")',
        "No safe macro graph creation API is exposed in this UE version",
        'self.assertGreater(data["counts"]["macros"], 0)',
        'self.assertGreater(data["counts"]["graphs"], 0)',
        'self.assertGreater(data["counts"]["nodes"], 0)',
    ):
        assert macro_contract in inspection_test
    for assertion in (
        '"variables": 1',
        '"components": 2',
        '"functions": 2',
        '"macros": 0',
        '"events": 1',
        '"dispatchers": 0',
        '"interfaces": 0',
        '"graphs": 3',
        '"nodes": 6',
        'data["capabilities"]["k2_schema"]',
        'data["capabilities"]["has_scs"]',
        "self.assertSuccess(created)",
        "def tearDownClass(cls):",
        "unreal.EditorAssetLibrary.list_assets(",
    ):
        assert assertion in inspection_test
    assert 'self.skipTest(f"Widget Blueprint fixture unavailable' not in inspection_test

    delete_asset = base.split("def delete_asset", 1)[1].split(
        "def delete_actor_by_label", 1
    )[0]
    assert "self.assertTrue(deleted" in delete_asset
    assert "self.assertFalse(" in delete_asset
    assert "except Exception" not in delete_asset


def test_native_blueprint2_fixtures_use_unique_root_and_cleanup_packages():
    native_tests = (PRIVATE / "MCPythonBlueprint2Tests.cpp").read_text(
        encoding="utf-8"
    )

    assert 'TEXT("/Game/__MCPTests/Blueprint2_%s")' in native_tests
    assert "FGuid::NewGuid()" in native_tests
    assert "CleanupFixturePackages" in native_tests
    assert "ON_SCOPE_EXIT" in native_tests
    assert 'TEXT("/MCPythonTests/' not in native_tests
    assert 'TEXT("/Game/Tests/MCP/Blueprint2Native' not in native_tests


def test_compact_inspection_exposes_runtime_helpers_and_stable_selected_ids():
    header = HELPER_HEADER.read_text(encoding="utf-8")
    inspection = INSPECTION_SOURCE.read_text(encoding="utf-8")
    actions = BLUEPRINT_ACTIONS.read_text(encoding="utf-8")

    for declaration in (
        "FString GraphId;",
        "FString NodeId;",
        "FString PinId;",
        "FString StableId;",
        "FString NodeClass;",
        "FString ObjectPath;",
        "static FString GetBlueprintBrief(UBlueprint* Blueprint);",
        "static FString GetBlueprint2Capabilities(UBlueprint* Blueprint);",
    ):
        assert declaration in header
    for token in (
        "BuildCapabilities(Blueprint)",
        "DescribeGraphTarget(",
        "DescribeNodeTarget(",
        "DescribePinTarget(",
        "UK2Node_CustomEvent",
        "UEdGraphSchema_K2::PC_MCDelegate",
    ):
        assert token in inspection
    assert '"stable_id"' in actions
    assert '"graph_id"' in actions
    assert 'return blueprint2.call_asset_helper("get_blueprint_brief", asset_path)' in actions


def test_selected_node_helpers_reject_non_blueprint_editors_before_cast():
    source = HELPER_SOURCE.read_text(encoding="utf-8")

    for function_name in (
        "GetSelectedBlueprintNodes",
        "GetSelectedBlueprintNodeInfos",
    ):
        body = source.split(f"UMCPythonHelper::{function_name}", 1)[1]
        body = body.split("\n}", 1)[0]
        blueprint_guard = body.index("Cast<UBlueprint>(Asset)")
        editor_kind_guard = body.index("IsSupportedBlueprintSelectionEditor")
        toolkit_cast = body.index("static_cast<FAssetEditorToolkit*>")
        blueprint_editor_cast = body.index("static_cast<FBlueprintEditor*>")
        assert blueprint_guard < editor_kind_guard
        assert editor_kind_guard < min(toolkit_cast, blueprint_editor_cast)
        assert "GetAssociatedTabManager" in body
    core = CORE_SOURCE.read_text(encoding="utf-8")
    assert "IsSupportedBlueprintSelectionEditor" in core
    for editor_name in (
        "BlueprintEditor",
        "WidgetBlueprintEditor",
        "AnimationBlueprintEditor",
    ):
        assert f'TEXT("{editor_name}")' in core
