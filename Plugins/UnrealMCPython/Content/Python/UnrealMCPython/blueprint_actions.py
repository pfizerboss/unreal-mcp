# Copyright (c) 2025 GenOrca. All Rights Reserved.

import unreal
import json
import traceback
from copy import deepcopy
from collections import deque

def _load_asset(asset_path, expected_class=None):
    """Load an asset and optionally verify its class. Returns (asset, error_json_str)."""
    asset = unreal.EditorAssetLibrary.load_asset(asset_path)
    if asset is None:
        return None, json.dumps({
            "success": False,
            "message": f"Asset not found or failed to load: {asset_path}"
        })
    if expected_class is not None and not isinstance(asset, expected_class):
        return None, json.dumps({
            "success": False,
            "message": f"Asset at '{asset_path}' is {type(asset).__name__}, expected {expected_class.__name__}."
        })
    return asset, None


def _target_identity(target_id, guid_kind):
    if target_id and target_id.startswith("fallback:"):
        return "qualified_name_fallback", False
    return guid_kind, bool(target_id)


def _add_fallback_qualification(
    record, target_id, owner_id, name, type_path, prefix=""
):
    if not target_id or not target_id.startswith("fallback:"):
        return
    field_prefix = f"{prefix}_" if prefix else ""
    record[f"{field_prefix}owner_id"] = owner_id
    record[f"{field_prefix}name"] = name
    record[f"{field_prefix}type_path"] = type_path


# ─── Read Actions ─────────────────────────────────────────────────────────────

def ue_get_selected_bp_nodes() -> str:
    """Returns information about currently selected blueprint nodes in the editor."""
    try:
        reflected_infos = unreal.MCPythonHelper.get_selected_blueprint_node_infos()
        node_infos = []
        for info in reflected_infos:
            id_kind, stable = _target_identity(info.stable_id, "node_guid")
            graph_id_kind, graph_stable = _target_identity(
                info.graph_id,
                "graph_guid",
            )
            node_record = {
                "name": info.node_name,
                "class": info.node_class,
                "object_path": info.object_path,
                "stable_id": info.stable_id,
                "graph_id": info.graph_id,
                "id_kind": id_kind,
                "stable": stable,
                "graph_id_kind": graph_id_kind,
                "graph_stable": graph_stable,
            }
            _add_fallback_qualification(
                node_record,
                info.stable_id,
                getattr(info, "owner_id", ""),
                info.node_name,
                getattr(info, "type_path", ""),
            )
            _add_fallback_qualification(
                node_record,
                info.graph_id,
                getattr(info, "graph_owner_id", ""),
                getattr(info, "graph_name", ""),
                getattr(info, "graph_type_path", ""),
                "graph",
            )
            node_infos.append(node_record)
        return json.dumps({
            "success": True,
            "selected_nodes_count": len(node_infos),
            "selected_nodes": node_infos
        })
    except Exception as e:
        return json.dumps({"success": False, "message": str(e), "traceback": traceback.format_exc()})


def ue_get_selected_bp_node_infos() -> str:
    """Returns compact blueprint node info optimized for LLM token efficiency."""
    try:
        node_infos = unreal.MCPythonHelper.get_selected_blueprint_node_infos()

        stable_to_id = {
            n.stable_id: i
            for i, n in enumerate(node_infos)
            if n.stable_id
        }

        def link_to_dict(link):
            graph_id_kind, graph_stable = _target_identity(
                link.graph_id,
                "graph_guid",
            )
            node_id_kind, node_stable = _target_identity(
                link.node_id,
                "node_guid",
            )
            pin_id_kind, pin_stable = _target_identity(
                link.pin_id,
                "pin_guid",
            )
            d = {
                "graph_id": link.graph_id,
                "node_id": link.node_id,
                "pin_id": link.pin_id,
                "graph_id_kind": graph_id_kind,
                "graph_stable": graph_stable,
                "node_id_kind": node_id_kind,
                "node_stable": node_stable,
                "pin_id_kind": pin_id_kind,
                "pin_stable": pin_stable,
            }
            _add_fallback_qualification(
                d,
                link.graph_id,
                getattr(link, "graph_owner_id", ""),
                getattr(link, "graph_name", ""),
                getattr(link, "graph_type_path", ""),
                "graph",
            )
            _add_fallback_qualification(
                d,
                link.node_id,
                getattr(link, "node_owner_id", ""),
                link.node_name,
                getattr(link, "node_type_path", ""),
                "node",
            )
            _add_fallback_qualification(
                d,
                link.pin_id,
                getattr(link, "owner_id", ""),
                getattr(link, "name", link.pin_name),
                getattr(link, "type_path", ""),
            )
            if link.node_id in stable_to_id:
                d["node"] = stable_to_id[link.node_id]
            else:
                d["node"] = link.node_title
            if link.pin_name:
                d["pin"] = link.pin_name
            return d

        def pin_to_dict(pin):
            name = pin.friendly_name if pin.friendly_name else pin.pin_name
            id_kind, stable = _target_identity(pin.stable_id, "pin_guid")
            graph_id_kind, graph_stable = _target_identity(
                pin.graph_id,
                "graph_guid",
            )
            node_id_kind, node_stable = _target_identity(
                pin.node_id,
                "node_guid",
            )
            d = {
                "name": name,
                "dir": pin.direction,
                "stable_id": pin.stable_id,
                "pin_id": pin.pin_id,
                "graph_id": pin.graph_id,
                "node_id": pin.node_id,
                "id_kind": id_kind,
                "stable": stable,
                "graph_id_kind": graph_id_kind,
                "graph_stable": graph_stable,
                "node_id_kind": node_id_kind,
                "node_stable": node_stable,
            }
            _add_fallback_qualification(
                d,
                pin.stable_id,
                getattr(pin, "owner_id", ""),
                pin.pin_name,
                getattr(pin, "type_path", ""),
            )
            if (
                pin.stable_id.startswith("fallback:")
                and name != pin.pin_name
            ):
                d["display_name"] = name
            ptype = pin.pin_type
            if pin.pin_sub_type:
                ptype += ":" + pin.pin_sub_type
            d["type"] = ptype
            if pin.default_value:
                d["default"] = pin.default_value
            linked = list(pin.linked_to)
            if linked:
                d["linked"] = [link_to_dict(l) for l in linked]
            return d

        def node_to_dict(node, idx):
            id_kind, stable = _target_identity(node.stable_id, "node_guid")
            graph_id_kind, graph_stable = _target_identity(
                node.graph_id,
                "graph_guid",
            )
            d = {
                "id": idx,
                "stable_id": node.stable_id,
                "graph_id": node.graph_id,
                "title": node.node_title,
                "id_kind": id_kind,
                "stable": stable,
                "graph_id_kind": graph_id_kind,
                "graph_stable": graph_stable,
            }
            _add_fallback_qualification(
                d,
                node.stable_id,
                getattr(node, "owner_id", ""),
                node.node_name,
                getattr(node, "type_path", ""),
            )
            _add_fallback_qualification(
                d,
                node.graph_id,
                getattr(node, "graph_owner_id", ""),
                getattr(node, "graph_name", ""),
                getattr(node, "graph_type_path", ""),
                "graph",
            )
            if node.node_comment:
                d["comment"] = node.node_comment
            d["pins"] = [pin_to_dict(p) for p in node.pins]
            return d

        nodes = [node_to_dict(n, i) for i, n in enumerate(node_infos)]
        return json.dumps({
            "success": True,
            "nodes": nodes
        })
    except Exception as e:
        return json.dumps({"success": False, "message": str(e), "traceback": traceback.format_exc()})


