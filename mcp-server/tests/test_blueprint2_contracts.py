"""Strict contracts for the additive Blueprint 2 registry surface."""

import ast
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

    validator = Draft202012Validator(TYPE_SPEC)
    with pytest.raises(ValidationError):
        validator.validate(
            {
                "kind": "array",
                "item": {"kind": "array", "item": {"kind": "bool"}},
            }
        )

    with pytest.raises(ValidationError):
        validator.validate(
            {
                "kind": "set",
                "item": {"kind": "array", "item": {"kind": "int"}},
            }
        )
    with pytest.raises(ValidationError):
        validator.validate(
            {
                "kind": "map",
                "key": {"kind": "set", "item": {"kind": "name"}},
                "value": {"kind": "string"},
            }
        )
    with pytest.raises(ValidationError):
        validator.validate(
            {
                "kind": "map",
                "key": {"kind": "string"},
                "value": {"kind": "array", "item": {"kind": "int"}},
            }
        )

    validator.validate(
        {
            "kind": "array",
            "item": {
                "kind": "struct",
                "type_path": "/Script/CoreUObject.Vector",
            },
        }
    )


def test_canonical_blueprint_type_refs_resolve_inside_action_schema():
    schema = _new_specs()["create_blueprint_function"]["input_schema"]
    validator = Draft202012Validator(schema)

    def params(type_value: dict) -> dict:
        return {
            "asset_path": "/Game/BP_Player",
            "function_name": "GroupScores",
            "inputs": [{"name": "ScoresByPlayer", "type": type_value}],
        }

    validator.validate(params({"kind": "array", "item": {"kind": "int"}}))
    with pytest.raises(ValidationError):
        validator.validate(
            params(
                {
                    "kind": "array",
                    "item": {"kind": "array", "item": {"kind": "int"}},
                }
            )
        )


def test_blueprint_member_parameter_lists_match_the_native_bound():
    specs = _new_specs()
    parameter_fields = (
        ("create_blueprint_function", "inputs"),
        ("create_blueprint_function", "outputs"),
        ("set_blueprint_function_signature", "inputs"),
        ("set_blueprint_function_signature", "outputs"),
        ("create_blueprint_macro", "inputs"),
        ("create_blueprint_macro", "outputs"),
        ("create_custom_event", "parameters"),
        ("add_event_dispatcher", "parameters"),
    )

    for action, field in parameter_fields:
        schema = specs[action]["input_schema"]["properties"][field]
        assert schema["maxItems"] == 128, (action, field)


def test_blueprint_member_names_match_the_ue57_validator_bound():
    specs = _new_specs()
    create_schema = specs["create_blueprint_function"]["input_schema"]
    rename_schema = specs["rename_blueprint_function"]["input_schema"]

    assert create_schema["properties"]["function_name"]["maxLength"] == 100
    assert rename_schema["properties"]["new_name"]["maxLength"] == 100
    assert (
        create_schema["properties"]["inputs"]["items"]["properties"]["name"][
            "maxLength"
        ]
        == 100
    )


