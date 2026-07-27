"""Strict contracts for the additive Blueprint 2 registry surface."""

import ast
import asyncio
import importlib.util
import json
import sys
import types
from pathlib import Path

import pytest
from jsonschema import Draft202012Validator, ValidationError

sys.path.insert(0, str(Path(__file__).parents[1]))

import generate_catalog
from generate_catalog import build, build_registry
from unreal_mcp.contracts import ToolResult


NEW_BLUEPRINT2_ACTIONS = {
    "get_blueprint_brief",
    "inspect_blueprint",
    "create_blueprint_function",
    "rename_blueprint_function",
    "set_blueprint_function_signature",
    "delete_blueprint_function",
    "create_blueprint_macro",
    "delete_blueprint_macro",
    "create_custom_event",
    "delete_custom_event",
    "add_event_dispatcher",
    "remove_event_dispatcher",
    "add_blueprint_interface",
    "remove_blueprint_interface",
    "add_reflected_blueprint_node",
    "set_blueprint_node_properties",
    "disconnect_blueprint_pins",
    "rename_blueprint_variable",
    "remove_blueprint_variable",
    "set_blueprint_variable_default",
    "set_blueprint_variable_metadata",
    "set_blueprint_variable_replication",
    "rename_blueprint_component",
    "reparent_blueprint_component",
    "reorder_blueprint_component",
    "set_blueprint_component_transform",
    "get_blueprint_health",
    "snapshot_blueprint_graph",
    "diff_blueprint_graphs",
}


READ_ACTIONS = {
    "get_blueprint_brief",
    "inspect_blueprint",
    "snapshot_blueprint_graph",
    "diff_blueprint_graphs",
}

WRITE_ACTIONS = {
    "create_blueprint_function",
    "set_blueprint_function_signature",
    "create_blueprint_macro",
    "create_custom_event",
    "add_event_dispatcher",
    "add_blueprint_interface",
    "add_reflected_blueprint_node",
    "set_blueprint_node_properties",
    "disconnect_blueprint_pins",
    "set_blueprint_variable_default",
    "set_blueprint_variable_metadata",
    "set_blueprint_variable_replication",
    "set_blueprint_component_transform",
}

DESTRUCTIVE_ACTIONS = {
    "rename_blueprint_function",
    "delete_blueprint_function",
    "delete_blueprint_macro",
    "delete_custom_event",
    "remove_event_dispatcher",
    "remove_blueprint_interface",
    "rename_blueprint_variable",
    "remove_blueprint_variable",
    "rename_blueprint_component",
    "reparent_blueprint_component",
    "reorder_blueprint_component",
}


def _new_specs() -> dict:
    registry = build_registry()["blueprint"]
    missing = NEW_BLUEPRINT2_ACTIONS - set(registry)
    assert not missing, f"missing Blueprint 2 actions: {sorted(missing)}"
    return {name: registry[name] for name in NEW_BLUEPRINT2_ACTIONS}


def test_blueprint2_actions_are_additive_and_registry_aligned():
    catalog = build()
    registry = build_registry()

    assert NEW_BLUEPRINT2_ACTIONS <= set(catalog["blueprint"])
    assert set(catalog["blueprint"]) == set(registry["blueprint"])
    # The 19 legacy Blueprint actions remain available alongside all 29 additions.
    assert len(catalog["blueprint"]) == 48
    assert sum(len(actions) for actions in catalog.values()) == 290


def test_canonical_blueprint_type_schema_is_exact_and_bounded():
    from unreal_mcp.blueprint2_action_specs import TYPE_SPEC

    Draft202012Validator.check_schema(TYPE_SPEC)
    choices = {
        choice["properties"]["kind"]["const"]: choice
        for choice in TYPE_SPEC["oneOf"]
    }
    assert set(choices) == {
        "bool",
        "byte",
        "int",
        "int64",
        "real",
        "string",
        "name",
        "text",
        "enum",
        "struct",
        "object",
        "class",
        "interface",
        "soft_object",
        "soft_class",
        "array",
        "set",
        "map",
    }
    assert choices["real"]["properties"]["precision"]["enum"] == ["float", "double"]
    assert choices["enum"]["properties"]["type_path"]["pattern"] == r"^/Script/"
    assert choices["struct"]["properties"]["type_path"]["pattern"] == r"^/Script/"
    for kind in {"object", "class", "interface", "soft_object", "soft_class"}:
        assert choices[kind]["properties"]["class_path"]["pattern"] == r"^/Script/"
    for choice in choices.values():
        assert choice["additionalProperties"] is False
        assert "kind" in choice["required"]

    nested = {"kind": "bool"}
    for _ in range(8):
        nested = {"kind": "array", "item": nested}
    validator = Draft202012Validator(TYPE_SPEC)
    validator.validate(nested)
    with pytest.raises(ValidationError):
        validator.validate({"kind": "array", "item": nested})