def ue_get_blueprint_graph_info(asset_path: str = None, graph_name: str = "EventGraph") -> str:
    """Returns the full graph info for a Blueprint graph."""
    if asset_path is None:
        return json.dumps({"success": False, "message": "Required parameter 'asset_path' is missing."})
    try:
        bp, err = _load_asset(asset_path, unreal.Blueprint)
        if err:
            return err
        result_json = unreal.MCPythonHelper.get_blueprint_graph_info(bp, graph_name)
        return result_json
    except Exception as e:
        return json.dumps({"success": False, "message": str(e), "traceback": traceback.format_exc()})


def ue_list_callable_functions(asset_path: str = None, filter: str = "") -> str:
    """Lists callable functions available in a Blueprint context."""
    if asset_path is None:
        return json.dumps({"success": False, "message": "Required parameter 'asset_path' is missing."})
    try:
        bp, err = _load_asset(asset_path, unreal.Blueprint)
        if err:
            return err
        result_json = unreal.MCPythonHelper.list_callable_functions(bp, filter)
        return result_json
    except Exception as e:
        return json.dumps({"success": False, "message": str(e), "traceback": traceback.format_exc()})


def ue_list_blueprint_variables(asset_path: str = None) -> str:
    """Lists all variables defined in a Blueprint."""
    if asset_path is None:
        return json.dumps({"success": False, "message": "Required parameter 'asset_path' is missing."})
    try:
        bp, err = _load_asset(asset_path, unreal.Blueprint)
        if err:
            return err
        result_json = unreal.MCPythonHelper.list_blueprint_variables(bp)
        return result_json
    except Exception as e:
        return json.dumps({"success": False, "message": str(e), "traceback": traceback.format_exc()})


# ─── Write Actions ────────────────────────────────────────────────────────────

def ue_add_blueprint_node(asset_path: str = None, graph_name: str = "EventGraph",
                          node_json: dict = None) -> str:
    """Adds a single node to a Blueprint graph."""
    if asset_path is None:
        return json.dumps({"success": False, "message": "Required parameter 'asset_path' is missing."})
    if node_json is None:
        return json.dumps({"success": False, "message": "Required parameter 'node_json' is missing."})
    try:
        bp, err = _load_asset(asset_path, unreal.Blueprint)
        if err:
            return err
        node_json_str = json.dumps(node_json)
        result_json = unreal.MCPythonHelper.add_blueprint_node(bp, graph_name, node_json_str)
        return result_json
    except Exception as e:
        return json.dumps({"success": False, "message": str(e), "traceback": traceback.format_exc()})


def ue_connect_blueprint_pins(asset_path: str = None, graph_name: str = "EventGraph",
                              source_node: str = None, source_pin: str = None,
                              target_node: str = None, target_pin: str = None) -> str:
    """Connects two pins in a Blueprint graph."""
    if asset_path is None:
        return json.dumps({"success": False, "message": "Required parameter 'asset_path' is missing."})
    for name, val in [("source_node", source_node), ("source_pin", source_pin),
                      ("target_node", target_node), ("target_pin", target_pin)]:
        if val is None:
            return json.dumps({"success": False, "message": f"Required parameter '{name}' is missing."})
    try:
        bp, err = _load_asset(asset_path, unreal.Blueprint)
        if err:
            return err
        result_json = unreal.MCPythonHelper.connect_blueprint_pins(
            bp, graph_name, source_node, source_pin, target_node, target_pin)
        return result_json
    except Exception as e:
        return json.dumps({"success": False, "message": str(e), "traceback": traceback.format_exc()})


def ue_remove_blueprint_node(asset_path: str = None, graph_name: str = "EventGraph",
                             node_name: str = None) -> str:
    """Removes a node from a Blueprint graph."""
    if asset_path is None:
        return json.dumps({"success": False, "message": "Required parameter 'asset_path' is missing."})
    if node_name is None:
        return json.dumps({"success": False, "message": "Required parameter 'node_name' is missing."})
    try:
        bp, err = _load_asset(asset_path, unreal.Blueprint)
        if err:
            return err
        result_json = unreal.MCPythonHelper.remove_blueprint_node(bp, graph_name, node_name)
        return result_json
    except Exception as e:
        return json.dumps({"success": False, "message": str(e), "traceback": traceback.format_exc()})


def ue_build_blueprint_graph(asset_path: str = None, graph_name: str = "EventGraph",
                             graph_structure: dict = None) -> str:
    """Builds a Blueprint graph from JSON adjacency list."""
    if asset_path is None:
        return json.dumps({"success": False, "message": "Required parameter 'asset_path' is missing."})
    if graph_structure is None:
        return json.dumps({"success": False, "message": "Required parameter 'graph_structure' is missing."})
    try:
        bp, err = _load_asset(asset_path, unreal.Blueprint)
        if err:
            return err
        graph_json_str = json.dumps(graph_structure)
        result_json = unreal.MCPythonHelper.build_blueprint_graph(bp, graph_name, graph_json_str)
        return result_json
    except Exception as e:
        return json.dumps({"success": False, "message": str(e), "traceback": traceback.format_exc()})


def ue_compile_blueprint(asset_path: str = None) -> str:
    """Compiles a Blueprint with stable structured compiler diagnostics and recovery actions."""
    if asset_path is None:
        return json.dumps({"success": False, "message": "Required parameter 'asset_path' is missing."})
    try:
        bp, err = _load_asset(asset_path, unreal.Blueprint)
        if err:
            return err
        result_json = unreal.MCPythonHelper.compile_blueprint(bp)
        return result_json
    except Exception as e:
        return json.dumps({"success": False, "message": str(e), "traceback": traceback.format_exc()})