def test_inspect_queries_are_bounded_and_independently_pageable():
    schema = _new_specs()["inspect_blueprint"]["input_schema"]
    assert schema["required"] == ["asset_path"]
    queries = schema["properties"]["queries"]
    assert "minItems" not in queries
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
    assert query["properties"]["detail"] == {
        "type": "string",
        "enum": ["compact", "detailed"],
        "default": "compact",
    }
    assert query["additionalProperties"] is False
    assert "compact" not in schema["properties"]
    Draft202012Validator(schema).validate({"asset_path": "/Game/BP_Player", "queries": []})
    Draft202012Validator(schema).validate({"asset_path": "/Game/BP_Player"})


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
    from unreal_mcp.blueprint2_action_specs import (
        FALLBACK_GRAPH_ID,
        FALLBACK_NODE_ID,
        FALLBACK_VARIABLE_ID,
        GRAPH_ID,
        INTERFACE_ID,
        NODE_ID,
        PIN_ID,
        VARIABLE_ID,
        COMPONENT_ID,
    )

    target_fields = {
        "rename_blueprint_function": {"function_id": FALLBACK_GRAPH_ID},
        "set_blueprint_function_signature": {"function_id": FALLBACK_GRAPH_ID},
        "delete_blueprint_function": {"function_id": FALLBACK_GRAPH_ID},
        "delete_blueprint_macro": {"macro_id": FALLBACK_GRAPH_ID},
        "delete_custom_event": {"event_id": FALLBACK_NODE_ID},
        "remove_event_dispatcher": {"dispatcher_id": FALLBACK_VARIABLE_ID},
        "remove_blueprint_interface": {"interface_id": INTERFACE_ID},
        "add_reflected_blueprint_node": {"graph_id": GRAPH_ID},
        "set_blueprint_node_properties": {"node_id": NODE_ID},
        "disconnect_blueprint_pins": {
            "pin_id": PIN_ID,
            "source_pin_id": PIN_ID,
            "target_pin_id": PIN_ID,
        },
        "rename_blueprint_variable": {"variable_id": FALLBACK_VARIABLE_ID},
        "remove_blueprint_variable": {"variable_id": FALLBACK_VARIABLE_ID},
        "set_blueprint_variable_default": {"variable_id": VARIABLE_ID},
        "set_blueprint_variable_metadata": {"variable_id": VARIABLE_ID},
        "set_blueprint_variable_replication": {"variable_id": VARIABLE_ID},
        "rename_blueprint_component": {"component_id": COMPONENT_ID},
        "reparent_blueprint_component": {
            "component_id": COMPONENT_ID,
            "parent_component_id": COMPONENT_ID,
        },
        "reorder_blueprint_component": {"component_id": COMPONENT_ID},
        "set_blueprint_component_transform": {"component_id": COMPONENT_ID},
    }
    specs = _new_specs()
    for action, fields in target_fields.items():
        properties = specs[action]["input_schema"]["properties"]
        for field, expected_schema in fields.items():
            assert properties[field] == expected_schema, f"{action}.{field}"


def test_stable_id_schema_accepts_qualified_fallback_ids():
    from unreal_mcp.blueprint2_action_specs import STABLE_ID

    validator = Draft202012Validator(STABLE_ID)
    for stable_id in (
        "graph:00112233-4455-6677-8899-aabbccddeeff",
        "node:00112233-4455-6677-8899-aabbccddeeff",
        "pin:00112233-4455-6677-8899-aabbccddeeff",
        "variable:00112233-4455-6677-8899-aabbccddeeff",
        "component:00112233-4455-6677-8899-aabbccddeeff",
        "interface:/Script/Engine.Interface",
        "interface:/Game/Interfaces/BPI_Test.BPI_Test_C",
        "interface:/Engine/Interfaces/BPI_Test.BPI_Test_C",
        "interface:/MyPlugin/API/BPI_Test.BPI_Test_C",
        "fallback:graph:c6afa87de837f324fd224c43d4f24e5fe74ce74d",
    ):
        validator.validate(stable_id)

    for invalid_id in (
        "graph:not-a-guid",
        "pin:one",
        "graph:00112233445566778899aabbccddeeff",
        "graph:00112233-4455-6677-8899-AABBCCDDEEFF",
        "interface:not-a-path",
        "interface:/Game/Interfaces/BPI_Test",
        "fallback:graph:not-a-sha1",
        "fallback:interface:c6afa87de837f324fd224c43d4f24e5fe74ce74d",
    ):
        with pytest.raises(ValidationError):
            validator.validate(invalid_id)


