"""Strict Action Registry contracts for the universal Blueprint 2 surface."""

from copy import deepcopy
from typing import Any


ASSET_PATH = {"type": "string", "format": "unreal-asset-path", "minLength": 1}
LOWER_GUID = (
    r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-"
    r"[0-9a-f]{4}-[0-9a-f]{12}"
)
ZERO_GUID = r"00000000-0000-0000-0000-000000000000"
NON_ZERO_LOWER_GUID = rf"(?!{ZERO_GUID}$){LOWER_GUID}"


def _guid_id(kind: str, *, allow_fallback: bool = False) -> dict:
    persisted = rf"{kind}:{NON_ZERO_LOWER_GUID}"
    if allow_fallback:
        return {
            "type": "string",
            "pattern": rf"^(?:{persisted}|fallback:{kind}:[0-9a-f]{{40}})$",
        }
    return {"type": "string", "pattern": rf"^{persisted}$"}


GRAPH_ID = _guid_id("graph")
NODE_ID = _guid_id("node")
PIN_ID = _guid_id("pin")
VARIABLE_ID = _guid_id("variable")
COMPONENT_ID = _guid_id("component")
FALLBACK_GRAPH_ID = _guid_id("graph", allow_fallback=True)
FALLBACK_NODE_ID = _guid_id("node", allow_fallback=True)
FALLBACK_VARIABLE_ID = _guid_id("variable", allow_fallback=True)
INTERFACE_ID = {
    "type": "string",
    "pattern": r"^interface:/[A-Za-z][A-Za-z0-9_]*/[^\s:]+\.[^\s/:.]+$",
}
STABLE_ID = {
    "type": "string",
    "pattern": (
        rf"^(?:(?:graph|node|pin|variable|component):{NON_ZERO_LOWER_GUID}"
        r"|interface:/[A-Za-z][A-Za-z0-9_]*/[^\s:]+\.[^\s/:.]+"
        r"|fallback:(?:graph|node|pin|variable|component):[0-9a-f]{40})$"
    ),
}
NAME = {
    "type": "string",
    "pattern": r"^[A-Za-z_][A-Za-z0-9_]*$",
    "maxLength": 100,
}
REPLICATION_CONDITION = {
    "type": "string",
    "enum": [
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
    ],
}


def _object(properties: dict[str, dict], required: tuple[str, ...] = ()) -> dict:
    schema: dict[str, Any] = {
        "type": "object",
        "properties": deepcopy(properties),
        "additionalProperties": False,
    }
    if required:
        schema["required"] = list(required)
    return schema


def _array(items: dict, **keywords: Any) -> dict:
    return {"type": "array", "items": deepcopy(items), **deepcopy(keywords)}


def _type_choices(
    scalar: dict | None = None,
    *,
    reference_path: dict | None = None,
) -> list[dict]:
    path_schema = reference_path or {
        "type": "string",
        "pattern": r"^/Script/",
    }
    choices = [
        _object({"kind": {"const": kind}}, ("kind",))
        for kind in ("bool", "byte", "int", "int64", "string", "name", "text")
    ]
    choices.append(
        _object(
            {
                "kind": {"const": "real"},
                "precision": {"type": "string", "enum": ["float", "double"]},
            },
            ("kind", "precision"),
        )
    )
    for kind in ("enum", "struct"):
        choices.append(
            _object(
                {
                    "kind": {"const": kind},
                    "type_path": path_schema,
                },
                ("kind", "type_path"),
            )
        )
    for kind in ("object", "class", "interface", "soft_object", "soft_class"):
        choices.append(
            _object(
                {
                    "kind": {"const": kind},
                    "class_path": path_schema,
                },
                ("kind", "class_path"),
            )
        )
    if scalar is not None:
        choices.append(
            _object(
                {"kind": {"const": "array"}, "item": scalar},
                ("kind", "item"),
            )
        )
        choices.append(
            _object(
                {"kind": {"const": "set"}, "item": scalar},
                ("kind", "item"),
            )
        )
        choices.append(
            _object(
                {
                    "kind": {"const": "map"},
                    "key": scalar,
                    "value": scalar,
                },
                ("kind", "key", "value"),
            )
        )
    return choices


def type_spec() -> dict:
    """Return canonical Blueprint types without implicit nested containers."""
    resource_id = "urn:unreal-mcp:blueprint2:type-spec"
    scalar = {"$ref": f"{resource_id}#/$defs/scalar"}
    choices = _type_choices(scalar)
    # Keep the public primitive exactly {"oneOf": choices}. The first choice is
    # an embedded schema resource so references remain valid after TYPE_SPEC is
    # deep-copied into a larger action input schema.
    choices[0]["$id"] = resource_id
    choices[0]["$defs"] = {"scalar": {"oneOf": _type_choices()}}
    return {"oneOf": choices}


TYPE_SPEC = type_spec()


def snapshot_type_spec() -> dict:
    """Return serialized pin types, including types from mounted assets."""
    resource_id = "urn:unreal-mcp:blueprint2:snapshot-type-spec"
    reference_path = {
        "type": "string",
        "pattern": r"^/[A-Za-z][A-Za-z0-9_]*/[^\s:]+\.[^\s/:.]+$",
        "minLength": 5,
    }
    scalar = {"$ref": f"{resource_id}#/$defs/scalar"}
    choices = _type_choices(scalar, reference_path=reference_path)
    choices[0]["$id"] = resource_id
    choices[0]["$defs"] = {
        "scalar": {
            "oneOf": _type_choices(reference_path=reference_path),
        }
    }
    choices.extend(
        [
            _object({"kind": {"const": "exec"}}, ("kind",)),
            _object({"kind": {"const": "unknown"}}, ("kind",)),
        ]
    )
    return {"oneOf": choices}


PARAMETER = _object(
    {"name": NAME, "type": TYPE_SPEC, "default": {}},
    ("name", "type"),
)


FULL_UNREAL_PATH = {
    "type": "string",
    "pattern": r"^/Script/",
    "minLength": 9,
}
SNAPSHOT_PIN_TYPE = snapshot_type_spec()
SNAPSHOT_POSITION = _object(
    {
        "x": {
            "type": "integer",
            "minimum": -2_147_483_648,
            "maximum": 2_147_483_647,
        },
        "y": {
            "type": "integer",
            "minimum": -2_147_483_648,
            "maximum": 2_147_483_647,
        },
    },
    ("x", "y"),
)
SNAPSHOT_PIN = _object(
    {
        "id": PIN_ID,
        "name": {"type": "string"},
        "direction": {"type": "string", "enum": ["input", "output"]},
        "type": SNAPSHOT_PIN_TYPE,
        "default": {},
    },
    ("id", "name", "direction", "type", "default"),
)
SNAPSHOT_NODE = _object(
    {
        "id": NODE_ID,
        "class_path": FULL_UNREAL_PATH,
        "position": SNAPSHOT_POSITION,
        "comment": {"type": "string"},
        "properties": {"type": "object"},
        "pins": _array(SNAPSHOT_PIN),
    },
    ("id", "class_path", "position", "comment", "properties", "pins"),
)
SNAPSHOT_CONNECTION = _object(
    {
        "source_pin_id": PIN_ID,
        "target_pin_id": PIN_ID,
    },
    ("source_pin_id", "target_pin_id"),
)
SNAPSHOT_GRAPH = _object(
    {
        "id": GRAPH_ID,
        "name": {"type": "string"},
        "schema_path": FULL_UNREAL_PATH,
        "nodes": _array(SNAPSHOT_NODE),
        "connections": _array(SNAPSHOT_CONNECTION),
    },
    ("id", "name", "schema_path", "nodes", "connections"),
)
BLUEPRINT_GRAPH_SNAPSHOT = _object(
    {
        "snapshot_version": {"const": 1},
        "asset_path": ASSET_PATH,
        "blueprint_class": FULL_UNREAL_PATH,
        "graphs": _array(SNAPSHOT_GRAPH),
        "digest": {
            "type": "string",
            "pattern": r"^sha1:[0-9a-f]{40}$",
        },
    },
    ("snapshot_version", "asset_path", "blueprint_class", "graphs", "digest"),
)
EMPTY_BLUEPRINT_GRAPH_SNAPSHOT_EXAMPLE = {
    "snapshot_version": 1,
    "asset_path": "/Game/BP_Player.BP_Player",
    "blueprint_class": "/Script/Engine.Blueprint",
    "graphs": [],
    "digest": "sha1:2537c5fff3f344fa5f11db5ac3b896bcc4d35c1e",
}
INTERFACE_CLASS_PATH = {
    "type": "string",
    "pattern": r"^/[A-Za-z][A-Za-z0-9_]*/[^\s:]+\.[^\s/:.]+$",
    "minLength": 5,
}
VECTOR = _array(
    {
        "type": "number",
        "minimum": -1_000_000_000,
        "maximum": 1_000_000_000,
    },
    minItems=3,
    maxItems=3,
)
TRANSFORM = _object(
    {"location": VECTOR, "rotation": VECTOR, "scale": VECTOR},
)
TRANSFORM["minProperties"] = 1

