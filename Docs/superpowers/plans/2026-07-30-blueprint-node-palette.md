# Blueprint Node Palette Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add four safe, context-aware MCP actions that search Unreal Engine's real Blueprint node palette, describe native actions, spawn a selected action, and suggest actions compatible with a stable pin.

**Architecture:** Enumerate `FBlueprintActionDatabase` and apply `FBlueprintActionFilter` in a focused editor-only C++ adapter. Keep action and cursor capabilities in the shared Blueprint 2.0 internal layer as bounded, editor-session-scoped primitive records; re-resolve native spawners before every mutation. Expose thin Python wrappers and strict generated Action Registry schemas, then verify behavior through native, in-editor, workflow, and live MCP tests.

**Tech Stack:** Unreal Engine 5.7 editor C++, BlueprintGraph/Kismet/UnrealEd APIs, Unreal Python, Python 3.11+, FastMCP 3.2.4, Pydantic 2, JSON Schema Draft 2020-12, pytest/pytest-asyncio, UE Automation Tests, UBT.

---

## Scope and fixed decisions

This plan implements
`Docs/superpowers/specs/2026-07-30-blueprint-node-palette-design.md` as one
cohesive phase. It adds exactly these public Blueprint actions:

1. `search_blueprint_node_actions`
2. `describe_blueprint_node_action`
3. `add_blueprint_action_node`
4. `suggest_blueprint_nodes_for_pin`

With no unrelated changes, the Blueprint domain moves from 48 to 52 actions,
the full catalog moves from 290 to 294 actions, and the domain count remains
22. Existing action signatures remain compatible.

The source of truth is Unreal's action database and native context filter. The
implementation must not add a node-class allowlist as a substitute catalog,
accept arbitrary node-class names, call `execute_python`, compile or save a
Blueprint implicitly, start PIE, or create unrelated assets.

The four exact Python signatures are:

```python
def ue_search_blueprint_node_actions(
    asset_path: str = None,
    graph_id: str = None,
    query: str = "",
    filters: dict = None,
    cursor: str = "",
    limit: int = 50,
) -> str

def ue_describe_blueprint_node_action(action_id: str = None) -> str

def ue_add_blueprint_action_node(
    asset_path: str = None,
    graph_id: str = None,
    action_id: str = None,
    position: dict = None,
    bindings: list[str] = (),
) -> str

def ue_suggest_blueprint_nodes_for_pin(
    asset_path: str = None,
    graph_id: str = None,
    pin_id: str = None,
    query: str = "",
    cursor: str = "",
    limit: int = 50,
) -> str
```

`describe_blueprint_node_action` is JSON-only because the opaque action record
already binds the asset and graph. The other three actions load the Blueprint
through `blueprint2.call_asset_helper`.

## Wire contract

Action and binding IDs use these exact patterns:

```text
action:[0-9a-f]{40}
binding:[0-9a-f]{40}
```

Palette cursors are opaque strings beginning with `palette-cursor:`. Public
callers never decode tokens and never supply a raw spawner, pointer, object
path as a binding, node-class string, display title, or array index to spawn.

Every compact action card contains these fields:

```json
{
  "action_id": "action:0123456789abcdef0123456789abcdef01234567",
  "title": "Get Actor Location",
  "category": "Transformation",
  "keywords": ["location", "position"],
  "action_kind": "function",
  "node_class_path": "/Script/BlueprintGraph.K2Node_CallFunction",
  "owner_path": "/Script/Engine.Actor",
  "member_path": "/Script/Engine.Actor:K2_GetActorLocation",
  "pure": true,
  "compatible": true,
  "compatibility_summary": "Available in the requested graph context.",
  "requires_binding": false,
  "bindings": []
}
```

Fields with no native value use an empty string, `null` for `pure`, or an empty
array. Cards never omit fields based on node family, which keeps downstream LLM
parsing predictable.

Search and suggestion success data use:

```json
{
  "asset_path": "/Game/BP_Player.BP_Player",
  "graph_id": "graph:11111111-1111-4111-8111-111111111111",
  "source_pin_id": null,
  "items": [],
  "total_count": 0,
  "returned_count": 0,
  "next_cursor": "",
  "result_digest": "sha1:0123456789abcdef0123456789abcdef01234567"
}
```

Describe success data adds `tooltip`, `documentation_link`,
`documentation_excerpt`, `restrictions`, `pin_preview_available`, and
`template_pins` to the card fields. A template pin has `name`, `direction`,
`type`, and `default`; it has no persisted `pin_id` because it is not in the
target graph.

Spawn success data uses:

```json
{
  "asset_path": "/Game/BP_Player.BP_Player",
  "graph_id": "graph:11111111-1111-4111-8111-111111111111",
  "action_id": "action:0123456789abcdef0123456789abcdef01234567",
  "node_id": "node:22222222-2222-4222-8222-222222222222",
  "class_path": "/Script/BlueprintGraph.K2Node_CallFunction",
  "position": {"x": 320, "y": 160},
  "pin_ids": [],
  "pins": [],
  "auxiliary_node_ids": []
}
```

## Deterministic identity and ordering

The shared layer stores only primitive records and never holds a raw spawner
between calls. An action record contains editor session ID, asset path, graph
ID, graph schema path, optional source pin ID, query/filter digest, result-set
digest, native spawner signature, owner path, sorted binding paths, and the
final sort key.

The canonical candidate key is:

```text
<owner-path>\n<spawner-signature>\n<sorted-binding-paths-joined-with-newlines>
```

The action digest is SHA-1 over the canonical action record and is registered
in a bounded session map. Maximums are 4,096 action records, 1,024 cursor
records, and 1,024 binding records. Records expire after 30 minutes of editor
time and oldest records are evicted first. Unknown/evicted/malformed tokens are
`INVALID_INPUT`; records that still exist but no longer match the session,
asset, graph, pin, schema, or result digest are `PRECONDITION_FAILED`.