def test_fallback_capable_mutations_require_explicit_qualification():
    specs = _new_specs()
    fallback_ids = {
        "rename_blueprint_function": (
            "function_id",
            "fallback:graph:1111111111111111111111111111111111111111",
            {
                "function_owner_id": "/Game/BP_Player.BP_Player",
                "function_name": "CalculateScore",
                "function_type_path": "/Script/BlueprintGraph.EdGraphSchema_K2",
            },
        ),
        "set_blueprint_function_signature": (
            "function_id",
            "fallback:graph:2222222222222222222222222222222222222222",
            {
                "function_owner_id": "/Game/BP_Player.BP_Player",
                "function_name": "CalculateScore",
                "function_type_path": "/Script/BlueprintGraph.EdGraphSchema_K2",
            },
        ),
        "delete_blueprint_function": (
            "function_id",
            "fallback:graph:3333333333333333333333333333333333333333",
            {
                "function_owner_id": "/Game/BP_Player.BP_Player",
                "function_name": "CalculateScore",
                "function_type_path": "/Script/BlueprintGraph.EdGraphSchema_K2",
            },
        ),
        "delete_blueprint_macro": (
            "macro_id",
            "fallback:graph:4444444444444444444444444444444444444444",
            {
                "macro_owner_id": "/Game/BP_Player.BP_Player",
                "macro_name": "ClampScore",
                "macro_type_path": "/Script/BlueprintGraph.EdGraphSchema_K2",
            },
        ),
        "delete_custom_event": (
            "event_id",
            "fallback:node:5555555555555555555555555555555555555555",
            {
                "event_name": "OnScoreChanged",
                "owner_graph_id": "graph:11111111-1111-4111-8111-111111111111",
                "event_type_path": "/Script/BlueprintGraph.K2Node_CustomEvent",
            },
        ),
        "remove_event_dispatcher": (
            "dispatcher_id",
            "fallback:variable:6666666666666666666666666666666666666666",
            {
                "dispatcher_owner_id": "/Game/BP_Player.BP_Player",
                "dispatcher_name": "ScoreChanged",
                "dispatcher_type_path": "category=mcdelegate",
            },
        ),
        "rename_blueprint_variable": (
            "variable_id",
            "fallback:variable:7777777777777777777777777777777777777777",
            {
                "variable_owner_id": "/Game/BP_Player.BP_Player",
                "variable_name": "Score",
                "variable_type_path": "category=int\ncontainer=none",
            },
        ),
        "remove_blueprint_variable": (
            "variable_id",
            "fallback:variable:8888888888888888888888888888888888888888",
            {
                "variable_owner_id": "/Game/BP_Player.BP_Player",
                "variable_name": "Score",
                "variable_type_path": "category=int\ncontainer=none",
            },
        ),
    }

    for action, (id_field, fallback_id, qualification) in fallback_ids.items():
        spec = specs[action]
        schema = spec["input_schema"]
        properties = schema["properties"]
        assert properties["allow_name_fallback"] == {
            "type": "boolean",
            "default": False,
        }, action
        for field in qualification:
            assert field in properties, f"{action}.{field}"

        params = dict(spec["examples"][0]["params"])
        params[id_field] = fallback_id
        validator = Draft202012Validator(schema)
        with pytest.raises(ValidationError):
            validator.validate(params)

        qualified = {
            **params,
            "allow_name_fallback": True,
            **qualification,
        }
        validator.validate(qualified)
        for field in qualification:
            missing_one = dict(qualified)
            del missing_one[field]
            with pytest.raises(ValidationError):
                validator.validate(missing_one)


def test_mutation_target_fields_reject_wrong_id_kinds():
    specs = _new_specs()
    cases = {
        "rename_blueprint_function": ("function_id", "pin:33333333-3333-4333-8333-333333333333"),
        "delete_blueprint_macro": ("macro_id", "node:22222222-2222-4222-8222-222222222222"),
        "delete_custom_event": ("event_id", "graph:11111111-1111-4111-8111-111111111111"),
        "remove_event_dispatcher": ("dispatcher_id", "component:55555555-5555-4555-8555-555555555555"),
        "remove_blueprint_interface": ("interface_id", "variable:44444444-4444-4444-8444-444444444444"),
        "add_reflected_blueprint_node": ("graph_id", "pin:33333333-3333-4333-8333-333333333333"),
        "set_blueprint_node_properties": ("node_id", "graph:11111111-1111-4111-8111-111111111111"),
        "set_blueprint_variable_default": ("variable_id", "component:55555555-5555-4555-8555-555555555555"),
        "rename_blueprint_component": ("component_id", "variable:44444444-4444-4444-8444-444444444444"),
    }
    for action, (field, wrong_id) in cases.items():
        spec = specs[action]
        params = dict(spec["examples"][0]["params"])
        params[field] = wrong_id
        with pytest.raises(ValidationError):
            Draft202012Validator(spec["input_schema"]).validate(params)