CHANGE_RECORD = _object(
    {
        "step_id": {"type": "string", "minLength": 1},
        "kind": {"type": "string", "enum": ["create", "update", "delete"]},
        "target_id": STABLE_ID,
        "details": {"type": "object"},
    },
    ("kind", "target_id", "details"),
)
WARNING_RECORD = _object(
    {
        "code": {"type": "string", "minLength": 1},
        "message": {"type": "string", "minLength": 1},
        "path": {"type": ["string", "null"]},
        "details": {"type": "object"},
    },
    ("code", "message", "details"),
)
ERROR_RECORD = _object(
    {
        "code": {
            "type": "string",
            "enum": [
                "INVALID_INPUT",
                "UNKNOWN_ACTION",
                "CONFLICT",
                "PRECONDITION_FAILED",
                "CONFIRMATION_REQUIRED",
                "CONFIRMATION_EXPIRED",
                "PLUGIN_REQUIRED",
                "UE_VERSION_UNSUPPORTED",
                "UE_UNAVAILABLE",
                "TIMEOUT",
                "COMPILE_FAILED",
                "VERIFICATION_FAILED",
                "TRANSACTION_FAILED",
                "ROLLBACK_FAILED",
                "INTERNAL_ERROR",
            ],
        },
        "path": {"type": ["string", "null"]},
        "message": {"type": "string", "minLength": 1},
        "retryable": {"type": "boolean"},
        "hint": {"type": "string", "minLength": 1},
        "details": {"type": "object"},
        "trace_id": {"type": "string", "minLength": 1},
    },
    ("code", "path", "message", "retryable", "hint", "details", "trace_id"),
)
NEXT_ACTION = _object(
    {
        "domain": {"type": "string", "minLength": 1},
        "action": {"type": "string", "minLength": 1},
        "params": {"type": "object"},
    },
    ("domain", "action", "params"),
)

OUTPUT_SCHEMA = {
    "type": "object",
    "properties": {
        "success": {"type": "boolean"},
        "status": {"type": "string", "enum": ["succeeded", "failed", "partial"]},
        "summary": {"type": "string"},
        "data": {"type": "object"},
        "changes": _array(CHANGE_RECORD),
        "warnings": _array(WARNING_RECORD),
        "errors": _array(ERROR_RECORD),
        "next_actions": _array(NEXT_ACTION),
        "trace_id": {"type": "string"},
    },
    "required": [
        "success",
        "status",
        "summary",
        "data",
        "changes",
        "warnings",
        "errors",
        "next_actions",
        "trace_id",
    ],
    "additionalProperties": False,
}

CAPABILITIES_DATA = _object(
    {
        "api_version": {"type": "integer", "const": 2},
        "engine_version": {"type": "string", "minLength": 1},
        "scalar_kinds": _array({"type": "string"}),
        "container_kinds": _array({"type": "string"}),
        "node_families": _array({"type": "string"}),
        "supports_k2_schema": {"type": "boolean"},
        "supports_scs_operations": {"type": "boolean"},
        "supports_compiler_tokens": {"type": "boolean"},
        "has_k2_graphs": {"type": "boolean"},
        "all_graphs_k2_schema": {"type": "boolean"},
        "k2_schema": {"type": "boolean"},
        "compiler_tokens": {"type": "boolean"},
        "has_scs": {"type": "boolean"},
        "scs_operations": {"type": "boolean"},
        "supports_blueprint_node_palette": {"type": "boolean"},
        "has_palette_compatible_graphs": {"type": "boolean"},
    },
    (
        "api_version",
        "engine_version",
        "scalar_kinds",
        "container_kinds",
        "node_families",
        "supports_k2_schema",
        "supports_scs_operations",
        "supports_compiler_tokens",
        "has_k2_graphs",
        "all_graphs_k2_schema",
        "k2_schema",
        "compiler_tokens",
        "has_scs",
        "scs_operations",
        "supports_blueprint_node_palette",
        "has_palette_compatible_graphs",
    ),
)
BLUEPRINT_BRIEF_COUNTS = _object(
    {
        name: {"type": "integer", "minimum": 0}
        for name in (
            "variables",
            "components",
            "functions",
            "macros",
            "events",
            "dispatchers",
            "interfaces",
            "graphs",
            "nodes",
        )
    },
    (
        "variables",
        "components",
        "functions",
        "macros",
        "events",
        "dispatchers",
        "interfaces",
        "graphs",
        "nodes",
    ),
)
BLUEPRINT_BRIEF_DATA = _object(
    {
        "asset_path": {"type": "string", "minLength": 1},
        "blueprint_class_path": {"type": "string", "minLength": 1},
        "parent_class_path": {"type": "string"},
        "generated_class_path": {"type": "string"},
        "skeleton_class_path": {"type": "string"},
        "compile_status": {
            "type": "string",
            "enum": [
                "Unknown",
                "Dirty",
                "Error",
                "UpToDate",
                "BeingCreated",
                "UpToDateWithWarnings",
            ],
        },
        "interfaces": _array({"type": "string", "minLength": 1}),
        "top_level_components": _array({"type": "string", "minLength": 1}),
        "graphs": _array({"type": "string", "minLength": 1}),
        "counts": BLUEPRINT_BRIEF_COUNTS,
        "capabilities": CAPABILITIES_DATA,
    },
    (
        "asset_path",
        "blueprint_class_path",
        "parent_class_path",
        "generated_class_path",
        "skeleton_class_path",
        "compile_status",
        "interfaces",
        "top_level_components",
        "graphs",
        "counts",
        "capabilities",
    ),
)


def _output_with_success_data(data_schema: dict) -> dict:
    schema = deepcopy(OUTPUT_SCHEMA)
    schema["allOf"] = [
        {
            "if": {
                "properties": {"success": {"const": True}},
                "required": ["success"],
            },
            "then": {"properties": {"data": deepcopy(data_schema)}},
        }
    ]
    return schema


def _unsupported_error(action: str) -> dict:
    return {
        "success": False,
        "status": "failed",
        "summary": "Blueprint 2 C++ core is unavailable.",
        "data": {},
        "changes": [],
        "warnings": [],
        "errors": [
            {
                "code": "UE_VERSION_UNSUPPORTED",
                "path": None,
                "message": "This action requires the Blueprint 2 C++ core.",
                "retryable": False,
                "hint": "Install a plugin build that provides blueprint2_cpp_core.",
                "details": {
                    "capability": "blueprint2_cpp_core",
                    "action": action,
                },
                "trace_id": "blueprint2-contract-example",
            }
        ],
        "next_actions": [],
        "trace_id": "blueprint2-contract-example",
    }


def _spec(
    action: str,
    description: str,
    input_schema: dict,
    example_params: dict,
    *,
    effect: str,
    risk: str,
    idempotent: bool,
    supports_preview: bool,
    supports_undo: bool,
    requires_confirmation: bool,
    success_data_schema: dict | None = None,
) -> dict:
    return {
        "title": action.replace("_", " ").title(),
        "description": description,
        "result_kind": "json",
        "input_schema": deepcopy(input_schema),
        "output_schema": (
            _output_with_success_data(success_data_schema)
            if success_data_schema is not None
            else deepcopy(OUTPUT_SCHEMA)
        ),
        "effect": effect,
        "risk": risk,
        "idempotent": idempotent,
        "supports_preview": supports_preview,
        "supports_undo": supports_undo,
        "requires_confirmation": requires_confirmation,
        "ue_versions": ["5.6", "5.7", "5.8"],
        "required_plugins": [],
        "examples": [{"action": action, "params": deepcopy(example_params)}],
        "error_examples": [_unsupported_error(action)],
    }


