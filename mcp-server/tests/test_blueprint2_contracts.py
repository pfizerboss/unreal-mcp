"""Strict contracts for the additive Blueprint 2 registry surface."""

import ast
import hashlib
import importlib.util
import json
import sys
import types
from copy import deepcopy
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
    "search_blueprint_node_actions",
    "describe_blueprint_node_action",
    "add_blueprint_action_node",
    "suggest_blueprint_nodes_for_pin",
    "suggest_blueprint_nodes_for_connection",
    "add_blueprint_connected_action_node",
    "insert_blueprint_action_node",
    "preview_blueprint_action_replacement",
    "replace_blueprint_node_with_action",
}

PALETTE_ACTIONS = {
    "search_blueprint_node_actions",
    "describe_blueprint_node_action",
    "add_blueprint_action_node",
    "suggest_blueprint_nodes_for_pin",
}

SEMANTIC_ACTIONS = {
    "suggest_blueprint_nodes_for_connection",
    "add_blueprint_connected_action_node",
    "insert_blueprint_action_node",
    "preview_blueprint_action_replacement",
    "replace_blueprint_node_with_action",
}


READ_ACTIONS = {
    "get_blueprint_brief",
    "inspect_blueprint",
    "snapshot_blueprint_graph",
    "diff_blueprint_graphs",
    "search_blueprint_node_actions",
    "describe_blueprint_node_action",
    "suggest_blueprint_nodes_for_pin",
    "suggest_blueprint_nodes_for_connection",
    "preview_blueprint_action_replacement",
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
    "add_blueprint_action_node",
    "add_blueprint_connected_action_node",
    "insert_blueprint_action_node",
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
    "replace_blueprint_node_with_action",
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
    # The 19 legacy Blueprint actions remain available alongside all 38 additions.
    assert len(catalog["blueprint"]) == 57
    assert sum(len(actions) for actions in catalog.values()) == 299


def test_semantic_blueprint_contracts_are_closed_bounded_and_opaque():
    specs = _new_specs()
    suggest = specs["suggest_blueprint_nodes_for_connection"]["input_schema"]
    connected = specs["add_blueprint_connected_action_node"]["input_schema"]
    insert = specs["insert_blueprint_action_node"]["input_schema"]
    preview = specs["preview_blueprint_action_replacement"]["input_schema"]
    replace = specs["replace_blueprint_node_with_action"]["input_schema"]

    for schema in (suggest, connected, insert, preview, replace):
        assert schema["additionalProperties"] is False
        Draft202012Validator.check_schema(schema)

    assert suggest["required"] == [
        "asset_path",
        "graph_id",
        "source_pin_id",
        "target_pin_id",
    ]
    assert suggest["properties"]["limit"] == {
        "type": "integer",
        "minimum": 1,
        "maximum": 200,
        "default": 50,
    }
    assert suggest["properties"]["cursor"]["pattern"] == (
        r"^(?:|palette-cursor:[0-9a-f]{40})$"
    )
    assert connected["required"] == [
        "asset_path",
        "graph_id",
        "pin_id",
        "action_id",
        "connection_binding_id",
        "position",
    ]
    assert connected["properties"]["bindings"]["maxItems"] == 32
    assert insert["required"] == [
        "asset_path",
        "graph_id",
        "source_pin_id",
        "target_pin_id",
        "action_id",
        "input_binding_id",
        "output_binding_id",
        "position",
    ]
    assert preview["properties"]["pin_mapping"]["maxItems"] == 256
    assert preview["properties"]["pin_mapping"]["uniqueItems"] is True
    assert replace["properties"]["replacement_plan_id"]["pattern"] == (
        r"^replacement-plan:[0-9a-f]{40}$"
    )

    Draft202012Validator(connected).validate(
        {
            "asset_path": "/Game/BP.BP",
            "graph_id": "graph:11111111-1111-4111-8111-111111111111",
            "pin_id": "pin:22222222-2222-4222-8222-222222222222",
            "action_id": "action:" + "a" * 40,
            "connection_binding_id": "binding:" + "b" * 40,
            "position": {"x": 320, "y": 160},
            "allow_conversion": False,
            "bindings": [],
        }
    )
    with pytest.raises(ValidationError):
        Draft202012Validator(replace).validate(
            {
                "asset_path": "/Game/BP.BP",
                "graph_id": "graph:11111111-1111-4111-8111-111111111111",
                "replacement_plan_id": "replacement-plan:tampered",
                "allow_loss": False,
            }
        )

    for action in SEMANTIC_ACTIONS:
        Draft202012Validator.check_schema(specs[action]["output_schema"])


def test_semantic_inputs_reject_tampered_duplicate_and_over_limit_values():
    specs = _new_specs()
    invalid_cases = []

    connected = deepcopy(
        specs["add_blueprint_connected_action_node"]["examples"][0]["params"]
    )
    connected["action_id"] = "action:tampered"
    invalid_cases.append(("add_blueprint_connected_action_node", connected))

    connected_binding = deepcopy(
        specs["add_blueprint_connected_action_node"]["examples"][0]["params"]
    )
    connected_binding["connection_binding_id"] = "binding:tampered"
    invalid_cases.append(
        ("add_blueprint_connected_action_node", connected_binding)
    )

    connected_many = deepcopy(
        specs["add_blueprint_connected_action_node"]["examples"][0]["params"]
    )
    connected_many["bindings"] = [
        f"binding:{index:040x}" for index in range(33)
    ]
    invalid_cases.append(
        ("add_blueprint_connected_action_node", connected_many)
    )

    preview_duplicate = deepcopy(
        specs["preview_blueprint_action_replacement"]["examples"][0]["params"]
    )
    mapping = {
        "old_pin_id": "pin:22222222-2222-4222-8222-222222222222",
        "new_binding_id": "binding:" + "b" * 40,
    }
    preview_duplicate["pin_mapping"] = [mapping, deepcopy(mapping)]
    invalid_cases.append(
        ("preview_blueprint_action_replacement", preview_duplicate)
    )

    preview_many = deepcopy(
        specs["preview_blueprint_action_replacement"]["examples"][0]["params"]
    )
    preview_many["pin_mapping"] = [
        {
            "old_pin_id": (
                f"pin:00000001-0001-4001-8001-{index + 1:012x}"
            ),
            "new_binding_id": f"binding:{index:040x}",
        }
        for index in range(257)
    ]
    invalid_cases.append(
        ("preview_blueprint_action_replacement", preview_many)
    )

    for action, params in invalid_cases:
        with pytest.raises(ValidationError):
            Draft202012Validator(
                specs[action]["input_schema"]
            ).validate(params)


def test_semantic_success_contracts_enforce_nonempty_topology():
    specs = _new_specs()

    pin_page = specs["suggest_blueprint_nodes_for_pin"]["output_schema"][
        "allOf"
    ][0]["then"]["properties"]["data"]
    pin_bindings = pin_page["properties"]["items"]["items"]["properties"][
        "connection_bindings"
    ]
    with pytest.raises(ValidationError):
        Draft202012Validator(pin_bindings).validate([])

    connection_page = specs[
        "suggest_blueprint_nodes_for_connection"
    ]["output_schema"]["allOf"][0]["then"]["properties"]["data"]
    response = {
        "kind": "direct",
        "message": "",
        "requires_conversion": False,
    }
    pair = {
        "input_binding_id": "binding:" + "b" * 40,
        "output_binding_id": "binding:" + "c" * 40,
        "input_pin_name": "In",
        "output_pin_name": "Out",
        "input_type": {"kind": "int"},
        "output_type": {"kind": "int"},
        "source_response": response,
        "target_response": response,
        "requires_conversion": False,
        "rank": 0,
    }
    card = {
        "action_id": "action:" + "a" * 40,
        "title": "Identity",
        "category": "Utilities",
        "keywords": ["identity"],
        "action_kind": "function",
        "node_class_path": "/Script/BlueprintGraph.K2Node_CallFunction",
        "owner_path": "/Script/Engine.KismetSystemLibrary",
        "member_path": "/Script/Engine.KismetSystemLibrary:Identity",
        "pure": True,
        "compatible": True,
        "compatibility_summary": "Directly bridges both pins.",
        "requires_binding": False,
        "bindings": [],
        "binding_pairs": [pair],
    }
    page = {
        "asset_path": "/Game/BP.BP",
        "graph_id": "graph:11111111-1111-4111-8111-111111111111",
        "source_pin_id": "pin:22222222-2222-4222-8222-222222222222",
        "target_pin_id": "pin:33333333-3333-4333-8333-333333333333",
        "allow_conversion": False,
        "items": [card],
        "total_count": 1,
        "returned_count": 1,
        "next_cursor": "",
        "result_digest": "sha1:" + "d" * 40,
    }
    Draft202012Validator(connection_page).validate(page)
    with pytest.raises(ValidationError):
        invalid_card = deepcopy(card)
        invalid_card["binding_pairs"] = []
        Draft202012Validator(connection_page).validate(
            {**page, "items": [invalid_card]}
        )
    with pytest.raises(ValidationError):
        Draft202012Validator(connection_page).validate(
            {
                **page,
                "items": [deepcopy(card) for _ in range(201)],
                "total_count": 201,
                "returned_count": 200,
            }
        )

    for action, minimum in (
        ("add_blueprint_connected_action_node", 1),
        ("insert_blueprint_action_node", 2),
    ):
        data = specs[action]["output_schema"]["allOf"][0]["then"][
            "properties"
        ]["data"]
        connections = data["properties"]["connections"]
        assert connections["minItems"] == minimum
        with pytest.raises(ValidationError):
            Draft202012Validator(connections).validate([])


def test_blueprint_palette_contracts_are_closed_bounded_and_opaque():
    specs = _new_specs()
    search = specs["search_blueprint_node_actions"]["input_schema"]
    describe = specs["describe_blueprint_node_action"]["input_schema"]
    spawn = specs["add_blueprint_action_node"]["input_schema"]
    suggest = specs["suggest_blueprint_nodes_for_pin"]["input_schema"]

    assert search["required"] == ["asset_path", "graph_id"]
    assert search["properties"]["limit"] == {
        "type": "integer",
        "minimum": 1,
        "maximum": 200,
        "default": 50,
    }
    assert search["properties"]["filters"]["additionalProperties"] is False
    assert describe["required"] == ["action_id"]
    assert spawn["required"] == [
        "asset_path",
        "graph_id",
        "action_id",
        "position",
    ]
    assert suggest["required"] == ["asset_path", "graph_id", "pin_id"]
    assert spawn["properties"]["action_id"]["pattern"] == (
        r"^action:[0-9a-f]{40}$"
    )
    assert spawn["properties"]["bindings"]["maxItems"] == 32

    for action in PALETTE_ACTIONS:
        Draft202012Validator.check_schema(specs[action]["input_schema"])
        Draft202012Validator.check_schema(specs[action]["output_schema"])

    search_data = specs["search_blueprint_node_actions"]["output_schema"][
        "allOf"
    ][0]["then"]["properties"]["data"]
    assert search_data["additionalProperties"] is False
    assert search_data["required"] == [
        "asset_path",
        "graph_id",
        "source_pin_id",
        "items",
        "total_count",
        "returned_count",
        "next_cursor",
        "result_digest",
    ]
    card = search_data["properties"]["items"]["items"]
    assert card["additionalProperties"] is False
    assert set(card["required"]) == {
        "action_id",
        "title",
        "category",
        "keywords",
        "action_kind",
        "node_class_path",
        "owner_path",
        "member_path",
        "pure",
        "compatible",
        "compatibility_summary",
        "requires_binding",
        "bindings",
    }


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
            "parent_component_id": {"anyOf": [COMPONENT_ID, {"type": "null"}]},
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


@pytest.mark.parametrize(
    ("schema_name", "kind"),
    (("GRAPH_ID", "graph"), ("NODE_ID", "node"), ("PIN_ID", "pin")),
)
def test_persisted_snapshot_id_schemas_reject_zero_guid(schema_name, kind):
    from unreal_mcp import blueprint2_action_specs

    schema = getattr(blueprint2_action_specs, schema_name)
    with pytest.raises(ValidationError):
        Draft202012Validator(schema).validate(
            f"{kind}:00000000-0000-0000-0000-000000000000"
        )


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


def test_reparent_component_uses_null_for_the_scs_root():
    schema = _new_specs()["reparent_blueprint_component"]["input_schema"]
    validator = Draft202012Validator(schema)
    common = {
        "asset_path": "/Game/BP_Player",
        "component_id": "component:55555555-5555-4555-8555-555555555555",
    }

    validator.validate({**common, "parent_component_id": None})
    validator.validate(
        {
            **common,
            "parent_component_id": (
                "component:66666666-6666-4666-8666-666666666666"
            ),
        }
    )
    with pytest.raises(ValidationError):
        validator.validate({**common, "parent_component_id": ""})


def test_diff_queries_use_fixed_sections_and_independent_pagination():
    specs = _new_specs()
    snapshot_schema = specs["snapshot_blueprint_graph"]["input_schema"]
    assert snapshot_schema["required"] == ["asset_path"]
    assert "graph_ids" in snapshot_schema["properties"]
    assert "graph_id" not in snapshot_schema["properties"]
    assert "detailed" not in snapshot_schema["properties"]
    assert snapshot_schema["properties"]["graph_ids"]["maxItems"] == 64

    schema = specs["diff_blueprint_graphs"]["input_schema"]
    assert schema["required"] == ["before_snapshot", "after_snapshot"]
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
    assert query["properties"]["detail"]["enum"] == [
        "compact",
        "detailed",
    ]
    assert "detailed" not in query["properties"]
    assert query["additionalProperties"] is False

    validator = Draft202012Validator(schema)
    example_params = specs["diff_blueprint_graphs"]["examples"][0]["params"]
    validator.validate(
        {
            "before_snapshot": example_params["before_snapshot"],
            "after_snapshot": example_params["after_snapshot"],
        }
    )
    validator.validate(
        {
            "before_snapshot": example_params["before_snapshot"],
            "after_snapshot": example_params["after_snapshot"],
            "queries": [],
        }
    )


def test_diff_snapshots_have_strict_v1_schema_and_working_example():
    spec = _new_specs()["diff_blueprint_graphs"]
    schema = spec["input_schema"]
    snapshot = schema["properties"]["before_snapshot"]

    assert snapshot["required"] == [
        "snapshot_version",
        "asset_path",
        "blueprint_class",
        "graphs",
        "digest",
    ]
    assert snapshot["additionalProperties"] is False
    graph = snapshot["properties"]["graphs"]["items"]
    assert graph["required"] == [
        "id",
        "name",
        "schema_path",
        "nodes",
        "connections",
    ]
    assert graph["properties"]["id"]["pattern"].startswith("^graph:")
    assert "fallback" not in graph["properties"]["id"]["pattern"]
    node = graph["properties"]["nodes"]["items"]
    assert node["properties"]["id"]["pattern"].startswith("^node:")
    pin = node["properties"]["pins"]["items"]
    assert pin["properties"]["id"]["pattern"].startswith("^pin:")
    assert pin["properties"]["type"]["oneOf"]
    assert any(
        choice.get("properties", {}).get("kind", {}).get("const") == "exec"
        for choice in pin["properties"]["type"]["oneOf"]
    )
    assert pin["properties"]["default"] == {}

    nested_snapshot = {
        "snapshot_version": 1,
        "asset_path": "/Game/BP_Player.BP_Player",
        "blueprint_class": "/Script/Engine.Blueprint",
        "graphs": [
            {
                "id": "graph:11111111-1111-4111-8111-111111111111",
                "name": "EventGraph",
                "schema_path": "/Script/BlueprintGraph.EdGraphSchema_K2",
                "nodes": [
                    {
                        "id": "node:22222222-2222-4222-8222-222222222222",
                        "class_path": (
                            "/Script/BlueprintGraph.K2Node_CustomEvent"
                        ),
                        "position": {"x": 0, "y": 0},
                        "comment": "",
                        "properties": {},
                        "pins": [
                            {
                                "id": (
                                    "pin:33333333-3333-4333-8333-333333333333"
                                ),
                                "name": "execute",
                                "direction": "input",
                                "type": {"kind": "exec"},
                                "default": None,
                            },
                            {
                                "id": (
                                    "pin:44444444-4444-4444-8444-444444444444"
                                ),
                                "name": "values",
                                "direction": "input",
                                "type": {
                                    "kind": "array",
                                    "item": {"kind": "bool"},
                                },
                                "default": [],
                            },
                            {
                                "id": (
                                    "pin:55555555-5555-4555-8555-555555555555"
                                ),
                                "name": "item",
                                "direction": "input",
                                "type": {
                                    "kind": "object",
                                    "class_path": "/Game/BP_Item.BP_Item_C",
                                },
                                "default": None,
                            },
                            {
                                "id": (
                                    "pin:66666666-6666-4666-8666-666666666666"
                                ),
                                "name": "item_type",
                                "direction": "input",
                                "type": {
                                    "kind": "enum",
                                    "type_path": "/Game/E_Item.E_Item",
                                },
                                "default": "None",
                            },
                        ],
                    }
                ],
                "connections": [],
            }
        ],
        "digest": "sha1:" + "0" * 40,
    }
    validator = Draft202012Validator(schema)
    validator.validate(
        {
            "before_snapshot": nested_snapshot,
            "after_snapshot": nested_snapshot,
            "queries": [],
        }
    )
    example_params = spec["examples"][0]["params"]
    validator.validate(example_params)
    unsigned_example = {
        key: value
        for key, value in example_params["before_snapshot"].items()
        if key != "digest"
    }
    expected_digest = "sha1:" + hashlib.sha1(
        json.dumps(
            unsigned_example,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
    ).hexdigest()
    assert example_params["before_snapshot"]["digest"] == expected_digest
    assert example_params["after_snapshot"]["digest"] == expected_digest

    with pytest.raises(ValidationError):
        validator.validate(
            {
                "before_snapshot": {"snapshot_id": "before"},
                "after_snapshot": nested_snapshot,
            }
        )
    fallback_snapshot = deepcopy(nested_snapshot)
    fallback_snapshot["graphs"][0]["id"] = "fallback:graph:" + "0" * 40
    with pytest.raises(ValidationError):
        validator.validate(
            {
                "before_snapshot": fallback_snapshot,
                "after_snapshot": nested_snapshot,
            }
        )


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
                    "supports_blueprint_node_palette": True,
                    "has_palette_compatible_graphs": True,
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
    conditions = [
        "none",
        "initial_only",
        "owner_only",
        "skip_owner",
        "simulated_only",
        "autonomous_only",
        "simulated_or_physics",
        "initial_or_owner",
        "custom",
        "replay_or_owner",
        "replay_only",
        "simulated_only_no_replay",
        "simulated_or_physics_no_replay",
        "skip_replay",
    ]
    validator.validate({**common, "mode": "none"})
    validator.validate({**common, "mode": "none", "condition": "none"})
    validator.validate({**common, "mode": "none", "notify_function_name": ""})
    for condition in conditions:
        validator.validate(
            {**common, "mode": "replicated", "condition": condition}
        )
    validator.validate(
        {
            **common,
            "mode": "rep_notify",
            "notify_function_name": "OnRep_Score",
            "condition": "owner_only",
        }
    )
    with pytest.raises(ValidationError):
        validator.validate({**common, "mode": "rep_notify"})
    with pytest.raises(ValidationError):
        validator.validate(
            {**common, "mode": "none", "notify_function_name": "OnRep_Score"}
        )
    with pytest.raises(ValidationError):
        validator.validate({**common, "mode": "none", "condition": "owner_only"})
    with pytest.raises(ValidationError):
        validator.validate(
            {**common, "mode": "replicated", "notify_function_name": "OnRep_Score"}
        )
    with pytest.raises(ValidationError):
        validator.validate({**common, "mode": "replicated", "condition": "dynamic"})


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
        "asset_path, variable_id, mode, notify_function_name='', condition='none'"
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
            "search_blueprint_node_actions",
            "describe_blueprint_node_action",
            "add_blueprint_action_node",
            "suggest_blueprint_nodes_for_pin",
            "suggest_blueprint_nodes_for_connection",
            "add_blueprint_connected_action_node",
            "insert_blueprint_action_node",
            "preview_blueprint_action_replacement",
            "replace_blueprint_node_with_action",
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


def test_live_blueprint2_workflow_uses_safe_graph_and_diff_contracts():
    e2e_path = Path(__file__).with_name("test_e2e.py")
    tree = ast.parse(e2e_path.read_text(encoding="utf-8"))
    workflow = next(
        node
        for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name == "test_blueprint2_workflow_round_trip"
    )
    source = ast.unparse(workflow)

    assert "'type': 'Event'" not in source
    assert "get_blueprint_graph_info" in source
    assert "['graph_id']" in source
    assert "startswith('graph:')" in source
    assert "['id_kind'] == 'graph_guid'" in source
    assert "['stable'] is True" in source
    assert "['graph_id'] == graph_id" in source
    assert "['graph_id_kind'] == 'graph_guid'" in source
    assert "['graph_stable'] is True" in source
    assert source.index("get('saved') is False") < source.index("'save_asset'")
    assert source.index("'save_asset'") < source.index("action='plan'")
    for value in (
        "Branch",
        "Sequence",
        "CallFunction",
        "Actor",
        "K2_GetActorLocation",
        "source_node': 'branch",
        "target_node': 'sequence",
        "total_count",
        "delete_asset",
    ):
        assert value in source
    assert "execute_python" not in source


def test_live_blueprint_palette_round_trip_uses_the_public_workflow():
    e2e_path = Path(__file__).with_name("test_e2e.py")
    tree = ast.parse(e2e_path.read_text(encoding="utf-8"))
    workflow = next(
        node
        for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name == "test_blueprint_palette_round_trip"
    )
    source = ast.unparse(workflow)

    for action in (
        "create_blueprint",
        "inspect_blueprint",
        "search_blueprint_node_actions",
        "describe_blueprint_node_action",
        "snapshot_blueprint_graph",
        "add_blueprint_action_node",
        "suggest_blueprint_nodes_for_pin",
        "compile_blueprint",
        "get_blueprint_health",
        "save_asset",
        "diff_blueprint_graphs",
        "delete_asset",
    ):
        assert action in source
    for workflow_action in ("plan", "apply", "undo"):
        assert f"action='{workflow_action}'" in source
    assert "_assert_not_connection_error" in source
    assert "get('saved') is False" in source
    assert source.count("get('saved') is not True") >= 2
    assert "execute_python" not in source

    transport_calls = [
        node
        for node in ast.walk(workflow)
        if isinstance(node, ast.Call)
        and isinstance(node.func, ast.Attribute)
        and node.func.attr in {"_dispatch", "workflow"}
    ]
    checked_calls = [
        node
        for node in ast.walk(workflow)
        if isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
        and node.func.id == "checked"
    ]
    assert transport_calls
    assert all(
        any(
            transport is nested
            for checked_call in checked_calls
            for nested in ast.walk(checked_call)
        )
        for transport in transport_calls
    )

    save_lines = [
        node.lineno
        for node in transport_calls
        if any(
            isinstance(child, ast.Constant) and child.value == "save_asset"
            for child in ast.walk(node)
        )
    ]
    plan_lines = [
        node.lineno
        for node in transport_calls
        if node.func.attr == "workflow"
        and any(
            keyword.arg == "action"
            and isinstance(keyword.value, ast.Constant)
            and keyword.value.value == "plan"
            for keyword in node.keywords
        )
    ]
    assert save_lines and plan_lines and min(save_lines) < min(plan_lines)

    finally_bodies = [
        ast.unparse(ast.Module(body=node.finalbody, type_ignores=[]))
        for node in ast.walk(workflow)
        if isinstance(node, ast.Try) and node.finalbody
    ]
    assert any("delete_asset" in body for body in finally_bodies)

    assigned_names = {
        node.id
        for node in ast.walk(workflow)
        if isinstance(node, ast.Name) and isinstance(node.ctx, ast.Store)
    }
    assert {
        "seen_action_ids",
        "followed_cursor",
        "applied_snapshot",
        "applied_diff",
    } <= assigned_names
    total_count_comparisons = [
        node
        for node in ast.walk(workflow)
        if isinstance(node, ast.Compare)
        and isinstance(node.left, ast.Subscript)
        and isinstance(node.left.slice, ast.Constant)
        and node.left.slice.value == "total_count"
        and len(node.ops) == 1
        and len(node.comparators) == 1
        and isinstance(node.comparators[0], ast.Constant)
        and node.comparators[0].value == 0
    ]
    assert any(
        isinstance(comparison.ops[0], ast.Gt)
        for comparison in total_count_comparisons
    )
    assert any(
        isinstance(comparison.ops[0], ast.Eq)
        for comparison in total_count_comparisons
    )


def test_live_e2e_accounts_for_every_json_action():
    e2e_path = Path(__file__).with_name("test_e2e.py")
    tree = ast.parse(e2e_path.read_text(encoding="utf-8"))
    exclude_node = next(
        node for node in tree.body
        if isinstance(node, ast.Assign)
        and any(
            isinstance(target, ast.Name) and target.id == "_EXCLUDE"
            for target in node.targets
        )
    )
    excluded = ast.literal_eval(exclude_node.value)

    catalog = build()
    all_pairs = {
        (domain, action)
        for domain, actions in catalog.items()
        for action in actions
    }
    test_names = {
        node.name for node in tree.body if isinstance(node, ast.FunctionDef)
    }
    spec = importlib.util.spec_from_file_location(
        "_blueprint2_e2e_accounting", e2e_path
    )
    e2e = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(e2e)
    sweep_pairs = e2e._all_action_pairs()
    sweep_pair_set = set(sweep_pairs)

    assert excluded == {("vision", "capture_viewport")}
    assert len(all_pairs) == 299
    assert len(sweep_pairs) == len(sweep_pair_set)
    assert sweep_pair_set.isdisjoint(excluded)
    assert sweep_pair_set | excluded == all_pairs
    assert len(sweep_pair_set) == 298
    assert "test_vision_capture_returns_image" in test_names
