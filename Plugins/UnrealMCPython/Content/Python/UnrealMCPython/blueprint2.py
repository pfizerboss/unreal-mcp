"""Thin Python adapters for the Universal Blueprint 2 C++ core."""

from copy import deepcopy
import json
import uuid

import unreal


def _failure(code: str, path: str, message: str, hint: str, details: dict) -> str:
    trace_id = str(uuid.uuid4())
    return json.dumps(
        {
            "success": False,
            "status": "failed",
            "summary": message,
            "data": {},
            "changes": [],
            "warnings": [],
            "errors": [
                {
                    "code": code,
                    "path": path,
                    "message": message,
                    "retryable": False,
                    "hint": hint,
                    "details": deepcopy(details),
                    "trace_id": trace_id,
                }
            ],
            "next_actions": [],
            "trace_id": trace_id,
        },
        separators=(",", ":"),
        ensure_ascii=False,
    )


def load_blueprint(asset_path: str):
    """Load one Blueprint asset, accepting derived Blueprint asset classes."""
    asset = unreal.EditorAssetLibrary.load_asset(asset_path)
    return asset if isinstance(asset, unreal.Blueprint) else None


def call_asset_helper(
    helper_name: str, asset_path: str, request: dict | None = None
) -> str:
    """Call a Blueprint helper without parsing or reserializing its result."""
    blueprint = load_blueprint(asset_path)
    if blueprint is None:
        return _failure(
            "PRECONDITION_FAILED",
            "asset_path",
            f"Blueprint asset was not found: {asset_path}",
            "Provide the full object path of an existing Blueprint asset.",
            {"asset_path": asset_path},
        )
    helper = getattr(unreal.MCPythonHelper, helper_name)
    if request is None:
        return helper(blueprint)
    payload = json.dumps(
        deepcopy(request), separators=(",", ":"), ensure_ascii=False
    )
    return helper(blueprint, payload)


def call_json_helper(helper_name: str, request: dict) -> str:
    """Call a JSON-only helper and preserve its serialized result exactly."""
    payload = json.dumps(
        deepcopy(request), separators=(",", ":"), ensure_ascii=False
    )
    return getattr(unreal.MCPythonHelper, helper_name)(payload)


def target_request(
    stable_id: str,
    *,
    allow_name_fallback=False,
    owner_id="",
    name="",
    type_path="",
) -> dict:
    """Build the explicit target descriptor signed by workflow planning."""
    return {
        "id": stable_id,
        "allow_name_fallback": bool(allow_name_fallback),
        "owner_id": owner_id,
        "name": name,
        "type_path": type_path,
    }