def _read(
    action: str,
    description: str,
    input_schema: dict,
    example_params: dict,
    *,
    success_data_schema: dict | None = None,
) -> dict:
    return _spec(
        action,
        description,
        input_schema,
        example_params,
        effect="read",
        risk="low",
        idempotent=True,
        supports_preview=False,
        supports_undo=False,
        requires_confirmation=False,
        success_data_schema=success_data_schema,
    )


def _write(
    action: str,
    description: str,
    input_schema: dict,
    example_params: dict,
    *,
    idempotent: bool,
    success_data_schema: dict | None = None,
) -> dict:
    return _spec(
        action,
        description,
        input_schema,
        example_params,
        effect="write",
        risk="medium",
        idempotent=idempotent,
        supports_preview=True,
        supports_undo=True,
        requires_confirmation=True,
        success_data_schema=success_data_schema,
    )


def _destructive(
    action: str,
    description: str,
    input_schema: dict,
    example_params: dict,
    *,
    success_data_schema: dict | None = None,
) -> dict:
    return _spec(
        action,
        description,
        input_schema,
        example_params,
        effect="destructive",
        risk="high",
        idempotent=False,
        supports_preview=True,
        supports_undo=True,
        requires_confirmation=True,
        success_data_schema=success_data_schema,
    )


PARAMETERS = _array(PARAMETER, maxItems=128)
PARAMETERS_WITH_DEFAULT = {**deepcopy(PARAMETERS), "default": []}
ACCESS = {
    "type": "string",
    "enum": ["public", "protected", "private"],
    "default": "public",
}
POSITION = _object(
    {
        "x": {"type": "number", "minimum": -1_000_000_000, "maximum": 1_000_000_000},
        "y": {"type": "number", "minimum": -1_000_000_000, "maximum": 1_000_000_000},
    },
    ("x", "y"),
)

ACTION_ID = {"type": "string", "pattern": r"^action:[0-9a-f]{40}$"}
BINDING_ID = {"type": "string", "pattern": r"^binding:[0-9a-f]{40}$"}
SHA1_DIGEST = {"type": "string", "pattern": r"^sha1:[0-9a-f]{40}$"}
PALETTE_CURSOR = {
    "type": "string",
    "maxLength": 4096,
    "default": "",
}
PALETTE_LIMIT = {
    "type": "integer",
    "minimum": 1,
    "maximum": 200,
    "default": 50,
}
PALETTE_QUERY = {"type": "string", "maxLength": 256, "default": ""}
PALETTE_ACTION_KINDS = [
    "function",
    "event",
    "variable",
    "macro",
    "delegate",
    "cast",
    "async",
    "flow_control",
    "operator",
    "struct",
    "other",
]
PALETTE_FILTERS = _object(
    {
        "action_kinds": _array(
            {"type": "string", "enum": PALETTE_ACTION_KINDS},
            maxItems=11,
            uniqueItems=True,
        ),
        "categories": _array(
            {"type": "string", "minLength": 1, "maxLength": 256},
            maxItems=32,
            uniqueItems=True,
        ),
        "owner_paths": _array(
            {"type": "string", "minLength": 1, "maxLength": 1024},
            maxItems=32,
            uniqueItems=True,
        ),
        "pure_only": {"type": "boolean", "default": False},
    }
)
PALETTE_FILTERS_WITH_DEFAULT = deepcopy(PALETTE_FILTERS)
PALETTE_FILTERS_WITH_DEFAULT["default"] = {}

PALETTE_BINDING = _object(
    {
        "binding_id": BINDING_ID,
        "object_path": {"type": "string", "minLength": 1},
        "class_path": {"type": "string", "minLength": 1},
    },
    ("binding_id", "object_path", "class_path"),
)
PALETTE_ACTION_CARD_REQUIRED = (
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
)
PALETTE_ACTION_CARD_PROPERTIES = {
    "action_id": ACTION_ID,
    "title": {"type": "string", "minLength": 1},
    "category": {"type": "string"},
    "keywords": _array({"type": "string", "minLength": 1}, maxItems=128),
    "action_kind": {"type": "string", "enum": PALETTE_ACTION_KINDS},
    "node_class_path": {"type": "string", "minLength": 1},
    "owner_path": {"type": "string"},
    "member_path": {"type": "string"},
    "pure": {"type": ["boolean", "null"]},
    "compatible": {"type": "boolean", "const": True},
    "compatibility_summary": {"type": "string", "minLength": 1},
    "requires_binding": {"type": "boolean"},
    "bindings": _array(PALETTE_BINDING, maxItems=32),
}
PALETTE_ACTION_CARD = _object(
    PALETTE_ACTION_CARD_PROPERTIES,
    PALETTE_ACTION_CARD_REQUIRED,
)
PALETTE_PAGE_DATA = _object(
    {
        "asset_path": ASSET_PATH,
        "graph_id": GRAPH_ID,
        "source_pin_id": {"anyOf": [deepcopy(PIN_ID), {"type": "null"}]},
        "items": _array(PALETTE_ACTION_CARD, maxItems=200),
        "total_count": {"type": "integer", "minimum": 0},
        "returned_count": {
            "type": "integer",
            "minimum": 0,
            "maximum": 200,
        },
        "next_cursor": {"type": "string", "maxLength": 4096},
        "result_digest": SHA1_DIGEST,
    },
    (
        "asset_path",
        "graph_id",
        "source_pin_id",
        "items",
        "total_count",
        "returned_count",
        "next_cursor",
        "result_digest",
    ),
)
PALETTE_PIN_PREVIEW = _object(
    {
        "name": {"type": "string"},
        "direction": {"type": "string", "enum": ["input", "output"]},
        "type": SNAPSHOT_PIN_TYPE,
        "default": {},
    },
    ("name", "direction", "type", "default"),
)
PALETTE_DESCRIBE_DATA = _object(
    {
        "asset_path": ASSET_PATH,
        "graph_id": GRAPH_ID,
        "source_pin_id": {"anyOf": [deepcopy(PIN_ID), {"type": "null"}]},
        **deepcopy(PALETTE_ACTION_CARD_PROPERTIES),
        "tooltip": {"type": "string"},
        "documentation_link": {"type": "string"},
        "documentation_excerpt": {"type": "string"},
        "restrictions": _array({"type": "string", "minLength": 1}),
        "pin_preview_available": {"type": "boolean"},
        "template_pins": _array(PALETTE_PIN_PREVIEW, maxItems=256),
    },
    (
        "asset_path",
        "graph_id",
        "source_pin_id",
        *PALETTE_ACTION_CARD_REQUIRED,
        "tooltip",
        "documentation_link",
        "documentation_excerpt",
        "restrictions",
        "pin_preview_available",
        "template_pins",
    ),
)
PALETTE_SPAWN_DATA = _object(
    {
        "asset_path": ASSET_PATH,
        "graph_id": GRAPH_ID,
        "action_id": ACTION_ID,
        "node_id": NODE_ID,
        "class_path": {"type": "string", "minLength": 1},
        "position": POSITION,
        "pin_ids": _array(PIN_ID, maxItems=256, uniqueItems=True),
        "pins": _array(SNAPSHOT_PIN, maxItems=256),
        "auxiliary_node_ids": _array(NODE_ID, maxItems=256, uniqueItems=True),
    },
    (
        "asset_path",
        "graph_id",
        "action_id",
        "node_id",
        "class_path",
        "position",
        "pin_ids",
        "pins",
        "auxiliary_node_ids",
    ),
)

