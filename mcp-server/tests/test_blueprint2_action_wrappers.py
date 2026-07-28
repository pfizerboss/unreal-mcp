"""Offline contracts for the shared Universal Blueprint 2 core."""

import importlib.util
import json
from pathlib import Path
import sys
from types import SimpleNamespace

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