Normalize searchable text with `TrimStartAndEnd().ToLower()`. Split the query
on whitespace; every token must occur in title, category, keyword, owner, or
member text. Score each candidate as follows and then sort by descending score,
normalized category, normalized title, candidate key, and action ID:

```text
1000 exact full-title match
 800 title starts with the full query
 600 title contains the full query
 400 keywords contain the full query
 300 category contains the full query
 200 owner or member path contains the full query
  20 for each query token found in the title
  10 for each query token found elsewhere
   0 for an empty query
```

Deduplicate exact candidate keys before scoring. The result-set digest covers
the complete sorted eligible set before pagination. A continuation cursor
stores the request digest, result digest, last complete sort key, and page
limit. The next request must repeat asset, graph, pin, query, filters, and limit
exactly.

## Unreal Engine 5.7 API baseline

Use the installed declarations, not inferred signatures:

```cpp
FBlueprintActionDatabase::FActionRegistry const&
    FBlueprintActionDatabase::GetAllActions();

FBlueprintActionInfo::FBlueprintActionInfo(
    UObject const* ActionOwner,
    UBlueprintNodeSpawner const* Action);

bool FBlueprintActionFilter::IsFiltered(FBlueprintActionInfo& BlueprintAction);

FBlueprintActionUiSpec UBlueprintNodeSpawner::GetUiSpec(
    FBlueprintActionContext const& Context,
    IBlueprintNodeBinder::FBindingSet const& Bindings) const;

FBlueprintNodeSignature UBlueprintNodeSpawner::GetSpawnerSignature() const;

UEdGraphNode* UBlueprintNodeSpawner::Invoke(
    UEdGraph* ParentGraph,
    IBlueprintNodeBinder::FBindingSet const& Bindings,
    FVector2D const Location) const;
```

Use `FObjectKey::ResolveObjectPtr()` for action owners. Build the filter with
the Blueprint and graph in `Context`, add the optional stable pin to
`Context.Pins`, add the Blueprint skeleton/generated class to `TargetClasses`,
and enable `BPFILTER_RejectNonImportedFields` and
`BPFILTER_RejectIncompatibleThreadSafety`. Do not set
`BPFILTER_PermitDeprecated`.

Additional policy rejects null, abstract, deprecated, newer-version, hidden,
template-only, and internal-use-only node classes or associated members. It
also calls `IsTemplateNodeFilteredOut`. There is no public bypass flag.

Only UE 5.7 headers are locally authoritative. Keep version-dependent code in
the palette adapter or shared internal implementation under explicit engine
guards. Do not report UE 5.6 or 5.8 as passed without real engines.

## File responsibility map

- Create `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintPalette.cpp` — request parsing, native database enumeration, filtering, cards, descriptions, spawn, and pin suggestions.
- Create `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprintPaletteTests.cpp` — focused native identity, pagination, filtering, stale-token, and spawn tests.
- Create `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_palette.py` — real editor acceptance and workflow undo tests.
- Modify `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprint2Internal.h` — primitive palette context/token/cursor types and function declarations.
- Modify `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprint2Internal.cpp` — bounded session registries, canonical digests, resolution, eviction, and palette capability flag.
- Modify `Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h` — four reflected entry points.
- Modify `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py` — four thin `ue_*` wrappers.
- Modify `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/run_all.py` — load the palette acceptance suite.
- Modify `mcp-server/src/unreal_mcp/blueprint2_action_specs.py` — token, filter, card, page, describe, and spawn schemas plus four action specs.
- Modify `mcp-server/tests/test_blueprint2_contracts.py` — schemas, effects, signatures, examples, strict bounds, totals, and E2E accounting.
- Modify `mcp-server/tests/test_blueprint2_action_wrappers.py` — exact wrapper forwarding and forbidden-side-effect source checks.
- Modify `mcp-server/tests/test_registry_generation.py` and `mcp-server/tests/test_coverage.py` — 294/52 action accounting.
- Modify `mcp-server/tests/test_e2e.py` — live search/describe/spawn/suggest/compile/undo scenario.
- Regenerate `mcp-server/src/unreal_mcp/dispatchers/_catalog.py` and `mcp-server/src/unreal_mcp/dispatchers/_registry.py`.
- Modify `README.md` and `mcp-server/README.md` — 294-action totals and palette-first LLM workflow.

---

### Task 1: Define strict contracts and thin Python wrappers

**Files:**
- Modify: `mcp-server/tests/test_blueprint2_contracts.py`
- Modify: `mcp-server/tests/test_blueprint2_action_wrappers.py`
- Modify: `mcp-server/tests/test_registry_generation.py`
- Modify: `mcp-server/tests/test_coverage.py`
- Modify: `mcp-server/src/unreal_mcp/blueprint2_action_specs.py`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py`
- Regenerate: `mcp-server/src/unreal_mcp/dispatchers/_catalog.py`
- Regenerate: `mcp-server/src/unreal_mcp/dispatchers/_registry.py`

- [ ] **Step 1: Write failing catalog and schema tests**

Add the four names to `NEW_BLUEPRINT2_ACTIONS`; add search, describe, and
suggest to `READ_ACTIONS`; add spawn to `WRITE_ACTIONS`. Change exact totals to
52 Blueprint and 294 overall. Add this focused test:

```python
PALETTE_ACTIONS = {
    "search_blueprint_node_actions",
    "describe_blueprint_node_action",
    "add_blueprint_action_node",
    "suggest_blueprint_nodes_for_pin",
}