REPLACEMENT_PLAN_ID = {
    "type": "string",
    "pattern": r"^replacement-plan:[0-9a-f]{40}$",
}
SEMANTIC_PALETTE_CURSOR = {
    "type": "string",
    "pattern": r"^(?:|palette-cursor:[0-9a-f]{40})$",
    "default": "",
}
CONNECTION_RESPONSE_KIND = {
    "type": "string",
    "enum": [
        "direct",
        "break_planned_source_link",
        "break_planned_target_link",
        "break_planned_both_links",
        "conversion_node",
        "promotion",
    ],
}
CONNECTION_RESPONSE = _object(
    {
        "kind": CONNECTION_RESPONSE_KIND,
        "message": {"type": "string"},
        "requires_conversion": {"type": "boolean"},
    },
    ("kind", "message", "requires_conversion"),
)
SEMANTIC_EDGE = _object(
    {
        "source_pin_id": PIN_ID,
        "target_pin_id": PIN_ID,
        "response": CONNECTION_RESPONSE,
        "auxiliary_node_ids": _array(
            NODE_ID,
            maxItems=32,
            uniqueItems=True,
        ),
    },
    (
        "source_pin_id",
        "target_pin_id",
        "response",
        "auxiliary_node_ids",
    ),
)
CONNECTION_BINDING = _object(
    {
        "binding_id": BINDING_ID,
        "pin_name": {"type": "string"},
        "direction": {"type": "string", "enum": ["input", "output"]},
        "type": SNAPSHOT_PIN_TYPE,
        "response": CONNECTION_RESPONSE,
        "rank": {"type": "integer", "minimum": 0},
    },
    ("binding_id", "pin_name", "direction", "type", "response", "rank"),
)
CONNECTION_BINDING_PAIR = _object(
    {
        "input_binding_id": BINDING_ID,
        "output_binding_id": BINDING_ID,
        "input_pin_name": {"type": "string"},
        "output_pin_name": {"type": "string"},
        "input_type": SNAPSHOT_PIN_TYPE,
        "output_type": SNAPSHOT_PIN_TYPE,
        "source_response": CONNECTION_RESPONSE,
        "target_response": CONNECTION_RESPONSE,
        "requires_conversion": {"type": "boolean"},
        "rank": {"type": "integer", "minimum": 0},
    },
    (
        "input_binding_id",
        "output_binding_id",
        "input_pin_name",
        "output_pin_name",
        "input_type",
        "output_type",
        "source_response",
        "target_response",
        "requires_conversion",
        "rank",
    ),
)
PALETTE_PIN_SUGGESTION_CARD = _object(
    {
        **deepcopy(PALETTE_ACTION_CARD_PROPERTIES),
        "connection_bindings": _array(
            CONNECTION_BINDING,
            minItems=1,
            maxItems=256,
        ),
    },
    (*PALETTE_ACTION_CARD_REQUIRED, "connection_bindings"),
)
PALETTE_PIN_PAGE_DATA = _object(
    {
        "asset_path": ASSET_PATH,
        "graph_id": GRAPH_ID,
        "source_pin_id": PIN_ID,
        "items": _array(PALETTE_PIN_SUGGESTION_CARD, maxItems=200),
        "total_count": {"type": "integer", "minimum": 0},
        "returned_count": {
            "type": "integer",
            "minimum": 0,
            "maximum": 200,
        },
        "next_cursor": {"type": "string", "maxLength": 4096},
        "result_digest": SHA1_DIGEST,
    },
    (
        "asset_path",
        "graph_id",
        "source_pin_id",
        "items",
        "total_count",
        "returned_count",
        "next_cursor",
        "result_digest",
    ),
)
CONNECTION_ACTION_CARD = _object(
    {
        **deepcopy(PALETTE_ACTION_CARD_PROPERTIES),
        "binding_pairs": _array(
            CONNECTION_BINDING_PAIR,
            minItems=1,
            maxItems=65_536,
        ),
    },
    (*PALETTE_ACTION_CARD_REQUIRED, "binding_pairs"),
)
CONNECTION_PAGE_DATA = _object(
    {
        "asset_path": ASSET_PATH,
        "graph_id": GRAPH_ID,
        "source_pin_id": PIN_ID,
        "target_pin_id": PIN_ID,
        "allow_conversion": {"type": "boolean"},
        "items": _array(CONNECTION_ACTION_CARD, maxItems=200),
        "total_count": {"type": "integer", "minimum": 0},
        "returned_count": {
            "type": "integer",
            "minimum": 0,
            "maximum": 200,
        },
        "next_cursor": SEMANTIC_PALETTE_CURSOR,
        "result_digest": SHA1_DIGEST,
    },
    (
        "asset_path",
        "graph_id",
        "source_pin_id",
        "target_pin_id",
        "allow_conversion",
        "items",
        "total_count",
        "returned_count",
        "next_cursor",
        "result_digest",
    ),
)
CONNECTED_SPAWN_DATA = _object(
    {
        **deepcopy(PALETTE_SPAWN_DATA["properties"]),
        "source_pin_id": PIN_ID,
        "connection_binding_id": BINDING_ID,
        "connections": _array(
            SEMANTIC_EDGE,
            minItems=1,
            maxItems=512,
        ),
        "transaction_recorded": {"const": True},
        "saved": {"const": False},
    },
    (
        *PALETTE_SPAWN_DATA["required"],
        "source_pin_id",
        "connection_binding_id",
        "connections",
        "transaction_recorded",
        "saved",
    ),
)
REPLACED_CONNECTION = _object(
    {
        "source_pin_id": PIN_ID,
        "target_pin_id": PIN_ID,
    },
    ("source_pin_id", "target_pin_id"),
)
INSERT_DATA = _object(
    {
        **deepcopy(PALETTE_SPAWN_DATA["properties"]),
        "input_binding_id": BINDING_ID,
        "output_binding_id": BINDING_ID,
        "replaced_connection": REPLACED_CONNECTION,
        "connections": _array(
            SEMANTIC_EDGE,
            minItems=2,
            maxItems=512,
        ),
        "transaction_recorded": {"const": True},
        "saved": {"const": False},
    },
    (
        *PALETTE_SPAWN_DATA["required"],
        "input_binding_id",
        "output_binding_id",
        "replaced_connection",
        "connections",
        "transaction_recorded",
        "saved",
    ),
)
PIN_MAPPING_INPUT = _object(
    {
        "old_pin_id": PIN_ID,
        "new_binding_id": BINDING_ID,
    },
    ("old_pin_id", "new_binding_id"),
)
REPLACEMENT_MAPPING = _object(
    {
        "old_pin_id": PIN_ID,
        "new_binding_id": BINDING_ID,
        "new_pin_name": {"type": "string"},
        "origin": {"type": "string", "enum": ["explicit", "inferred"]},
        "reason": {"type": "string", "minLength": 1},
    },
    (
        "old_pin_id",
        "new_binding_id",
        "new_pin_name",
        "origin",
        "reason",
    ),
)
RETAINED_CONNECTION = _object(
    {
        "old_pin_id": PIN_ID,
        "new_binding_id": BINDING_ID,
        "linked_pin_id": PIN_ID,
        "response": CONNECTION_RESPONSE,
    },
    ("old_pin_id", "new_binding_id", "linked_pin_id", "response"),
)
RETAINED_DEFAULT = _object(
    {
        "old_pin_id": PIN_ID,
        "new_binding_id": BINDING_ID,
        "value": {},
    },
    ("old_pin_id", "new_binding_id", "value"),
)
LOST_CONNECTION = _object(
    {
        "old_pin_id": PIN_ID,
        "linked_pin_id": PIN_ID,
        "reason": {"type": "string", "minLength": 1},
    },
    ("old_pin_id", "linked_pin_id", "reason"),
)
LOST_DEFAULT = _object(
    {
        "old_pin_id": PIN_ID,
        "value": {},
        "reason": {"type": "string", "minLength": 1},
    },
    ("old_pin_id", "value", "reason"),
)
UNSUPPORTED_METADATA = _object(
    {
        "field": {"type": "string", "minLength": 1},
        "reason": {"type": "string", "minLength": 1},
    },
    ("field", "reason"),
)
SELECTED_ACTION_SUMMARY = _object(
    {
        "action_id": ACTION_ID,
        "title": {"type": "string", "minLength": 1},
        "node_class_path": {"type": "string", "minLength": 1},
        "owner_path": {"type": "string"},
        "member_path": {"type": "string"},
    },
    ("action_id", "title", "node_class_path", "owner_path", "member_path"),
)
TARGET_NODE_SUMMARY = _object(
    {
        "node_id": NODE_ID,
        "class_path": {"type": "string", "minLength": 1},
        "title": {"type": "string", "minLength": 1},
        "position": POSITION,
        "comment": {"type": "string"},
        "comment_bubble_visible": {"type": "boolean"},
        "enabled_state": {
            "type": "string",
            "enum": ["enabled", "disabled", "development_only"],
        },
    },
    (
        "node_id",
        "class_path",
        "title",
        "position",
        "comment",
        "comment_bubble_visible",
        "enabled_state",
    ),
)
REPLACEMENT_PREVIEW_DATA = _object(
    {
        "replacement_plan_id": REPLACEMENT_PLAN_ID,
        "asset_path": ASSET_PATH,
        "graph_id": GRAPH_ID,
        "node_snapshot_digest": SHA1_DIGEST,
        "action_result_digest": SHA1_DIGEST,
        "selected_action": SELECTED_ACTION_SUMMARY,
        "target_node": TARGET_NODE_SUMMARY,
        "mappings": _array(REPLACEMENT_MAPPING, maxItems=256),
        "retained_connections": _array(
            RETAINED_CONNECTION,
            maxItems=4096,
        ),
        "retained_defaults": _array(RETAINED_DEFAULT, maxItems=256),
        "unmapped_connections": _array(
            LOST_CONNECTION,
            maxItems=4096,
        ),
        "unmapped_defaults": _array(LOST_DEFAULT, maxItems=256),
        "unsupported_metadata": _array(
            UNSUPPORTED_METADATA,
            maxItems=256,
        ),
        "loss_count": {"type": "integer", "minimum": 0},
        "warnings": _array({"type": "string", "minLength": 1}, maxItems=512),
        "applicable": {"type": "boolean"},
        "allow_conversion": {"type": "boolean"},
        "allow_loss": {"type": "boolean"},
    },
    (
        "replacement_plan_id",
        "asset_path",
        "graph_id",
        "node_snapshot_digest",
        "action_result_digest",
        "selected_action",
        "target_node",
        "mappings",
        "retained_connections",
        "retained_defaults",
        "unmapped_connections",
        "unmapped_defaults",
        "unsupported_metadata",
        "loss_count",
        "warnings",
        "applicable",
        "allow_conversion",
        "allow_loss",
    ),
)
PRESERVED_METADATA = {
    "type": "string",
    "enum": [
        "position",
        "comment",
        "comment_bubble_visible",
        "enabled_state",
    ],
}
REPLACEMENT_APPLY_DATA = _object(
    {
        "replacement_plan_id": REPLACEMENT_PLAN_ID,
        "asset_path": ASSET_PATH,
        "graph_id": GRAPH_ID,
        "action_id": ACTION_ID,
        "old_node_id": NODE_ID,
        "new_node_id": NODE_ID,
        "class_path": {"type": "string", "minLength": 1},
        "position": POSITION,
        "pin_ids": _array(PIN_ID, maxItems=256, uniqueItems=True),
        "pins": _array(SNAPSHOT_PIN, maxItems=256),
        "auxiliary_node_ids": _array(
            NODE_ID,
            maxItems=256,
            uniqueItems=True,
        ),
        "connections": _array(SEMANTIC_EDGE, maxItems=4096),
        "preserved_defaults": _array(RETAINED_DEFAULT, maxItems=256),
        "dropped_connections": _array(LOST_CONNECTION, maxItems=4096),
        "dropped_defaults": _array(LOST_DEFAULT, maxItems=256),
        "preserved_metadata": _array(
            PRESERVED_METADATA,
            maxItems=4,
            uniqueItems=True,
        ),
        "transaction_recorded": {"const": True},
        "saved": {"const": False},
    },
    (
        "replacement_plan_id",
        "asset_path",
        "graph_id",
        "action_id",
        "old_node_id",
        "new_node_id",
        "class_path",
        "position",
        "pin_ids",
        "pins",
        "auxiliary_node_ids",
        "connections",
        "preserved_defaults",
        "dropped_connections",
        "dropped_defaults",
        "preserved_metadata",
        "transaction_recorded",
        "saved",
    ),
)