# ─── Component Management ──────────────────────────────────────────────────────

def ue_list_blueprint_components(asset_path: str = None) -> str:
    """Lists all SCS components on a Blueprint."""
    if asset_path is None:
        return json.dumps({"success": False, "message": "Required parameter 'asset_path' is missing."})
    try:
        bp, err = _load_asset(asset_path, unreal.Blueprint)
        if err:
            return err
        return unreal.MCPythonHelper.list_blueprint_components(bp)
    except Exception as e:
        return json.dumps({"success": False, "message": str(e), "traceback": traceback.format_exc()})


def ue_add_component_to_blueprint(asset_path: str = None,
                                   component_class_path: str = None,
                                   component_name: str = None,
                                   location_x: float = 0.0, location_y: float = 0.0, location_z: float = 0.0,
                                   rotation_pitch: float = 0.0, rotation_yaw: float = 0.0, rotation_roll: float = 0.0,
                                   parent_component_name: str = "") -> str:
    """Adds a component to a Blueprint's SCS."""
    if asset_path is None:
        return json.dumps({"success": False, "message": "Required parameter 'asset_path' is missing."})
    if component_class_path is None:
        return json.dumps({"success": False, "message": "Required parameter 'component_class_path' is missing."})
    if component_name is None:
        return json.dumps({"success": False, "message": "Required parameter 'component_name' is missing."})
    try:
        bp, err = _load_asset(asset_path, unreal.Blueprint)
        if err:
            return err
        result = unreal.MCPythonHelper.add_component_to_blueprint(
            bp, component_class_path, component_name,
            location_x, location_y, location_z,
            rotation_pitch, rotation_yaw, rotation_roll,
            parent_component_name or ""
        )
        return result
    except Exception as e:
        return json.dumps({"success": False, "message": str(e), "traceback": traceback.format_exc()})


def ue_remove_component_from_blueprint(asset_path: str = None, component_name: str = None) -> str:
    """Removes a component by variable name from a Blueprint's SCS."""
    if asset_path is None:
        return json.dumps({"success": False, "message": "Required parameter 'asset_path' is missing."})
    if component_name is None:
        return json.dumps({"success": False, "message": "Required parameter 'component_name' is missing."})
    try:
        bp, err = _load_asset(asset_path, unreal.Blueprint)
        if err:
            return err
        result = unreal.MCPythonHelper.remove_component_from_blueprint(bp, component_name)
        return result
    except Exception as e:
        return json.dumps({"success": False, "message": str(e), "traceback": traceback.format_exc()})


def ue_set_component_property(asset_path: str = None, component_name: str = None,
                               property_name: str = None, value: str = None) -> str:
    """Sets a property on a component template in a Blueprint's SCS."""
    if asset_path is None:
        return json.dumps({"success": False, "message": "Required parameter 'asset_path' is missing."})
    if component_name is None:
        return json.dumps({"success": False, "message": "Required parameter 'component_name' is missing."})
    if property_name is None:
        return json.dumps({"success": False, "message": "Required parameter 'property_name' is missing."})
    if value is None:
        return json.dumps({"success": False, "message": "Required parameter 'value' is missing."})
    try:
        bp, err = _load_asset(asset_path, unreal.Blueprint)
        if err:
            return err
        result = unreal.MCPythonHelper.set_component_property(bp, component_name, property_name, value)
        return result
    except Exception as e:
        return json.dumps({"success": False, "message": str(e), "traceback": traceback.format_exc()})


# ─── Graph Auto-Layout ─────────────────────────────────────────────────────────

def ue_set_blueprint_node_position(asset_path: str = None, graph_name: str = "EventGraph",
                                    node_name: str = None, pos_x: float = 0.0, pos_y: float = 0.0) -> str:
    """Sets the canvas position of a node in a Blueprint graph."""
    if asset_path is None:
        return json.dumps({"success": False, "message": "Required parameter 'asset_path' is missing."})
    if node_name is None:
        return json.dumps({"success": False, "message": "Required parameter 'node_name' is missing."})
    try:
        bp, err = _load_asset(asset_path, unreal.Blueprint)
        if err:
            return err
        return unreal.MCPythonHelper.set_blueprint_node_position(bp, graph_name, node_name, pos_x, pos_y)
    except Exception as e:
        return json.dumps({"success": False, "message": str(e), "traceback": traceback.format_exc()})


def ue_auto_layout_graph(asset_path: str = None, graph_name: str = "EventGraph",
                          x_step: float = 380.0, y_step: float = 200.0) -> str:
    """Auto-lays out all nodes in a Blueprint graph using DAG topological sort."""
    if asset_path is None:
        return json.dumps({"success": False, "message": "Required parameter 'asset_path' is missing."})
    try:
        bp, err = _load_asset(asset_path, unreal.Blueprint)
        if err:
            return err

        graph_info_str = unreal.MCPythonHelper.get_blueprint_graph_info(bp, graph_name)
        graph_info = json.loads(graph_info_str)
        if not graph_info.get("success"):
            return graph_info_str

        nodes = graph_info.get("nodes", [])
        if not nodes:
            return json.dumps({"success": True, "message": "No nodes to lay out.", "positioned": 0})

        node_names = [n["node_name"] for n in nodes]
        name_set = set(node_names)

        in_degree = {n: 0 for n in node_names}
        successors = {n: [] for n in node_names}

        ENTRY_TYPES = {"K2Node_Event", "K2Node_CustomEvent", "K2Node_InputKey",
                       "K2Node_InputAction", "K2Node_FunctionEntry"}

        for node in nodes:
            node_name = node["node_name"]
            for pin in node.get("pins", []):
                if pin.get("direction") != "Output":
                    continue
                pin_type = pin.get("type", "")
                if pin_type not in ("exec", ""):
                    continue
                for link in pin.get("linked_to", []):
                    target = link.get("node_name", "")
                    if target in name_set and target != node_name:
                        if target not in successors[node_name]:
                            successors[node_name].append(target)
                            in_degree[target] += 1

        forced_entry = set()
        for node in nodes:
            node_class = node.get("node_class", node.get("node_name", ""))
            for et in ENTRY_TYPES:
                if et in node_class or et in node.get("node_name", ""):
                    forced_entry.add(node["node_name"])
                    break
        for node in nodes:
            n = node["node_name"]
            if in_degree[n] == 0:
                for pin in node.get("pins", []):
                    if pin.get("direction") == "Output" and pin.get("type") in ("exec", ""):
                        forced_entry.add(n)
                        break

        column = {}
        queue = deque()
        for n in node_names:
            if in_degree[n] == 0 or n in forced_entry:
                column[n] = 0
                queue.append(n)

        while queue:
            n = queue.popleft()
            for s in successors[n]:
                if column.get(s, -1) < column[n] + 1:
                    column[s] = column[n] + 1
                in_degree[s] -= 1
                if in_degree[s] <= 0 and s not in column:
                    queue.append(s)

        for n in node_names:
            if n not in column:
                column[n] = 0

        col_row = {}
        positions = {}
        for node in nodes:
            n = node["node_name"]
            c = column[n]
            r = col_row.get(c, 0)
            positions[n] = (c * x_step, r * y_step)
            col_row[c] = r + 1

        errors = []
        positioned = 0
        for n, (px, py) in positions.items():
            result_str = unreal.MCPythonHelper.set_blueprint_node_position(bp, graph_name, n, px, py)
            result = json.loads(result_str)
            if result.get("success"):
                positioned += 1
            else:
                errors.append(f"{n}: {result.get('message', '?')}")

        return json.dumps({
            "success": True,
            "positioned": positioned,
            "total": len(node_names),
            "errors": errors,
            "message": f"Auto-layout complete: {positioned}/{len(node_names)} nodes positioned.",
        })
    except Exception as e:
        return json.dumps({"success": False, "message": str(e), "traceback": traceback.format_exc()})