def test_canonical_blueprint_type_recursion_resolves_inside_action_schema():
    schema = _new_specs()["create_blueprint_function"]["input_schema"]
    validator = Draft202012Validator(schema)

    def params(type_value: dict) -> dict:
        return {
            "asset_path": "/Game/BP_Player",
            "function_name": "GroupScores",
            "inputs": [{"name": "ScoresByPlayer", "type": type_value}],
        }

    validator.validate(params({"kind": "array", "item": {"kind": "int"}}))
    nested = {"kind": "int"}
    for _ in range(8):
        nested = {"kind": "array", "item": nested}
    validator.validate(params(nested))
    with pytest.raises(ValidationError):
        validator.validate(params({"kind": "array", "item": nested}))


def test_inspect_queries_are_bounded_and_independently_pageable():
    schema = _new_specs()["inspect_blueprint"]["input_schema"]
    queries = schema["properties"]["queries"]
    assert queries["minItems"] == 1
    query = queries["items"]
    assert set(query["properties"]["op"]["enum"]) == {
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
    }
    assert query["properties"]["limit"] == {
        "type": "integer",
        "minimum": 1,
        "maximum": 500,
        "default": 100,
    }
    assert query["properties"]["cursor"]["type"] == "string"
    assert query["additionalProperties"] is False


def test_contracts_require_full_unreal_object_paths():
    specs = _new_specs()
    path_schemas = [
        specs["add_blueprint_interface"]["input_schema"]["properties"]["interface_path"],
        specs["add_reflected_blueprint_node"]["input_schema"]["properties"]["member_path"],
    ]
    for path_schema in path_schemas:
        validator = Draft202012Validator(path_schema)
        validator.validate("/Script/Engine.Actor")
        with pytest.raises(ValidationError):
            validator.validate("Engine.Actor")


def test_mutation_targets_use_stable_ids():
    from unreal_mcp.blueprint2_action_specs import STABLE_ID

    target_fields = {
        "rename_blueprint_function": ["function_id"],
        "set_blueprint_function_signature": ["function_id"],
        "delete_blueprint_function": ["function_id"],
        "delete_blueprint_macro": ["macro_id"],
        "delete_custom_event": ["event_id"],
        "remove_event_dispatcher": ["dispatcher_id"],
        "remove_blueprint_interface": ["interface_id"],
        "add_reflected_blueprint_node": ["graph_id"],
        "set_blueprint_node_properties": ["node_id"],
        "rename_blueprint_variable": ["variable_id"],
        "remove_blueprint_variable": ["variable_id"],
        "set_blueprint_variable_default": ["variable_id"],
        "set_blueprint_variable_metadata": ["variable_id"],
        "set_blueprint_variable_replication": ["variable_id"],
        "rename_blueprint_component": ["component_id"],
        "reparent_blueprint_component": ["component_id", "parent_component_id"],
        "reorder_blueprint_component": ["component_id"],
        "set_blueprint_component_transform": ["component_id"],
        "snapshot_blueprint_graph": ["graph_id"],
    }
    specs = _new_specs()
    for action, fields in target_fields.items():
        properties = specs[action]["input_schema"]["properties"]
        for field in fields:
            assert properties[field] == STABLE_ID, f"{action}.{field}"


def test_disconnect_blueprint_pins_has_exclusive_target_forms():
    schema = _new_specs()["disconnect_blueprint_pins"]["input_schema"]
    validator = Draft202012Validator(schema)
    common = {"asset_path": "/Game/BP_Player"}

    validator.validate({**common, "pin_id": "pin:one"})
    validator.validate(
        {**common, "source_pin_id": "pin:source", "target_pin_id": "pin:target"}
    )
    with pytest.raises(ValidationError):
        validator.validate(common)
    with pytest.raises(ValidationError):
        validator.validate({**common, "source_pin_id": "pin:source"})
    with pytest.raises(ValidationError):
        validator.validate(
            {
                **common,
                "pin_id": "pin:one",
                "source_pin_id": "pin:source",
                "target_pin_id": "pin:target",
            }
        )


