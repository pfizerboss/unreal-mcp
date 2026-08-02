# Copyright (c) 2025 GenOrca. All Rights Reserved.

"""
End-to-end tests: MCP server dispatcher -> real TCP -> C++ server -> ue_* -> back.

Unlike test_dispatcher.py (which mocks the TCP layer), these run the FULL chain
with NO mocks. They prove the actual scenario an MCP client triggers:
routing + socket round-trip + C++ dispatch + response unwrapping.

Requires a running Unreal editor with the UnrealMCPython TCP server on :12029.
If the port is not reachable, the whole module is skipped (so CI / offline runs
stay green; run locally with the editor open to exercise these).

Run:
    cd mcp-server && uv run --extra dev pytest tests/test_e2e.py -v
"""

import asyncio
import socket
from uuid import uuid4

import pytest

import unreal_mcp.dispatcher as disp
from unreal_mcp.core import send_to_unreal
from unreal_mcp.dispatchers._catalog import CATALOG

HOST, PORT = "127.0.0.1", 12029

# The only action excluded from the exhaustive JSON round-trip returns a typed
# MCP Image and is covered by test_vision_capture_returns_image below.
_EXCLUDE = {
    ("vision", "capture_viewport"),
}


def _editor_reachable() -> bool:
    try:
        with socket.create_connection((HOST, PORT), timeout=0.5):
            return True
    except OSError:
        return False


pytestmark = pytest.mark.skipif(
    not _editor_reachable(),
    reason=f"Unreal TCP server not reachable on {HOST}:{PORT} (open the editor to run E2E).",
)

# Editor-crash guard: if the editor was up when the module started but dies
# mid-suite, every remaining test must FAIL (not skip, and not "pass" via a
# connection-error dict that happens to carry a 'success' key). A green E2E run
# must mean the editor survived the whole sweep — otherwise a `pytest && ...`
# release chain would happily commit/PR on top of a crash.
_EDITOR_WAS_UP = _editor_reachable()
_CONNECTION_ERROR_MARKERS = ("Connection refused", "ConnectionReset", "Socket timeout",
                             "No response received", "[WinError")


@pytest.fixture(autouse=True)
def _fail_if_editor_crashed():
    if _EDITOR_WAS_UP and not _editor_reachable():
        pytest.fail(f"Unreal editor crashed during the E2E suite "
                    f"({HOST}:{PORT} no longer reachable). Investigate before merging.")
    yield


def _assert_not_connection_error(r, label):
    if isinstance(r, dict):
        msg = str(r.get("message", ""))
        assert not any(m in msg for m in _CONNECTION_ERROR_MARKERS), \
            f"{label}: editor connection lost mid-call: {msg}"


def run(coro):
    return asyncio.run(coro)


def test_list_actors_round_trip():
    """A read action proves unwrapping: 'actors' is an INNER field of the action result."""
    r = run(disp._dispatch("actor", "list_all_with_locations", {}))
    assert r.get("success") is True
    assert "actors" in r, f"inner field missing — response not unwrapped: {r}"
    assert isinstance(r["actors"], list)


def test_spawn_and_delete_round_trip():
    spawn = run(disp._dispatch("actor", "spawn_from_class",
                               {"class_path": "/Script/Engine.PointLight",
                                "location": [0, 0, 700]}))
    assert spawn.get("success") is True, spawn
    label = spawn.get("actor_label")
    assert label, f"actor_label missing — not unwrapped: {spawn}"
    # cleanup through the same chain
    deleted = run(disp._dispatch("actor", "delete_by_label", {"actor_label": label}))
    assert deleted.get("success") is True, deleted


def test_inner_action_failure_is_surfaced():
    """Action failure must come back as success=False (not buried in a result string)."""
    r = run(disp._dispatch("actor", "set_transform",
                           {"actor_label": "NoSuchActor_E2E_XYZ", "location": [0, 0, 0]}))
    assert r.get("success") is False
    assert "message" in r


def test_list_actions_offline_path_still_works():
    """list_actions must not touch TCP even when the editor is up."""
    r = run(disp._dispatch("material", "list_actions", {}))
    assert r["success"] is True
    assert r["domain"] == "material"


def test_execute_python_round_trip():
    """execute_python runs real Unreal Python through the chain."""
    r = run(disp.util(action="execute_python",
                         params={"code": "print('e2e_marker_42')"}))
    # send_python_exec returns the raw wrapper; the printed marker is in 'result'.
    blob = r.get("result", "") + r.get("message", "")
    assert "e2e_marker_42" in blob, f"execute_python did not echo marker: {r}"


def test_vision_capture_returns_image():
    """vision capture_viewport returns an MCP Image (PNG) through the full chain."""
    from fastmcp.utilities.types import Image
    r = run(disp.vision(action="capture_viewport", params={"width": 320, "height": 180}))
    assert isinstance(r, Image), f"expected an Image, got {type(r).__name__}: {r}"
    # the PNG bytes should carry a valid signature
    data = getattr(r, "data", b"")
    assert data[:4] == b"\x89PNG", "capture did not return a PNG"