def ue_create_blueprint(asset_path: str = None, parent_class_path: str = "/Script/Engine.Actor") -> str:
    """Creates a Blueprint asset with the given parent class (default Actor)."""
    if asset_path is None:
        return json.dumps({"success": False, "message": "Required parameter 'asset_path' is missing."})
    try:
        if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
            return json.dumps({"success": False, "message": f"Asset already exists: {asset_path}"})
        parent = unreal.load_class(None, parent_class_path)
        if not parent:
            return json.dumps({"success": False, "message": f"Parent class not found: {parent_class_path}"})
        asset_path = asset_path.rstrip("/")
        idx = asset_path.rfind("/")
        name, package = asset_path[idx + 1:], asset_path[:idx]
        factory = unreal.BlueprintFactory()
        factory.set_editor_property("parent_class", parent)
        bp = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, package, unreal.Blueprint, factory)
        if not bp:
            return json.dumps({"success": False, "message": f"Failed to create Blueprint at {asset_path}."})
        return json.dumps({
            "success": True,
            "asset_path": asset_path,
            "parent_class": parent_class_path,
            "saved": False,
        })
    except Exception as e:
        return json.dumps({"success": False, "message": str(e), "traceback": traceback.format_exc()})


_BP_VAR_TYPES = {"int", "byte", "bool", "real", "name", "string", "text"}


def ue_add_variable(asset_path: str = None, variable_name: str = None, variable_type: str = "real") -> str:
    """Adds a member variable to a Blueprint. variable_type: int, byte, bool, real (float), name, string, text."""
    if asset_path is None or variable_name is None:
        return json.dumps({"success": False, "message": "Required parameters: asset_path, variable_name."})
    vt = (variable_type or "real").lower()
    if vt == "float":
        vt = "real"
    if vt not in _BP_VAR_TYPES:
        return json.dumps({"success": False, "message": f"Unsupported variable_type '{variable_type}'.",
                           "valid_types": sorted(_BP_VAR_TYPES | {"float"})})
    from UnrealMCPython import blueprint2

    request = deepcopy({
        "asset_path": asset_path,
        "variable_name": variable_name,
        "variable_type": vt,
    })
    return blueprint2.call_asset_helper(
        "add_blueprint_variable", asset_path, request
    )


def ue_set_variable_flags(asset_path: str = None, variable_name: str = None,
                          instance_editable: bool = None, expose_on_spawn: bool = None) -> str:
    """Sets a Blueprint variable's 'Instance Editable' and/or 'Expose On Spawn' flags."""
    if asset_path is None or variable_name is None:
        return json.dumps({"success": False, "message": "Required parameters: asset_path, variable_name."})
    if instance_editable is None and expose_on_spawn is None:
        return json.dumps({"success": False, "message": "Provide instance_editable and/or expose_on_spawn."})
    from UnrealMCPython import blueprint2

    request = {
        "asset_path": asset_path,
        "variable_name": variable_name,
    }
    if instance_editable is not None:
        request["instance_editable"] = instance_editable
    if expose_on_spawn is not None:
        request["expose_on_spawn"] = expose_on_spawn
    return blueprint2.call_asset_helper(
        "set_blueprint_variable_flags", asset_path, deepcopy(request)
    )


def _blueprint2_unsupported(action: str) -> str:
    """Return the fixed envelope used until the Blueprint 2 C++ core lands."""
    trace_id = "blueprint2-cpp-core-unavailable"
    return json.dumps({
        "success": False,
        "status": "failed",
        "summary": "Blueprint 2 C++ core is unavailable.",
        "data": {},
        "changes": [],
        "warnings": [],
        "errors": [{
            "code": "UE_VERSION_UNSUPPORTED",
            "path": None,
            "message": "This action requires the Blueprint 2 C++ core.",
            "retryable": False,
            "hint": "Install a plugin build that provides blueprint2_cpp_core.",
            "details": {"capability": "blueprint2_cpp_core", "action": action},
            "trace_id": trace_id,
        }],
        "next_actions": [],
        "trace_id": trace_id,
    })


def ue_get_blueprint_brief(asset_path: str = None) -> str:
    """Returns a bounded orientation summary for one Blueprint."""
    from UnrealMCPython import blueprint2

    return blueprint2.call_asset_helper("get_blueprint_brief", asset_path)


def ue_inspect_blueprint(asset_path: str = None, queries: list[dict] = (),
                         cursor: str = "") -> str:
    """Runs bounded, independently paginated queries against one Blueprint."""
    from UnrealMCPython import blueprint2

    request = {"queries": [dict(query) for query in queries]}
    if cursor:
        request["cursor"] = cursor
    return blueprint2.call_asset_helper("inspect_blueprint", asset_path, request)


def ue_search_blueprint_node_actions(
    asset_path: str = None,
    graph_id: str = None,
    query: str = "",
    filters: dict = None,
    cursor: str = "",
    limit: int = 50,
) -> str:
    """Search Unreal's native Blueprint action palette for one graph."""
    from UnrealMCPython import blueprint2

    request = {
        "graph_id": graph_id,
        "query": query,
        "filters": deepcopy(filters) if filters is not None else {},
        "cursor": cursor,
        "limit": limit,
    }
    return blueprint2.call_asset_helper(
        "search_blueprint_node_actions", asset_path, request
    )


