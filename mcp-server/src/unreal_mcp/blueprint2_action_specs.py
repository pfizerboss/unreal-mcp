"""Strict Action Registry contracts for the universal Blueprint 2 surface."""

from copy import deepcopy
from typing import Any


ASSET_PATH = {"type": "string", "format": "unreal-asset-path", "minLength": 1}
STABLE_ID = {
    "type": "string",
    "pattern": (
        r"^(?:(graph|node|pin|variable|component|interface):.+"
        r"|fallback:(graph|node|pin|variable|component|interface):[0-9a-f]{40})$"
    ),
}
NAME = {
    "type": "string",
    "pattern": r"^[A-Za-z_][A-Za-z0-9_]*$",
    "maxLength": 1024,
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


def _type_choices(depth: int, nested: dict | None = None) -> list[dict]:
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
                    "type_path": {"type": "string", "pattern": r"^/Script/"},
                },
                ("kind", "type_path"),
            )
        )
    for kind in ("object", "class", "interface", "soft_object", "soft_class"):
        choices.append(
            _object(
                {
                    "kind": {"const": kind},
                    "class_path": {"type": "string", "pattern": r"^/Script/"},
                },
                ("kind", "class_path"),
            )
        )
    if depth < 8 and nested is not None:
        for kind in ("array", "set"):
            choices.append(
                _object(
                    {"kind": {"const": kind}, "item": nested},
                    ("kind", "item"),
                )
            )
        choices.append(
            _object(
                {
                    "kind": {"const": "map"},
                    "key": nested,
                    "value": nested,
                },
                ("kind", "key", "value"),
            )
        )
    return choices


def type_spec(depth: int = 0) -> dict:
    """Return the bounded recursive canonical Blueprint type schema."""
    if depth >= 8:
        return {"oneOf": _type_choices(depth)}

    resource_id = f"urn:unreal-mcp:blueprint2:type-spec:{depth}"
    definitions: dict[str, dict] = {}
    for nested_depth in range(8, depth, -1):
        next_schema = None
        if nested_depth < 8:
            next_schema = {
                "$ref": f"{resource_id}#/$defs/type_{nested_depth + 1}"
            }
        definitions[f"type_{nested_depth}"] = {
            "oneOf": _type_choices(nested_depth, next_schema)
        }

    nested = {"$ref": f"{resource_id}#/$defs/type_{depth + 1}"}
    choices = _type_choices(depth, nested)
    # Keep the public primitive exactly {"oneOf": choices}. The first choice is
    # an embedded schema resource so references remain valid after TYPE_SPEC is
    # deep-copied into a larger action input schema.
    choices[0]["$id"] = resource_id
    choices[0]["$defs"] = definitions
    return {"oneOf": choices}


TYPE_SPEC = type_spec()
PARAMETER = _object(
    {"name": NAME, "type": TYPE_SPEC, "default": {}},
    ("name", "type"),
)


FULL_UNREAL_PATH = {
    "type": "string",
    "pattern": r"^/Script/",
    "minLength": 9,
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
) -> dict:
    return {
        "title": action.replace("_", " ").title(),
        "description": description,
        "result_kind": "json",
        "input_schema": deepcopy(input_schema),
        "output_schema": deepcopy(OUTPUT_SCHEMA),
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
    action: str, description: str, input_schema: dict, example_params: dict
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
    )


def _write(
    action: str,
    description: str,
    input_schema: dict,
    example_params: dict,
    *,
    idempotent: bool,
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
    )


def _destructive(
    action: str, description: str, input_schema: dict, example_params: dict
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
    )