INSPECT_OPS = [
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
]
INSPECT_QUERY = _object(
    {
        "op": {"type": "string", "enum": INSPECT_OPS},
        "graph_id": STABLE_ID,
        "member_id": STABLE_ID,
        "node_id": STABLE_ID,
        "kind": {"type": "string", "minLength": 1},
        "name_pattern": {"type": "string", "minLength": 1},
        "class_path": FULL_UNREAL_PATH,
        "detail": {"type": "string", "enum": ["compact", "detailed"], "default": "compact"},
        "limit": {"type": "integer", "minimum": 1, "maximum": 500, "default": 100},
        "cursor": {"type": "string", "default": ""},
    },
    ("op",),
)


def _fallback_capable_target(
    schema: dict,
    id_field: str,
    name_field: str,
    qualification_fields: dict[str, dict],
) -> dict:
    result = deepcopy(schema)
    result["properties"]["allow_name_fallback"] = {
        "type": "boolean",
        "default": False,
    }
    result["properties"][name_field] = deepcopy(NAME)
    for field, field_schema in qualification_fields.items():
        result["properties"][field] = deepcopy(field_schema)
    result.setdefault("allOf", []).append(
        {
            "if": {
                "properties": {id_field: {"pattern": r"^fallback:"}},
                "required": [id_field],
            },
            "then": {
                "properties": {"allow_name_fallback": {"const": True}},
                "required": [
                    "allow_name_fallback",
                    name_field,
                    *qualification_fields,
                ],
            },
        }
    )
    return result

FUNCTION_PROPERTIES = {
    "asset_path": ASSET_PATH,
    "function_name": NAME,
    "inputs": PARAMETERS_WITH_DEFAULT,
    "outputs": PARAMETERS_WITH_DEFAULT,
    "pure": {"type": "boolean", "default": False},
    "const": {"type": "boolean", "default": False},
    "access": ACCESS,
    "category": {"type": "string", "default": ""},
    "description": {"type": "string", "default": ""},
}
FUNCTION_EXAMPLE = {
    "asset_path": "/Game/BP_Player",
    "function_name": "CalculateScore",
    "inputs": [{"name": "BaseScore", "type": {"kind": "int"}}],
    "outputs": [{"name": "Score", "type": {"kind": "int"}}],
    "pure": True,
    "const": True,
    "access": "public",
    "category": "Scoring",
    "description": "Calculates a score.",
}