def test_interface_class_paths_accept_all_unreal_mount_roots():
    schema = _new_specs()["add_blueprint_interface"]["input_schema"]
    validator = Draft202012Validator(schema)
    for interface_path in (
        "/Script/CoreUObject.Interface",
        "/Game/API/BPI_Usable.BPI_Usable_C",
        "/Engine/API/BPI_Usable.BPI_Usable_C",
        "/MyPlugin/API/BPI_Usable.BPI_Usable_C",
    ):
        validator.validate(
            {
                "asset_path": "/Game/BP_Player",
                "interface_path": interface_path,
            }
        )

    for invalid_path in (
        "Game/API/BPI_Usable.BPI_Usable_C",
        "/Game/API/BPI_Usable",
        "/Game/API/BPI_Usable.BPI_Usable_C:Subobject",
    ):
        with pytest.raises(ValidationError):
            validator.validate(
                {
                    "asset_path": "/Game/BP_Player",
                    "interface_path": invalid_path,
                }
            )


def test_disconnect_blueprint_pins_has_exclusive_target_forms():
    schema = _new_specs()["disconnect_blueprint_pins"]["input_schema"]
    validator = Draft202012Validator(schema)
    common = {"asset_path": "/Game/BP_Player"}
    pin_id = "pin:11111111-1111-4111-8111-111111111111"
    source_pin_id = "pin:22222222-2222-4222-8222-222222222222"
    target_pin_id = "pin:33333333-3333-4333-8333-333333333333"

    validator.validate({**common, "pin_id": pin_id})
    validator.validate(
        {
            **common,
            "source_pin_id": source_pin_id,
            "target_pin_id": target_pin_id,
        }
    )
    with pytest.raises(ValidationError):
        validator.validate(common)
    with pytest.raises(ValidationError):
        validator.validate({**common, "source_pin_id": source_pin_id})
    with pytest.raises(ValidationError):
        validator.validate(
            {
                **common,
                "pin_id": pin_id,
                "source_pin_id": source_pin_id,
                "target_pin_id": target_pin_id,
            }
        )


def test_set_blueprint_node_properties_example_uses_public_allowlist_key():
    params = _new_specs()["set_blueprint_node_properties"]["examples"][0][
        "params"
    ]

    assert params["properties"] == {"comment": "Validated comment"}


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
        "component_id": "component:55555555-5555-4555-8555-555555555555",
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