def ue_describe_blueprint_node_action(action_id: str = None) -> str:
    """Describe one opaque Blueprint palette action."""
    from UnrealMCPython import blueprint2

    return blueprint2.call_json_helper(
        "describe_blueprint_node_action", {"action_id": action_id}
    )


def ue_add_blueprint_action_node(
    asset_path: str = None,
    graph_id: str = None,
    action_id: str = None,
    position: dict = None,
    bindings: list[str] = (),
) -> str:
    """Spawn one native palette action in a Blueprint graph."""
    from UnrealMCPython import blueprint2

    request = {
        "graph_id": graph_id,
        "action_id": action_id,
        "position": deepcopy(position),
        "bindings": list(bindings),
    }
    return blueprint2.call_asset_helper(
        "add_blueprint_action_node", asset_path, request
    )


def ue_suggest_blueprint_nodes_for_pin(
    asset_path: str = None,
    graph_id: str = None,
    pin_id: str = None,
    query: str = "",
    cursor: str = "",
    limit: int = 50,
) -> str:
    """Return native palette actions compatible with one stable pin."""
    from UnrealMCPython import blueprint2

    request = {
        "graph_id": graph_id,
        "pin_id": pin_id,
        "query": query,
        "cursor": cursor,
        "limit": limit,
    }
    return blueprint2.call_asset_helper(
        "suggest_blueprint_nodes_for_pin", asset_path, request
    )


def ue_create_blueprint_function(asset_path: str = None, function_name: str = None,
                                       inputs: list = [], outputs: list = [],
                                       pure: bool = False, const: bool = False,
                                       access: str = "public", category: str = "",
                                       description: str = "") -> str:
    """Creates a Blueprint function with a complete ordered signature."""
    from UnrealMCPython import blueprint2

    request = deepcopy({
        "function_name": function_name,
        "inputs": inputs,
        "outputs": outputs,
        "pure": pure,
        "const": const,
        "access": access,
        "category": category,
        "description": description,
    })
    return blueprint2.call_asset_helper(
        "create_blueprint_function", asset_path, request
    )


def ue_rename_blueprint_function(asset_path: str = None, function_id: str = None,
                                       new_name: str = None,
                                       allow_name_fallback: bool = False,
                                       function_name: str = "",
                                       function_owner_id: str = "",
                                       function_type_path: str = "") -> str:
    """Renames a Blueprint function targeted by stable ID."""
    from UnrealMCPython import blueprint2

    request = deepcopy({
        "function_id": function_id,
        "new_name": new_name,
        "allow_name_fallback": allow_name_fallback,
        "function_name": function_name,
        "function_owner_id": function_owner_id,
        "function_type_path": function_type_path,
    })
    return blueprint2.call_asset_helper(
        "rename_blueprint_function", asset_path, request
    )


def ue_set_blueprint_function_signature(asset_path: str = None,
                                              function_id: str = None,
                                              inputs: list = None,
                                              outputs: list = None,
                                              pure: bool = None,
                                              const: bool = None,
                                              access: str = None,
                                              category: str = None,
                                              description: str = None,
                                              allow_name_fallback: bool = False,
                                              function_name: str = "",
                                              function_owner_id: str = "",
                                              function_type_path: str = "") -> str:
    """Replaces the complete signature and metadata of a Blueprint function."""
    from UnrealMCPython import blueprint2

    request = deepcopy({
        "function_id": function_id,
        "inputs": inputs,
        "outputs": outputs,
        "pure": pure,
        "const": const,
        "access": access,
        "category": category,
        "description": description,
        "allow_name_fallback": allow_name_fallback,
        "function_name": function_name,
        "function_owner_id": function_owner_id,
        "function_type_path": function_type_path,
    })
    return blueprint2.call_asset_helper(
        "set_blueprint_function_signature", asset_path, request
    )


def ue_delete_blueprint_function(asset_path: str = None,
                                       function_id: str = None,
                                       allow_name_fallback: bool = False,
                                       function_name: str = "",
                                       function_owner_id: str = "",
                                       function_type_path: str = "") -> str:
    """Deletes a Blueprint function targeted by stable ID."""
    from UnrealMCPython import blueprint2

    request = deepcopy({
        "function_id": function_id,
        "allow_name_fallback": allow_name_fallback,
        "function_name": function_name,
        "function_owner_id": function_owner_id,
        "function_type_path": function_type_path,
    })
    return blueprint2.call_asset_helper(
        "delete_blueprint_function", asset_path, request
    )


def ue_create_blueprint_macro(asset_path: str = None, macro_name: str = None,
                                    inputs: list = [], outputs: list = [],
                                    pure: bool = False, category: str = "",
                                    description: str = "") -> str:
    """Creates a Blueprint macro with ordered tunnel parameters."""
    from UnrealMCPython import blueprint2

    request = deepcopy({
        "macro_name": macro_name,
        "inputs": inputs,
        "outputs": outputs,
        "pure": pure,
        "category": category,
        "description": description,
    })
    return blueprint2.call_asset_helper(
        "create_blueprint_macro", asset_path, request
    )


def ue_delete_blueprint_macro(asset_path: str = None, macro_id: str = None,
                                    allow_name_fallback: bool = False,
                                    macro_name: str = "",
                                    macro_owner_id: str = "",
                                    macro_type_path: str = "") -> str:
    """Deletes a Blueprint macro targeted by stable ID."""
    from UnrealMCPython import blueprint2

    request = deepcopy({
        "macro_id": macro_id,
        "allow_name_fallback": allow_name_fallback,
        "macro_name": macro_name,
        "macro_owner_id": macro_owner_id,
        "macro_type_path": macro_type_path,
    })
    return blueprint2.call_asset_helper(
        "delete_blueprint_macro", asset_path, request
    )


def ue_create_custom_event(asset_path: str = None, graph_id: str = None,
                                  event_name: str = None, parameters: list = [],
                                  pos_x: float = 0.0,
                                  pos_y: float = 0.0) -> str:
    """Creates a custom event with ordered parameters."""
    from UnrealMCPython import blueprint2

    request = deepcopy({
        "graph_id": graph_id,
        "event_name": event_name,
        "parameters": parameters,
        "pos_x": pos_x,
        "pos_y": pos_y,
    })
    return blueprint2.call_asset_helper(
        "create_custom_event", asset_path, request
    )


def ue_delete_custom_event(asset_path: str = None, event_id: str = None,
                                  allow_name_fallback: bool = False,
                                  event_name: str = "",
                                  owner_graph_id: str = "",
                                  event_type_path: str = "") -> str:
    """Deletes a custom event targeted by stable ID."""
    from UnrealMCPython import blueprint2

    request = deepcopy({
        "event_id": event_id,
        "allow_name_fallback": allow_name_fallback,
        "event_name": event_name,
        "owner_graph_id": owner_graph_id,
        "event_type_path": event_type_path,
    })
    return blueprint2.call_asset_helper(
        "delete_custom_event", asset_path, request
    )