def test_component_transform_vectors_are_bounded():
    schema = _new_specs()["set_blueprint_component_transform"]["input_schema"]
    transform = schema["properties"]["transform"]
    for name in ("location", "rotation", "scale"):
        vector = transform["properties"][name]
        assert vector["minItems"] == 3
        assert vector["maxItems"] == 3
        assert vector["items"]["minimum"] == -1_000_000_000
        assert vector["items"]["maximum"] == 1_000_000_000

    validator = Draft202012Validator(schema)
    params = {
        "asset_path": "/Game/BP_Player",
        "component_id": "component:root",
        "transform": {"location": [0, 0, 0]},
    }
    validator.validate(params)
    with pytest.raises(ValidationError):
        validator.validate(
            {**params, "transform": {"location": [0, 0, 1_000_000_001]}}
        )


def test_diff_queries_use_fixed_sections_and_independent_pagination():
    schema = _new_specs()["diff_blueprint_graphs"]["input_schema"]
    query = schema["properties"]["queries"]["items"]
    assert query["properties"]["section"]["enum"] == [
        "nodes",
        "pins",
        "connections",
        "properties",
        "positions",
    ]
    assert query["properties"]["limit"]["minimum"] == 1
    assert query["properties"]["limit"]["maximum"] == 500
    assert query["properties"]["cursor"]["type"] == "string"
    assert query["properties"]["detailed"]["type"] == "boolean"
    assert query["additionalProperties"] is False


def test_blueprint2_output_envelopes_are_strictly_structured():
    specs = _new_specs()
    schema = specs["get_blueprint_brief"]["output_schema"]
    validator = Draft202012Validator(schema)
    validator.validate(specs["get_blueprint_brief"]["error_examples"][0])

    malformed = dict(specs["get_blueprint_brief"]["error_examples"][0])
    malformed["errors"] = [{}]
    with pytest.raises(ValidationError):
        validator.validate(malformed)

    malformed = dict(specs["get_blueprint_brief"]["error_examples"][0])
    malformed["status"] = "anything-goes"
    with pytest.raises(ValidationError):
        validator.validate(malformed)


def test_conditional_replication_contract_rejects_ambiguous_notify_names():
    schema = _new_specs()["set_blueprint_variable_replication"]["input_schema"]
    validator = Draft202012Validator(schema)
    common = {
        "asset_path": "/Game/BP_Player",
        "variable_id": "variable:score-guid",
    }
    validator.validate({**common, "mode": "none"})
    validator.validate({**common, "mode": "replicated"})
    validator.validate(
        {**common, "mode": "rep_notify", "notify_function_name": "OnRep_Score"}
    )
    with pytest.raises(ValidationError):
        validator.validate({**common, "mode": "rep_notify"})
    with pytest.raises(ValidationError):
        validator.validate(
            {**common, "mode": "none", "notify_function_name": "OnRep_Score"}
        )


def test_catalog_defaults_match_registry_optionality_for_blueprint2_actions():
    catalog = build()["blueprint"]
    assert catalog["create_blueprint_function"]["params"] == (
        "asset_path, function_name, inputs=[], outputs=[], pure=False, const=False, "
        "access='public', category='', description=''"
    )
    assert catalog["create_blueprint_macro"]["params"] == (
        "asset_path, macro_name, inputs=[], outputs=[]"
    )
    assert catalog["create_custom_event"]["params"] == (
        "asset_path, event_name, parameters=[]"
    )
    assert catalog["add_event_dispatcher"]["params"] == (
        "asset_path, dispatcher_name, parameters=[]"
    )
    assert catalog["disconnect_blueprint_pins"]["params"] == (
        "asset_path, pin_id='', source_pin_id='', target_pin_id=''"
    )
    assert catalog["set_blueprint_variable_replication"]["params"] == (
        "asset_path, variable_id, mode, notify_function_name=''"
    )
    reflected = _new_specs()["add_reflected_blueprint_node"]["input_schema"]
    assert "position" in reflected["required"]