def test_blueprint_palette_contracts_are_closed_bounded_and_opaque():
    specs = _new_specs()
    search = specs["search_blueprint_node_actions"]["input_schema"]
    describe = specs["describe_blueprint_node_action"]["input_schema"]
    spawn = specs["add_blueprint_action_node"]["input_schema"]
    suggest = specs["suggest_blueprint_nodes_for_pin"]["input_schema"]

    assert search["required"] == ["asset_path", "graph_id"]
    assert search["properties"]["limit"] == {
        "type": "integer", "minimum": 1, "maximum": 200, "default": 50
    }
    assert search["properties"]["filters"]["additionalProperties"] is False
    assert describe["required"] == ["action_id"]
    assert spawn["required"] == [
        "asset_path", "graph_id", "action_id", "position"
    ]
    assert suggest["required"] == ["asset_path", "graph_id", "pin_id"]
    assert spawn["properties"]["action_id"]["pattern"] == (
        r"^action:[0-9a-f]{40}$"
    )
    assert spawn["properties"]["bindings"]["maxItems"] == 32
    for action in PALETTE_ACTIONS:
        Draft202012Validator.check_schema(specs[action]["input_schema"])
        Draft202012Validator.check_schema(specs[action]["output_schema"])
```

Update `test_registry_generation.py` and `test_coverage.py` exact assertions to
294 and 52. Update the E2E accounting assertions in
`test_blueprint2_contracts.py` to 294 total and 293 sweep pairs because the one
typed-image exclusion remains unchanged.

- [ ] **Step 2: Run the focused tests to verify the red state**

Run from `mcp-server`:

```powershell
uv run --extra dev pytest tests/test_blueprint2_contracts.py tests/test_registry_generation.py tests/test_coverage.py -q
```

Expected: FAIL because the four specs and wrappers do not exist and generated
totals are still 290/48.

- [ ] **Step 3: Add exact reusable schemas and action metadata**

Add these primitives to `blueprint2_action_specs.py`:

```python
ACTION_ID = {"type": "string", "pattern": r"^action:[0-9a-f]{40}$"}
BINDING_ID = {"type": "string", "pattern": r"^binding:[0-9a-f]{40}$"}
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
    "function", "event", "variable", "macro", "delegate", "cast",
    "async", "flow_control", "operator", "struct", "other",
]
PALETTE_FILTERS = _object({
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
})
```

Define closed `PALETTE_BINDING`, `PALETTE_ACTION_CARD`,
`PALETTE_PAGE_DATA`, `PALETTE_PIN_PREVIEW`, `PALETTE_DESCRIBE_DATA`, and
`PALETTE_SPAWN_DATA` schemas matching the wire contract above. Add an optional
`success_data_schema` argument to `_write` and forward it to `_spec`, so spawn
also has a strict successful `data` contract.

Add all four specs to `BLUEPRINT2_ACTION_SPECS`. Search/describe/suggest use
`_read`; spawn uses `_write(..., idempotent=False,
success_data_schema=PALETTE_SPAWN_DATA)`. All input objects remain closed.

Add `supports_blueprint_node_palette` and `has_palette_compatible_graphs` as
required booleans in `CAPABILITIES_DATA`.

- [ ] **Step 4: Add thin wrappers and wrapper-forwarding tests**

Add the exact functions from the fixed-signature section to
`blueprint_actions.py`. Their bodies are:

```python
def ue_search_blueprint_node_actions(
    asset_path: str = None, graph_id: str = None, query: str = "",
    filters: dict = None, cursor: str = "", limit: int = 50,
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
    asset_path: str = None, graph_id: str = None,
    action_id: str = None, position: dict = None,
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
    asset_path: str = None, graph_id: str = None, pin_id: str = None,
    query: str = "", cursor: str = "", limit: int = 50,
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
```

In `test_blueprint2_action_wrappers.py`, use `_load_variable_wrapper` to assert
each helper name, asset path, and exact request dictionary. Assert describe
uses `call_json_helper` and the other three use `call_asset_helper`.

- [ ] **Step 5: Regenerate and run the offline contract gate**

Run from `mcp-server`:

```powershell
uv run python generate_catalog.py
uv run --extra dev pytest tests/test_blueprint2_contracts.py tests/test_blueprint2_action_wrappers.py tests/test_registry_generation.py tests/test_coverage.py -q
```

Expected: generation reports 294 actions across 22 domains; focused tests PASS.

- [ ] **Step 6: Commit the public contracts**

```powershell
git add -- mcp-server/src/unreal_mcp/blueprint2_action_specs.py mcp-server/src/unreal_mcp/dispatchers/_catalog.py mcp-server/src/unreal_mcp/dispatchers/_registry.py mcp-server/tests/test_blueprint2_contracts.py mcp-server/tests/test_blueprint2_action_wrappers.py mcp-server/tests/test_registry_generation.py mcp-server/tests/test_coverage.py Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py
git commit -m "feat: define Blueprint palette action contracts"
```

---

### Task 2: Implement bounded opaque action, binding, and cursor records

**Files:**
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprint2Internal.h`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprint2Internal.cpp`
- Create: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprintPaletteTests.cpp`

- [ ] **Step 1: Write failing native identity tests**

Create native tests under these names:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprintPaletteTokenTest,
    "UnrealMCP.Blueprint2.Palette.TokenIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMCPythonBlueprintPaletteCursorTest,
    "UnrealMCP.Blueprint2.Palette.CursorIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
```

`TokenIdentity` registers one action record and asserts:

```cpp
TestTrue(TEXT("opaque action prefix"), Id.StartsWith(TEXT("action:")));
TestEqual(TEXT("opaque action length"), Id.Len(), 47);
TestTrue(TEXT("same canonical record is deterministic"), Id == RepeatedId);
TestTrue(TEXT("original context resolves"), ResolvePaletteActionToken(
    Id, Context, Resolved, Error));
TestFalse(TEXT("cross-graph context rejected"), ResolvePaletteActionToken(
    Id, OtherGraphContext, Resolved, Error));
TestEqual(TEXT("cross-graph is a precondition"), Error.Code,
    FString(TEXT("PRECONDITION_FAILED")));
TestFalse(TEXT("tampered token rejected"), ResolvePaletteActionToken(
    Id.LeftChop(1) + TEXT("0"), Context, Resolved, Error));
TestEqual(TEXT("tampering is invalid input"), Error.Code,
    FString(TEXT("INVALID_INPUT")));
```

`CursorIdentity` registers a cursor with one last sort key, resolves it under
the same context, and rejects changed query digest, result digest, and limit as
`PRECONDITION_FAILED`. Register 1,025 cursors and verify the oldest is evicted
as `INVALID_INPUT`. Use a test-only clock override instead of sleeping to prove
30-minute expiry.

- [ ] **Step 2: Build to verify the tests fail to compile**

Run from the repository root:

```powershell
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' UnrealMCPSampleEditor Win64 Development "$PWD\UnrealMCPSample.uproject" -WaitMutex -NoHotReload
```

Expected: FAIL because palette record types and functions are not declared.

- [ ] **Step 3: Add the primitive internal types and exact API**

Add to `MCPythonBlueprint2Internal.h`:

```cpp
struct FPaletteContext
{
    FString AssetPath;
    FString GraphId;
    FString GraphSchemaPath;
    FString SourcePinId;
    FString RequestDigest;
    FString ResultDigest;
    int32 Limit = 50;
};

struct FPaletteActionRecord
{
    FPaletteContext Context;
    FString ActionId;
    FString CandidateKey;
    FString SpawnerSignature;
    FString OwnerPath;
    TArray<FString> BindingPaths;
    FString SortKey;
};

struct FPaletteCursorRecord
{
    FPaletteContext Context;
    FString CursorId;
    FString LastSortKey;
};

struct FPaletteBindingRecord
{
    FString BindingId;
    FString ActionId;
    FString ObjectPath;
    FString ExpectedClassPath;
};

FString RegisterPaletteActionToken(FPaletteActionRecord& Record);
bool ResolvePaletteActionToken(
    const FString& ActionId,
    const FPaletteContext& Expected,
    FPaletteActionRecord& OutRecord,
    FError& OutError);
FString RegisterPaletteCursor(FPaletteCursorRecord& Record);
bool ResolvePaletteCursor(
    const FString& Cursor,
    const FPaletteContext& Expected,
    FPaletteCursorRecord& OutRecord,
    FError& OutError);
FString RegisterPaletteBinding(FPaletteBindingRecord& Record);
bool ResolvePaletteBindings(
    const FString& ActionId,
    const TArray<FString>& BindingIds,
    TArray<FPaletteBindingRecord>& OutRecords,
    FError& OutError);
void ResetPaletteTokenStateForTests();
void SetPaletteTokenClockForTests(const TOptional<FDateTime>& Now);
```

The test hooks are compiled only under `WITH_DEV_AUTOMATION_TESTS`.

- [ ] **Step 4: Implement canonical registration, validation, expiry, and eviction**

In the shared `.cpp`, use one private state object guarded by `FCriticalSection`:

```cpp
struct FPaletteTokenState
{
    TMap<FString, FStoredPaletteAction> Actions;
    TMap<FString, FStoredPaletteCursor> Cursors;
    TMap<FString, FStoredPaletteBinding> Bindings;
    TArray<FString> ActionOrder;
    TArray<FString> CursorOrder;
    TArray<FString> BindingOrder;
    TOptional<FDateTime> TestNow;
    FCriticalSection Mutex;
};
```

Each stored record adds `EditorSessionId`, `CreatedAt`, and `LastUsedAt`.
Canonical strings use explicit field order and sorted binding paths. Generate
IDs with existing `Sha1Hex`; never serialize pointer values. Validate token
prefix and 40 lower-case hex characters before map lookup. Purge records older
than 30 minutes before every registration and resolution, then enforce the
4,096/1,024/1,024 bounds in insertion order.

`ResolvePaletteActionToken` distinguishes malformed/missing records from
records whose expected context no longer matches. `ResolvePaletteBindings`
rejects duplicates, cross-action binding IDs, missing records, and more than 32
bindings.

- [ ] **Step 5: Build and run the focused native tests**

```powershell
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' UnrealMCPSampleEditor Win64 Development "$PWD\UnrealMCPSample.uproject" -WaitMutex -NoHotReload
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' "$PWD\UnrealMCPSample.uproject" -unattended -nop4 -nosplash -NullRHI -ExecCmds="Automation RunTests UnrealMCP.Blueprint2.Palette; Quit" -TestExit="Automation Test Queue Empty" -log
```

Expected: UBT succeeds; both native tests pass.

- [ ] **Step 6: Commit the opaque identity core**

```powershell
git add -- Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprint2Internal.h Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprint2Internal.cpp Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprintPaletteTests.cpp
git commit -m "feat: add Blueprint palette opaque identities"
```

---

### Task 3: Implement native palette search and action description

**Files:**
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h`
- Create: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintPalette.cpp`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprintPaletteTests.cpp`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprint2Internal.cpp`

- [ ] **Step 1: Write failing search/describe automation tests**

Add tests named:

```cpp
"UnrealMCP.Blueprint2.Palette.SearchPagination"
"UnrealMCP.Blueprint2.Palette.Describe"
```

Create a transient Actor Blueprint fixture and stable EventGraph ID. Search for
`Get Actor Location` twice and assert byte-for-byte equal `items`, equal
`result_digest`, nonempty `action_id`, and deterministic ordering. Search with
`limit=1`, follow `next_cursor` with the identical request, and assert no
duplicate action IDs. Reject a changed query with the old cursor.

Describe the first action and assert its title, node class, owner/member paths,
binding list, restrictions, and pin-preview availability fields. Record the
target graph node count before and after describe and assert it is unchanged.

- [ ] **Step 2: Build to verify the missing entry points fail**

Run the UE 5.7 UBT command from Task 2.

Expected: FAIL because `SearchBlueprintNodeActions` and
`DescribeBlueprintNodeAction` do not exist.

- [ ] **Step 3: Declare the reflected helpers**

Add to `MCPythonHelper.h`:

```cpp
/** Search Unreal's native action database for one Blueprint graph. */
UFUNCTION(BlueprintCallable, Category="Editor|MCPython")
static FString SearchBlueprintNodeActions(
    UBlueprint* Blueprint, const FString& RequestJson);

/** Describe a previously returned opaque palette action. */
UFUNCTION(BlueprintCallable, Category="Editor|MCPython")
static FString DescribeBlueprintNodeAction(const FString& RequestJson);

/** Spawn a previously returned opaque palette action. */
UFUNCTION(BlueprintCallable, Category="Editor|MCPython")
static FString AddBlueprintActionNode(
    UBlueprint* Blueprint, const FString& RequestJson);

/** Search native actions compatible with one stable pin. */
UFUNCTION(BlueprintCallable, Category="Editor|MCPython")
static FString SuggestBlueprintNodesForPin(
    UBlueprint* Blueprint, const FString& RequestJson);
```

The last two may return a structured `UE_VERSION_UNSUPPORTED` stub until their
own tasks land, but they must compile and preserve the four public wrappers.

- [ ] **Step 4: Implement closed request parsing and native enumeration**

In `MCPythonHelper_BlueprintPalette.cpp`, define focused private types:

```cpp
struct FPaletteFilters
{
    TSet<FString> ActionKinds;
    TArray<FString> CategoryPrefixes;
    TSet<FString> OwnerPaths;
    bool bPureOnly = false;
};

struct FPaletteCandidate
{
    const UObject* Owner = nullptr;
    const UBlueprintNodeSpawner* Spawner = nullptr;
    IBlueprintNodeBinder::FBindingSet Bindings;
    FString CandidateKey;
    FString SpawnerSignature;
    FString OwnerPath;
    FString MemberPath;
    FString NodeClassPath;
    FString Title;
    FString Category;
    FString Tooltip;
    TArray<FString> Keywords;
    FString ActionKind;
    TOptional<bool> bPure;
    int32 Score = 0;
    FString SortKey;
};
```

`ParseSearchRequest` accepts only `graph_id`, `query`, `filters`, `cursor`, and
`limit`; applies the schema bounds again in C++; and reports the exact field in
`INVALID_INPUT`. Resolve the graph through `ResolveTarget` and require a stable
graph ID.

`BuildCandidates` iterates `GetAllActions()`, resolves each owner, constructs
`FBlueprintActionInfo`, applies the native filter and additional safety policy,
obtains `GetUiSpec`, classifies the action, applies filters/query, deduplicates
the canonical key, and produces the deterministic sort key. Treat an invalid
`GetSpawnerSignature()` as unsafe and omit it.

Classify kinds in this order: async/latent, event, variable, macro, delegate,
cast, flow control, operator, struct, associated function, other. Pure is true
only for an associated function with `FUNC_BlueprintPure`; otherwise false or
unknown.

- [ ] **Step 5: Implement paging, cards, tokens, and description**

Compute `result_digest` over all sorted candidate keys. Validate a supplied
cursor before finding the first sort key greater than its stored last key.
Register action tokens only for returned page items. Set `next_cursor` only
when another item remains.

Description resolves the action record, finds/loads its Blueprint, resolves
the exact stable graph and optional source pin, rebuilds candidates, compares
the result digest, and matches owner path plus spawner signature plus binding
paths. It returns `PRECONDITION_FAILED` if the matching native action is gone.

For pin preview, call `PrimeDefaultUiSpec(Graph)`, then inspect only
`GetCachedTemplateNode()`. Serialize visible template pins if one exists. Do
not call `Invoke` on the target graph. If no cached template is available,
return `pin_preview_available=false` and `template_pins=[]`.

- [ ] **Step 6: Advertise runtime palette capability**

In `BuildCapabilities`, add:

```cpp
Result->SetBoolField(TEXT("supports_blueprint_node_palette"), true);
Result->SetBoolField(
    TEXT("has_palette_compatible_graphs"), bHasK2Graph);
```

Non-5.7 guards may set support false until proven compatible, but the local
5.7 build must return true.

- [ ] **Step 7: Build and run search/describe native tests**

```powershell
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' UnrealMCPSampleEditor Win64 Development "$PWD\UnrealMCPSample.uproject" -WaitMutex -NoHotReload
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' "$PWD\UnrealMCPSample.uproject" -unattended -nop4 -nosplash -NullRHI -ExecCmds="Automation RunTests UnrealMCP.Blueprint2.Palette; Quit" -TestExit="Automation Test Queue Empty" -log
```

Expected: UBT and both tests pass; no target graph mutation occurs in either
read action.

- [ ] **Step 8: Commit search and description**

```powershell
git add -- Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintPalette.cpp Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprint2Internal.cpp Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprintPaletteTests.cpp
git commit -m "feat: search and describe Blueprint palette actions"
```

---

### Task 4: Spawn palette actions transactionally with stable results

**Files:**
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintPalette.cpp`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprintPaletteTests.cpp`
- Modify: `mcp-server/tests/test_blueprint2_action_wrappers.py`

- [ ] **Step 1: Write failing spawn, tamper, stale, and rollback tests**

Add `UnrealMCP.Blueprint2.Palette.Spawn` and test this sequence:

1. Search `Get Actor Location` and retain its `action_id`.
2. Record graph nodes, Blueprint compile status, and package dirty state.
3. Spawn at `{320, 160}`.
4. Assert exactly one primary new node, stable `node_id`, all stable `pin_id`
   values, exact position, class path, one create change record, and an explicit
   `compile_blueprint` next action.
5. Assert Blueprint status becomes dirty but no compile or save occurred.
6. Tamper the action ID and assert `INVALID_INPUT` with no graph change.
7. Change the graph GUID after search and assert `PRECONDITION_FAILED` before
   invocation, then restore the GUID.
8. Invoke an action whose singleton node already exists; assert rollback and no
   second node.

Add an offline source test:

```python
def test_palette_spawn_has_no_implicit_compile_save_or_python_escape():
    source = (PRIVATE / "MCPythonHelper_BlueprintPalette.cpp").read_text(
        encoding="utf-8"
    )
    assert "CompileBlueprint" not in source
    assert "SaveAsset" not in source
    assert "EditorAssetLibrary" not in source
    assert "execute_python" not in source
    assert "UBlueprintNodeSpawner::Invoke" in source or "->Invoke(" in source
```

- [ ] **Step 2: Run the red gates**

Run the wrapper source test and the UE 5.7 palette native prefix.

Expected: FAIL because spawn still returns the temporary unsupported result.

- [ ] **Step 3: Parse and revalidate the spawn request before mutation**

Accept only `graph_id`, `action_id`, `position`, and `bindings`. Require finite
position coordinates in the existing graph-coordinate range and at most 32
unique binding IDs. Resolve the graph and action token, rebuild the exact
candidate set, compare result digest, reapply the native filter, and resolve
every opaque binding. No `FMutationScope` exists before all checks pass.

Snapshot the graph node pointers and stable IDs before invocation. Reject
action records originating from a pin suggestion only if that source pin is
now missing or no longer belongs to this graph; otherwise they remain valid
spawn capabilities.

- [ ] **Step 4: Invoke inside the existing transaction and validate the delta**

Use this mutation shape:

```cpp
FMutationScope Scope(NSLOCTEXT(
    "MCPython", "AddBlueprintActionNode", "Add Blueprint palette node"));
if (!Scope.IsValid())
{
    return PaletteFailure(
        TEXT("transaction"),
        TEXT("Could not begin a Blueprint palette transaction."),
        TEXT("TRANSACTION_FAILED"));
}
Scope.Modify(Blueprint);
Scope.Modify(Graph);

UEdGraphNode* NewNode = Candidate.Spawner->Invoke(
    Graph, ResolvedBindings, FVector2D(PositionX, PositionY));
```

After invocation, require `NewNode`, require it belongs to the requested graph,
require it was absent from the pre-snapshot, and require stable node and visible
pin IDs. Collect every other new graph node as `auxiliary_node_ids`. If any
requirement fails, call `Scope.Rollback()` and return the appropriate
`PRECONDITION_FAILED`, `INTERNAL_ERROR`, or `ROLLBACK_FAILED` envelope.

Call only `FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint)`. Serialize
full pin records with `SerializeTypeSpec` and normalized defaults. Never call a
compiler or save API.

- [ ] **Step 5: Build and run spawn tests**

```powershell
uv run --project mcp-server --extra dev pytest mcp-server/tests/test_blueprint2_action_wrappers.py -q
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' UnrealMCPSampleEditor Win64 Development "$PWD\UnrealMCPSample.uproject" -WaitMutex -NoHotReload
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' "$PWD\UnrealMCPSample.uproject" -unattended -nop4 -nosplash -NullRHI -ExecCmds="Automation RunTests UnrealMCP.Blueprint2.Palette.Spawn; Quit" -TestExit="Automation Test Queue Empty" -log
```

Expected: offline source guard and native spawn suite pass.

- [ ] **Step 6: Commit transactional spawning**

```powershell
git add -- Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintPalette.cpp Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprintPaletteTests.cpp mcp-server/tests/test_blueprint2_action_wrappers.py
git commit -m "feat: spawn Blueprint palette actions safely"
```

---

### Task 5: Add real pin-context suggestions

**Files:**
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintPalette.cpp`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprintPaletteTests.cpp`

- [ ] **Step 1: Write failing pin-context tests**

Add `UnrealMCP.Blueprint2.Palette.PinSuggestions`. Create an integer output pin,
a boolean output pin, and an execution output pin in the fixture. For each pin,
call the suggestion helper with an empty query and a bounded limit. Assert:

```cpp
TestTrue(TEXT("integer suggestions exist"), IntegerIds.Num() > 0);
TestTrue(TEXT("boolean suggestions exist"), BooleanIds.Num() > 0);
TestTrue(TEXT("exec suggestions exist"), ExecIds.Num() > 0);
TestTrue(TEXT("typed contexts differ"), IntegerIds != BooleanIds);
TestTrue(TEXT("data and exec contexts differ"), IntegerIds != ExecIds);
```

Also assert every returned action record contains the exact source `pin_id`, a
cursor cannot be replayed for another pin, a pin from another graph is rejected
without mutation, and an action returned from suggestions can be described and
spawned while the source pin remains valid.

- [ ] **Step 2: Run the focused native test to verify failure**

Run the native test name above.

Expected: FAIL because the suggestion entry point is still unsupported.

- [ ] **Step 3: Resolve the stable pin and build the native context**

Parse only `graph_id`, `pin_id`, `query`, `cursor`, and `limit`. Resolve both
stable IDs with `ResolveTarget`; require the pin's owning node belongs to the
requested graph. Build the same filter as search, then add the pin to
`Filter.Context.Pins`.

If `PinSubCategoryObject` is a `UClass`, add it to `TargetClasses`. For an
object target pin, also inspect the owning node's self pin and add that class
when available. Keep native thread-safety and imported-field rejection flags.

- [ ] **Step 4: Reuse the shared candidate/page path**

Call the same `BuildCandidates`, scoring, result-digest, token registration,
and pagination functions used by search. Do not fork a second sorting or cursor
implementation. Set `source_pin_id` in the page response and action context.

Do not spawn a template, create a target node, or call `TryCreateConnection` in
this action. Unreal's action filter is the compatibility authority; the result
summary must say `compatible according to the current native action filter`
rather than promise a later binding or connection will succeed.

- [ ] **Step 5: Build and run the complete native palette prefix**

```powershell
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' UnrealMCPSampleEditor Win64 Development "$PWD\UnrealMCPSample.uproject" -WaitMutex -NoHotReload
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' "$PWD\UnrealMCPSample.uproject" -unattended -nop4 -nosplash -NullRHI -ExecCmds="Automation RunTests UnrealMCP.Blueprint2.Palette; Quit" -TestExit="Automation Test Queue Empty" -log
```

Expected: every token, cursor, search, describe, spawn, and suggestion native
test passes.

- [ ] **Step 6: Commit pin suggestions**

```powershell
git add -- Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintPalette.cpp Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprintPaletteTests.cpp
git commit -m "feat: suggest Blueprint actions for stable pins"
```

---

### Task 6: Prove real editor behavior and workflow rollback

**Files:**
- Create: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_palette.py`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/run_all.py`
- Modify: `mcp-server/tests/test_coverage.py`

- [ ] **Step 1: Create the in-editor fixture and helpers**

Follow `test_blueprint2_graph.py`: create a unique transient Actor Blueprint,
record its full object path, obtain the stable EventGraph ID through
`ue_inspect_blueprint`, and clean all assets in `addCleanup` and
`tearDownClass`.

Add helpers with these exact responsibilities:

```python
def _search(self, query="", *, filters=None, cursor="", limit=50):
    return call_action(
        "blueprint_actions", "ue_search_blueprint_node_actions",
        asset_path=self.asset_path, graph_id=self.graph_id,
        query=query, filters=filters or {}, cursor=cursor, limit=limit,
    )