def test_gltf_import_round_trip():
    """glTF import via the deferred-tick path: export the engine Cube to .glb, import it,
    then poll get_gltf_import_status until done. Each dispatch is its own game-thread task,
    so the editor ticks between calls and the Interchange async import can run + complete."""
    import time

    # 1. export /Engine/BasicShapes/Cube to a temp .glb (via execute_python)
    export_code = (
        "import unreal, os\n"
        "glb = os.path.join(unreal.Paths.project_saved_dir(), 'MCP_E2E_gltf.glb').replace(chr(92), '/')\n"
        "cube = unreal.EditorAssetLibrary.load_asset('/Engine/BasicShapes/Cube')\n"
        "t = unreal.AssetExportTask(); t.object = cube; t.filename = glb\n"
        "t.automated = True; t.replace_identical = True; t.prompt = False\n"
        "ok = unreal.Exporter.run_asset_export_task(t)\n"
        "print('GLBPATH=' + glb if (ok and os.path.isfile(glb)) else 'GLBFAIL')\n"
    )
    r = run(disp.util(action="execute_python", params={"code": export_code}))
    blob = (r.get("result", "") or "") + (r.get("message", "") or "")
    assert "GLBPATH=" in blob, f"glb export failed: {blob[:300]}"
    glb = blob.split("GLBPATH=", 1)[1].split()[0].strip().strip('"')

    dest = "/Game/Tests/MCP_E2E/glb"
    try:
        imp = run(disp._dispatch("asset", "import_gltf",
                                 {"file_path": glb, "destination_path": dest}))
        assert imp.get("success") and imp.get("pending"), f"schedule failed: {imp}"

        st = {}
        for _ in range(40):  # ~20s budget for the async Interchange import
            st = run(disp._dispatch("asset", "get_gltf_import_status",
                                    {"destination_path": dest}))
            _assert_not_connection_error(st, "get_gltf_import_status")
            if st.get("done"):
                break
            time.sleep(0.5)
        assert st.get("success") and st.get("done"), f"import did not complete: {st}"
        classes = [a["class"] for a in st["imported_assets"]]
        assert "StaticMesh" in classes, f"no StaticMesh among imported: {st}"
    finally:
        run(disp.util(action="execute_python", params={
            "code": f"import unreal; unreal.EditorAssetLibrary.delete_directory('{dest}')"}))


# ── exhaustive: every catalog action survives the full chain ───────────────────

def _all_action_pairs():
    pairs = []
    for domain, actions in CATALOG.items():
        for action in actions:
            if (domain, action) in _EXCLUDE:
                continue
            pairs.append((domain, action))
    return pairs


@pytest.mark.parametrize("domain,action", _all_action_pairs())
def test_every_action_round_trips(domain, action):
    """
    Drive every action through the real chain with empty params and assert we get
    back an unwrapped dict containing 'success'. This proves routing + TCP +
    C++ dispatch + result unwrapping work for the action — independent of whether
    the action *succeeds* with no args (validation failures still return a dict).

    Empty params are safe: execute_action wraps any exception (incl. missing-arg
    TypeError) as {"success": false, ...}, and ue_* functions validate required
    params before doing work.
    """
    if domain == "util":
        r = run(disp.util(action=action, params={}))
    elif domain == "vision":
        r = run(disp.vision(action=action, params={}))
    elif domain == "workflow":
        r = run(disp.workflow(action=action, params={}))
    else:
        r = run(disp._dispatch(domain, action, {}))
    assert isinstance(r, dict), f"{domain}.{action} returned non-dict: {r!r}"
    assert "success" in r, f"chain/unwrap failed for {domain}.{action}: {r!r}"
    _assert_not_connection_error(r, f"{domain}.{action}")