def test_duplicate_blueprint2_metadata_is_rejected(monkeypatch):
    duplicate = {
        **generate_catalog.BLUEPRINT2_ACTION_SPECS,
        "create_blueprint": generate_catalog.BLUEPRINT2_ACTION_SPECS[
            "get_blueprint_brief"
        ],
    }
    monkeypatch.setattr(generate_catalog, "BLUEPRINT2_ACTION_SPECS", duplicate)
    with pytest.raises(ValueError, match=r"Duplicate Blueprint metadata:.*create_blueprint"):
        generate_catalog._load_action_module("blueprint")


def test_all_blueprint2_specs_have_complete_safety_and_valid_examples():
    from unreal_mcp.blueprint2_action_specs import BLUEPRINT2_ACTION_SPECS

    assert set(BLUEPRINT2_ACTION_SPECS) == NEW_BLUEPRINT2_ACTIONS
    required_fields = {
        "title",
        "description",
        "result_kind",
        "input_schema",
        "output_schema",
        "effect",
        "risk",
        "idempotent",
        "supports_preview",
        "supports_undo",
        "requires_confirmation",
        "ue_versions",
        "examples",
        "error_examples",
    }
    for action, spec in BLUEPRINT2_ACTION_SPECS.items():
        assert required_fields <= set(spec), action
        assert spec["ue_versions"] == ["5.6", "5.7", "5.8"]
        Draft202012Validator.check_schema(spec["input_schema"])
        Draft202012Validator.check_schema(spec["output_schema"])
        validator = Draft202012Validator(spec["input_schema"])
        for example in spec["examples"]:
            assert example["action"] == action
            validator.validate(example["params"])
        for error_example in spec["error_examples"]:
            ToolResult.model_validate(error_example)

    for action in READ_ACTIONS:
        spec = BLUEPRINT2_ACTION_SPECS[action]
        assert (spec["effect"], spec["risk"]) == ("read", "low")
        assert spec["supports_preview"] is False
        assert spec["supports_undo"] is False
        assert spec["requires_confirmation"] is False

    health = BLUEPRINT2_ACTION_SPECS["get_blueprint_health"]
    assert (health["effect"], health["risk"]) == ("write", "high")
    assert health["supports_preview"] is False
    assert health["supports_undo"] is False

    for action in WRITE_ACTIONS:
        spec = BLUEPRINT2_ACTION_SPECS[action]
        assert (spec["effect"], spec["risk"]) == ("write", "medium")
        assert spec["supports_preview"] is True
        assert spec["supports_undo"] is True

    for action in DESTRUCTIVE_ACTIONS:
        spec = BLUEPRINT2_ACTION_SPECS[action]
        assert (spec["effect"], spec["risk"]) == ("destructive", "high")
        assert spec["supports_preview"] is True
        assert spec["supports_undo"] is True
        assert spec["requires_confirmation"] is True


def test_blueprint2_wrappers_have_fixed_signatures_and_structured_stubs(monkeypatch):
    action_path = (
        Path(__file__).parents[2]
        / "Plugins"
        / "UnrealMCPython"
        / "Content"
        / "Python"
        / "UnrealMCPython"
        / "blueprint_actions.py"
    )
    tree = ast.parse(action_path.read_text(encoding="utf-8"))
    nodes = {
        node.name[3:]: node
        for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef) and node.name.startswith("ue_")
    }
    specs = _new_specs()
    assert NEW_BLUEPRINT2_ACTIONS <= set(nodes)
    for action in NEW_BLUEPRINT2_ACTIONS:
        args = [arg.arg for arg in nodes[action].args.args]
        assert args == list(specs[action]["input_schema"]["properties"]), action

    monkeypatch.setitem(sys.modules, "unreal", types.ModuleType("unreal"))
    module_spec = importlib.util.spec_from_file_location("blueprint_actions_contract", action_path)
    assert module_spec is not None and module_spec.loader is not None
    module = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(module)
    for action in NEW_BLUEPRINT2_ACTIONS:
        result = asyncio.run(getattr(module, f"ue_{action}")())
        payload = json.loads(result)
        ToolResult.model_validate(payload)
        assert payload["errors"][0]["code"] == "UE_VERSION_UNSUPPORTED"
        assert payload["errors"][0]["details"]["capability"] == "blueprint2_cpp_core"