def _find_action(self, query, predicate=lambda item: True):
    result = self._search(query, limit=200)
    self.assertSuccess(result)
    matches = [item for item in result["data"]["items"] if predicate(item)]
    self.assertTrue(matches, (query, result))
    return matches[0]

def _spawn(self, action, x, y):
    return call_action(
        "blueprint_actions", "ue_add_blueprint_action_node",
        asset_path=self.asset_path, graph_id=self.graph_id,
        action_id=action["action_id"], position={"x": x, "y": y},
        bindings=[item["binding_id"] for item in action["bindings"]],
    )
```

- [ ] **Step 2: Add acceptance tests for the four public actions**

Add tests whose source explicitly references all four `ue_*` names so the
offline coverage gate remains debt-free:

```text
test_search_is_deterministic_paginated_and_context_bound
test_describe_is_read_only_and_rejects_tampering
test_spawn_function_event_macro_cast_and_latent_families
test_spawn_returns_inspectable_stable_nodes_and_pins
test_pin_suggestions_differ_by_pin_type_and_direction
test_palette_spawn_requires_explicit_compile_and_never_saves
test_palette_spawn_rolls_back_with_outer_workflow
test_palette_workflow_stress_12_of_12
test_available_plugin_defined_action_round_trip
```

For family coverage, select by returned `action_kind`, class/member path, and
title rather than relying only on localized display text. Cover:

- `Actor:K2_GetActorLocation` function;
- `Actor:ReceiveBeginPlay` event where the fixture permits another event, or a
  different allowed event when BeginPlay already exists;
- one Engine macro-library action such as `ForEachLoop`;
- dynamic cast to Actor;
- a latent/async action such as `KismetSystemLibrary:Delay` in EventGraph.

For the enabled GameplayAbilities plugin, search for an action whose owner or
node class path begins with `/Script/GameplayAbilities`. If the native palette
does not expose one for an Actor EventGraph, call `skipTest` with the exact
reason that this graph has no compatible plugin-defined action; do not turn the
absence into a false pass.

Compare every spawned node and pin ID to detailed `inspect_blueprint` output.
Connect at least one returned compatible pin with existing stable-pin connect
actions. Explicitly call `ue_compile_blueprint` and `ue_get_blueprint_health`
after graph construction.

For workflow rollback, capture `ue_snapshot_blueprint_graph`, begin the existing
workflow transaction, spawn one palette node inside an atomic step, roll back,
and assert a new snapshot is byte-for-byte equal to the original after removing
only trace IDs.

The stress test performs 12 independent search/spawn/rollback cycles. It takes
a fresh snapshot and fresh action ID for every cycle, alternates function,
macro, and cast actions, asserts exact graph restoration after each rollback,
increments `completed` only after the restoration assertion, then executes:

```python
print(f"Palette workflow stress: {completed}/12")
self.assertEqual(completed, 12)
```

- [ ] **Step 3: Register and run the new editor suite**

Add this module immediately after `test_blueprint2_graph` in `run_all.py`:

```python
"UnrealMCPython.tests.test_blueprint2_palette",
```

With the UE 5.7 editor open, run in its Python console:

```python
import importlib, unittest
module = importlib.import_module("UnrealMCPython.tests.test_blueprint2_palette")
module = importlib.reload(module)
suite = unittest.defaultTestLoader.loadTestsFromModule(module)
result = unittest.TextTestRunner(verbosity=2).run(suite)
assert result.wasSuccessful()
```

Expected: nine palette tests pass, with only the explicitly unsupported
plugin-family case allowed to skip; zero failures and errors; no leftover
`/Game/__MCPTests` assets.

- [ ] **Step 4: Run the offline coverage gate**

```powershell
uv run --project mcp-server --extra dev pytest mcp-server/tests/test_coverage.py mcp-server/tests/test_blueprint2_contracts.py -q
```

Expected: 52 Blueprint actions are referenced by real editor suites and
`KNOWN_UNTESTED["blueprint"]` remains absent.

- [ ] **Step 5: Commit editor acceptance coverage**

```powershell
git add -- Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_palette.py Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/run_all.py mcp-server/tests/test_coverage.py
git commit -m "test: verify Blueprint palette editor workflows"
```

---

### Task 7: Add live MCP coverage, documentation, and release gates

**Files:**
- Modify: `mcp-server/tests/test_e2e.py`
- Modify: `mcp-server/tests/test_blueprint2_contracts.py`
- Modify: `README.md`
- Modify: `mcp-server/README.md`
- Regenerate: `mcp-server/src/unreal_mcp/dispatchers/_catalog.py`
- Regenerate: `mcp-server/src/unreal_mcp/dispatchers/_registry.py`

- [ ] **Step 1: Write the live palette workflow test**

Add `test_blueprint_palette_round_trip` beside the existing Blueprint 2 live
workflow. It must:

1. create an unsaved Actor Blueprint;
2. inspect the stable EventGraph ID;
3. search `Get Actor Location` with `limit=1` and follow a cursor when present;
4. describe the selected action;
5. snapshot the graph;
6. spawn at a unique position;
7. inspect and compare returned node/pin IDs;
8. request suggestions for one returned pin;
9. explicitly compile and run health checks;
10. execute a second spawn through `workflow.plan`/`apply` and restore it with
    `workflow.undo`;
11. diff against the pre-workflow snapshot and require no remaining workflow
    change;
12. delete the test asset in `finally`.

Every response passes `_assert_not_connection_error`. Assert the create and
palette mutation responses never report `saved=True`.

- [ ] **Step 2: Update documentation and exact totals**

Change 290 to 294 in both READMEs and update the Blueprint workflow to:

```text
get_blueprint_brief -> inspect_blueprint
-> search_blueprint_node_actions / suggest_blueprint_nodes_for_pin
-> describe_blueprint_node_action -> add_blueprint_action_node
-> connect_blueprint_pins -> compile_blueprint -> get_blueprint_health
-> asset.save_asset (only when persistence is wanted)
```

Document that action IDs are editor-session and graph-context capabilities,
that stale IDs require a new search, and that search results are bounded to 200
per page.

Update all remaining exact catalog assertions to 294 total, 52 Blueprint, 293
JSON sweep pairs, and 22 domains. Regenerate catalog and registry once more.

- [ ] **Step 3: Run the complete offline gate**

From `mcp-server`:

```powershell
uv run python generate_catalog.py
git diff --exit-code -- src/unreal_mcp/dispatchers/_catalog.py src/unreal_mcp/dispatchers/_registry.py
uv run --extra dev pytest -q
```

Expected: generated files are clean, at least the existing 583 offline tests
plus the new palette tests pass, and only environment-gated tests skip.

- [ ] **Step 4: Run the UE 5.7 build and native release gates**

From the repository root:

```powershell
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' UnrealMCPSampleEditor Win64 Development "$PWD\UnrealMCPSample.uproject" -WaitMutex -NoHotReload
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' "$PWD\UnrealMCPSample.uproject" -unattended -nop4 -nosplash -NullRHI -ExecCmds="Automation RunTests UnrealMCP.Blueprint2; Quit" -TestExit="Automation Test Queue Empty" -log
```

Expected: UBT succeeds and every Blueprint2 native test passes with zero
failures.

- [ ] **Step 5: Run full in-editor and workflow acceptance**

Open the UE 5.7 editor and run:

```python
import runpy; runpy.run_module("UnrealMCPython.tests.run_all", run_name="__main__")
```

Expected: the prior 407-test baseline plus new palette tests has zero failures
and errors; only explicit capability skips are allowed. The registered
`test_palette_workflow_stress_12_of_12` case must print and pass
`Palette workflow stress: 12/12`.

- [ ] **Step 6: Run the live MCP E2E gate**

With the editor TCP server reachable on `127.0.0.1:12029`, run:

```powershell
uv run --project mcp-server --extra dev pytest mcp-server/tests/test_e2e.py -v
```

Expected: every one of the 294 catalog actions returns an unwrapped result, the
dedicated palette workflow passes, the editor stays alive, and no test asset is
left behind.

- [ ] **Step 7: Record unavailable compatibility gates honestly**

Run UE 5.6 and UE 5.8 build/native/editor gates only if complete local engine
installations are available. Otherwise record both as `not run: engine/source
unavailable`; do not describe them as passing based on schemas or version
guards.

- [ ] **Step 8: Commit the release proof and push only the user fork branch**

```powershell
git add -- README.md mcp-server/README.md mcp-server/tests/test_e2e.py mcp-server/tests/test_blueprint2_contracts.py mcp-server/src/unreal_mcp/dispatchers/_catalog.py mcp-server/src/unreal_mcp/dispatchers/_registry.py
git commit -m "test: prove Blueprint palette MCP workflows"
git status --short
git push origin codex/llm-friendly-expansion
```

Expected: clean worktree and a successful push to
`origin/codex/llm-friendly-expansion`; no upstream remote is used.

## Final evidence checklist

Before claiming the phase complete, preserve the exact command outputs for:

- 294 actions across 22 domains and 52 Blueprint actions;
- complete offline pytest result;
- UE 5.7 UBT result;
- `UnrealMCP.Blueprint2` native automation result;
- full in-editor result and cleanup assertion;
- 12/12 workflow stress result;
- live MCP E2E result;
- git status and pushed commit SHA;
- explicit UE 5.6 and UE 5.8 not-run reasons when unavailable.

Do not infer success from source inspection when a runnable gate exists. A
failed or unavailable editor gate remains visible in the final handoff.