def test_blueprint_brief_success_data_schema_is_strict():
    spec = _new_specs()["get_blueprint_brief"]
    schema = spec["output_schema"]
    validator = Draft202012Validator(schema)
    success = {
        "success": True,
        "status": "succeeded",
        "summary": "Blueprint brief returned.",
        "data": {
            "asset_path": "/Game/BP_Player.BP_Player",
            "blueprint_class_path": "/Script/Engine.Blueprint",
            "parent_class_path": "/Script/Engine.Actor",
            "generated_class_path": "/Game/BP_Player.BP_Player_C",
            "skeleton_class_path": "/Game/BP_Player.SKEL_BP_Player_C",
            "compile_status": "UpToDate",
            "interfaces": ["/Script/CoreUObject.Interface"],
            "top_level_components": ["DefaultSceneRoot"],
            "graphs": ["EventGraph"],
            "counts": {
                "variables": 1,
                "components": 1,
                "functions": 0,
                "macros": 0,
                "events": 1,
                "dispatchers": 0,
                "interfaces": 1,
                "graphs": 1,
                "nodes": 3,
            },
            "capabilities": {
                "api_version": 2,
                "engine_version": "5.7.0",
                "scalar_kinds": ["bool"],
                "container_kinds": ["array"],
                "node_families": ["event"],
                "supports_k2_schema": True,
                "supports_scs_operations": True,
                "supports_compiler_tokens": True,
                "has_k2_graphs": True,
                "all_graphs_k2_schema": True,
                "k2_schema": True,
                "compiler_tokens": True,
                "has_scs": True,
                "scs_operations": True,
            },
        },
        "changes": [],
        "warnings": [],
        "errors": [],
        "next_actions": [],
        "trace_id": "brief-contract",
    }
    validator.validate(success)
    validator.validate(spec["error_examples"][0])

    missing_path = json.loads(json.dumps(success))
    del missing_path["data"]["asset_path"]
    with pytest.raises(ValidationError):
        validator.validate(missing_path)

    missing_count = json.loads(json.dumps(success))
    del missing_count["data"]["counts"]["nodes"]
    with pytest.raises(ValidationError):
        validator.validate(missing_count)

    unknown_field = json.loads(json.dumps(success))
    unknown_field["data"]["full_nodes"] = []
    with pytest.raises(ValidationError):
        validator.validate(unknown_field)

    malformed = dict(spec["error_examples"][0])
    malformed["status"] = "anything-goes"
    with pytest.raises(ValidationError):
        validator.validate(malformed)


def test_conditional_replication_contract_rejects_ambiguous_notify_names():
    schema = _new_specs()["set_blueprint_variable_replication"]["input_schema"]
    validator = Draft202012Validator(schema)
    common = {
        "asset_path": "/Game/BP_Player",
        "variable_id": "variable:44444444-4444-4444-8444-444444444444",
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
        "asset_path, macro_name, inputs=[], outputs=[], pure=False, "
        "category='', description=''"
    )
    assert catalog["create_custom_event"]["params"] == (
        "asset_path, graph_id, event_name, parameters=[], pos_x=0.0, pos_y=0.0"
    )
    assert catalog["add_event_dispatcher"]["params"] == (
        "asset_path, dispatcher_name, parameters=[], category='', description=''"
    )
    assert catalog["disconnect_blueprint_pins"]["params"] == (
        "asset_path, pin_id='', source_pin_id='', target_pin_id=''"
    )
    assert catalog["set_blueprint_variable_replication"]["params"] == (
        "asset_path, variable_id, mode, notify_function_name=''"
    )
    reflected = _new_specs()["add_reflected_blueprint_node"]["input_schema"]
    assert "position" in reflected["required"]
    assert reflected["properties"]["member_kind"]["enum"] == [
        "function",
        "property_get",
        "property_set",
        "cast_to",
        "enum_literal",
        "make_struct",
        "break_struct",
    ]


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
        if isinstance(node, ast.FunctionDef) and node.name.startswith("ue_")
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
    active_actions = {
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
    }
    for action in NEW_BLUEPRINT2_ACTIONS - active_actions:
        result = getattr(module, f"ue_{action}")()
        payload = json.loads(result)
        ToolResult.model_validate(payload)
        assert payload["errors"][0]["code"] == "UE_VERSION_UNSUPPORTED"
        assert payload["errors"][0]["details"]["capability"] == "blueprint2_cpp_core"


def test_inspection_materializes_detailed_records_after_pagination():
    source = (
        Path(__file__).parents[2]
        / "Plugins"
        / "UnrealMCPython"
        / "Source"
        / "UnrealMCPython"
        / "Private"
        / "MCPythonHelper_BlueprintInspection.cpp"
    ).read_text(encoding="utf-8")

    page_start = source.index("const int32 End =")
    materialize = source.index("MaterializeInspectRecord", page_start)
    append_item = source.index("Items.Add", page_start)

    assert page_start < materialize < append_item