def ue_add_event_dispatcher(asset_path: str = None,
                                   dispatcher_name: str = None,
                                   parameters: list = [], category: str = "",
                                   description: str = "") -> str:
    """Adds an event dispatcher with ordered parameters."""
    from UnrealMCPython import blueprint2

    request = deepcopy({
        "dispatcher_name": dispatcher_name,
        "parameters": parameters,
        "category": category,
        "description": description,
    })
    return blueprint2.call_asset_helper(
        "add_event_dispatcher", asset_path, request
    )


def ue_remove_event_dispatcher(asset_path: str = None,
                                     dispatcher_id: str = None,
                                     allow_name_fallback: bool = False,
                                     dispatcher_name: str = "",
                                     dispatcher_owner_id: str = "",
                                     dispatcher_type_path: str = "") -> str:
    """Removes an event dispatcher targeted by stable ID."""
    from UnrealMCPython import blueprint2

    request = deepcopy({
        "dispatcher_id": dispatcher_id,
        "allow_name_fallback": allow_name_fallback,
        "dispatcher_name": dispatcher_name,
        "dispatcher_owner_id": dispatcher_owner_id,
        "dispatcher_type_path": dispatcher_type_path,
    })
    return blueprint2.call_asset_helper(
        "remove_event_dispatcher", asset_path, request
    )


def ue_add_blueprint_interface(asset_path: str = None,
                                     interface_path: str = None) -> str:
    """Adds a Blueprint interface by full Unreal object path."""
    from UnrealMCPython import blueprint2

    request = deepcopy({"interface_path": interface_path})
    return blueprint2.call_asset_helper(
        "add_blueprint_interface", asset_path, request
    )


def ue_remove_blueprint_interface(asset_path: str = None,
                                        interface_id: str = None) -> str:
    """Removes an implemented Blueprint interface targeted by stable ID."""
    from UnrealMCPython import blueprint2

    request = deepcopy({"interface_id": interface_id})
    return blueprint2.call_asset_helper(
        "remove_blueprint_interface", asset_path, request
    )


def ue_add_reflected_blueprint_node(asset_path: str = None, graph_id: str = None,
                                          member_kind: str = None, member_path: str = None,
                                          position: dict = None) -> str:
    """Adds a reflected node using a full Unreal member object path."""
    from UnrealMCPython import blueprint2

    request = deepcopy({
        "graph_id": graph_id,
        "member_kind": member_kind,
        "member_path": member_path,
        "position": position,
    })
    return blueprint2.call_asset_helper(
        "add_reflected_blueprint_node", asset_path, request
    )


def ue_set_blueprint_node_properties(asset_path: str = None, node_id: str = None,
                                           properties: dict = None) -> str:
    """Sets allowlisted reflected properties on a node targeted by stable ID."""
    from UnrealMCPython import blueprint2

    request = deepcopy({"node_id": node_id, "properties": properties})
    return blueprint2.call_asset_helper(
        "set_blueprint_node_properties", asset_path, request
    )


def ue_disconnect_blueprint_pins(asset_path: str = None, pin_id: str = "",
                                       source_pin_id: str = "",
                                       target_pin_id: str = "") -> str:
    """Disconnects one pin entirely or one exact stable pin pair."""
    from UnrealMCPython import blueprint2

    request = deepcopy({
        "pin_id": pin_id,
        "source_pin_id": source_pin_id,
        "target_pin_id": target_pin_id,
    })
    return blueprint2.call_asset_helper(
        "disconnect_blueprint_pins", asset_path, request
    )


def ue_rename_blueprint_variable(asset_path: str = None, variable_id: str = None,
                                       new_name: str = None,
                                       allow_name_fallback: bool = False,
                                       variable_name: str = "",
                                       variable_owner_id: str = "",
                                       variable_type_path: str = "") -> str:
    """Renames a Blueprint variable targeted by stable ID."""
    from UnrealMCPython import blueprint2

    request = deepcopy({
        "variable_id": variable_id,
        "new_name": new_name,
        "allow_name_fallback": allow_name_fallback,
        "variable_name": variable_name,
        "variable_owner_id": variable_owner_id,
        "variable_type_path": variable_type_path,
    })
    return blueprint2.call_asset_helper(
        "rename_blueprint_variable", asset_path, request
    )


def ue_remove_blueprint_variable(asset_path: str = None,
                                       variable_id: str = None,
                                       allow_name_fallback: bool = False,
                                       variable_name: str = "",
                                       variable_owner_id: str = "",
                                       variable_type_path: str = "") -> str:
    """Removes a Blueprint variable targeted by stable ID."""
    from UnrealMCPython import blueprint2

    request = deepcopy({
        "variable_id": variable_id,
        "allow_name_fallback": allow_name_fallback,
        "variable_name": variable_name,
        "variable_owner_id": variable_owner_id,
        "variable_type_path": variable_type_path,
    })
    return blueprint2.call_asset_helper(
        "remove_blueprint_variable", asset_path, request
    )


def ue_set_blueprint_variable_default(asset_path: str = None,
                                            variable_id: str = None,
                                            default=None) -> str:
    """Sets a Blueprint variable default as a canonical JSON value."""
    from UnrealMCPython import blueprint2

    request = deepcopy({"variable_id": variable_id, "default": default})
    return blueprint2.call_asset_helper(
        "set_blueprint_variable_default", asset_path, request
    )


def ue_set_blueprint_variable_metadata(asset_path: str = None,
                                             variable_id: str = None,
                                             metadata: dict = None) -> str:
    """Sets supported Blueprint variable metadata."""
    from UnrealMCPython import blueprint2

    request = deepcopy({"variable_id": variable_id, "metadata": metadata})
    return blueprint2.call_asset_helper(
        "set_blueprint_variable_metadata", asset_path, request
    )


def ue_set_blueprint_variable_replication(asset_path: str = None,
                                                variable_id: str = None,
                                                mode: str = None,
                                                notify_function_name: str = "",
                                                condition: str = "none") -> str:
    """Sets supported Blueprint variable replication behavior."""
    from UnrealMCPython import blueprint2

    request = deepcopy({
        "variable_id": variable_id,
        "mode": mode,
        "notify_function_name": notify_function_name,
        "condition": condition,
    })
    return blueprint2.call_asset_helper(
        "set_blueprint_variable_replication", asset_path, request
    )