def test_workflow_plan_apply_undo_round_trip():
    """A live workflow commits an actor transform, exposes undo metadata, and cleans its lease."""
    actor_label = None
    spawn_location = [812345.0, -812345.0, 34567.0]
    try:
        spawn = run(disp._dispatch(
            "actor",
            "spawn_from_class",
            {
                "class_path": "/Script/Engine.PointLight",
                "location": spawn_location,
            },
        ))
        assert spawn.get("success") is True, spawn
        actor_label = spawn.get("actor_label")
        assert actor_label, spawn

        distinct_label = f"MCP_Workflow_E2E_Disposable_{uuid4().hex}"
        renamed = run(disp._dispatch(
            "actor",
            "rename_actor",
            {"actor_label": actor_label, "new_label": distinct_label},
        ))
        assert renamed.get("success") is True, renamed
        actor_label = renamed["new_label"]

        original = run(disp._dispatch(
            "actor", "get_transform", {"actor_label": actor_label}
        ))
        assert original.get("success") is True, original
        target_location = [
            original["location"][0] + 1111.0,
            original["location"][1] + 2222.0,
            original["location"][2] + 3333.0,
        ]

        planned = run(disp.workflow(action="plan", params={
            "operations": [
                {
                    "id": "move-disposable-actor",
                    "domain": "actor",
                    "action": "set_transform",
                    "params": {
                        "actor_label": actor_label,
                        "location": target_location,
                        "rotation": original["rotation"],
                        "scale": original["scale"],
                    },
                }
            ]
        }))
        assert planned.get("success") is True, planned
        plan_id = planned["data"]["workflow_id"]
        confirmation_token = planned["data"]["confirmation_token"]

        applied = run(disp.workflow(action="apply", params={
            "plan_id": plan_id,
            "confirmation_token": confirmation_token,
            "wait_for_completion": True,
        }))
        assert applied.get("success") is True, applied
        assert applied.get("status") in {"succeeded", "needs_attention"}, applied

        moved = run(disp._dispatch(
            "actor", "get_transform", {"actor_label": actor_label}
        ))
        assert moved.get("success") is True, moved
        assert moved["location"] == pytest.approx(target_location, abs=0.001)

        commit = applied["data"]
        undo_token = commit.get("undo_token")
        expects_undo_token = (
            commit.get("transaction_recorded") is True
            and commit.get("undo_available") is True
        )
        assert bool(undo_token) is expects_undo_token, applied

        if undo_token:
            undone = run(disp.workflow(action="undo", params={
                "plan_id": plan_id,
                "undo_token": undo_token,
            }))
            assert undone.get("success") is True, undone
            assert undone.get("data", {}).get("undone") is True, undone

            restored = run(disp._dispatch(
                "actor", "get_transform", {"actor_label": actor_label}
            ))
            assert restored.get("success") is True, restored
            for field in ("location", "rotation", "scale"):
                assert restored[field] == pytest.approx(original[field], abs=0.001)

        current = run(disp.workflow(
            action="get", params={"plan_id": plan_id}
        ))
        assert current.get("success") is True, current
        assert current["data"]["workflow"]["id"] == plan_id
        assert current["data"]["workflow"]["status"] == applied["status"]
        if undo_token:
            assert current["data"]["workflow"]["undo_token"] == ""

        context = run(send_to_unreal(
            "UnrealMCPython.workflow_actions",
            "ue_get_editor_context",
            {"asset_paths": []},
        ))
        assert context.get("success") is True, context
        lease = context["workflow_transaction"]
        assert lease["active"] is False, lease
        assert lease["watchdog_registered"] is False, lease
    finally:
        if actor_label:
            run(disp._dispatch(
                "actor", "delete_by_label", {"actor_label": actor_label}
            ))


def test_blueprint2_workflow_round_trip():
    """A mixed Blueprint workflow compiles, diffs, and restores its graph."""
    asset_path = f"/Game/__MCPTests/Blueprint2_{uuid4().hex}"
    try:
        created = run(disp._dispatch(
            "blueprint",
            "create_blueprint",
            {"asset_path": asset_path, "parent_class_path": "/Script/Engine.Actor"},
        ))
        assert created.get("success") is True, created
        assert created.get("saved") is False, created
        saved = run(disp._dispatch(
            "asset", "save_asset", {"asset_path": asset_path}
        ))
        assert saved.get("success") is True, saved

        brief = run(disp._dispatch(
            "blueprint", "get_blueprint_brief", {"asset_path": asset_path}
        ))
        assert brief.get("success") is True, brief
        assert "EventGraph" in brief["data"]["graphs"], brief
        inspected = run(disp._dispatch(
            "blueprint",
            "inspect_blueprint",
            {
                "asset_path": asset_path,
                "queries": [{"op": "events", "detail": "detailed", "limit": 100}],
            },
        ))
        assert inspected.get("success") is True, inspected
        event_items = inspected["data"]["results"][0]["items"]
        assert event_items, inspected
        graph_info = run(disp._dispatch(
            "blueprint",
            "get_blueprint_graph_info",
            {"asset_path": asset_path, "graph_name": "EventGraph"},
        ))
        assert graph_info.get("success") is True, graph_info
        graph_id = graph_info["graph_id"]
        assert graph_id.startswith("graph:"), graph_info
        assert not graph_id.startswith("fallback:"), graph_info
        assert graph_info["id_kind"] == "graph_guid", graph_info
        assert graph_info["stable"] is True, graph_info
        event_graphs = [
            item for item in event_items
            if item.get("graph_id") == graph_id
        ]
        assert event_graphs, (inspected, graph_info)
        event_record = event_graphs[0]
        assert event_record["graph_id"] == graph_id, event_record
        assert event_record["graph_id_kind"] == "graph_guid", event_record
        assert event_record["graph_stable"] is True, event_record

        pre = run(disp._dispatch(
            "blueprint",
            "snapshot_blueprint_graph",
            {"asset_path": asset_path, "graph_ids": [graph_id]},
        ))
        assert pre.get("success") is True, pre

        operations = [
            {
                "id": "create-function",
                "domain": "blueprint",
                "action": "create_blueprint_function",
                "params": {
                    "asset_path": asset_path,
                    "function_name": "ComputeValue",
                    "inputs": [],
                    "outputs": [{"name": "Value", "type": {"kind": "int"}}],
                },
            },
            {
                "id": "add-variable",
                "domain": "blueprint",
                "action": "add_variable",
                "params": {
                    "asset_path": asset_path,
                    "variable_name": "Counter",
                    "variable_type": "int",
                },
            },
            {
                "id": "add-component",
                "domain": "blueprint",
                "action": "add_component_to_blueprint",
                "params": {
                    "asset_path": asset_path,
                    "component_class_path": "/Script/Engine.SceneComponent",
                    "component_name": "WorkflowRoot",
                },
            },
            {
                "id": "build-graph",
                "domain": "blueprint",
                "action": "build_blueprint_graph",
                "params": {
                    "asset_path": asset_path,
                    "graph_name": "EventGraph",
                    "graph_structure": {
                        "nodes": [
                            {"id": "branch", "type": "Branch"},
                            {"id": "sequence", "type": "Sequence"},
                            {
                                "id": "actor_location",
                                "type": "CallFunction",
                                "target": "Actor",
                                "function_name": "K2_GetActorLocation",
                            },
                        ],
                        "connections": [
                            {
                                "source_node": "branch",
                                "source_pin": "then",
                                "target_node": "sequence",
                                "target_pin": "execute",
                            }
                        ],
                    },
                },
            },
        ]
        planned = run(disp.workflow(action="plan", params={"operations": operations}))
        assert planned.get("success") is True, planned
        plan_id = planned["data"]["workflow_id"]
        applied = run(disp.workflow(action="apply", params={
            "plan_id": plan_id,
            "confirmation_token": planned["data"]["confirmation_token"],
            "wait_for_completion": True,
        }))
        assert applied.get("success") is True, applied
        assert applied["data"].get("transaction_recorded") is True, applied
        undo_token = applied["data"].get("undo_token")
        assert undo_token, applied

        compiled = run(disp._dispatch(
            "blueprint", "compile_blueprint", {"asset_path": asset_path}
        ))
        assert compiled.get("success") is True, compiled
        health = run(disp._dispatch(
            "blueprint", "get_blueprint_health", {"asset_path": asset_path}
        ))
        assert health.get("success") is True, health
        assert health["data"]["healthy"] is True, health

        post = run(disp._dispatch(
            "blueprint",
            "snapshot_blueprint_graph",
            {"asset_path": asset_path, "graph_ids": [graph_id]},
        ))
        assert post.get("success") is True, post
        changed = run(disp._dispatch(
            "blueprint",
            "diff_blueprint_graphs",
            {
                "before_snapshot": pre["data"],
                "after_snapshot": post["data"],
                "queries": [],
            },
        ))
        assert changed.get("success") is True, changed
        assert any(
            section["total_count"] > 0
            for section in changed["data"]["sections"]
        ), changed

        undone = run(disp.workflow(action="undo", params={
            "plan_id": plan_id,
            "undo_token": undo_token,
        }))
        assert undone.get("success") is True, undone
        restored = run(disp._dispatch(
            "blueprint",
            "snapshot_blueprint_graph",
            {"asset_path": asset_path, "graph_ids": [graph_id]},
        ))
        assert restored.get("success") is True, restored
        assert restored["data"] == pre["data"], (pre, restored)
    finally:
        present = run(disp._dispatch(
            "asset", "asset_exists", {"asset_path": asset_path}
        ))
        if present.get("exists") is True:
            deleted = run(disp._dispatch(
                "asset", "delete_asset", {"asset_path": asset_path}
            ))
            assert deleted.get("success") is True, deleted
        exists = run(disp._dispatch(
            "asset", "asset_exists", {"asset_path": asset_path}
        ))
        assert exists.get("exists") is False, exists
        canary = run(disp._dispatch("actor", "list_all_with_locations", {}))
        assert canary.get("success") is True, canary


