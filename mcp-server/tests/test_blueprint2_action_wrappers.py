"""Offline contracts for the shared Universal Blueprint 2 core."""

import importlib.util
import json
from pathlib import Path
import sys
from types import SimpleNamespace


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


def test_compact_inspection_exposes_runtime_helpers_and_stable_selected_ids():
    header = HELPER_HEADER.read_text(encoding="utf-8")
    inspection = INSPECTION_SOURCE.read_text(encoding="utf-8")
    actions = BLUEPRINT_ACTIONS.read_text(encoding="utf-8")

    for declaration in (
        "FString GraphId;",
        "FString NodeId;",
        "FString PinId;",
        "FString StableId;",
        "static FString GetBlueprintBrief(UBlueprint* Blueprint);",
        "static FString GetBlueprint2Capabilities(UBlueprint* Blueprint);",
    ):
        assert declaration in header
    for token in (
        "BuildCapabilities(Blueprint)",
        "MakeTargetId(",
        "ETargetKind::Graph",
        "ETargetKind::Node",
        "ETargetKind::Pin",
        "UK2Node_CustomEvent",
        "UEdGraphSchema_K2::PC_MCDelegate",
    ):
        assert token in inspection
    assert '"stable_id"' in actions
    assert '"graph_id"' in actions
    assert 'return blueprint2.call_asset_helper("get_blueprint_brief", asset_path)' in actions