PARAMETERS = _array(PARAMETER)
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
        "limit": {"type": "integer", "minimum": 1, "maximum": 500, "default": 100},
        "cursor": {"type": "string", "default": ""},
    },
    ("op",),
)

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
    ),
    "inspect_blueprint": _read(
        "inspect_blueprint",
        "Run bounded, independently paginated queries against one Blueprint.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "queries": _array(INSPECT_QUERY, minItems=1, maxItems=32),
                "compact": {"type": "boolean", "default": True},
                "cursor": {"type": "string", "default": ""},
            },
            ("asset_path", "queries"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "queries": [{"op": "functions", "limit": 100}],
            "compact": True,
        },
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
        _object(
            {"asset_path": ASSET_PATH, "function_id": STABLE_ID, "new_name": NAME},
            ("asset_path", "function_id", "new_name"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "function_id": "graph:function-guid",
            "new_name": "CalculateFinalScore",
        },
    ),
    "set_blueprint_function_signature": _write(
        "set_blueprint_function_signature",
        "Replace the complete signature and metadata of a Blueprint function.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "function_id": STABLE_ID,
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
        {
            "asset_path": "/Game/BP_Player",
            "function_id": "graph:function-guid",
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
        _object(
            {"asset_path": ASSET_PATH, "function_id": STABLE_ID},
            ("asset_path", "function_id"),
        ),
        {"asset_path": "/Game/BP_Player", "function_id": "graph:function-guid"},
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
            },
            ("asset_path", "macro_name"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "macro_name": "ClampScore",
            "inputs": [{"name": "Value", "type": {"kind": "int"}}],
            "outputs": [{"name": "Result", "type": {"kind": "int"}}],
        },
        idempotent=False,
    ),
    "delete_blueprint_macro": _destructive(
        "delete_blueprint_macro",
        "Delete a Blueprint macro targeted by stable ID.",
        _object(
            {"asset_path": ASSET_PATH, "macro_id": STABLE_ID},
            ("asset_path", "macro_id"),
        ),
        {"asset_path": "/Game/BP_Player", "macro_id": "graph:macro-guid"},
    ),
    "create_custom_event": _write(
        "create_custom_event",
        "Create a custom event with ordered parameters.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "event_name": NAME,
                "parameters": PARAMETERS_WITH_DEFAULT,
            },
            ("asset_path", "event_name"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "event_name": "OnScoreChanged",
            "parameters": [{"name": "Score", "type": {"kind": "int"}}],
        },
        idempotent=False,
    ),
    "delete_custom_event": _destructive(
        "delete_custom_event",
        "Delete a custom event targeted by stable ID.",
        _object(
            {"asset_path": ASSET_PATH, "event_id": STABLE_ID},
            ("asset_path", "event_id"),
        ),
        {"asset_path": "/Game/BP_Player", "event_id": "node:event-guid"},
    ),
    "add_event_dispatcher": _write(
        "add_event_dispatcher",
        "Add an event dispatcher with ordered parameters.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "dispatcher_name": NAME,
                "parameters": PARAMETERS_WITH_DEFAULT,
            },
            ("asset_path", "dispatcher_name"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "dispatcher_name": "ScoreChanged",
            "parameters": [{"name": "Score", "type": {"kind": "int"}}],
        },
        idempotent=False,
    ),
    "remove_event_dispatcher": _destructive(
        "remove_event_dispatcher",
        "Remove an event dispatcher targeted by stable ID.",
        _object(
            {"asset_path": ASSET_PATH, "dispatcher_id": STABLE_ID},
            ("asset_path", "dispatcher_id"),
        ),
        {"asset_path": "/Game/BP_Player", "dispatcher_id": "variable:dispatcher-guid"},
    ),
    "add_blueprint_interface": _write(
        "add_blueprint_interface",
        "Add a Blueprint interface by full Unreal object path.",
        _object(
            {"asset_path": ASSET_PATH, "interface_path": FULL_UNREAL_PATH},
            ("asset_path", "interface_path"),
        ),
        {"asset_path": "/Game/BP_Player", "interface_path": "/Script/Game.PlayerInterface"},
        idempotent=False,
    ),
    "remove_blueprint_interface": _destructive(
        "remove_blueprint_interface",
        "Remove an implemented Blueprint interface targeted by stable ID.",
        _object(
            {"asset_path": ASSET_PATH, "interface_id": STABLE_ID},
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
                "graph_id": STABLE_ID,
                "member_kind": {
                    "type": "string",
                    "enum": ["function", "property", "class", "enum", "struct"],
                },
                "member_path": FULL_UNREAL_PATH,
                "position": POSITION,
            },
            ("asset_path", "graph_id", "member_kind", "member_path", "position"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "graph_id": "graph:event-guid",
            "member_kind": "function",
            "member_path": "/Script/Engine.Actor.K2_GetActorLocation",
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
                "node_id": STABLE_ID,
                "properties": {"type": "object", "minProperties": 1},
            },
            ("asset_path", "node_id", "properties"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "node_id": "node:node-guid",
            "properties": {"NodeComment": "Validated comment"},
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
                    "pin_id": STABLE_ID,
                    "source_pin_id": STABLE_ID,
                    "target_pin_id": STABLE_ID,
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
        {"asset_path": "/Game/BP_Player", "pin_id": "pin:pin-guid"},
        idempotent=True,
    ),
    "rename_blueprint_variable": _destructive(
        "rename_blueprint_variable",
        "Rename a Blueprint variable targeted by stable ID.",
        _object(
            {"asset_path": ASSET_PATH, "variable_id": STABLE_ID, "new_name": NAME},
            ("asset_path", "variable_id", "new_name"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "variable_id": "variable:variable-guid",
            "new_name": "FinalScore",
        },
    ),
    "remove_blueprint_variable": _destructive(
        "remove_blueprint_variable",
        "Remove a Blueprint variable targeted by stable ID.",
        _object(
            {"asset_path": ASSET_PATH, "variable_id": STABLE_ID},
            ("asset_path", "variable_id"),
        ),
        {"asset_path": "/Game/BP_Player", "variable_id": "variable:variable-guid"},
    ),
    "set_blueprint_variable_default": _write(
        "set_blueprint_variable_default",
        "Set a Blueprint variable default as a canonical JSON value.",
        _object(
            {"asset_path": ASSET_PATH, "variable_id": STABLE_ID, "default": {}},
            ("asset_path", "variable_id", "default"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "variable_id": "variable:variable-guid",
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
                "variable_id": STABLE_ID,
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
            "variable_id": "variable:variable-guid",
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
                    "variable_id": STABLE_ID,
                    "mode": {
                        "type": "string",
                        "enum": ["none", "replicated", "rep_notify"],
                    },
                    "notify_function_name": NAME,
                },
                ("asset_path", "variable_id", "mode"),
            ),
            "oneOf": [
                {
                    "properties": {"mode": {"const": "rep_notify"}},
                    "required": ["notify_function_name"],
                },
                {
                    "properties": {"mode": {"enum": ["none", "replicated"]}},
                    "not": {"required": ["notify_function_name"]},
                },
            ],
        },
        {
            "asset_path": "/Game/BP_Player",
            "variable_id": "variable:variable-guid",
            "mode": "rep_notify",
            "notify_function_name": "OnRep_Score",
        },
        idempotent=True,
    ),
    "rename_blueprint_component": _destructive(
        "rename_blueprint_component",
        "Rename an SCS component targeted by stable ID.",
        _object(
            {"asset_path": ASSET_PATH, "component_id": STABLE_ID, "new_name": NAME},
            ("asset_path", "component_id", "new_name"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "component_id": "component:component-guid",
            "new_name": "PlayerMesh",
        },
    ),
    "reparent_blueprint_component": _destructive(
        "reparent_blueprint_component",
        "Reparent an SCS component using stable component IDs.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "component_id": STABLE_ID,
                "parent_component_id": STABLE_ID,
            },
            ("asset_path", "component_id", "parent_component_id"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "component_id": "component:mesh-guid",
            "parent_component_id": "component:root-guid",
        },
    ),
    "reorder_blueprint_component": _destructive(
        "reorder_blueprint_component",
        "Move an SCS component to an explicit sibling index.",
        _object(
            {
                "asset_path": ASSET_PATH,
                "component_id": STABLE_ID,
                "sibling_index": {"type": "integer", "minimum": 0, "maximum": 2_147_483_647},
            },
            ("asset_path", "component_id", "sibling_index"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "component_id": "component:mesh-guid",
            "sibling_index": 0,
        },
    ),
    "set_blueprint_component_transform": _write(
        "set_blueprint_component_transform",
        "Set bounded relative transform fields on an SCS component.",
        _object(
            {"asset_path": ASSET_PATH, "component_id": STABLE_ID, "transform": TRANSFORM},
            ("asset_path", "component_id", "transform"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "component_id": "component:mesh-guid",
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
                "graph_id": STABLE_ID,
                "detailed": {"type": "boolean", "default": False},
            },
            ("asset_path", "graph_id"),
        ),
        {
            "asset_path": "/Game/BP_Player",
            "graph_id": "graph:event-guid",
            "detailed": False,
        },
    ),
    "diff_blueprint_graphs": _read(
        "diff_blueprint_graphs",
        "Diff two deterministic graph snapshots with independently paginated sections.",
        _object(
            {
                "before_snapshot": {"type": "object", "minProperties": 1},
                "after_snapshot": {"type": "object", "minProperties": 1},
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
                            "detailed": {"type": "boolean", "default": False},
                        },
                        ("section",),
                    ),
                    minItems=1,
                    maxItems=5,
                ),
            },
            ("before_snapshot", "after_snapshot", "queries"),
        ),
        {
            "before_snapshot": {"snapshot_id": "before"},
            "after_snapshot": {"snapshot_id": "after"},
            "queries": [{"section": "nodes", "limit": 100, "detailed": False}],
        },
    ),
}