def test_blueprint_palette_round_trip():
    """Native palette capabilities survive the full MCP and workflow chain."""
    asset_path = f"/Game/__MCPTests/BlueprintPalette_{uuid4().hex}"

    def checked(result, label):
        _assert_not_connection_error(result, label)
        return result

    def search_location_action():
        cursor = ""
        seen_action_ids = set()
        seen_cursors = set()
        followed_cursor = False
        found_action = None
        for _ in range(20):
            if cursor:
                assert cursor not in seen_cursors, cursor
                seen_cursors.add(cursor)
            searched = checked(run(disp._dispatch(
                "blueprint",
                "search_blueprint_node_actions",
                {
                    "asset_path": asset_path,
                    "graph_id": graph_id,
                    "query": "Get Actor Location",
                    "filters": {"action_kinds": ["function"]},
                    "cursor": cursor,
                    "limit": 1,
                },
            )), "search_blueprint_node_actions")
            assert searched.get("success") is True, searched
            for item in searched["data"]["items"]:
                action_id = item["action_id"]
                assert action_id not in seen_action_ids, searched
                seen_action_ids.add(action_id)
                if item["member_path"].endswith(":K2_GetActorLocation"):
                    found_action = item
            next_cursor = searched["data"]["next_cursor"]
            if found_action is not None and (followed_cursor or not next_cursor):
                return found_action
            if not next_cursor:
                break
            assert next_cursor not in seen_cursors, searched
            cursor = next_cursor
            followed_cursor = True
        pytest.fail("Get Actor Location was not found through bounded pages")

    try:
        created = checked(run(disp._dispatch(
            "blueprint",
            "create_blueprint",
            {
                "asset_path": asset_path,
                "parent_class_path": "/Script/Engine.Actor",
            },
        )), "create_blueprint")
        assert created.get("success") is True, created
        assert created.get("saved") is False, created

        inspected = checked(run(disp._dispatch(
            "blueprint",
            "inspect_blueprint",
            {
                "asset_path": asset_path,
                "queries": [
                    {"op": "events", "detail": "detailed", "limit": 100}
                ],
            },
        )), "inspect_blueprint graph")
        assert inspected.get("success") is True, inspected
        event_items = inspected["data"]["results"][0]["items"]
        graph_ids = {
            item["graph_id"]
            for item in event_items
            if item.get("graph_id", "").startswith("graph:")
        }
        assert len(graph_ids) == 1, inspected
        graph_id = next(iter(graph_ids))
        assert all(
            item["graph_id_kind"] == "graph_guid"
            and item["graph_stable"] is True
            for item in event_items
            if item.get("graph_id") == graph_id
        ), inspected

        action = search_location_action()
        described = checked(run(disp._dispatch(
            "blueprint",
            "describe_blueprint_node_action",
            {"action_id": action["action_id"]},
        )), "describe_blueprint_node_action")
        assert described.get("success") is True, described
        assert described["data"]["action_id"] == action["action_id"]

        initial = checked(run(disp._dispatch(
            "blueprint",
            "snapshot_blueprint_graph",
            {"asset_path": asset_path, "graph_ids": [graph_id]},
        )), "snapshot_blueprint_graph initial")
        assert initial.get("success") is True, initial

        spawned = checked(run(disp._dispatch(
            "blueprint",
            "add_blueprint_action_node",
            {
                "asset_path": asset_path,
                "graph_id": graph_id,
                "action_id": action["action_id"],
                "position": {"x": 432, "y": 176},
                "bindings": [
                    item["binding_id"] for item in action["bindings"]
                ],
            },
        )), "add_blueprint_action_node")
        assert spawned.get("success") is True, spawned
        assert spawned.get("saved") is not True, spawned
        node_id = spawned["data"]["node_id"]
        pin_ids = spawned["data"]["pin_ids"]
        assert node_id.startswith("node:"), spawned
        assert pin_ids and all(pin.startswith("pin:") for pin in pin_ids)

        node_inspection = checked(run(disp._dispatch(
            "blueprint",
            "inspect_blueprint",
            {
                "asset_path": asset_path,
                "queries": [
                    {
                        "op": "nodes",
                        "member_id": node_id,
                        "detail": "detailed",
                    },
                    {
                        "op": "pins",
                        "node_id": node_id,
                        "detail": "detailed",
                        "limit": 500,
                    },
                ],
            },
        )), "inspect_blueprint spawned node")
        assert node_inspection.get("success") is True, node_inspection
        nodes = node_inspection["data"]["results"][0]["items"]
        pins = node_inspection["data"]["results"][1]["items"]
        assert len(nodes) == 1 and nodes[0]["node_id"] == node_id
        assert {pin["pin_id"] for pin in pins} == set(pin_ids)

        suggested = checked(run(disp._dispatch(
            "blueprint",
            "suggest_blueprint_nodes_for_pin",
            {
                "asset_path": asset_path,
                "graph_id": graph_id,
                "pin_id": pin_ids[0],
                "query": "",
                "cursor": "",
                "limit": 25,
            },
        )), "suggest_blueprint_nodes_for_pin")
        assert suggested.get("success") is True, suggested
        assert suggested["data"]["source_pin_id"] == pin_ids[0]
        assert suggested["data"]["items"], suggested

        compiled = checked(run(disp._dispatch(
            "blueprint", "compile_blueprint", {"asset_path": asset_path}
        )), "compile_blueprint")
        assert compiled.get("success") is True, compiled
        health = checked(run(disp._dispatch(
            "blueprint", "get_blueprint_health", {"asset_path": asset_path}
        )), "get_blueprint_health")
        assert health.get("success") is True, health
        assert health["data"]["healthy"] is True, health

        saved = checked(run(disp._dispatch(
            "asset", "save_asset", {"asset_path": asset_path}
        )), "save_asset before workflow")
        assert saved.get("success") is True, saved

        workflow_action = search_location_action()
        pre_workflow = checked(run(disp._dispatch(
            "blueprint",
            "snapshot_blueprint_graph",
            {"asset_path": asset_path, "graph_ids": [graph_id]},
        )), "snapshot_blueprint_graph before workflow")
        assert pre_workflow.get("success") is True, pre_workflow

        planned = checked(run(disp.workflow(action="plan", params={
            "operations": [
                {
                    "id": "spawn-palette-node",
                    "domain": "blueprint",
                    "action": "add_blueprint_action_node",
                    "params": {
                        "asset_path": asset_path,
                        "graph_id": graph_id,
                        "action_id": workflow_action["action_id"],
                        "position": {"x": 912, "y": 432},
                        "bindings": [
                            item["binding_id"]
                            for item in workflow_action["bindings"]
                        ],
                    },
                }
            ]
        })), "workflow.plan palette spawn")
        assert planned.get("success") is True, planned
        plan_id = planned["data"]["workflow_id"]
        applied = checked(run(disp.workflow(action="apply", params={
            "plan_id": plan_id,
            "confirmation_token": planned["data"]["confirmation_token"],
            "wait_for_completion": True,
        })), "workflow.apply palette spawn")
        assert applied.get("success") is True, applied
        assert applied["data"].get("transaction_recorded") is True, applied
        assert applied.get("saved") is not True, applied
        undo_token = applied["data"].get("undo_token")
        assert undo_token, applied

        applied_snapshot = checked(run(disp._dispatch(
            "blueprint",
            "snapshot_blueprint_graph",
            {"asset_path": asset_path, "graph_ids": [graph_id]},
        )), "snapshot_blueprint_graph after workflow apply")
        assert applied_snapshot.get("success") is True, applied_snapshot
        applied_diff = checked(run(disp._dispatch(
            "blueprint",
            "diff_blueprint_graphs",
            {
                "before_snapshot": pre_workflow["data"],
                "after_snapshot": applied_snapshot["data"],
                "queries": [],
            },
        )), "diff_blueprint_graphs after workflow apply")
        assert applied_diff.get("success") is True, applied_diff
        assert any(
            section["total_count"] > 0
            for section in applied_diff["data"]["sections"]
        ), applied_diff

        undone = checked(run(disp.workflow(action="undo", params={
            "plan_id": plan_id,
            "undo_token": undo_token,
        })), "workflow.undo palette spawn")
        assert undone.get("success") is True, undone

        restored = checked(run(disp._dispatch(
            "blueprint",
            "snapshot_blueprint_graph",
            {"asset_path": asset_path, "graph_ids": [graph_id]},
        )), "snapshot_blueprint_graph restored")
        assert restored.get("success") is True, restored
        assert restored["data"] == pre_workflow["data"], (
            pre_workflow,
            restored,
        )
        diff = checked(run(disp._dispatch(
            "blueprint",
            "diff_blueprint_graphs",
            {
                "before_snapshot": pre_workflow["data"],
                "after_snapshot": restored["data"],
                "queries": [],
            },
        )), "diff_blueprint_graphs restored workflow")
        assert diff.get("success") is True, diff
        assert all(
            section["total_count"] == 0
            for section in diff["data"]["sections"]
        ), diff
    finally:
        present = checked(run(disp._dispatch(
            "asset", "asset_exists", {"asset_path": asset_path}
        )), "asset_exists palette cleanup")
        if present.get("exists") is True:
            deleted = checked(run(disp._dispatch(
                "asset", "delete_asset", {"asset_path": asset_path}
            )), "delete_asset palette cleanup")
            assert deleted.get("success") is True, deleted
        absent = checked(run(disp._dispatch(
            "asset", "asset_exists", {"asset_path": asset_path}
        )), "asset_exists after palette cleanup")
        assert absent.get("exists") is False, absent