def ue_rename_blueprint_component(asset_path: str = None,
                                         component_id: str = None,
                                         new_name: str = None) -> str:
    """Renames an SCS component targeted by stable ID."""
    from UnrealMCPython import blueprint2

    return blueprint2.call_asset_helper(
        "rename_blueprint_component",
        asset_path,
        {"component_id": component_id, "new_name": new_name},
    )


def ue_reparent_blueprint_component(asset_path: str = None,
                                          component_id: str = None,
                                          parent_component_id: str = None) -> str:
    """Reparents an SCS component using stable component IDs."""
    from UnrealMCPython import blueprint2

    return blueprint2.call_asset_helper(
        "reparent_blueprint_component",
        asset_path,
        {
            "component_id": component_id,
            "parent_component_id": parent_component_id,
        },
    )


def ue_reorder_blueprint_component(asset_path: str = None,
                                         component_id: str = None,
                                         sibling_index: int = None) -> str:
    """Moves an SCS component to an explicit sibling index."""
    from UnrealMCPython import blueprint2

    return blueprint2.call_asset_helper(
        "reorder_blueprint_component",
        asset_path,
        {"component_id": component_id, "sibling_index": sibling_index},
    )


def ue_set_blueprint_component_transform(asset_path: str = None,
                                               component_id: str = None,
                                               transform: dict = None) -> str:
    """Sets bounded relative transform fields on an SCS component."""
    from UnrealMCPython import blueprint2

    request = deepcopy({
        "component_id": component_id,
        "transform": transform,
    })
    return blueprint2.call_asset_helper(
        "set_blueprint_component_transform", asset_path, request
    )


def ue_get_blueprint_health(asset_path: str = None,
                                  include_warnings: bool = True) -> str:
    """Compiles explicitly and returns structured Blueprint health diagnostics."""
    from UnrealMCPython import blueprint2

    native_result = blueprint2.call_asset_helper(
        "get_blueprint_health", asset_path
    )
    if include_warnings:
        return native_result
    try:
        result = json.loads(native_result)
    except (TypeError, ValueError):
        return native_result
    data = result.get("data")
    if isinstance(data, dict) and isinstance(data.get("issues"), list):
        issues = [
            issue
            for issue in data["issues"]
            if not isinstance(issue, dict)
            or issue.get("severity") != "warning"
        ]
        data["issues"] = issues
        data["issue_count"] = len(issues)
        data["error_count"] = sum(
            isinstance(issue, dict) and issue.get("severity") == "error"
            for issue in issues
        )
        data["warning_count"] = 0
    result["warnings"] = []
    return json.dumps(result, separators=(",", ":"), ensure_ascii=False)


def ue_snapshot_blueprint_graph(
    asset_path: str = None,
    graph_ids: list[str] = (),
) -> str:
    """Returns a deterministic compact graph snapshot ordered by stable ID."""
    from UnrealMCPython import blueprint2

    request = deepcopy({"graph_ids": list(graph_ids or [])})
    return blueprint2.call_asset_helper(
        "snapshot_blueprint_graph", asset_path, request
    )


def ue_diff_blueprint_graphs(
    before_snapshot: dict = None,
    after_snapshot: dict = None,
    queries: list[dict] = (),
) -> str:
    """Diffs two graph snapshots with independently paginated sections."""
    from UnrealMCPython import blueprint2

    request = deepcopy({
        "before_snapshot": before_snapshot,
        "after_snapshot": after_snapshot,
        "queries": list(queries or []),
    })
    return blueprint2.call_json_helper("diff_blueprint_graphs", request)