BLUEPRINT2_ACTION_SPECS = {
    "get_blueprint_brief": _read(
        "get_blueprint_brief",
        "Return a bounded orientation summary for one Blueprint.",
        _object({"asset_path": ASSET_PATH}, ("asset_path",)),
        {"asset_path": "/Game/BP_Player"},
        success_data_schema=BLUEPRINT_BRIEF_DATA,
    ),
    "inspect_blueprint": _read(
        "inspect_blueprint",
        "Run bounded, independently paginated queries against one Blueprint.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "queries": _array(INSPECT_QUERY, maxItems=32),
                "cursor": {"type": "string", "default": ""},
            },
            ("asset_path",),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "queries": [{"op": "functions", "limit": 100}],
        },
    ),
    "search_blueprint_node_actions": _read(
        "search_blueprint_node_actions",
        "Search Unreal's native Blueprint action palette for one graph.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "graph_id": GRAPH_ID,
                "query": PALETTE_QUERY,
                "filters": PALETTE_FILTERS_WITH_DEFAULT,
                "cursor": PALETTE_CURSOR,
                "limit": PALETTE_LIMIT,
            },
            ("asset_path", "graph_id"),
        ),
        {
            "asset_path": "/Game/BP_Player.BP_Player",
            "graph_id": "graph:11111111-1111-4111-8111-111111111111",
            "query": "Get Actor Location",
            "filters": {"action_kinds": ["function"], "pure_only": True},
            "limit": 50,
        },
        success_data_schema=PALETTE_PAGE_DATA,
    ),
    "describe_blueprint_node_action": _read(
        "describe_blueprint_node_action",
        "Describe one opaque Blueprint palette action.",
        _object({"action_id": ACTION_ID}, ("action_id",)),
        {"action_id": "action:0123456789abcdef0123456789abcdef01234567"},
        success_data_schema=PALETTE_DESCRIBE_DATA,
    ),
    "add_blueprint_action_node": _write(
        "add_blueprint_action_node",
        "Spawn one native palette action in a Blueprint graph.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "graph_id": GRAPH_ID,
                "action_id": ACTION_ID,
                "position": POSITION,
                "bindings": _array(
                    BINDING_ID,
                    maxItems=32,
                    uniqueItems=True,
                    default=[],
                ),
            },
            ("asset_path", "graph_id", "action_id", "position"),
        ),
        {
            "asset_path": "/Game/BP_Player.BP_Player",
            "graph_id": "graph:11111111-1111-4111-8111-111111111111",
            "action_id": "action:0123456789abcdef0123456789abcdef01234567",
            "position": {"x": 320, "y": 160},
            "bindings": [],
        },
        idempotent=False,
        success_data_schema=PALETTE_SPAWN_DATA,
    ),
    "suggest_blueprint_nodes_for_pin": _read(
        "suggest_blueprint_nodes_for_pin",
        "Return native palette actions compatible with one stable pin.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "graph_id": GRAPH_ID,
                "pin_id": PIN_ID,
                "query": PALETTE_QUERY,
                "cursor": PALETTE_CURSOR,
                "limit": PALETTE_LIMIT,
            },
            ("asset_path", "graph_id", "pin_id"),
        ),
        {
            "asset_path": "/Game/BP_Player.BP_Player",
            "graph_id": "graph:11111111-1111-4111-8111-111111111111",
            "pin_id": "pin:22222222-2222-4222-8222-222222222222",
            "query": "Branch",
            "limit": 50,
        },
        success_data_schema=PALETTE_PIN_PAGE_DATA,
    ),
    "suggest_blueprint_nodes_for_connection": _read(
        "suggest_blueprint_nodes_for_connection",
        "Return native palette actions that can bridge two stable pins.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "graph_id": GRAPH_ID,
                "source_pin_id": PIN_ID,
                "target_pin_id": PIN_ID,
                "query": PALETTE_QUERY,
                "filters": PALETTE_FILTERS_WITH_DEFAULT,
                "allow_conversion": {"type": "boolean", "default": False},
                "cursor": SEMANTIC_PALETTE_CURSOR,
                "limit": PALETTE_LIMIT,
            },
            (
                "asset_path",
                "graph_id",
                "source_pin_id",
                "target_pin_id",
            ),
        ),
        {
            "asset_path": "/Game/BP_Player.BP_Player",
            "graph_id": "graph:11111111-1111-4111-8111-111111111111",
            "source_pin_id": "pin:22222222-2222-4222-8222-222222222222",
            "target_pin_id": "pin:33333333-3333-4333-8333-333333333333",
            "query": "Convert",
            "filters": {"action_kinds": ["function"]},
            "allow_conversion": False,
            "limit": 50,
        },
        success_data_schema=CONNECTION_PAGE_DATA,
    ),
    "add_blueprint_connected_action_node": _write(
        "add_blueprint_connected_action_node",
        "Spawn one pin-bound palette action and connect it atomically.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "graph_id": GRAPH_ID,
                "pin_id": PIN_ID,
                "action_id": ACTION_ID,
                "connection_binding_id": BINDING_ID,
                "position": POSITION,
                "allow_conversion": {"type": "boolean", "default": False},
                "bindings": _array(
                    BINDING_ID,
                    maxItems=32,
                    uniqueItems=True,
                    default=[],
                ),
            },
            (
                "asset_path",
                "graph_id",
                "pin_id",
                "action_id",
                "connection_binding_id",
                "position",
            ),
        ),
        {
            "asset_path": "/Game/BP_Player.BP_Player",
            "graph_id": "graph:11111111-1111-4111-8111-111111111111",
            "pin_id": "pin:22222222-2222-4222-8222-222222222222",
            "action_id": "action:0123456789abcdef0123456789abcdef01234567",
            "connection_binding_id": (
                "binding:89abcdef0123456789abcdef0123456789abcdef"
            ),
            "position": {"x": 320, "y": 160},
            "allow_conversion": False,
            "bindings": [],
        },
        idempotent=False,
        success_data_schema=CONNECTED_SPAWN_DATA,
    ),
    "insert_blueprint_action_node": _write(
        "insert_blueprint_action_node",
        "Insert one two-pin-bound palette action into an existing edge.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "graph_id": GRAPH_ID,
                "source_pin_id": PIN_ID,
                "target_pin_id": PIN_ID,
                "action_id": ACTION_ID,
                "input_binding_id": BINDING_ID,
                "output_binding_id": BINDING_ID,
                "position": POSITION,
                "bindings": _array(
                    BINDING_ID,
                    maxItems=32,
                    uniqueItems=True,
                    default=[],
                ),
            },
            (
                "asset_path",
                "graph_id",
                "source_pin_id",
                "target_pin_id",
                "action_id",
                "input_binding_id",
                "output_binding_id",
                "position",
            ),
        ),
        {
            "asset_path": "/Game/BP_Player.BP_Player",
            "graph_id": "graph:11111111-1111-4111-8111-111111111111",
            "source_pin_id": "pin:22222222-2222-4222-8222-222222222222",
            "target_pin_id": "pin:33333333-3333-4333-8333-333333333333",
            "action_id": "action:0123456789abcdef0123456789abcdef01234567",
            "input_binding_id": (
                "binding:89abcdef0123456789abcdef0123456789abcdef"
            ),
            "output_binding_id": (
                "binding:fedcba9876543210fedcba9876543210fedcba98"
            ),
            "position": {"x": 480, "y": 160},
            "bindings": [],
        },
        idempotent=False,
        success_data_schema=INSERT_DATA,
    ),
    "preview_blueprint_action_replacement": _read(
        "preview_blueprint_action_replacement",
        "Preview a snapshot-bound palette action replacement without mutation.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "graph_id": GRAPH_ID,
                "node_id": NODE_ID,
                "action_id": ACTION_ID,
                "bindings": _array(
                    BINDING_ID,
                    maxItems=32,
                    uniqueItems=True,
                    default=[],
                ),
                "pin_mapping": _array(
                    PIN_MAPPING_INPUT,
                    maxItems=256,
                    uniqueItems=True,
                    default=[],
                ),
                "allow_conversion": {"type": "boolean", "default": False},
                "allow_loss": {"type": "boolean", "default": False},
            },
            ("asset_path", "graph_id", "node_id", "action_id"),
        ),
        {
            "asset_path": "/Game/BP_Player.BP_Player",
            "graph_id": "graph:11111111-1111-4111-8111-111111111111",
            "node_id": "node:44444444-4444-4444-8444-444444444444",
            "action_id": "action:0123456789abcdef0123456789abcdef01234567",
            "bindings": [],
            "pin_mapping": [],
            "allow_conversion": False,
            "allow_loss": False,
        },
        success_data_schema=REPLACEMENT_PREVIEW_DATA,
    ),
    "replace_blueprint_node_with_action": _destructive(
        "replace_blueprint_node_with_action",
        "Apply one unchanged snapshot-bound Blueprint replacement plan.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "graph_id": GRAPH_ID,
                "replacement_plan_id": REPLACEMENT_PLAN_ID,
                "allow_loss": {"type": "boolean", "default": False},
            },
            ("asset_path", "graph_id", "replacement_plan_id"),
        ),
        {
            "asset_path": "/Game/BP_Player.BP_Player",
            "graph_id": "graph:11111111-1111-4111-8111-111111111111",
            "replacement_plan_id": (
                "replacement-plan:0123456789abcdef0123456789abcdef01234567"
            ),
            "allow_loss": False,
        },
        success_data_schema=REPLACEMENT_APPLY_DATA,
    ),
    "create_blueprint_function": _write(
        "create_blueprint_function",
        "Create a Blueprint function with a complete ordered signature.",
        _object(FUNCTION_PROPERTIES, ("asset_path", "function_name")),
        FUNCTION_EXAMPLE,
        idempotent=False,
    ),
    "rename_blueprint_function": _destructive(
        "rename_blueprint_function",
        "Rename a Blueprint function targeted by stable ID.",
        _fallback_capable_target(
            _object(
                {
                    "asset_path": ASSET_PATH,
                    "function_id": FALLBACK_GRAPH_ID,
                    "new_name": NAME,
                },
                ("asset_path", "function_id", "new_name"),
            ),
            "function_id",
            "function_name",
            {
                "function_owner_id": INTERFACE_CLASS_PATH,
                "function_type_path": {"type": "string", "minLength": 1},
            },
        ),
        {
            "asset_path": "/Game/BP_Player",
            "function_id": "graph:11111111-1111-4111-8111-111111111111",
            "new_name": "CalculateFinalScore",
        },
    ),
    "set_blueprint_function_signature": _write(
        "set_blueprint_function_signature",
        "Replace the complete signature and metadata of a Blueprint function.",
        _fallback_capable_target(
            _object(
                {
                    "asset_path": ASSET_PATH,
                    "function_id": FALLBACK_GRAPH_ID,
                    "inputs": PARAMETERS,
                    "outputs": PARAMETERS,
                    "pure": {"type": "boolean"},
                    "const": {"type": "boolean"},
                    "access": {"type": "string", "enum": ["public", "protected", "private"]},
                    "category": {"type": "string"},
                    "description": {"type": "string"},
                },
                (
                    "asset_path",
                    "function_id",
                    "inputs",
                    "outputs",
                    "pure",
                    "const",
                    "access",
                    "category",
                    "description",
                ),
            ),
            "function_id",
            "function_name",
            {
                "function_owner_id": INTERFACE_CLASS_PATH,
                "function_type_path": {"type": "string", "minLength": 1},
            },
        ),
        {
            "asset_path": "/Game/BP_Player",
            "function_id": "graph:11111111-1111-4111-8111-111111111111",
            "inputs": [{"name": "BaseScore", "type": {"kind": "int"}}],
            "outputs": [{"name": "Score", "type": {"kind": "int"}}],
            "pure": True,
            "const": True,
            "access": "public",
            "category": "Scoring",
            "description": "Calculates a score.",
        },
        idempotent=True,
    ),
    "delete_blueprint_function": _destructive(
        "delete_blueprint_function",
        "Delete a Blueprint function targeted by stable ID.",
        _fallback_capable_target(
            _object(
                {"asset_path": ASSET_PATH, "function_id": FALLBACK_GRAPH_ID},
                ("asset_path", "function_id"),
            ),
            "function_id",
            "function_name",
            {
                "function_owner_id": INTERFACE_CLASS_PATH,
                "function_type_path": {"type": "string", "minLength": 1},
            },
        ),
        {
            "asset_path": "/Game/BP_Player",
            "function_id": "graph:11111111-1111-4111-8111-111111111111",
        },
    ),
    "create_blueprint_macro": _write(
        "create_blueprint_macro",
        "Create a Blueprint macro with ordered tunnel parameters.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "macro_name": NAME,
                "inputs": PARAMETERS_WITH_DEFAULT,
                "outputs": PARAMETERS_WITH_DEFAULT,
                "pure": {"type": "boolean"},
                "category": {"type": "string"},
                "description": {"type": "string"},
            },
            ("asset_path", "macro_name"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "macro_name": "ClampScore",
            "inputs": [{"name": "Value", "type": {"kind": "int"}}],
            "outputs": [{"name": "Result", "type": {"kind": "int"}}],
            "pure": True,
            "category": "Scoring",
            "description": "Clamp a score.",
        },
        idempotent=False,
    ),
    "delete_blueprint_macro": _destructive(
        "delete_blueprint_macro",
        "Delete a Blueprint macro targeted by stable ID.",
        _fallback_capable_target(
            _object(
                {"asset_path": ASSET_PATH, "macro_id": FALLBACK_GRAPH_ID},
                ("asset_path", "macro_id"),
            ),
            "macro_id",
            "macro_name",
            {
                "macro_owner_id": INTERFACE_CLASS_PATH,
                "macro_type_path": {"type": "string", "minLength": 1},
            },
        ),
        {
            "asset_path": "/Game/BP_Player",
            "macro_id": "graph:11111111-1111-4111-8111-111111111111",
        },
    ),
    "create_custom_event": _write(
        "create_custom_event",
        "Create a custom event with ordered parameters.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "graph_id": GRAPH_ID,
                "event_name": NAME,
                "parameters": PARAMETERS_WITH_DEFAULT,
                "pos_x": {"type": "number"},
                "pos_y": {"type": "number"},
            },
            ("asset_path", "graph_id", "event_name"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "graph_id": "graph:11111111-1111-4111-8111-111111111111",
            "event_name": "OnScoreChanged",
            "parameters": [{"name": "Score", "type": {"kind": "int"}}],
            "pos_x": 160.0,
            "pos_y": 320.0,
        },
        idempotent=False,
    ),
    "delete_custom_event": _destructive(
        "delete_custom_event",
        "Delete a custom event targeted by stable ID.",
        _fallback_capable_target(
            _object(
                {"asset_path": ASSET_PATH, "event_id": FALLBACK_NODE_ID},
                ("asset_path", "event_id"),
            ),
            "event_id",
            "event_name",
            {
                "owner_graph_id": FALLBACK_GRAPH_ID,
                "event_type_path": {"type": "string", "minLength": 1},
            },
        ),
        {
            "asset_path": "/Game/BP_Player",
            "event_id": "node:22222222-2222-4222-8222-222222222222",
        },
    ),
    "add_event_dispatcher": _write(
        "add_event_dispatcher",
        "Add an event dispatcher with ordered parameters.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "dispatcher_name": NAME,
                "parameters": PARAMETERS_WITH_DEFAULT,
                "category": {"type": "string"},
                "description": {"type": "string"},
            },
            ("asset_path", "dispatcher_name"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "dispatcher_name": "ScoreChanged",
            "parameters": [{"name": "Score", "type": {"kind": "int"}}],
            "category": "Scoring",
            "description": "Emitted when the score changes.",
        },
        idempotent=False,
    ),
    "remove_event_dispatcher": _destructive(
        "remove_event_dispatcher",
        "Remove an event dispatcher targeted by stable ID.",
        _fallback_capable_target(
            _object(
                {"asset_path": ASSET_PATH, "dispatcher_id": FALLBACK_VARIABLE_ID},
                ("asset_path", "dispatcher_id"),
            ),
            "dispatcher_id",
            "dispatcher_name",
            {
                "dispatcher_owner_id": INTERFACE_CLASS_PATH,
                "dispatcher_type_path": {"type": "string", "minLength": 1},
            },
        ),
        {
            "asset_path": "/Game/BP_Player",
            "dispatcher_id": "variable:44444444-4444-4444-8444-444444444444",
        },
    ),
    "add_blueprint_interface": _write(
        "add_blueprint_interface",
        "Add a Blueprint interface by full Unreal object path.",
        _object(
            {"asset_path": ASSET_PATH, "interface_path": INTERFACE_CLASS_PATH},
            ("asset_path", "interface_path"),
        ),
        {"asset_path": "/Game/BP_Player", "interface_path": "/Script/Game.PlayerInterface"},
        idempotent=False,
    ),
    "remove_blueprint_interface": _destructive(
        "remove_blueprint_interface",
        "Remove an implemented Blueprint interface targeted by stable ID.",
        _object(
            {"asset_path": ASSET_PATH, "interface_id": INTERFACE_ID},
            ("asset_path", "interface_id"),
        ),
        {"asset_path": "/Game/BP_Player", "interface_id": "interface:/Script/Game.PlayerInterface"},
    ),
    "add_reflected_blueprint_node": _write(
        "add_reflected_blueprint_node",
        "Add a reflected node using a full Unreal member object path.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "graph_id": GRAPH_ID,
                "member_kind": {
                    "type": "string",
                    "enum": [
                        "function",
                        "property_get",
                        "property_set",
                        "cast_to",
                        "enum_literal",
                        "make_struct",
                        "break_struct",
                    ],
                },
                "member_path": FULL_UNREAL_PATH,
                "position": POSITION,
            },
            ("asset_path", "graph_id", "member_kind", "member_path", "position"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "graph_id": "graph:11111111-1111-4111-8111-111111111111",
            "member_kind": "function",
            "member_path": "/Script/Engine.Actor:K2_GetActorLocation",
            "position": {"x": 320, "y": 160},
        },
        idempotent=False,
    ),
    "set_blueprint_node_properties": _write(
        "set_blueprint_node_properties",
        "Set allowlisted reflected properties on a node targeted by stable ID.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "node_id": NODE_ID,
                "properties": {"type": "object", "minProperties": 1},
            },
            ("asset_path", "node_id", "properties"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "node_id": "node:22222222-2222-4222-8222-222222222222",
            "properties": {"comment": "Validated comment"},
        },
        idempotent=True,
    ),
    "disconnect_blueprint_pins": _write(
        "disconnect_blueprint_pins",
        "Disconnect one pin entirely or one exact stable pin pair.",
        {
            **_object(
                {
                    "asset_path": ASSET_PATH,
                    "pin_id": PIN_ID,
                    "source_pin_id": PIN_ID,
                    "target_pin_id": PIN_ID,
                },
                ("asset_path",),
            ),
            "oneOf": [
                {
                    "required": ["pin_id"],
                    "not": {
                        "anyOf": [
                            {"required": ["source_pin_id"]},
                            {"required": ["target_pin_id"]},
                        ]
                    },
                },
                {
                    "required": ["source_pin_id", "target_pin_id"],
                    "not": {"required": ["pin_id"]},
                },
            ],
        },
        {
            "asset_path": "/Game/BP_Player",
            "pin_id": "pin:33333333-3333-4333-8333-333333333333",
        },
        idempotent=True,
    ),
    "rename_blueprint_variable": _destructive(
        "rename_blueprint_variable",
        "Rename a Blueprint variable targeted by stable ID.",
        _fallback_capable_target(
            _object(
                {
                    "asset_path": ASSET_PATH,
                    "variable_id": FALLBACK_VARIABLE_ID,
                    "new_name": NAME,
                },
                ("asset_path", "variable_id", "new_name"),
            ),
            "variable_id",
            "variable_name",
            {
                "variable_owner_id": INTERFACE_CLASS_PATH,
                "variable_type_path": {"type": "string", "minLength": 1},
            },
        ),
        {
            "asset_path": "/Game/BP_Player",
            "variable_id": "variable:44444444-4444-4444-8444-444444444444",
            "new_name": "FinalScore",
        },
    ),
    "remove_blueprint_variable": _destructive(
        "remove_blueprint_variable",
        "Remove a Blueprint variable targeted by stable ID.",
        _fallback_capable_target(
            _object(
                {"asset_path": ASSET_PATH, "variable_id": FALLBACK_VARIABLE_ID},
                ("asset_path", "variable_id"),
            ),
            "variable_id",
            "variable_name",
            {
                "variable_owner_id": INTERFACE_CLASS_PATH,
                "variable_type_path": {"type": "string", "minLength": 1},
            },
        ),
        {
            "asset_path": "/Game/BP_Player",
            "variable_id": "variable:44444444-4444-4444-8444-444444444444",
        },
    ),
    "set_blueprint_variable_default": _write(
        "set_blueprint_variable_default",
        "Set a Blueprint variable default as a canonical JSON value.",
        _object(
            {"asset_path": ASSET_PATH, "variable_id": VARIABLE_ID, "default": {}},
            ("asset_path", "variable_id", "default"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "variable_id": "variable:44444444-4444-4444-8444-444444444444",
            "default": 100,
        },
        idempotent=True,
    ),
    "set_blueprint_variable_metadata": _write(
        "set_blueprint_variable_metadata",
        "Set supported Blueprint variable metadata.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "variable_id": VARIABLE_ID,
                "metadata": {
                    **_object(
                        {
                            "category": {"type": "string"},
                            "tooltip": {"type": "string"},
                            "visible": {"type": "boolean"},
                            "instance_editable": {"type": "boolean"},
                            "expose_on_spawn": {"type": "boolean"},
                            "save_game": {"type": "boolean"},
                            "cinematic": {"type": "boolean"},
                        }
                    ),
                    "minProperties": 1,
                },
            },
            ("asset_path", "variable_id", "metadata"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "variable_id": "variable:44444444-4444-4444-8444-444444444444",
            "metadata": {"category": "Scoring", "instance_editable": True},
        },
        idempotent=True,
    ),
    "set_blueprint_variable_replication": _write(
        "set_blueprint_variable_replication",
        "Set supported Blueprint variable replication behavior.",
        {
            **_object(
                {
                    "asset_path": ASSET_PATH,
                    "variable_id": VARIABLE_ID,
                    "mode": {
                        "type": "string",
                        "enum": ["none", "replicated", "rep_notify"],
                    },
                    "notify_function_name": {"type": "string"},
                    "condition": REPLICATION_CONDITION,
                },
                ("asset_path", "variable_id", "mode"),
            ),
            "oneOf": [
                {
                    "properties": {
                        "mode": {"const": "rep_notify"},
                        "notify_function_name": NAME,
                    },
                    "required": ["notify_function_name"],
                },
                {
                    "properties": {
                        "mode": {"const": "replicated"},
                        "notify_function_name": {"maxLength": 0},
                    },
                },
                {
                    "properties": {
                        "mode": {"const": "none"},
                        "notify_function_name": {"maxLength": 0},
                        "condition": {"const": "none"},
                    },
                },
            ],
        },
        {
            "asset_path": "/Game/BP_Player",
            "variable_id": "variable:44444444-4444-4444-8444-444444444444",
            "mode": "rep_notify",
            "notify_function_name": "OnRep_Score",
            "condition": "owner_only",
        },
        idempotent=True,
    ),
    "rename_blueprint_component": _destructive(
        "rename_blueprint_component",
        "Rename an SCS component targeted by stable ID.",
        _object(
            {"asset_path": ASSET_PATH, "component_id": COMPONENT_ID, "new_name": NAME},
            ("asset_path", "component_id", "new_name"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "component_id": "component:55555555-5555-4555-8555-555555555555",
            "new_name": "PlayerMesh",
        },
    ),
    "reparent_blueprint_component": _destructive(
        "reparent_blueprint_component",
        "Reparent an SCS component using stable component IDs.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "component_id": COMPONENT_ID,
                "parent_component_id": {
                    "anyOf": [COMPONENT_ID, {"type": "null"}],
                },
            },
            ("asset_path", "component_id", "parent_component_id"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "component_id": "component:55555555-5555-4555-8555-555555555555",
            "parent_component_id": "component:66666666-6666-4666-8666-666666666666",
        },
    ),
    "reorder_blueprint_component": _destructive(
        "reorder_blueprint_component",
        "Move an SCS component to an explicit sibling index.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "component_id": COMPONENT_ID,
                "sibling_index": {"type": "integer", "minimum": 0, "maximum": 2_147_483_647},
            },
            ("asset_path", "component_id", "sibling_index"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "component_id": "component:55555555-5555-4555-8555-555555555555",
            "sibling_index": 0,
        },
    ),
    "set_blueprint_component_transform": _write(
        "set_blueprint_component_transform",
        "Set bounded relative transform fields on an SCS component.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "component_id": COMPONENT_ID,
                "transform": TRANSFORM,
            },
            ("asset_path", "component_id", "transform"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "component_id": "component:55555555-5555-4555-8555-555555555555",
            "transform": {"location": [0, 0, 100], "rotation": [0, 90, 0], "scale": [1, 1, 1]},
        },
        idempotent=True,
    ),
    "get_blueprint_health": _spec(
        "get_blueprint_health",
        "Compile explicitly and return structured Blueprint health diagnostics.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "include_warnings": {"type": "boolean", "default": True},
            },
            ("asset_path",),
        ),
        {"asset_path": "/Game/BP_Player", "include_warnings": True},
        effect="write",
        risk="high",
        idempotent=False,
        supports_preview=False,
        supports_undo=False,
        requires_confirmation=True,
    ),
    "snapshot_blueprint_graph": _read(
        "snapshot_blueprint_graph",
        "Return a deterministic compact graph snapshot ordered by stable ID.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "graph_ids": _array(
                    GRAPH_ID,
                    maxItems=64,
                    uniqueItems=True,
                    default=[],
                ),
            },
            ("asset_path",),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "graph_ids": [
                "graph:11111111-1111-4111-8111-111111111111",
            ],
        },
    ),
    "diff_blueprint_graphs": _read(
        "diff_blueprint_graphs",
        "Diff two deterministic graph snapshots with independently paginated sections.",
        _object(
            {
                "before_snapshot": BLUEPRINT_GRAPH_SNAPSHOT,
                "after_snapshot": BLUEPRINT_GRAPH_SNAPSHOT,
                "queries": _array(
                    _object(
                        {
                            "section": {
                                "type": "string",
                                "enum": ["nodes", "pins", "connections", "properties", "positions"],
                            },
                            "limit": {
                                "type": "integer",
                                "minimum": 1,
                                "maximum": 500,
                                "default": 100,
                            },
                            "cursor": {"type": "string", "default": ""},
                            "detail": {
                                "type": "string",
                                "enum": ["compact", "detailed"],
                                "default": "compact",
                            },
                        },
                        ("section",),
                    ),
                    minItems=0,
                    maxItems=5,
                    uniqueItems=True,
                    default=[],
                ),
            },
            ("before_snapshot", "after_snapshot"),
        ),
        {
            "before_snapshot": EMPTY_BLUEPRINT_GRAPH_SNAPSHOT_EXAMPLE,
            "after_snapshot": EMPTY_BLUEPRINT_GRAPH_SNAPSHOT_EXAMPLE,
            "queries": [
                {"section": "nodes", "limit": 100, "detail": "compact"},
            ],
        },
    ),
}