def test_blueprint_semantic_graph_editing_round_trip():
    """Five semantic Blueprint actions survive MCP, workflow, and cleanup."""
    asset_path = f"/Game/__MCPTests/BlueprintSemantic_{uuid4().hex}"

    def checked(result, label):
        _assert_not_connection_error(result, label)
        return result

    def dispatch(action, params):
        return checked(
            run(disp._dispatch("blueprint", action, params)), action
        )

    def inspect(queries):
        result = dispatch(
            "inspect_blueprint",
            {"asset_path": asset_path, "queries": queries},
        )
        assert result.get("success") is True, result
        return result["data"]["results"]

    def search(query, predicate):
        cursor = ""
        for _ in range(50):
            result = dispatch(
                "search_blueprint_node_actions",
                {
                    "asset_path": asset_path,
                    "graph_id": graph_id,
                    "query": query,
                    "filters": {},
                    "cursor": cursor,
                    "limit": 200,
                },
            )
            assert result.get("success") is True, result
            for item in result["data"]["items"]:
                if predicate(item):
                    return item
            cursor = result["data"]["next_cursor"]
            if not cursor:
                break
        pytest.fail(f"No semantic palette action matched {query!r}")

    def suggest_pin(pin_id, query):
        result = dispatch(
            "suggest_blueprint_nodes_for_pin",
            {
                "asset_path": asset_path,
                "graph_id": graph_id,
                "pin_id": pin_id,
                "query": query,
                "cursor": "",
                "limit": 200,
            },
        )
        assert result.get("success") is True, result
        return result

    def suggest_connection(source_pin_id, target_pin_id, query, predicate):
        cursor = ""
        for _ in range(50):
            result = dispatch(
                "suggest_blueprint_nodes_for_connection",
                {
                    "asset_path": asset_path,
                    "graph_id": graph_id,
                    "source_pin_id": source_pin_id,
                    "target_pin_id": target_pin_id,
                    "query": query,
                    "filters": {},
                    "allow_conversion": False,
                    "cursor": cursor,
                    "limit": 200,
                },
            )
            assert result.get("success") is True, result
            for item in result["data"]["items"]:
                if predicate(item) and item["binding_pairs"]:
                    return item, item["binding_pairs"][0]
            cursor = result["data"]["next_cursor"]
            if not cursor:
                break
        pytest.fail(f"No connection action matched {query!r}")

    def print_suggestion(pin_id):
        result = suggest_pin(pin_id, "Print String")
        for item in result["data"]["items"]:
            if item["member_path"].endswith(":PrintString"):
                assert item["connection_bindings"], item
                return item, item["connection_bindings"][0]
        pytest.fail(f"Print String is not compatible with {pin_id}: {result}")

    try:
        created = dispatch(
            "create_blueprint",
            {
                "asset_path": asset_path,
                "parent_class_path": "/Script/Engine.Actor",
            },
        )
        assert created.get("success") is True, created
        assert created.get("saved") is not True, created

        events = inspect(
            [{"op": "events", "detail": "detailed", "limit": 100}]
        )[0]["items"]
        graph_ids = {
            item["graph_id"]
            for item in events
            if item.get("graph_id", "").startswith("graph:")
        }
        assert len(graph_ids) == 1, events
        graph_id = next(iter(graph_ids))
        nodes = inspect(
            [
                {
                    "op": "nodes",
                    "graph_id": graph_id,
                    "detail": "detailed",
                    "limit": 500,
                }
            ]
        )[0]["items"]
        event_output = next(
            pin
            for node in nodes
            for pin in node.get("pins", [])
            if pin["direction"] == "output"
            and pin["type"]["kind"] == "exec"
        )
        event_pin_id = event_output["pin_id"]

        print_action, print_binding = print_suggestion(event_pin_id)
        connected = dispatch(
            "add_blueprint_connected_action_node",
            {
                "asset_path": asset_path,
                "graph_id": graph_id,
                "pin_id": event_pin_id,
                "action_id": print_action["action_id"],
                "connection_binding_id": print_binding["binding_id"],
                "position": {"x": 560, "y": 160},
                "allow_conversion": False,
                "bindings": [
                    item["binding_id"] for item in print_action["bindings"]
                ],
            },
        )
        assert connected.get("success") is True, connected
        assert connected["data"]["saved"] is False, connected
        target_node_id = connected["data"]["node_id"]
        target_input = next(
            pin
            for pin in connected["data"]["pins"]
            if pin["direction"] == "input" and pin["type"]["kind"] == "exec"
        )

        sequence, pair = suggest_connection(
            event_pin_id,
            target_input["id"],
            "Sequence",
            lambda item: item["node_class_path"].endswith(
                "K2Node_ExecutionSequence"
            ),
        )
        inserted = dispatch(
            "insert_blueprint_action_node",
            {
                "asset_path": asset_path,
                "graph_id": graph_id,
                "source_pin_id": event_pin_id,
                "target_pin_id": target_input["id"],
                "action_id": sequence["action_id"],
                "input_binding_id": pair["input_binding_id"],
                "output_binding_id": pair["output_binding_id"],
                "position": {"x": 320, "y": 160},
                "bindings": [
                    item["binding_id"] for item in sequence["bindings"]
                ],
            },
        )
        assert inserted.get("success") is True, inserted
        assert inserted["data"]["saved"] is False, inserted

        used_pin_ids = {
            edge[endpoint]
            for edge in inserted["data"]["connections"]
            for endpoint in ("source_pin_id", "target_pin_id")
        }
        free_sequence_output = next(
            pin
            for pin in inserted["data"]["pins"]
            if pin["direction"] == "output"
            and pin["type"]["kind"] == "exec"
            and pin["id"] not in used_pin_ids
        )
        second_action, second_binding = print_suggestion(
            free_sequence_output["id"]
        )
        second_connected = dispatch(
            "add_blueprint_connected_action_node",
            {
                "asset_path": asset_path,
                "graph_id": graph_id,
                "pin_id": free_sequence_output["id"],
                "action_id": second_action["action_id"],
                "connection_binding_id": second_binding["binding_id"],
                "position": {"x": 620, "y": 420},
                "allow_conversion": False,
                "bindings": [],
            },
        )
        assert second_connected.get("success") is True, second_connected
        assert second_connected["data"]["saved"] is False, second_connected
        workflow_pin = next(
            pin
            for pin in second_connected["data"]["pins"]
            if pin["direction"] == "output" and pin["type"]["kind"] == "exec"
        )

        semantic_snapshot = dispatch(
            "snapshot_blueprint_graph",
            {"asset_path": asset_path, "graph_ids": [graph_id]},
        )
        assert semantic_snapshot.get("success") is True, semantic_snapshot

        replacement_action = search(
            "Print String",
            lambda item: item["member_path"].endswith(":PrintString")
            and not item["requires_binding"],
        )
        strict_preview = dispatch(
            "preview_blueprint_action_replacement",
            {
                "asset_path": asset_path,
                "graph_id": graph_id,
                "node_id": target_node_id,
                "action_id": replacement_action["action_id"],
                "bindings": [],
                "pin_mapping": [],
                "allow_conversion": False,
                "allow_loss": False,
            },
        )
        assert strict_preview.get("success") is True, strict_preview
        assert strict_preview["data"]["applicable"] is True, strict_preview
        strict_apply = dispatch(
            "replace_blueprint_node_with_action",
            {
                "asset_path": asset_path,
                "graph_id": graph_id,
                "replacement_plan_id": strict_preview["data"][
                    "replacement_plan_id"
                ],
                "allow_loss": False,
            },
        )
        assert strict_apply.get("success") is True, strict_apply
        assert strict_apply["data"]["saved"] is False, strict_apply

        branch_action = search(
            "Branch",
            lambda item: item["node_class_path"].endswith("K2Node_IfThenElse"),
        )
        branch = dispatch(
            "add_blueprint_action_node",
            {
                "asset_path": asset_path,
                "graph_id": graph_id,
                "action_id": branch_action["action_id"],
                "position": {"x": 960, "y": 240},
                "bindings": [],
            },
        )
        assert branch.get("success") is True, branch
        condition = next(
            pin
            for pin in branch["data"]["pins"]
            if pin["direction"] == "input" and pin["type"]["kind"] == "bool"
        )
        changed = dispatch(
            "set_blueprint_node_properties",
            {
                "asset_path": asset_path,
                "node_id": branch["data"]["node_id"],
                "properties": {"pin_defaults": {condition["id"]: True}},
            },
        )
        assert changed.get("success") is True, changed
        loss_lists = []
        lossy_preview = None
        for allow_loss in (False, True):
            preview = dispatch(
                "preview_blueprint_action_replacement",
                {
                    "asset_path": asset_path,
                    "graph_id": graph_id,
                    "node_id": branch["data"]["node_id"],
                    "action_id": replacement_action["action_id"],
                    "bindings": [],
                    "pin_mapping": [],
                    "allow_conversion": False,
                    "allow_loss": allow_loss,
                },
            )
            assert preview.get("success") is True, preview
            assert preview["data"]["applicable"] is allow_loss, preview
            loss_lists.append(
                (
                    preview["data"]["unmapped_connections"],
                    preview["data"]["unmapped_defaults"],
                )
            )
            if allow_loss:
                lossy_preview = preview
        assert loss_lists[0] == loss_lists[1], loss_lists
        assert lossy_preview is not None
        lossy_apply = dispatch(
            "replace_blueprint_node_with_action",
            {
                "asset_path": asset_path,
                "graph_id": graph_id,
                "replacement_plan_id": lossy_preview["data"][
                    "replacement_plan_id"
                ],
                "allow_loss": True,
            },
        )
        assert lossy_apply.get("success") is True, lossy_apply
        assert lossy_apply["warnings"], lossy_apply
        assert lossy_apply["data"]["saved"] is False, lossy_apply

        compiled = dispatch("compile_blueprint", {"asset_path": asset_path})
        assert compiled.get("success") is True, compiled
        health = dispatch("get_blueprint_health", {"asset_path": asset_path})
        assert health.get("success") is True, health
        assert health["data"]["healthy"] is True, health

        saved = checked(
            run(
                disp._dispatch(
                    "asset", "save_asset", {"asset_path": asset_path}
                )
            ),
            "save_asset before semantic workflow",
        )
        assert saved.get("success") is True, saved

        workflow_action, workflow_binding = print_suggestion(
            workflow_pin["id"]
        )
        before_workflow = dispatch(
            "snapshot_blueprint_graph",
            {"asset_path": asset_path, "graph_ids": [graph_id]},
        )
        operations = [
            {
                "id": "semantic-connected-spawn",
                "domain": "blueprint",
                "action": "add_blueprint_connected_action_node",
                "params": {
                    "asset_path": asset_path,
                    "graph_id": graph_id,
                    "pin_id": workflow_pin["id"],
                    "action_id": workflow_action["action_id"],
                    "connection_binding_id": workflow_binding["binding_id"],
                    "position": {"x": 900, "y": 420},
                    "allow_conversion": False,
                    "bindings": [],
                },
            }
        ]
        planned = checked(
            run(disp.workflow(action="plan", params={"operations": operations})),
            "workflow.plan semantic",
        )
        assert planned.get("success") is True, planned
        applied = checked(
            run(
                disp.workflow(
                    action="apply",
                    params={
                        "plan_id": planned["data"]["workflow_id"],
                        "confirmation_token": planned["data"][
                            "confirmation_token"
                        ],
                        "wait_for_completion": True,
                    },
                )
            ),
            "workflow.apply semantic",
        )
        assert applied.get("success") is True, applied
        assert applied["data"].get("transaction_recorded") is True, applied
        undo_token = applied["data"].get("undo_token")
        assert undo_token, applied
        undone = checked(
            run(
                disp.workflow(
                    action="undo",
                    params={
                        "plan_id": planned["data"]["workflow_id"],
                        "undo_token": undo_token,
                    },
                )
            ),
            "workflow.undo semantic",
        )
        assert undone.get("success") is True, undone
        restored = dispatch(
            "snapshot_blueprint_graph",
            {"asset_path": asset_path, "graph_ids": [graph_id]},
        )
        assert restored["data"] == before_workflow["data"], (
            before_workflow,
            restored,
        )
        diff = dispatch(
            "diff_blueprint_graphs",
            {
                "before_snapshot": before_workflow["data"],
                "after_snapshot": restored["data"],
                "queries": [],
            },
        )
        assert all(
            section["total_count"] == 0
            for section in diff["data"]["sections"]
        ), diff
    finally:
        present = checked(
            run(
                disp._dispatch(
                    "asset", "asset_exists", {"asset_path": asset_path}
                )
            ),
            "asset_exists semantic cleanup",
        )
        if present.get("exists") is True:
            deleted = checked(
                run(
                    disp._dispatch(
                        "asset", "delete_asset", {"asset_path": asset_path}
                    )
                ),
                "delete_asset semantic cleanup",
            )
            assert deleted.get("success") is True, deleted
        absent = checked(
            run(
                disp._dispatch(
                    "asset", "asset_exists", {"asset_path": asset_path}
                )
            ),
            "asset_exists after semantic cleanup",
        )
        assert absent.get("exists") is False, absent
        assert _editor_reachable(), "Editor unreachable after semantic cleanup"


def test_zzz_editor_survived_suite():
    """Last test in the file: the editor must still be alive after the full sweep."""
    assert _editor_reachable(), "Unreal editor is no longer reachable after the E2E suite (it crashed mid-run)."