# Literal metadata consumed by mcp-server/generate_catalog.py.
ACTION_METADATA = {'add_blueprint_node': {'asset_path_params': ['asset_path'],
                        'description': 'Adds a single node to a Blueprint graph.',
                        'effect': 'write',
                        'idempotent': False,
                        'required_plugins': [],
                        'requires_confirmation': True,
                        'result_kind': 'json',
                        'risk': 'medium',
                        'supports_preview': True,
                        'supports_undo': True,
                        'title': 'Add Blueprint Node',
                        'ue_versions': ['5.6', '5.7', '5.8']},
 'add_component_to_blueprint': {'asset_path_params': ['asset_path', 'component_class_path'],
                                'description': "Adds a component to a Blueprint's SCS.",
                                'effect': 'write',
                                'idempotent': False,
                                'required_plugins': [],
                                'requires_confirmation': True,
                                'result_kind': 'json',
                                'risk': 'medium',
                                'supports_preview': True,
                                'supports_undo': True,
                                'title': 'Add Component To Blueprint',
                                'ue_versions': ['5.6', '5.7', '5.8']},
 'add_variable': {'asset_path_params': ['asset_path'],
                  'description': 'Adds a member variable to a Blueprint. variable_type: int, byte, '
                                 'bool, real (float), name, string, text.',
                  'effect': 'write',
                  'idempotent': False,
                  'required_plugins': [],
                  'requires_confirmation': True,
                  'result_kind': 'json',
                  'risk': 'medium',
                  'supports_preview': True,
                  'supports_undo': True,
                  'title': 'Add Variable',
                  'ue_versions': ['5.6', '5.7', '5.8']},
 'auto_layout_graph': {'asset_path_params': ['asset_path'],
                       'description': 'Auto-lays out all nodes in a Blueprint graph using DAG '
                                      'topological sort.',
                       'effect': 'write',
                       'idempotent': False,
                       'required_plugins': [],
                       'requires_confirmation': True,
                       'result_kind': 'json',
                       'risk': 'medium',
                       'supports_preview': True,
                       'supports_undo': True,
                       'title': 'Auto Layout Graph',
                       'ue_versions': ['5.6', '5.7', '5.8']},
 'build_blueprint_graph': {'asset_path_params': ['asset_path'],
                           'description': 'Builds a Blueprint graph from JSON adjacency list.',
                           'effect': 'destructive',
                           'idempotent': False,
                           'required_plugins': [],
                           'requires_confirmation': True,
                           'result_kind': 'json',
                           'risk': 'high',
                           'supports_preview': True,
                           'supports_undo': True,
                           'title': 'Build Blueprint Graph',
                           'ue_versions': ['5.6', '5.7', '5.8']},
 'compile_blueprint': {'asset_path_params': ['asset_path'],
                       'description': 'Compiles a Blueprint with stable structured compiler diagnostics and recovery actions.',
                       'effect': 'write',
                       'idempotent': False,
                       'required_plugins': [],
                       'requires_confirmation': True,
                       'result_kind': 'json',
                       'risk': 'high',
                       'supports_preview': False,
                       'supports_undo': False,
                       'title': 'Compile Blueprint',
                       'ue_versions': ['5.6', '5.7', '5.8']},
 'connect_blueprint_pins': {'asset_path_params': ['asset_path'],
                            'description': 'Connects two pins in a Blueprint graph.',
                            'effect': 'write',
                            'idempotent': False,
                            'required_plugins': [],
                            'requires_confirmation': True,
                            'result_kind': 'json',
                            'risk': 'medium',
                            'supports_preview': True,
                            'supports_undo': True,
                            'title': 'Connect Blueprint Pins',
                            'ue_versions': ['5.6', '5.7', '5.8']},
 'create_blueprint': {'asset_path_params': ['asset_path', 'parent_class_path'],
                      'description': 'Creates a Blueprint asset with the given parent class '
                                     '(default Actor).',
                      'effect': 'write',
                      'idempotent': False,
                      'required_plugins': [],
                      'requires_confirmation': True,
                      'result_kind': 'json',
                      'risk': 'medium',
                      'supports_preview': True,
                      'supports_undo': False,
                      'title': 'Create Blueprint',
                      'ue_versions': ['5.6', '5.7', '5.8']},
 'get_blueprint_graph_info': {'asset_path_params': ['asset_path'],
                              'description': 'Returns the full graph info for a Blueprint graph.',
                              'effect': 'read',
                              'idempotent': True,
                              'required_plugins': [],
                              'requires_confirmation': False,
                              'result_kind': 'json',
                              'risk': 'low',
                              'supports_preview': False,
                              'supports_undo': False,
                              'title': 'Get Blueprint Graph Info',
                              'ue_versions': ['5.6', '5.7', '5.8']},
 'get_selected_bp_node_infos': {'description': 'Returns compact blueprint node info optimized for '
                                               'LLM token efficiency.',
                                'effect': 'read',
                                'idempotent': True,
                                'required_plugins': [],
                                'requires_confirmation': False,
                                'result_kind': 'json',
                                'risk': 'low',
                                'supports_preview': False,
                                'supports_undo': False,
                                'title': 'Get Selected Bp Node Infos',
                                'ue_versions': ['5.6', '5.7', '5.8']},
 'get_selected_bp_nodes': {'description': 'Returns information about currently selected blueprint '
                                          'nodes in the editor.',
                           'effect': 'read',
                           'idempotent': True,
                           'required_plugins': [],
                           'requires_confirmation': False,
                           'result_kind': 'json',
                           'risk': 'low',
                           'supports_preview': False,
                           'supports_undo': False,
                           'title': 'Get Selected Bp Nodes',
                           'ue_versions': ['5.6', '5.7', '5.8']},
 'list_blueprint_components': {'asset_path_params': ['asset_path'],
                               'description': 'Lists all SCS components on a Blueprint.',
                               'effect': 'read',
                               'idempotent': True,
                               'required_plugins': [],
                               'requires_confirmation': False,
                               'result_kind': 'json',
                               'risk': 'low',
                               'supports_preview': False,
                               'supports_undo': False,
                               'title': 'List Blueprint Components',
                               'ue_versions': ['5.6', '5.7', '5.8']},
 'list_blueprint_variables': {'asset_path_params': ['asset_path'],
                              'description': 'Lists all variables defined in a Blueprint.',
                              'effect': 'read',
                              'idempotent': True,
                              'required_plugins': [],
                              'requires_confirmation': False,
                              'result_kind': 'json',
                              'risk': 'low',
                              'supports_preview': False,
                              'supports_undo': False,
                              'title': 'List Blueprint Variables',
                              'ue_versions': ['5.6', '5.7', '5.8']},
 'list_callable_functions': {'asset_path_params': ['asset_path'],
                             'description': 'Lists callable functions available in a Blueprint '
                                            'context.',
                             'effect': 'read',
                             'idempotent': True,
                             'required_plugins': [],
                             'requires_confirmation': False,
                             'result_kind': 'json',
                             'risk': 'low',
                             'supports_preview': False,
                             'supports_undo': False,
                             'title': 'List Callable Functions',
                             'ue_versions': ['5.6', '5.7', '5.8']},
 'remove_blueprint_node': {'asset_path_params': ['asset_path'],
                           'description': 'Removes a node from a Blueprint graph.',
                           'effect': 'destructive',
                           'idempotent': False,
                           'required_plugins': [],
                           'requires_confirmation': True,
                           'result_kind': 'json',
                           'risk': 'high',
                           'supports_preview': True,
                           'supports_undo': True,
                           'title': 'Remove Blueprint Node',
                           'ue_versions': ['5.6', '5.7', '5.8']},
 'remove_component_from_blueprint': {'asset_path_params': ['asset_path'],
                                     'description': 'Removes a component by variable name from a '
                                                    "Blueprint's SCS.",
                                     'effect': 'destructive',
                                     'idempotent': False,
                                     'required_plugins': [],
                                     'requires_confirmation': True,
                                     'result_kind': 'json',
                                     'risk': 'high',
                                     'supports_preview': True,
                                     'supports_undo': True,
                                     'title': 'Remove Component From Blueprint',
                                     'ue_versions': ['5.6', '5.7', '5.8']},
 'set_blueprint_node_position': {'asset_path_params': ['asset_path'],
                                 'description': 'Sets the canvas position of a node in a Blueprint '
                                                'graph.',
                                 'effect': 'write',
                                 'idempotent': False,
                                 'required_plugins': [],
                                 'requires_confirmation': True,
                                 'result_kind': 'json',
                                 'risk': 'medium',
                                 'supports_preview': True,
                                 'supports_undo': True,
                                 'title': 'Set Blueprint Node Position',
                                 'ue_versions': ['5.6', '5.7', '5.8']},
 'set_component_property': {'asset_path_params': ['asset_path'],
                            'description': 'Sets a property on a component template in a '
                                           "Blueprint's SCS.",
                            'effect': 'write',
                            'idempotent': False,
                            'required_plugins': [],
                            'requires_confirmation': True,
                            'result_kind': 'json',
                            'risk': 'medium',
                            'supports_preview': True,
                            'supports_undo': True,
                            'title': 'Set Component Property',
                            'ue_versions': ['5.6', '5.7', '5.8']},
 'set_variable_flags': {'asset_path_params': ['asset_path'],
                        'description': "Sets a Blueprint variable's 'Instance Editable' and/or "
                                       "'Expose On Spawn' flags.",
                        'effect': 'write',
                        'idempotent': False,
                        'required_plugins': [],
                        'requires_confirmation': True,
                        'result_kind': 'json',
                        'risk': 'medium',
                        'supports_preview': True,
                        'supports_undo': True,
                        'title': 'Set Variable Flags',
                        'ue_versions': ['5.6', '5.7', '5.8']}}
