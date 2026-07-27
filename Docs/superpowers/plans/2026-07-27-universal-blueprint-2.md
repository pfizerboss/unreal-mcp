# Universal Blueprint 2.0 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver a generic, LLM-friendly Blueprint editing surface that can inspect, address, author, compile, diagnose, diff, and transactionally undo arbitrary supported Blueprint structures without gameplay-specific generation.

**Architecture:** Put all K2, Blueprint member, variable, SCS, compiler, stable-ID, cursor, and snapshot logic in focused editor-only C++ translation units. Keep Unreal Python functions as narrow JSON adapters, describe every public operation through strict generated Action Registry schemas, and reuse the existing workflow lease and outer transaction instead of adding another batch executor.

**Tech Stack:** Unreal Engine 5.6-5.8 editor C++, K2/BlueprintGraph/UnrealEd APIs, Unreal Python, Python 3.11+, FastMCP 3.2.4, Pydantic 2, JSON Schema Draft 2020-12, pytest/pytest-asyncio, UE in-editor `unittest`, UBT.

---

## Scope and fixed decisions

This plan implements the approved specification in
`Docs/superpowers/specs/2026-07-27-universal-blueprint-2-design.md` in four
independently testable stages. It does not create Character, GameMode, input,
HUD, project settings, PIE sessions, or any other gameplay foundation.

The public workflow namespace becomes exactly `plan`, `apply`, `get`, `cancel`,
and `undo`. The obsolete `plan_gameplay_foundation` and
`verify_gameplay_foundation` actions, prompt, capability flag, handler hooks,
and named tests are removed in Stage 1. Generic workflow behavior remains
unchanged.

All 18 existing Blueprint actions remain public. Their required arguments and
representative top-level response fields remain compatible. Existing graph,
node, pin, variable, and component name arguments may additionally contain a
prefixed stable ID; legacy exact-name resolution remains available only on
those pre-existing actions. New actions reject an unstable fallback unless
`allow_name_fallback=true` and the request also supplies owner, name, and type
constraints that resolve exactly one object.

The release adds 29 Blueprint actions, taking the Blueprint domain from 19 to
48 actions and the complete catalog from 263 to 290 actions after the two
workflow placeholders are removed.

## Verified Unreal API baseline

The implementation must use the installed UE 5.7 declarations below rather
than inferred signatures:

```cpp
// Kismet2/BlueprintEditorUtils.h
static UEdGraph* CreateNewGraph(
    UObject* ParentScope,
    const FName& GraphName,
    TSubclassOf<UEdGraph> GraphClass,
    TSubclassOf<UEdGraphSchema> SchemaClass);
template <typename SignatureType>
static void AddFunctionGraph(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    bool bIsUserCreated,
    SignatureType* SignatureFromObject);
static void AddMacroGraph(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    bool bIsUserCreated,
    UClass* SignatureFromClass);
static bool AddMemberVariable(
    UBlueprint* Blueprint,
    const FName& NewVarName,
    const FEdGraphPinType& NewVarType,
    const FString& DefaultValue = FString());
static bool ImplementNewInterface(
    UBlueprint* Blueprint,
    FTopLevelAssetPath InterfaceClassPathName);

// Kismet2/KismetEditorUtilities.h
static void CompileBlueprint(
    UBlueprint* BlueprintObj,
    EBlueprintCompileOptions CompileFlags = EBlueprintCompileOptions::None,
    FCompilerResultsLog* Results = nullptr);

// EdGraphSchema_K2.h
virtual const FPinConnectionResponse CanCreateConnection(
    const UEdGraphPin* A,
    const UEdGraphPin* B) const override;
virtual bool TryCreateConnection(UEdGraphPin* A, UEdGraphPin* B) const override;
virtual void BreakPinLinks(
    UEdGraphPin& TargetPin,
    bool bSendsNodeNotification) const override;
virtual void BreakSinglePinLink(
    UEdGraphPin* SourcePin,
    UEdGraphPin* TargetPin) const override;
virtual void GetPinDefaultValuesFromString(
    const FEdGraphPinType& PinType,
    UObject* OwningObject,
    const FString& NewValue,
    FString& UseDefaultValue,
    TObjectPtr<UObject>& UseDefaultObject,
    FText& UseDefaultText,
    bool bPreserveTextIdentity = true) const;
virtual bool DefaultValueSimpleValidation(
    const FEdGraphPinType& PinType,
    const FName PinName,
    const FString& NewDefaultValue,
    TObjectPtr<UObject> NewDefaultObject,
    const FText& InText,
    FString* OutMsg = nullptr) const;

// GraphDiffControl.h; retained as a parity oracle, not as the wire contract.
static bool DiffGraphs(
    UEdGraph* const OldGraph,
    UEdGraph* const NewGraph,
    TArray<FDiffSingleResult>& DiffsOut);
```

`FCompilerResultsLog::Messages`, `NumErrors`, and `NumWarnings` are public in
UE 5.7. Each message exposes severity and tokens; `FEdGraphToken::GetPin()` and
`GetGraphObject()` provide the stable pin/node association used in structured
diagnostics.

Only UE 5.7 headers are locally installed in full. UE 5.8 is installed without
its source headers, and UE 5.6 is not installed. Put every signature-dependent
adapter in `MCPythonBlueprint2Internal.cpp` behind `ENGINE_MAJOR_VERSION` and
`ENGINE_MINOR_VERSION` checks, build locally on 5.7, and leave explicit 5.6 and
5.8 worker gates in the final verification task.

## Stable wire contracts

Stable IDs use lower-case hyphenated GUIDs and these prefixes:

| Object | ID form | `id_kind` |
|---|---|---|
| Graph or graph-backed function/macro | `graph:<guid>` | `graph_guid` |
| Node or custom event | `node:<guid>` | `node_guid` |
| Pin | `pin:<guid>` | `pin_guid` |
| Blueprint variable or dispatcher | `variable:<guid>` | `variable_guid` |
| SCS component | `component:<guid>` | `scs_variable_guid` |
| Implemented interface | `interface:<full-class-path>` | `interface_path` |
| Missing persisted GUID | `fallback:<kind>:<sha1>` | `qualified_name_fallback` |

Every returned target record contains `id`, `id_kind`, and `stable`. A fallback
record also contains `owner_id`, `name`, and `type_path` when applicable.

Cursor payloads are base64url-encoded canonical JSON containing
`version`, `asset_path`, `query_digest`, `editor_session_id`, and `last_id`.
The digest is SHA-1 over an explicitly ordered string of the query fields; it
must not depend on `TMap` iteration order. A malformed cursor, a changed query,
a changed asset, or a restarted editor returns `INVALID_INPUT` at
`params.cursor` without mutating the asset.

New C++ helpers return the complete structured envelope:

```json
{
  "success": true,
  "status": "succeeded",
  "summary": "Created Blueprint function ComputeScore.",
  "data": {},
  "changes": [],
  "warnings": [],
  "errors": [],
  "next_actions": [],
  "trace_id": "lower-case-guid"
}
```

For the existing `compile_blueprint`, compatibility wins where names overlap:
top-level `status` remains `UpToDate`, `UpToDateWithWarnings`, `Error`,
`Dirty`, `BeingCreated`, or `Unknown`; structured state is also returned as
`data.result_status`. Its existing `message` remains and `summary`,
`diagnostics`, `warnings`, `errors`, and `next_actions` are additive.

## Public action signatures

The new `ue_*` wrappers in `blueprint_actions.py` have these exact signatures;
each delegates to `blueprint2.py` and sends one JSON request to the matching
snake-case `unreal.MCPythonHelper` method:

```python
def ue_get_blueprint_brief(asset_path: str = None) -> str
def ue_inspect_blueprint(
    asset_path: str = None,
    queries: list[dict] = (),
    cursor: str = "",
) -> str

def ue_create_blueprint_function(
    asset_path: str = None, name: str = None,
    inputs: list[dict] = (), outputs: list[dict] = (),
    pure: bool = False, const: bool = False, access: str = "public",
    category: str = "", description: str = "",
) -> str
def ue_rename_blueprint_function(
    asset_path: str = None, function_id: str = None, new_name: str = None,
    allow_name_fallback: bool = False, function_name: str = "",
) -> str
def ue_set_blueprint_function_signature(
    asset_path: str = None, function_id: str = None,
    inputs: list[dict] = None, outputs: list[dict] = None,
    pure: bool = False, const: bool = False, access: str = "public",
    category: str = "", description: str = "",
    allow_name_fallback: bool = False, function_name: str = "",
) -> str
def ue_delete_blueprint_function(
    asset_path: str = None, function_id: str = None,
    allow_name_fallback: bool = False, function_name: str = "",
) -> str
def ue_create_blueprint_macro(
    asset_path: str = None, name: str = None,
    inputs: list[dict] = (), outputs: list[dict] = (),
    pure: bool = False, category: str = "", description: str = "",
) -> str
def ue_delete_blueprint_macro(
    asset_path: str = None, macro_id: str = None,
    allow_name_fallback: bool = False, macro_name: str = "",
) -> str
def ue_create_custom_event(
    asset_path: str = None, graph_id: str = None, name: str = None,
    parameters: list[dict] = (), pos_x: float = 0.0, pos_y: float = 0.0,
) -> str
def ue_delete_custom_event(
    asset_path: str = None, event_id: str = None,
    allow_name_fallback: bool = False, event_name: str = "",
    owner_graph_id: str = "",
) -> str
def ue_add_event_dispatcher(
    asset_path: str = None, name: str = None,
    parameters: list[dict] = (), category: str = "",
    description: str = "",
) -> str
def ue_remove_event_dispatcher(
    asset_path: str = None, dispatcher_id: str = None,
    allow_name_fallback: bool = False, dispatcher_name: str = "",
) -> str
def ue_add_blueprint_interface(
    asset_path: str = None, interface_path: str = None,
) -> str
def ue_remove_blueprint_interface(
    asset_path: str = None, interface_path: str = None,
) -> str

def ue_add_reflected_blueprint_node(
    asset_path: str = None, graph_id: str = None,
    reference_kind: str = None, reference_path: str = None,
    pos_x: float = 0.0, pos_y: float = 0.0, options: dict = {},
) -> str
def ue_set_blueprint_node_properties(
    asset_path: str = None, node_id: str = None,
    properties: dict = None,
) -> str
def ue_disconnect_blueprint_pins(
    asset_path: str = None, pin_id: str = None,
    other_pin_id: str = "", break_all: bool = False,
) -> str

def ue_rename_blueprint_variable(
    asset_path: str = None, variable_id: str = None, new_name: str = None,
    allow_name_fallback: bool = False, variable_name: str = "",
) -> str
def ue_remove_blueprint_variable(
    asset_path: str = None, variable_id: str = None,
    allow_name_fallback: bool = False, variable_name: str = "",
) -> str
def ue_set_blueprint_variable_default(
    asset_path: str, variable_id: str, value,
) -> str
def ue_set_blueprint_variable_metadata(
    asset_path: str = None, variable_id: str = None,
    metadata: dict = None,
) -> str
def ue_set_blueprint_variable_replication(
    asset_path: str = None, variable_id: str = None,
    replication: dict = None,
) -> str

def ue_rename_blueprint_component(
    asset_path: str = None, component_id: str = None, new_name: str = None,
) -> str
def ue_reparent_blueprint_component(
    asset_path: str = None, component_id: str = None,
    parent_component_id: str = "",
) -> str
def ue_reorder_blueprint_component(
    asset_path: str = None, component_id: str = None,
    sibling_index: int = None,
) -> str
def ue_set_blueprint_component_transform(
    asset_path: str = None, component_id: str = None,
    transform: dict = None,
) -> str

def ue_get_blueprint_health(asset_path: str = None) -> str
def ue_snapshot_blueprint_graph(
    asset_path: str = None, graph_ids: list[str] = (),
) -> str
def ue_diff_blueprint_graphs(
    before_snapshot: dict = None, after_snapshot: dict = None,
    queries: list[dict] = (),
) -> str
```

The strict input schemas use the following required fields and closed rules:

| Action group | Required fields | Closed validation rules |
|---|---|---|
| `get_blueprint_brief` | `asset_path` | Unreal asset path |
| `inspect_blueprint` | `asset_path` | empty `queries` means one compact `overview`; otherwise 1-32 queries, approved `op`, detail `compact`/`detailed`, limit 1-500 |
| create function/macro | `asset_path`, `name` | ordered parameter arrays; access only public/protected/private; no extra parameter fields |
| rename/delete function/macro | `asset_path`, stable member ID | `new_name` for rename; fallback requires allow flag and old name |
| set function signature | `asset_path`, `function_id`, `inputs`, `outputs` | both complete arrays required, not patches |
| create/delete custom event | asset, graph/name or event ID | parameters ordered; fallback deletion also requires owner graph ID |
| add/remove dispatcher | asset plus name or dispatcher ID | ordered parameters; fallback removal requires old name |
| add/remove interface | `asset_path`, `interface_path` | full `/Script/` or Blueprint generated-class object path; no short class names |
| add reflected node | asset, graph ID, reference kind/path | kind is function/property_get/property_set/cast_to/enum_literal/make_struct/break_struct; path is full Unreal object path |
| set node properties | asset, node ID, properties | non-empty object; only the per-class allowlist in Task 11 |
| disconnect pins | asset, pin ID | exactly one of non-empty `other_pin_id` or `break_all=true` |
| rename/remove variable | asset, variable ID | rename also requires new name; fallback requires allow flag and old name |
| set variable default | asset, variable ID, `value` | `value` may be any JSON value but must validate against the resolved canonical type |
| set variable metadata | asset, variable ID, metadata | non-empty closed object of category/tooltip/visible/instance_editable/expose_on_spawn/save_game/cinematic |
| set variable replication | asset, variable ID, replication | exact mode object from Task 12; no unknown conditions |
| rename/reparent/reorder component | asset, component ID | rename needs new name; empty parent means root; sibling index is integer >= 0 |
| set component transform | asset, component ID, transform | non-empty closed object; location/rotation/scale are closed numeric XYZ or pitch/yaw/roll objects |
| health | `asset_path` | no additional parameters |
| snapshot | `asset_path` | zero or more stable graph IDs; empty means all supported graphs |
| diff | before/after snapshot | zero or more section queries; section enum nodes/pins/connections/properties/positions, detail compact/detailed, limit 1-500 |

All 29 output schemas require `success`, `status`, `summary`, `data`,
`changes`, `warnings`, `errors`, `next_actions`, and `trace_id`. Their common
schema is:

```python
RESULT_OUTPUT_SCHEMA = {
    "type": "object",
    "properties": {
        "success": {"type": "boolean"},
        "status": {"type": "string", "minLength": 1},
        "summary": {"type": "string"},
        "data": {"type": "object"},
        "changes": {"type": "array", "items": {"type": "object"}},
        "warnings": {"type": "array", "items": {"type": "object"}},
        "errors": {"type": "array", "items": {"type": "object"}},
        "next_actions": {"type": "array", "items": {"type": "object"}},
        "trace_id": {"type": "string", "minLength": 1},
    },
    "required": ["success", "status", "summary", "data", "changes",
                 "warnings", "errors", "next_actions", "trace_id"],
    "additionalProperties": False,
}
```

The empty `options={}` default is never mutated; `blueprint2.py` copies it with
`dict(options or {})` before constructing JSON.

## File responsibility map

### Unreal C++ plugin

- `Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h` —
  reflected Blueprint 2 entry points only; complex arguments are JSON strings.
- `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprint2Internal.h` —
  focused declarations for stable targets, canonical types, cursor pages,
  result builders, graph lookup, workflow-aware mutation scope, defaults,
  snapshot records, and version capabilities.
- `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprint2Internal.cpp` —
  the shared implementations and the only Blueprint 2 file containing engine
  minor-version guards.
- `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintInspection.cpp` —
  brief, capability serialization, query filtering, sorting, and pagination;
  owns the migrated implementations of legacy graph/variable/component lists.
- `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintMembers.cpp` —
  function, macro, event, dispatcher, and interface lifecycle.
- `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintGraph.cpp` —
  common/reflected node creation, allowlisted properties, stable pin
  connection/disconnection, layout, and migrated legacy graph mutations.
- `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintVariables.cpp` —
  variable lifecycle, canonical type/default conversion, metadata, and
  replication; owns the migrated legacy variable adapters.
- `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintComponents.cpp` —
  SCS lifecycle, GUID addressing, hierarchy, order, transform, reflected
  property allowlist, and migrated legacy component mutations.
- `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintDiagnostics.cpp` —
  compiler log normalization, health checks, deterministic snapshots, and
  JSON snapshot diff.
- `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprint2TypeTests.cpp` —
  editor automation coverage for canonical type/default parsing without
  exposing a test-only MCP action.
- `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper.cpp` —
  remove definitions moved into the six focused Blueprint files; keep
  unrelated helpers unchanged.
- `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelperInternal.h` —
  declare the editor-session and active-workflow accessors used by Blueprint 2.
- `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_Workflow.cpp` —
  expose read-only accessors over the existing lease globals; no workflow state
  machine changes.
- `Plugins/UnrealMCPython/Source/UnrealMCPython/UnrealMCPython.Build.cs` — keep
  the existing `UnrealEd`, `BlueprintGraph`, `Kismet`, and `Engine`
  dependencies; Blueprint 2 must not add an unnecessary runtime dependency.

### Unreal Python and in-editor tests

- `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint2.py` — shared
  Blueprint/subclass loading, copied request construction, helper invocation,
  and exception-to-JSON adaptation; no graph mutation logic.
- `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py` —
  keep existing wrappers/signatures and add the 29 exact public wrappers.
- `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/blueprint2_support.py` —
  unique `/Game/__MCPTests/Blueprint2_<uuid>` asset fixtures, snapshot helpers,
  cleanup, and retained-asset failure reporting.
- `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_inspection.py` —
  Stage 1 behavior and cursor tests.
- `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_members.py` —
  Stage 2 member lifecycle tests.
- `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_graph.py` —
  Stage 3 graph, pin, type, variable, and component tests.
- `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_diagnostics.py` —
  Stage 4 compile, health, snapshot, diff, and undo tests.
- `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/run_all.py` — load
  the four new suites in stage order.

### MCP server and offline tests

- `mcp-server/src/unreal_mcp/blueprint2_action_specs.py` — strict reusable
  JSON Schemas, safety declarations, examples, and output contracts for only
  the 29 new actions.
- `mcp-server/generate_catalog.py` — merge `BLUEPRINT2_ACTION_SPECS` with the
  legacy literal `ACTION_METADATA` for the Blueprint module.
- `mcp-server/src/unreal_mcp/special_action_specs.py`,
  `mcp-server/src/unreal_mcp/dispatcher.py`,
  `mcp-server/src/unreal_mcp/discovery.py`, and
  `mcp-server/src/unreal_mcp/workflows/handler.py` — remove the two obsolete
  workflow branches and prompt/capability plumbing.
- `mcp-server/tests/test_blueprint2_action_wrappers.py` — fake-Unreal tests for
  exact Python-to-C++ call shapes.
- `mcp-server/tests/test_blueprint2_contracts.py` — schema, safety, stable-ID,
  canonical type, cursor, snapshot, and diff contract tests.
- `mcp-server/tests/test_registry_generation.py`,
  `mcp-server/tests/test_discovery.py`,
  `mcp-server/tests/test_workflow_handler.py`, and
  `mcp-server/tests/test_coverage.py` — placeholder removal and named action
  coverage.
- `mcp-server/tests/test_e2e.py` — final real MCP/TCP/C++ workflow round trip.
- `mcp-server/src/unreal_mcp/dispatchers/_catalog.py` and `_registry.py` —
  regenerated, never hand-edited.
- `README.md` and `mcp-server/README.md` — document the Blueprint-first flow,
  stable IDs, compile-once rule, and supported versions.

---

## Stage 1 — compact inspection and public direction cleanup

### Task 1: Remove the gameplay-foundation placeholders

**Files:**
- Modify: `mcp-server/generate_catalog.py:87-119`
- Modify: `mcp-server/src/unreal_mcp/special_action_specs.py:338-389`
- Modify: `mcp-server/src/unreal_mcp/dispatcher.py:65-128`
- Modify: `mcp-server/src/unreal_mcp/discovery.py:151-168`
- Modify: `mcp-server/src/unreal_mcp/workflows/handler.py:10-90,225-242`
- Modify: `mcp-server/tests/test_registry_generation.py:14-25`
- Modify: `mcp-server/tests/test_discovery.py:55-63`
- Modify: `mcp-server/tests/test_workflow_handler.py:100-305`
- Modify: `mcp-server/tests/test_coverage.py:43-73`
- Regenerate: `mcp-server/src/unreal_mcp/dispatchers/_catalog.py`
- Regenerate: `mcp-server/src/unreal_mcp/dispatchers/_registry.py`

- [ ] **Step 1: Write the failing public-direction tests**

Change the workflow registry expectation to exactly five actions and replace
the prompt test with an absence assertion:

```python
def test_workflow_domain_exposes_only_generic_actions():
    expected = {"plan", "apply", "get", "cancel", "undo"}
    assert set(build()["workflow"]) == expected
    assert set(build_registry()["workflow"]) == expected


@pytest.mark.asyncio
async def test_gameplay_foundation_prompt_is_not_registered():
    from unreal_mcp.dispatcher import dispatcher_mcp
    prompts = await dispatcher_mcp.list_prompts()
    assert "gameplay_foundation" not in {prompt.name for prompt in prompts}
```

Add to the capabilities test:

```python
assert "gameplay_foundation_prompt" not in result["data"]["server"]
```

- [ ] **Step 2: Run the focused tests and verify the old branches are visible**

Run:

```powershell
Set-Location mcp-server
uv run --extra dev pytest tests/test_registry_generation.py tests/test_discovery.py tests/test_workflow_handler.py tests/test_coverage.py -q
```

Expected: FAIL because the catalog still contains two extra workflow actions,
the prompt is registered, and stale handler tests still name the hooks.

- [ ] **Step 3: Remove the obsolete paths**

Delete both entries from `EXTRA_ACTIONS["workflow"]` and
`SPECIAL_ACTION_SPECS["workflow"]`; remove them from `_LOCAL_ACTIONS`, delete
`gameplay_foundation_prompt`, remove `gameplay_planner`/`gameplay_verifier`
constructor arguments and `_foundation`, and remove the two action branches.
Delete their server-local coverage declarations and hook fixtures/tests. Keep
all generic workflow code byte-for-byte unless an import becomes unused.

- [ ] **Step 4: Regenerate and verify**

Run:

```powershell
Set-Location mcp-server
uv run python generate_catalog.py
uv run python validate_tools.py
uv run --extra dev pytest tests/test_registry_generation.py tests/test_discovery.py tests/test_workflow_handler.py tests/test_coverage.py -q
```

Expected: catalog check reports `261 actions across 22 domains` and all focused
tests PASS.

- [ ] **Step 5: Commit Stage 1 cleanup**

```powershell
git add mcp-server/generate_catalog.py mcp-server/src/unreal_mcp mcp-server/tests
git commit -m "chore: remove gameplay foundation placeholders"
```

### Task 2: Add strict Blueprint 2 registry contracts

**Files:**
- Create: `mcp-server/src/unreal_mcp/blueprint2_action_specs.py`
- Modify: `mcp-server/generate_catalog.py:20-190,360-385`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py:1-520`
- Create: `mcp-server/tests/test_blueprint2_contracts.py`
- Modify: `mcp-server/tests/test_registry_generation.py`

- [ ] **Step 1: Write failing contract and action-count tests**

Define the expected additions as one fixed set:

```python
NEW_BLUEPRINT2_ACTIONS = {
    "get_blueprint_brief", "inspect_blueprint",
    "create_blueprint_function", "rename_blueprint_function",
    "set_blueprint_function_signature", "delete_blueprint_function",
    "create_blueprint_macro", "delete_blueprint_macro",
    "create_custom_event", "delete_custom_event",
    "add_event_dispatcher", "remove_event_dispatcher",
    "add_blueprint_interface", "remove_blueprint_interface",
    "add_reflected_blueprint_node", "set_blueprint_node_properties",
    "disconnect_blueprint_pins", "rename_blueprint_variable",
    "remove_blueprint_variable", "set_blueprint_variable_default",
    "set_blueprint_variable_metadata", "set_blueprint_variable_replication",
    "rename_blueprint_component", "reparent_blueprint_component",
    "reorder_blueprint_component", "set_blueprint_component_transform",
    "get_blueprint_health", "snapshot_blueprint_graph",
    "diff_blueprint_graphs",
}


def test_blueprint2_action_set_and_total_are_exact():
    catalog = build()
    registry = build_registry()
    assert NEW_BLUEPRINT2_ACTIONS <= set(catalog["blueprint"])
    assert set(catalog["blueprint"]) == set(registry["blueprint"])
    assert len(catalog["blueprint"]) == 48
    assert sum(map(len, catalog.values())) == 290
```

Add schema assertions for canonical type recursion, inspect query limits, full
Unreal paths, stable target IDs, disconnect exclusivity, transform bounds,
diff query sections, all required safety fields, and examples validated by
`Draft202012Validator`.

- [ ] **Step 2: Run the contract tests and confirm missing actions**

Run:

```powershell
Set-Location mcp-server
uv run --extra dev pytest tests/test_blueprint2_contracts.py tests/test_registry_generation.py -q
```

Expected: FAIL because the 29 wrappers and their metadata do not exist.

- [ ] **Step 3: Add schema primitives and action records**

In `blueprint2_action_specs.py`, define and reuse these exact schema values:

```python
ASSET_PATH = {"type": "string", "format": "unreal-asset-path", "minLength": 1}
STABLE_ID = {
    "type": "string",
    "pattern": r"^(graph|node|pin|variable|component|interface):.+$",
}
NAME = {"type": "string", "pattern": r"^[A-Za-z_][A-Za-z0-9_]*$", "maxLength": 1024}
def type_spec(depth: int = 0) -> dict:
    choices = [
        {"type": "object", "properties": {"kind": {"enum": ["bool", "byte", "int", "int64", "string", "name", "text"]}}, "required": ["kind"], "additionalProperties": False},
        {"type": "object", "properties": {"kind": {"const": "real"}, "precision": {"enum": ["float", "double"]}}, "required": ["kind", "precision"], "additionalProperties": False},
        {"type": "object", "properties": {"kind": {"enum": ["enum", "struct"]}, "type_path": {"type": "string", "pattern": r"^/Script/"}}, "required": ["kind", "type_path"], "additionalProperties": False},
        {"type": "object", "properties": {"kind": {"enum": ["object", "class", "interface", "soft_object", "soft_class"]}, "class_path": {"type": "string", "pattern": r"^/Script/"}}, "required": ["kind", "class_path"], "additionalProperties": False},
    ]
    if depth < 8:
        nested = type_spec(depth + 1)
        choices.extend([
            {"type": "object", "properties": {"kind": {"enum": ["array", "set"]}, "item": nested}, "required": ["kind", "item"], "additionalProperties": False},
            {"type": "object", "properties": {"kind": {"const": "map"}, "key": nested, "value": nested}, "required": ["kind", "key", "value"], "additionalProperties": False},
        ])
    return {"oneOf": choices}

TYPE_SPEC = type_spec()
PARAMETER = {
    "type": "object",
    "properties": {"name": NAME, "type": TYPE_SPEC, "default": {}},
    "required": ["name", "type"],
    "additionalProperties": False,
}
```

Use schema helper functions that deep-copy nested schemas. The final
`BLUEPRINT2_ACTION_SPECS` must contain all 29 names, explicit
`input_schema`, `output_schema`, `effect`, `risk`, `idempotent`,
`supports_preview`, `supports_undo`, `requires_confirmation`,
`ue_versions=["5.6", "5.7", "5.8"]`, examples, and error examples.

Mark brief/inspect/snapshot/diff as read/low. Mark health as write/high with no
preview or undo. Mark create/set/add/connect-style actions write/medium and
undoable. Mark delete/remove/rename/reparent/reorder actions destructive/high
but still undoable because the C++ transaction records them.

- [ ] **Step 4: Merge Blueprint 2 specs during generation**

Import `BLUEPRINT2_ACTION_SPECS` in `generate_catalog.py`, merge it only for
the Blueprint domain before the missing/extra metadata check, and reject a name
collision with the legacy `ACTION_METADATA` map:

```python
if domain == "blueprint":
    overlap = set(metadata) & set(BLUEPRINT2_ACTION_SPECS)
    if overlap:
        raise ValueError(f"Duplicate Blueprint metadata: {sorted(overlap)}")
    metadata = {**metadata, **BLUEPRINT2_ACTION_SPECS}
```

Add the 29 wrapper signatures from the fixed signature section to
`blueprint_actions.py`. At this task they return a structured
`UE_VERSION_UNSUPPORTED` JSON envelope with capability
`blueprint2_cpp_core`; later tasks replace each body through `blueprint2.py`.
This makes catalog/schema work independently testable before C++ exists.

- [ ] **Step 5: Generate and pass contract tests**

Run:

```powershell
Set-Location mcp-server
uv run python generate_catalog.py
uv run python validate_tools.py
uv run --extra dev pytest tests/test_blueprint2_contracts.py tests/test_registry_generation.py -q
```

Expected: `OK: catalog and registry in sync (290 actions across 22 domains)`
and all focused tests PASS.

- [ ] **Step 6: Commit registry contracts**

```powershell
git add Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py mcp-server
git commit -m "feat: define Blueprint 2 action contracts"
```

### Task 3: Build stable IDs, cursors, results, and transaction integration

**Files:**
- Create: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprint2Internal.h`
- Create: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprint2Internal.cpp`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelperInternal.h`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_Workflow.cpp:48-58`
- Create: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint2.py`
- Create: `mcp-server/tests/test_blueprint2_action_wrappers.py`

- [ ] **Step 1: Write failing wrapper and internal-contract tests**

Load `blueprint2.py` with fake `unreal.EditorAssetLibrary` and
`MCPythonHelper`, then assert asset subclasses are accepted, request objects
are copied, helper JSON is passed through unchanged, and missing assets return
`PRECONDITION_FAILED`. Add source-contract assertions that the internal header
declares the exact structures and workflow accessors below.

```cpp
namespace UE::MCPython
{
const FGuid& GetEditorSessionId();
bool HasActiveWorkflowTransaction();
}

namespace UE::MCPython::Blueprint2
{
enum class ETargetKind : uint8 { Graph, Node, Pin, Variable, Component, Interface };

struct FTargetRef
{
    FString Id;
    FString OwnerId;
    FString Name;
    FString TypePath;
    bool bAllowNameFallback = false;
};

struct FResolvedTarget
{
    UObject* Object = nullptr;
    UEdGraph* Graph = nullptr;
    UEdGraphNode* Node = nullptr;
    UEdGraphPin* Pin = nullptr;
    FBPVariableDescription* Variable = nullptr;
    USCS_Node* Component = nullptr;
    FString Id;
    FString IdKind;
    bool bStable = false;
};

struct FPageRequest
{
    int32 Limit = 100;
    FString LastId;
    FString QueryDigest;
};

struct FRollbackResult
{
    bool bSucceeded = false;
    bool bDeferredToWorkflow = false;
    TArray<TSharedPtr<FJsonValue>> ResidualChanges;
};

class FMutationScope
{
public:
    explicit FMutationScope(const FText& Description);
    bool IsValid() const;
    void Modify(UObject* Object);
    FRollbackResult Rollback();
private:
    TUniquePtr<FScopedTransaction> LocalTransaction;
    int32 TransactionIndex = INDEX_NONE;
    FGuid TransactionGuid;
    bool bWorkflowOwned = false;
};
}
```

- [ ] **Step 2: Run tests and confirm the shared core is absent**

Run:

```powershell
Set-Location mcp-server
uv run --extra dev pytest tests/test_blueprint2_action_wrappers.py -q
```

Expected: FAIL because `blueprint2.py` and the C++ contract files are absent.

- [ ] **Step 3: Implement shared ID and result helpers**

Implement GUID encoding/decoding for all six prefixes, qualified-name SHA-1
fallbacks, strict stable resolution, legacy ID-or-name resolution, cursor
encoding/validation, canonical query digests, and structured success/failure
builders. `ResolveGraph` scans all graphs returned by `Blueprint->GetAllGraphs`;
node and pin resolution never crosses the resolved owner graph/node; variable
resolution uses `VarGuid`; component resolution uses `VariableGuid`.

The failure builder accepts only the approved error codes and always creates a
fresh trace GUID. It must serialize `path`, `retryable`, `hint`, and `details`
inside `errors[0]`; it must not serialize C++ call stacks or Python tracebacks.

- [ ] **Step 4: Implement workflow-aware mutation scope**

Expose `GetEditorSessionId()` and `HasActiveWorkflowTransaction()` from the
existing workflow translation unit. `FMutationScope` creates a local
`FScopedTransaction` only when no workflow transaction is active. Callers
finish all validation before constructing the scope, call `Modify()` on the
Blueprint and each affected graph/node/template, and call `Rollback()` when a
post-modification operation fails.

For a direct call, record the local transaction queue index/GUID, close the
transaction, verify it is still the current undo entry, and call guarded
`GEditor->UndoTransaction()`. For a workflow-owned call, do not touch the undo
queue; return `bDeferredToWorkflow=true` so the executor rolls back the single
outer transaction. If local guarded undo fails or post-rollback inspection
finds residual changes, return `ROLLBACK_FAILED` with normalized
`residual_changes`; never report the original operation error as if recovery
succeeded.

Do not call engine helpers known to create nested transactions from a
workflow-owned mutation: specifically avoid
`FBlueprintEditorUtils::RenameMemberVariable` and
`FBlueprintEditorUtils::RemoveInterface` in later tasks.

- [ ] **Step 5: Add version capability adapters**

Implement one `BuildCapabilities(UBlueprint*)` result containing
`api_version=2`, `engine_version`, supported canonical scalar/container kinds,
node families, K2 schema support, compiler-token support, and SCS operations.
Return per-asset false flags for missing SCS or non-K2 graphs. Confirm the
existing private module dependencies cover all includes; do not add
`BlueprintEditorLibrary` because member creation uses `FBlueprintEditorUtils`,
and do not add any dependency to public modules.

- [ ] **Step 6: Add the thin Python adapter**

Create `blueprint2.py` with these concrete functions:

```python
def load_blueprint(asset_path: str): ...
def call_asset_helper(helper_name: str, asset_path: str, request: dict | None = None) -> str: ...
def call_json_helper(helper_name: str, request: dict) -> str: ...
def target_request(stable_id: str, *, allow_name_fallback=False,
                   owner_id="", name="", type_path="") -> dict: ...
```

`call_asset_helper` loads `unreal.Blueprint` subclasses, converts the helper
name with `getattr(unreal.MCPythonHelper, helper_name)`, and passes compact
`json.dumps(..., separators=(",", ":"), ensure_ascii=False)`. It returns the
helper string without parsing and reserializing it.

- [ ] **Step 7: Run offline tests and a closed-editor build**

Run:

```powershell
Set-Location mcp-server
uv run --extra dev pytest tests/test_blueprint2_action_wrappers.py tests/test_contracts.py -q
Set-Location ..
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' UnrealMCPSampleEditor Win64 Development "$PWD\UnrealMCPSample.uproject" -WaitMutex -NoHotReload
```

Expected: offline tests PASS and UBT ends with `Result: Succeeded`.

- [ ] **Step 8: Commit the Blueprint 2 core**

```powershell
git add Plugins/UnrealMCPython/Source Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint2.py mcp-server/tests/test_blueprint2_action_wrappers.py
git commit -m "feat: add Blueprint 2 stable target core"
```

### Task 4: Implement Blueprint brief and runtime capabilities

**Files:**
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h`
- Create: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintInspection.cpp`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper.cpp:182-365,1264-1310`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/util_actions.py`
- Create: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/blueprint2_support.py`
- Create: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_inspection.py`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/run_all.py`
- Modify: `mcp-server/tests/test_discovery.py`

- [ ] **Step 1: Write the failing in-editor brief tests**

Create a unique Actor Blueprint in `setUp`, delete it in `finally`/`tearDown`,
and add `test_get_blueprint_brief`. Assert asset/class/parent/generated/skeleton
paths, compile status, interface/component/graph names, all ten counts,
asset-specific capabilities, and absence of embedded `nodes` and `pins` arrays.
Add `test_get_blueprint_brief_accepts_blueprint_subclass` using a Widget
Blueprint when UMG is enabled.

- [ ] **Step 2: Run the focused suite and verify the stub response**

In Unreal Python execute:

```python
import runpy
runpy.run_module("UnrealMCPython.tests.test_blueprint2_inspection", run_name="__main__")
```

Expected: `test_get_blueprint_brief` FAILS with
`UE_VERSION_UNSUPPORTED`/`blueprint2_cpp_core`.

- [ ] **Step 3: Add reflected declarations and brief serialization**

Add:

```cpp
UFUNCTION(BlueprintCallable, Category="Editor|MCPython")
static FString GetBlueprintBrief(UBlueprint* Blueprint);

UFUNCTION(BlueprintCallable, Category="Editor|MCPython")
static FString GetBlueprint2Capabilities(UBlueprint* Blueprint);
```

Serialize graph names without node bodies, top-level component names, interface
paths, compile status, and counts for variables/components/functions/macros/
events/dispatchers/interfaces/graphs/nodes. Count custom events by
`UK2Node_CustomEvent`, dispatchers by `PC_MCDelegate`, and graph nodes across
all graphs exactly once.

Move `GetBlueprintGraphInfo`, `ListCallableFunctions`,
`ListBlueprintVariables`, and `ListBlueprintComponents` definitions from
`MCPythonHelper.cpp` into the inspection file unchanged first; then add stable
IDs and only additive fields so their existing tests remain valid.

Extend `FMCPythonBlueprintNodeInfo`, `FMCPythonBlueprintPinInfo`, and
`FMCPythonPinLinkInfo` with graph/node/pin stable ID strings. Keep the selected
node info wrapper's legacy numeric `id`, but add `stable_id`, `graph_id`, and
stable pin/link IDs; add the same stable fields to `get_selected_bp_nodes`.

- [ ] **Step 4: Replace the public stub and expose project capability data**

`ue_get_blueprint_brief` calls
`blueprint2.call_asset_helper("get_blueprint_brief", asset_path)`. Extend
`util_actions.ue_get_project_info` to parse
`MCPythonHelper.get_blueprint2_capabilities(None)` and return it under
`blueprint2`; extend the offline fake-Unreal project-info test accordingly.

- [ ] **Step 5: Build and run brief plus legacy compatibility tests**

Run UBT with the Task 3 command, restart the editor, then run:

```python
import unittest
from UnrealMCPython.tests import test_blueprint, test_blueprint2_inspection
suite = unittest.TestSuite()
suite.addTests(unittest.defaultTestLoader.loadTestsFromModule(test_blueprint))
suite.addTests(unittest.defaultTestLoader.loadTestsFromModule(test_blueprint2_inspection))
unittest.TextTestRunner(verbosity=2).run(suite)
```

Expected: all existing Blueprint tests and the brief tests PASS; cleanup reports
no assets under `/Game/__MCPTests`.

- [ ] **Step 6: Commit brief inspection**

```powershell
git add Plugins/UnrealMCPython mcp-server/tests
git commit -m "feat: add compact Blueprint brief"
```

### Task 5: Implement filtered, paginated inspection

**Files:**
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintInspection.cpp`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_inspection.py`
- Modify: `mcp-server/tests/test_blueprint2_action_wrappers.py`

- [ ] **Step 1: Add one failing test per inspection behavior**

Add tests named `test_inspect_blueprint`,
`test_inspect_each_supported_op`, `test_inspect_compact_omits_details`,
`test_inspect_detailed_includes_requested_details`,
`test_inspect_paginates_each_query_independently`,
`test_inspect_rejects_stale_cursor`,
`test_inspect_rejects_query_mismatched_cursor`, and
`test_inspect_top_level_cursor_requires_one_query`. Build at least 120 nodes so
the default 100-item page produces a cursor.

- [ ] **Step 2: Run the suite and verify the inspect stub fails**

Use the Task 4 in-editor command. Expected: brief tests PASS and all inspect
tests FAIL at the stub.

- [ ] **Step 3: Implement the bounded query engine**

Add:

```cpp
UFUNCTION(BlueprintCallable, Category="Editor|MCPython")
static FString InspectBlueprint(UBlueprint* Blueprint, const FString& RequestJson);
```

Accept only `overview`, `variables`, `variable_defaults`, `components`,
`component_hierarchy`, `functions`, `macros`, `events`, `dispatchers`,
`interfaces`, `nodes`, `pins`, and `connections`. Validate all queries before
serializing any result. Apply graph/member/node/kind/name-pattern/class-path
filters, sort by stable ID then qualified name, clamp default limit to 100 and
reject limits above 500, and issue one cursor per query result.

Compact records contain ID triplets, kind, name, type summary, and child/count
summaries. Detailed records add titles, positions, typed pins/defaults,
metadata, component defaults, and linked IDs only for requested records.
Never include a full nested graph when the query asks for graph overview.

- [ ] **Step 4: Replace the wrapper stub and pass wrapper tests**

Construct request JSON as:

```python
request = {"queries": [dict(query) for query in queries]}
if cursor:
    request["cursor"] = cursor
return blueprint2.call_asset_helper("inspect_blueprint", asset_path, request)
```

Run:

```powershell
Set-Location mcp-server
uv run --extra dev pytest tests/test_blueprint2_action_wrappers.py tests/test_blueprint2_contracts.py -q
```

Expected: PASS.

- [ ] **Step 5: Build and run Stage 1 in-editor gates**

Run UBT, restart the editor, and run `test_blueprint2_inspection` plus legacy
`test_blueprint`. Expected: all tests PASS, stale/query-mismatched cursors
return `INVALID_INPUT`, and no retained test package is listed.

- [ ] **Step 6: Commit filtered inspection**

```powershell
git add Plugins/UnrealMCPython mcp-server/tests
git commit -m "feat: add filtered Blueprint inspection"
```

---

## Stage 2 — members, signatures, dispatchers, and interfaces

### Task 6: Implement the canonical type and default model

**Files:**
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprint2Internal.h`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprint2Internal.cpp`
- Modify: `mcp-server/tests/test_blueprint2_contracts.py`
- Create: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprint2TypeTests.cpp`

- [ ] **Step 1: Write failing canonical-type tests**

Add C++ automation table tests for bool, byte, int, int64, float, double, string, name,
text, enum, struct, object, class, interface, soft object, soft class, array,
set, and map. For path-backed types, use
`/Script/Engine.ECollisionChannel`, `/Script/CoreUObject.Vector`,
`/Script/Engine.Actor`, and `/Script/Engine.Texture2D`. Assert unknown kinds,
wrong object classes, recursive containers beyond depth 8, set/map container
keys, and lossy numeric defaults return `INVALID_INPUT` with the exact JSON
path of the invalid field.

- [ ] **Step 2: Build the failing automation test**

Run the UE 5.7 UBT command from Task 3. Expected: FAIL because
`ParseTypeSpec`, `SerializeTypeSpec`, and `NormalizeDefaultValue` are not yet
declared/defined for the automation test.

- [ ] **Step 3: Implement `FEdGraphPinType` parsing and serialization**

Add these exact internal entry points:

```cpp
bool ParseTypeSpec(
    const TSharedRef<FJsonObject>& Spec,
    FEdGraphPinType& OutType,
    FError& OutError,
    const FString& Path = TEXT("params.type"),
    int32 Depth = 0);
TSharedRef<FJsonObject> SerializeTypeSpec(const FEdGraphPinType& Type);
bool NormalizeDefaultValue(
    const FEdGraphPinType& Type,
    const TSharedPtr<FJsonValue>& JsonValue,
    UObject* Owner,
    FNormalizedDefault& OutDefault,
    FError& OutError,
    const FString& Path);
```

Map scalar kinds to `PC_Boolean`, `PC_Byte`, `PC_Int`, `PC_Int64`,
`PC_Float`, `PC_Double`, `PC_String`, `PC_Name`, `PC_Text`, `PC_Enum`,
`PC_Struct`, `PC_Object`, `PC_Class`, `PC_Interface`, `PC_SoftObject`, and
`PC_SoftClass`. Load path-backed types with `StaticFindObject`/`LoadObject`
and verify the exact `UEnum`, `UScriptStruct`, or `UClass` category before
accepting it. Set `ContainerType` to array/set/map and recursively populate the
map value terminal type.

- [ ] **Step 4: Validate complete defaults before mutation**

Convert JSON scalars and canonical full object paths into Unreal import text.
Serialize struct keys in reflected property order and containers in JSON order.
Then call `UEdGraphSchema_K2::GetPinDefaultValuesFromString` followed by
`DefaultValueSimpleValidation`. Do not write `FBPVariableDescription::DefaultValue`
or a pin until validation succeeds for the complete value. Return normalized
default string/object/text separately so callers can apply the correct field.

Legacy aliases normalize as follows only for pre-existing actions:

```text
bool->bool, byte->byte, int->int, int64->int64, float->real/float,
real->real/double, string->string, name->name, text->text
```

New actions accept only structured types.

- [ ] **Step 5: Build and run canonical-type automation tests**

Run UBT, then:

```powershell
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' "$PWD\UnrealMCPSample.uproject" -unattended -nop4 -nosplash -NullRHI -ExecCmds="Automation RunTests UnrealMCP.Blueprint2.CanonicalTypes; Quit" -TestExit="Automation Test Queue Empty" -log
```

Expected: automation reports `UnrealMCP.Blueprint2.CanonicalTypes` successful;
every valid type round-trips through `SerializeTypeSpec`, invalid paths identify
the correct parameter path, and no Blueprint becomes dirty on validation
failure.

- [ ] **Step 6: Commit canonical types**

```powershell
git add Plugins/UnrealMCPython/Source mcp-server/tests
git commit -m "feat: add canonical Blueprint types"
```

### Task 7: Implement Blueprint function lifecycle and complete signatures

**Files:**
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h`
- Create: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintMembers.cpp`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py`
- Create: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_members.py`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/run_all.py`
- Modify: `mcp-server/tests/test_blueprint2_action_wrappers.py`

- [ ] **Step 1: Write one failing lifecycle test per public function action**

Add `test_create_blueprint_function`, `test_rename_blueprint_function`,
`test_set_blueprint_function_signature`, and
`test_delete_blueprint_function`. Create `ComputeScore` with ordered `Actor`
and `double` inputs plus bool/double outputs, inspect it, replace the complete
signature in a different order, rename it, compile it, delete it, and inspect
after each mutation. Assert the graph GUID remains unchanged across signature
and rename operations.

Add rejection tests for duplicate member names, duplicate parameter names,
invalid access, an interface graph target, an unstable ID without explicit
fallback, and a fallback that matches more than one member.

- [ ] **Step 2: Run the member suite and verify all four stubs fail**

Expected: the four lifecycle tests return the temporary capability error while
the canonical-type tests remain green.

- [ ] **Step 3: Add exact reflected C++ entry points**

```cpp
static FString CreateBlueprintFunction(UBlueprint* Blueprint, const FString& RequestJson);
static FString RenameBlueprintFunction(UBlueprint* Blueprint, const FString& RequestJson);
static FString SetBlueprintFunctionSignature(UBlueprint* Blueprint, const FString& RequestJson);
static FString DeleteBlueprintFunction(UBlueprint* Blueprint, const FString& RequestJson);
```

Create the graph with `FBlueprintEditorUtils::CreateNewGraph` and
`FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, Graph, true,
nullptr)`. Find exactly one `UK2Node_FunctionEntry` and zero or one
`UK2Node_FunctionResult`. Inputs are user-defined output pins on the entry;
outputs are user-defined input pins on the result. Create a result node through
the K2 schema when the desired output list is non-empty.

- [ ] **Step 4: Apply complete desired signatures atomically**

Validate the full ordered input/output list and metadata before creating
`FMutationScope`. Remove only user-defined pins, recreate them in submitted
order with `CreateUserDefinedPin`, and apply defaults only after type/default
normalization. Set entry flags from a closed mask:

```cpp
FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public |
FUNC_Protected | FUNC_Private | FUNC_BlueprintPure | FUNC_Const
```

Exactly one of public/protected/private is set. Write category and tooltip via
`FKismetUserDeclaredFunctionMetadata`; call `ReconstructNode()` on entry/result
and `MarkBlueprintAsStructurallyModified` once.

- [ ] **Step 5: Rename and delete without implicit compile/save**

Resolve by graph GUID, validate collision-free names across functions, macros,
dispatchers, variables, components, and interface functions, then use
`FBlueprintEditorUtils::RenameGraph`. Delete only user-created function graphs
with `FBlueprintEditorUtils::RemoveGraph`. Return create/update/delete change
records, the stable graph ID, and `next_actions` recommending one explicit
compile. Do not compile or save.

- [ ] **Step 6: Replace wrappers and pass offline call-shape tests**

Each wrapper builds a copied request whose keys exactly match its signature and
calls the matching helper. Run:

```powershell
Set-Location mcp-server
uv run --extra dev pytest tests/test_blueprint2_action_wrappers.py tests/test_blueprint2_contracts.py tests/test_workflow_planner.py -q
```

Expected: PASS; the workflow planner accepts all four schemas and fingerprints
their `asset_path`.

- [ ] **Step 7: Build and run function lifecycle tests**

Run UBT, restart the editor, and run `test_blueprint2_members`. Expected: all
four named action tests PASS, compile diagnostics contain no error, and cleanup
removes the unique package.

- [ ] **Step 8: Commit function authoring**

```powershell
git add Plugins/UnrealMCPython mcp-server/tests
git commit -m "feat: author Blueprint functions"
```

### Task 8: Implement macros, custom events, and event dispatchers

**Files:**
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintMembers.cpp`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_members.py`
- Modify: `mcp-server/tests/test_blueprint2_action_wrappers.py`

- [ ] **Step 1: Write six failing named action tests**

Add `test_create_blueprint_macro`, `test_delete_blueprint_macro`,
`test_create_custom_event`, `test_delete_custom_event`,
`test_add_event_dispatcher`, and `test_remove_event_dispatcher`. Verify ordered
pins and metadata through `inspect_blueprint`, stable IDs after compile, and
complete disappearance after deletion. Add collision and invalid-graph tests.

- [ ] **Step 2: Run the suite and verify the six stubs fail**

Expected: function lifecycle remains green; all six new tests fail at their
stub responses.

- [ ] **Step 3: Implement macro lifecycle**

Create macros with `CreateNewGraph` and `AddMacroGraph`. Find the entry/exit
`UK2Node_Tunnel` nodes, create ordered output pins on entry and input pins on
exit, set tunnel metadata and purity, and mark structurally modified. Delete by
graph GUID only from `Blueprint->MacroGraphs`; reject inherited or interface
graphs.

- [ ] **Step 4: Implement custom event lifecycle**

Resolve a K2 ubergraph by stable graph ID, create `UK2Node_CustomEvent` through
`FGraphNodeCreator<UK2Node_CustomEvent>`, set `CustomFunctionName`, finalize the
node, allocate pins, then create ordered user-defined output parameter pins.
Return its `NodeGuid` and pin IDs. Delete by node GUID after verifying the node
class is `UK2Node_CustomEvent`.

- [ ] **Step 5: Implement dispatcher lifecycle exactly like the editor**

Create a `PC_MCDelegate` member variable, create a same-name K2 graph, call
`CreateDefaultNodesForGraph` and `CreateFunctionGraphTerminators`, mark the
entry editable, then append the graph to `Blueprint->DelegateSignatureGraphs`.
Populate ordered signature pins and metadata. If graph creation fails after
the variable was added, roll back the mutation scope so both changes recover.

For removal, resolve the variable GUID, verify `PC_MCDelegate`, remove its
signature graph with `RemoveGraph`, then call `RemoveMemberVariable`. Return
both delete records and never compile or save.

- [ ] **Step 6: Replace wrappers, build, and run tests**

Run offline wrapper/contracts/planner tests, UBT, and the member in-editor
suite. Expected: all ten Stage 2 member actions tested so far PASS and no
partial dispatcher remains after an injected invalid signature.

- [ ] **Step 7: Commit macro/event/dispatcher authoring**

```powershell
git add Plugins/UnrealMCPython mcp-server/tests
git commit -m "feat: author Blueprint macros and events"
```

### Task 9: Implement Blueprint interface lifecycle without nested transactions

**Files:**
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintMembers.cpp`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_members.py`
- Modify: `mcp-server/tests/test_blueprint2_action_wrappers.py`

- [ ] **Step 1: Write failing add/remove interface tests**

Create a temporary Blueprint Interface with one function, add it to the Actor
Blueprint, assert the returned interface ID and implementation graph IDs,
compile, remove it, and inspect the post-state. Add tests for non-interface
classes, duplicate implementation, missing implementation, and removal while
an outer workflow lease is active.

- [ ] **Step 2: Run and verify the two interface stubs fail**

Expected: prior Stage 2 tests PASS; add/remove interface tests fail at the
temporary capability response.

- [ ] **Step 3: Add a validated interface**

Load the full class path as `UClass`, require `CLASS_Interface`, reject an
existing path, then call
`FBlueprintEditorUtils::ImplementNewInterface(Blueprint,
InterfaceClass->GetClassPathName())`. Inspect the resulting
`FBPInterfaceDescription::Graphs`, serialize their `GraphGuid`s, and return
them in `data.implementation_graph_ids`.

- [ ] **Step 4: Remove an interface inside the owning transaction**

Do not call `FBlueprintEditorUtils::RemoveInterface`, because UE 5.7 opens its
own `FScopedTransaction`. Adapt its public-API sequence into the MCP mutation
scope: resolve the `FBPInterfaceDescription`, call
`RemoveInterfaceFunction` for its functions with `bPreserveFunction=false`,
remove matching interface event nodes, remove the description from
`ImplementedInterfaces`, call `RefreshAllNodes`, and mark structurally
modified. The action does not expose a preserve-functions mode.

- [ ] **Step 5: Replace wrappers and run Stage 2 gates**

Run wrapper/contracts/workflow-planner tests, UBT, legacy Blueprint tests, and
the full member suite. Expected: PASS, one outer workflow undo entry, no nested
entry, and no retained temporary Blueprint Interface.

- [ ] **Step 6: Commit interface authoring**

```powershell
git add Plugins/UnrealMCPython mcp-server/tests
git commit -m "feat: manage Blueprint interfaces"
```

---

## Stage 3 — graphs, pins, variables, and components

### Task 10: Split and expand common/reflected node creation

**Files:**
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h`
- Create: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintGraph.cpp`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper.cpp:681-1045,1379-1455`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py`
- Create: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_graph.py`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/run_all.py`
- Modify: `mcp-server/tests/test_blueprint2_action_wrappers.py`

- [ ] **Step 1: Write failing reflected and common-family tests**

Add `test_add_reflected_blueprint_node` for function, property get/set, class
cast, enum literal, make struct, and break struct paths. Extend the existing
`test_add_blueprint_node` coverage with branch, sequence, cast, variable
get/set, arithmetic, comparison, event, select, enum/int/string/name switch,
reroute, comment, make struct, and break struct cases. Assert returned node and
pin IDs, positions, class paths, and detailed inspection records.

- [ ] **Step 2: Run and verify reflected-node tests fail**

Expected: legacy common cases remain green; new families and the reflected
action fail.

- [ ] **Step 3: Move legacy graph code before changing behavior**

Move `AddBlueprintNode`, `ConnectBlueprintPins`, `RemoveBlueprintNode`,
`BuildBlueprintGraph`, `SetBlueprintNodePosition`,
`SetBlueprintNodePinDefault`, and their private node/layout helpers out of
`MCPythonHelper.cpp`. Build and run legacy tests before adding families; this
intermediate build must be behavior-identical.

- [ ] **Step 4: Implement reflected node factories**

Use `FGraphNodeCreator<T>` and finalize exactly once:

```text
function      -> UK2Node_CallFunction::SetFromFunction
property_get  -> UK2Node_VariableGet::VariableReference.SetFromField
property_set  -> UK2Node_VariableSet::VariableReference.SetFromField
cast_to       -> UK2Node_DynamicCast::TargetType
enum_literal  -> UK2Node_EnumLiteral::Enum
make_struct   -> UK2Node_MakeStruct::StructType
break_struct  -> UK2Node_BreakStruct::StructType
```

Resolve only complete paths. Function/property paths must identify the exact
reflected field; class/enum/struct paths must resolve to the exact expected
UObject class. Reject latent functions in function graphs that disallow them,
non-Blueprint-callable functions, editor-incompatible graph schemas, and node
classes that fail `CanCreateUnderSpecifiedSchema`.

- [ ] **Step 5: Expand the legacy common-node request**

Support these `node_json.type` values and fields:

```json
{"type":"Branch"}
{"type":"Sequence","output_count":3}
{"type":"CastTo","class_path":"/Script/Engine.Actor"}
{"type":"VariableGet","variable_id":"variable:<guid>"}
{"type":"VariableSet","variable_id":"variable:<guid>"}
{"type":"Operator","operator":"add","operand_type":{"kind":"real","precision":"double"}}
{"type":"Event","function_path":"/Script/Engine.Actor:ReceiveBeginPlay"}
{"type":"Select","option_count":3,"value_type":{"kind":"int"}}
{"type":"Switch","switch_kind":"enum","enum_path":"/Script/Engine.ECollisionChannel"}
{"type":"Reroute"}
{"type":"Comment","text":"Validate input","size_x":400,"size_y":180}
{"type":"MakeStruct","struct_path":"/Script/CoreUObject.Vector"}
{"type":"BreakStruct","struct_path":"/Script/CoreUObject.Vector"}
```

For operators, scan `UKismetMathLibrary` functions by the exact prefixes
`Add_`, `Subtract_`, `Multiply_`, `Divide_`, `EqualEqual_`, `NotEqual_`,
`Less_`, `LessEqual_`, `Greater_`, and `GreaterEqual_`; choose the unique
candidate whose first two input properties and return property exactly match
the canonical requested pin type. Create a `UK2Node_PromotableOperator` from
that function. Reject zero or multiple matches.

- [ ] **Step 6: Replace the reflected wrapper and preserve legacy outputs**

New results use the structured envelope. Existing `add_blueprint_node` also
keeps `node_name`, `node_title`, `pins`, and `message` at top level while
adding stable IDs and change records. Existing name fields may contain stable
IDs; old exact names continue to resolve.

- [ ] **Step 7: Build and run focused graph tests**

Run offline wrapper/contracts tests, UBT, `test_blueprint`, and
`test_blueprint2_graph`. Expected: every node family PASS and no invalid
reflected path mutates the graph.

- [ ] **Step 8: Commit node creation**

```powershell
git add Plugins/UnrealMCPython mcp-server/tests
git commit -m "feat: expand Blueprint node authoring"
```

### Task 11: Add allowlisted node properties and stable pin connect/disconnect

**Files:**
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintGraph.cpp`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_graph.py`
- Modify: `mcp-server/tests/test_blueprint2_action_wrappers.py`

- [ ] **Step 1: Write failing action and diagnostic tests**

Add `test_set_blueprint_node_properties`,
`test_disconnect_blueprint_pins`, stable-ID variants of the existing connect
test, same-direction/incompatible-type tests, schema-conversion insertion, pin
default validation, and unknown/read-only property rejection. Assert validation
completes before `Modify()` by comparing snapshots after each rejected call.

- [ ] **Step 2: Run and verify both new actions fail**

Expected: node creation is green; property/disconnect stubs fail.

- [ ] **Step 3: Implement a closed node-property allowlist**

Permit base-node `comment`, `comment_bubble_visible`, `enabled_state`, and
`position`; `UK2Node_ExecutionSequence.output_count`;
`UK2Node_Select.option_count`; string/name/int switch `cases`; and
`pin_defaults` keyed by stable pin ID. Use the node-class check before reading
any value. Reject every other field with `INVALID_INPUT` and include
`details.allowed_properties`.

For pin defaults, normalize against the pin type and call the K2 schema setter;
never write `DefaultValue`/`DefaultObject` directly. Reconstruct only node
classes whose public count/case API requires it, then re-resolve and return all
new pin IDs.

- [ ] **Step 4: Route all connections through the schema**

Resolve both stable pin IDs and require one input/one output. Capture graph
node GUIDs before `UEdGraphSchema_K2::TryCreateConnection`, call
`CanCreateConnection` for a structured preflight diagnostic, then diff the
node GUID set after the call to return `data.inserted_conversion_node_ids`.
Include source/target canonical type summaries when disallowed.

Update the legacy `connect_blueprint_pins` implementation to call this path.
Its four existing string arguments accept `node:<guid>`/`pin:<guid>` or exact
legacy names and retain the old `message` field.

- [ ] **Step 5: Implement exact and break-all disconnection**

Require either `other_pin_id` or `break_all=true`, never both. Use
`BreakSinglePinLink` for the exact pair and `BreakPinLinks` for all links.
Reject unlinked pairs as `PRECONDITION_FAILED`; return one delete change record
per removed normalized connection.

- [ ] **Step 6: Build, test, and commit**

Run offline tests, UBT, legacy Blueprint tests, and focused graph tests.
Expected: compatible/incompatible/conversion/disconnect cases PASS and graph
snapshots remain unchanged after rejected properties or connections.

```powershell
git add Plugins/UnrealMCPython mcp-server/tests
git commit -m "feat: add stable Blueprint pin editing"
```

### Task 12: Implement variable lifecycle, defaults, metadata, and replication

**Files:**
- Create: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintVariables.cpp`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint.py`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_graph.py`
- Modify: `mcp-server/tests/test_blueprint2_action_wrappers.py`

- [ ] **Step 1: Write five failing named action tests and legacy guards**

Add `test_rename_blueprint_variable`, `test_remove_blueprint_variable`,
`test_set_blueprint_variable_default`,
`test_set_blueprint_variable_metadata`, and
`test_set_blueprint_variable_replication`. Cover all canonical scalar/path/
container defaults, metadata fields, replicated/rep-notify modes, name
collisions, inherited variables, and invalid full-value rollback.

Extend existing add/list/flags tests to assert no implicit compile and no
implicit save, while retaining their existing result fields.

- [ ] **Step 2: Run and verify new actions plus no-save guards fail**

Expected: five stubs fail and the legacy add/flags actions violate the new
no-save/no-compile assertions.

- [ ] **Step 3: Move legacy variable operations into C++**

Make `ue_add_variable` normalize its legacy alias and call the new C++ add
path; make `ue_set_variable_flags` call metadata mutation. Remove
`BlueprintEditorLibrary.compile_blueprint` and
`EditorAssetLibrary.save_loaded_asset` from both wrappers. Preserve
`variable_name`, `variable_type`, and `applied` response fields.

Add reflected compatibility adapters:

```cpp
static FString AddBlueprintVariable(UBlueprint* Blueprint, const FString& RequestJson);
static FString SetBlueprintVariableFlags(UBlueprint* Blueprint, const FString& RequestJson);
```

- [ ] **Step 4: Implement GUID-targeted variable lifecycle**

Resolve `FBPVariableDescription::VarGuid`. Remove with
`RemoveMemberVariable`. For rename, do not call
`RenameMemberVariable` because it creates a nested transaction and may show a
rep-notify modal. Instead update `VarName`/`FriendlyName`, preserve or
explicitly update `RepNotifyFunc`, call `ReplaceVariableReferences`, validate
child variable collisions, rename a delegate graph when the variable is a
dispatcher, and mark structurally modified.

- [ ] **Step 5: Implement defaults and metadata**

Set `DefaultValue` only from `NormalizeDefaultValue`. Metadata accepts only
`category`, `tooltip`, `visible`, `instance_editable`, `expose_on_spawn`,
`save_game`, and `cinematic`. Map these to
`SetBlueprintVariableCategory`, `SetBlueprintVariableMetaData`/
`RemoveBlueprintVariableMetaData`, the Blueprint-visible/private flag,
`MD_ExposeOnSpawn`, `SetVariableSaveGameFlag`, and `SetInterpFlag`. Submit the
complete desired metadata object and return before/after values.

- [ ] **Step 6: Implement replication modes**

Accept exactly:

```json
{"mode":"none"}
{"mode":"replicated","condition":"none"}
{"mode":"rep_notify","notify_function":"OnRep_Health","condition":"owner_only"}
```

Map the closed condition set to `ELifetimeCondition`, update
`PropertyFlags` (`CPF_Net`, `CPF_RepNotify`) plus `ReplicationCondition`, and
use `SetBlueprintVariableRepNotifyFunc`. Validate the named notify function
signature before mutation. Reject replication on unsupported Blueprint classes
with `UE_VERSION_UNSUPPORTED`.

- [ ] **Step 7: Build, test, and commit**

Run offline tests, UBT, legacy Blueprint tests, and graph tests. Expected: all
five new actions PASS, invalid defaults leave byte-identical snapshots, and no
variable mutation compiles or saves.

```powershell
git add Plugins/UnrealMCPython mcp-server/tests
git commit -m "feat: expand Blueprint variable editing"
```

### Task 13: Implement GUID-addressed component hierarchy and transforms

**Files:**
- Create: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintComponents.cpp`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper.cpp:1206-1378`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint.py`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_graph.py`
- Modify: `mcp-server/tests/test_blueprint2_action_wrappers.py`

- [ ] **Step 1: Write four failing named action tests**

Add `test_rename_blueprint_component`,
`test_reparent_blueprint_component`,
`test_reorder_blueprint_component`, and
`test_set_blueprint_component_transform`. Cover nested and root components,
reference-preserving rename, reparent-to-root, cycle rejection, first/middle/
last sibling indexes, location/rotation/scale, non-scene transforms, read-only
properties, and unknown GUIDs.

- [ ] **Step 2: Run and verify all four stubs fail**

Expected: prior graph/variable tests PASS; component actions fail at stubs.

- [ ] **Step 3: Move legacy SCS operations and add stable IDs**

Move add/remove/property definitions from `MCPythonHelper.cpp` unchanged, then
add transaction integration. Keep `ListBlueprintComponents` in the inspection
translation unit where Task 4 already added GUID fields. Existing component name
arguments accept a stable ID or exact legacy name. `set_component_property`
must reject `CPF_EditConst`, transient, delegate, and unknown properties before
calling `ImportText_Direct`.

- [ ] **Step 4: Implement rename and transform**

Resolve `USCS_Node::VariableGuid`. Rename through
`FBlueprintEditorUtils::RenameComponentMemberVariable` after global collision
validation so references and inheritable templates update. Transform only a
`USceneComponent` template; accept partial `location`, `rotation`, and `scale`
objects, validate all numeric members first, call `Modify`, then set relative
transform fields.

- [ ] **Step 5: Implement cycle-safe reparent and explicit sibling order**

Find the current parent by scanning `GetAllNodes`. Reject self/descendant
parents with a DFS visited set. Detach with `RemoveChildNode(Node, true)` or
`SCS->RemoveNode(Node, false)`, then attach with `AddChildNode(Node, true)` or
`SCS->AddNode(Node)`. Reorder child siblings by copying the desired sequence,
removing each child with `bRemoveFromAllNodes=false`, and re-adding in order.
For roots, remove and re-add the ordered root list through the public SCS API,
call `ValidateSceneRootNodes` after the final add, and verify the resulting
order (noting that `AddNode` may also validate internally). If the engine
normalizes it to a different order, roll back and return
`UE_VERSION_UNSUPPORTED` with capability `root_component_reorder`.

- [ ] **Step 6: Build, test, and commit Stage 3**

Run offline tests, UBT, all legacy Blueprint tests, and
`test_blueprint2_graph`. Expected: all Stage 3 actions PASS, cycles leave the
hierarchy unchanged, references survive rename, and cleanup removes every
temporary asset.

```powershell
git add Plugins/UnrealMCPython mcp-server/tests
git commit -m "feat: expand Blueprint component editing"
```

---

## Stage 4 — compiler diagnostics, health, snapshots, and diff

### Task 14: Enhance compile results with stable structured diagnostics

**Files:**
- Create: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintDiagnostics.cpp`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper.cpp:1046-1080`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint.py`
- Create: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_diagnostics.py`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/run_all.py`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py`
- Modify: `mcp-server/tests/test_blueprint2_contracts.py`

- [ ] **Step 1: Write failing compile compatibility and diagnostic tests**

Keep the existing compile test and add valid, warning, missing-required-pin,
unresolved-member, and incompatible-type Blueprints. Assert existing top-level
`success`, `status`, and `message`; additive `summary`, `data.result_status`,
counts, structured diagnostics, graph/node/pin stable IDs, normalized code,
severity, message, and recovery hint. Compile failure must include one
`COMPILE_FAILED` error and must not save the package.

- [ ] **Step 2: Run the diagnostic suite and verify legacy output is too shallow**

Expected: the legacy compile test passes while all structured diagnostic
assertions fail.

- [ ] **Step 3: Move and enhance `CompileBlueprint`**

Move its definition to the diagnostics file. Create a local
`FCompilerResultsLog`, set `bSilentMode=true` and
`bAnnotateMentionedNodes=false`, and call:

```cpp
FKismetEditorUtilities::CompileBlueprint(
    Blueprint,
    EBlueprintCompileOptions::None,
    &Results);
```

Iterate `Results.Messages`. For each tokenized message, inspect
`FEdGraphToken`; prefer its pin, then graph object/node, to serialize stable
graph/node/pin IDs. Retain messages without a graph token with empty IDs.

- [ ] **Step 4: Normalize diagnostic codes and hints**

Use the first matching rule in this closed mapping:

```text
required pin / no value        -> BP_MISSING_REQUIRED_PIN
could not find / unresolved    -> BP_UNRESOLVED_MEMBER
not compatible / type mismatch -> BP_TYPE_MISMATCH
already exists / duplicate     -> BP_DUPLICATE_MEMBER
accessed none                  -> BP_POSSIBLE_NULL_ACCESS
all other errors               -> BP_COMPILE_ERROR
all other warnings             -> BP_COMPILE_WARNING
```

Hints respectively recommend connecting/setting the named pin, re-inspecting
the referenced member, inserting a conversion/changing type, renaming the
duplicate, validating the object, or inspecting the cited node. Preserve the
original compiler text verbatim in `message`.

- [ ] **Step 5: Preserve statuses and return explicit next actions**

Map `BS_UpToDateWithWarnings` in addition to the existing statuses. On error,
return `next_actions` for detailed node/pin inspection and health after repair;
on success, recommend snapshot/health. Do not open a transaction and do not
save.

- [ ] **Step 6: Build, test, and commit compiler diagnostics**

Run offline contract tests, UBT, legacy Blueprint tests, and the focused
diagnostic suite. Expected: valid/warning/error cases PASS and package dirty
state is not cleared by compile.

```powershell
git add Plugins/UnrealMCPython mcp-server/tests
git commit -m "feat: structure Blueprint compile diagnostics"
```

### Task 15: Implement explicit Blueprint health checks

**Files:**
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintDiagnostics.cpp`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_diagnostics.py`
- Modify: `mcp-server/tests/test_blueprint2_action_wrappers.py`

- [ ] **Step 1: Write the failing named health action test matrix**

Add `test_get_blueprint_health` plus focused tests for compiler warnings/errors,
disconnected required pins, unresolved calls/variables, missing interface
implementations, duplicate case-insensitive member names, invalid variable/
class defaults, SCS cycles/orphans/duplicate GUIDs, and a fully healthy asset.
Assert issue codes, stable targets, severity, and actionable hints.

- [ ] **Step 2: Run and verify the health stub fails**

Expected: compiler diagnostic tests PASS and health tests fail at the stub.

- [ ] **Step 3: Add the reflected health entry point**

```cpp
UFUNCTION(BlueprintCallable, Category="Editor|MCPython")
static FString GetBlueprintHealth(UBlueprint* Blueprint);
```

Health always compiles exactly once using the Task 14 helper, then performs
read-only structural checks. Mark the action write/high in metadata because
compilation mutates generated classes/editor state; it supports neither preview
nor undo.

- [ ] **Step 4: Implement deterministic structural health checks**

Check every visible input pin with no link/default through
`DefaultValueSimpleValidation`; resolve call-function and variable member
references; compare interface class functions to implementation graphs/events;
insert all member/component names into one case-insensitive collision map;
validate canonical variable defaults; validate class/object default paths; and
DFS the SCS hierarchy for cycles, missing `AllNodes`, duplicate GUIDs, and more
than one parent.

Deduplicate issues by `(code, graph_id, node_id, pin_id, member_id)`, sort by
severity then stable IDs, and return `data.healthy=true` only when there are no
errors. Warnings do not make the MCP call fail; compiler errors return
`success=false` and `COMPILE_FAILED`.

- [ ] **Step 5: Replace the wrapper, build, and run tests**

Run wrapper/contracts/planner tests, UBT, and the diagnostic in-editor suite.
Expected: every health condition produces exactly one normalized issue and the
healthy fixture returns empty issues.

- [ ] **Step 6: Commit health checks**

```powershell
git add Plugins/UnrealMCPython mcp-server/tests
git commit -m "feat: add Blueprint health checks"
```

### Task 16: Implement deterministic graph snapshots and paginated diff

**Files:**
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprint2Internal.h`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprint2Internal.cpp`
- Modify: `Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintDiagnostics.cpp`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_diagnostics.py`
- Modify: `mcp-server/tests/test_blueprint2_action_wrappers.py`
- Modify: `mcp-server/tests/test_blueprint2_contracts.py`

- [ ] **Step 1: Write failing named snapshot and diff action tests**

Add `test_snapshot_blueprint_graph` and `test_diff_blueprint_graphs`. Snapshot
the same graph twice and assert byte-equivalent canonical data/digest; mutate
nodes, pins, connections, properties, and positions; assert added/removed/
changed IDs and counts; test compact versus detailed records, independent
section cursors, malformed/mismatched cursors, reverse diff, and more than 100
records in one section.

- [ ] **Step 2: Run and verify both stubs fail**

Expected: compile/health tests PASS and snapshot/diff actions fail at stubs.

- [ ] **Step 3: Add exact reflected entry points**

```cpp
UFUNCTION(BlueprintCallable, Category="Editor|MCPython")
static FString SnapshotBlueprintGraph(UBlueprint* Blueprint, const FString& RequestJson);

UFUNCTION(BlueprintCallable, Category="Editor|MCPython")
static FString DiffBlueprintGraphs(const FString& RequestJson);
```

`SnapshotBlueprintGraph` accepts optional stable graph IDs; empty means all
supported graphs. `DiffBlueprintGraphs` accepts two complete snapshot objects
and per-section queries, so it does not load or mutate an asset.

- [ ] **Step 4: Serialize one canonical snapshot version**

Return this shape with every array sorted by stable ID:

```json
{
  "snapshot_version": 1,
  "asset_path": "/Game/BP_Test",
  "blueprint_class": "/Script/Engine.Blueprint",
  "graphs": [{
    "id": "graph:<guid>",
    "name": "EventGraph",
    "schema_path": "/Script/BlueprintGraph.EdGraphSchema_K2",
    "nodes": [{
      "id": "node:<guid>", "class_path": "/Script/BlueprintGraph.K2Node_IfThenElse",
      "position": {"x": 0, "y": 0}, "comment": "",
      "properties": {},
      "pins": [{"id": "pin:<guid>", "name": "execute", "direction": "input",
                "type": {"kind": "exec"}, "default": null}]
    }],
    "connections": [{"source_pin_id": "pin:<guid>", "target_pin_id": "pin:<guid>"}]
  }],
  "digest": "sha1:<hex>"
}
```

Emit each connection once, oriented output to input. Exclude display-only
localized text, transient object names, compiler messages, selection, zoom,
and package dirty state from the digest.

- [ ] **Step 5: Diff snapshots by stable identity**

Validate `snapshot_version`, recompute both digests, and reject tampered input.
Build sorted records for sections `nodes`, `pins`, `connections`, `properties`,
and `positions`. Each omitted query defaults to compact limit 100; each result
has its own `next_cursor` bound to both digests, section, detail mode, and editor
session. Compact items contain IDs and changed field names; detailed items
contain before/after values.

Use the engine's `FGraphDiffControl` declarations only as implementation
reference. Do not expose `EDiffType`, raw pointers, localized display strings,
or version-dependent `FDiffSingleResult` through the public contract.

- [ ] **Step 6: Replace wrappers and pass all focused tests**

`ue_snapshot_blueprint_graph` uses `call_asset_helper`; the diff wrapper uses
`call_json_helper` because it has no asset. Run offline tests, UBT, and the
diagnostic suite. Expected: deterministic digest, complete change categories,
independent cursors, and no mutation/dirty-state change during snapshot/diff.

- [ ] **Step 7: Commit snapshot and diff**

```powershell
git add Plugins/UnrealMCPython mcp-server/tests
git commit -m "feat: add Blueprint graph snapshots and diff"
```

### Task 17: Prove compatibility, workflow undo, and full release gates

**Files:**
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/run_all.py`
- Modify: `Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py`
- Modify: `mcp-server/tests/test_coverage.py`
- Modify: `mcp-server/tests/test_workflow_planner.py`
- Modify: `mcp-server/tests/test_e2e.py`
- Modify: `mcp-server/tests/test_fastmcp_v3.py`
- Modify: `README.md`
- Modify: `mcp-server/README.md`
- Regenerate: `mcp-server/src/unreal_mcp/dispatchers/_catalog.py`
- Regenerate: `mcp-server/src/unreal_mcp/dispatchers/_registry.py`

- [ ] **Step 1: Make named coverage include split Blueprint suites**

Change only the Blueprint branch of `_referenced`:

```python
def _referenced(domain: str) -> set[str]:
    paths = (
        sorted(PLUGIN_TESTS.glob("test_blueprint*.py"))
        if domain == "blueprint"
        else [PLUGIN_TESTS / f"test_{domain}.py"]
    )
    source = "\n".join(
        path.read_text(encoding="utf-8") for path in paths if path.exists()
    )
    return set(re.findall(r"ue_(\w+)", source))
```

Add a test that all 48 Blueprint actions are referenced and none are added to
`KNOWN_UNTESTED`.

- [ ] **Step 2: Assert workflow policy for every Blueprint mutation**

Parameterize all new writes/destructive actions plus migrated legacy graph,
variable, and component mutations. Assert `supports_undo=true`,
`requires_confirmation=true`, and planning succeeds without
`allow_non_undoable`. Keep `compile_blueprint` and `get_blueprint_health`
non-undoable/high-risk. Keep reads low-risk/idempotent.

Do not claim undo support for `create_blueprint` until a live test proves asset
creation records a restorable editor transaction; the E2E creates its fixture
outside the mutation workflow.

- [ ] **Step 3: Write the failing Universal Blueprint live workflow E2E**

Add `test_blueprint2_workflow_round_trip` using a unique
`/Game/__MCPTests/Blueprint2_<uuid>` asset:

1. Create the fixture Blueprint and take a pre-state graph snapshot.
2. Inspect brief and stable EventGraph ID.
3. Plan one workflow containing function creation, variable creation, component
   creation, and one expanded `build_blueprint_graph` request whose client refs
   create and connect common/reflected nodes.
4. Apply and require a recorded transaction plus undo token.
5. Compile explicitly, require healthy state, snapshot post-state, and diff it
   against pre-state.
6. Undo the workflow token, snapshot again, and require the graph snapshot to
   equal pre-state.
7. Delete the fixture in `finally`, assert the package no longer exists, and
   call `actor.list_all_with_locations` as an editor-liveness canary.

Expected before final integration: FAIL at the first action whose metadata or
transaction behavior is incomplete.

- [ ] **Step 4: Fix integration-only compatibility defects**

Fix only defects exposed by the E2E: preserve old fields, add missing `Modify`
calls, correct metadata, or make stable-ID resolution consistent. Do not add
game-specific helpers, implicit compile/save, arbitrary Python fallbacks, or a
second workflow executor. Regenerate catalog files after metadata changes.

Remove the remaining `EditorAssetLibrary.save_loaded_asset` call from the
legacy `create_blueprint` wrapper and add a regression assertion that creation
leaves the package dirty but unsaved. Keep `create_blueprint` non-undoable until
Unreal exposes a verified restorable asset-creation transaction.

- [ ] **Step 5: Run complete offline and generation gates**

Run:

```powershell
Set-Location mcp-server
uv run python generate_catalog.py
uv run python validate_tools.py
uv run --extra dev pytest -q
Set-Location ..
git diff --exit-code -- mcp-server/src/unreal_mcp/dispatchers/_catalog.py mcp-server/src/unreal_mcp/dispatchers/_registry.py
```

Expected: catalog reports exactly `290 actions across 22 domains`; all offline
tests PASS with zero failures; generated files are clean after regeneration.
The FastMCP namespace golden remains unchanged because there is still one
`blueprint(action, params)` tool.

- [ ] **Step 6: Run UE 5.7 build and complete in-editor gates**

Close Unreal Editor, then run:

```powershell
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' UnrealMCPSampleEditor Win64 Development "$PWD\UnrealMCPSample.uproject" -WaitMutex -NoHotReload
```

Restart the editor and execute:

```python
import runpy
runpy.run_module("UnrealMCPython.tests.run_all", run_name="__main__")
```

Expected: UBT `Result: Succeeded`; all in-editor suites report zero failures and
zero errors; the cleanup summary lists zero assets under `/Game/__MCPTests`.

- [ ] **Step 7: Run the real MCP/TCP/C++ E2E and liveness gate**

With the editor open:

```powershell
Set-Location mcp-server
uv run --extra dev pytest tests/test_e2e.py -v
```

Expected: every one of the 290 catalog actions returns an unwrapped dict on the
empty-parameter sweep, the Universal Blueprint workflow round trip PASSes, undo
restores the pre-snapshot, cleanup succeeds, and
`test_zzz_editor_survived_suite` PASSes.

- [ ] **Step 8: Run explicit version gates**

On matching workers, run the same closed-editor UBT command with UE 5.6 and UE
5.8 roots and run brief/capability plus focused in-editor suites. If a worker
is unavailable, record that absence in release notes without claiming the gate
passed. Any unavailable API must advertise a false runtime capability and
return `UE_VERSION_UNSUPPORTED` before mutation.

- [ ] **Step 9: Update user-facing documentation**

Document the recommended sequence:

```text
get_blueprint_brief -> inspect_blueprint -> workflow.plan -> workflow.apply
-> compile_blueprint -> get_blueprint_health/snapshot/diff -> workflow.undo
```

Include stable ID prefixes, cursor limits, canonical type examples, reflected
path examples, explicit compile/save behavior, UE 5.6-5.8 capability caveats,
and a statement that no base-game generator is included.

- [ ] **Step 10: Commit final integration**

```powershell
git add Plugins/UnrealMCPython mcp-server README.md
git commit -m "test: verify universal Blueprint workflows"
```

---

## Required final self-review

### Spec coverage review

Before claiming completion, re-read every section of the approved spec and
confirm this mapping against the implemented commits:

| Specification requirement | Plan tasks |
|---|---|
| Remove gameplay generator direction | 1 |
| Strict discovery schemas and version capabilities | 2-4, 17 |
| Stable graph/member/node/pin/variable/component IDs | 3-5 |
| Compact filtered inspection and per-query cursors | 4-5 |
| Functions and complete ordered signatures | 6-7 |
| Macros, custom events, dispatchers, interfaces | 8-9 |
| Common and full-path reflected nodes | 10 |
| Allowlisted node properties and pin connect/disconnect | 11 |
| Canonical types/defaults and variable lifecycle/metadata/replication | 6, 12 |
| Component GUID lifecycle/hierarchy/order/transform/properties | 13 |
| Compile diagnostics and health | 14-15 |
| Deterministic snapshots and independent paginated diff | 16 |
| Existing 18 Blueprint action compatibility | 4, 10-14, 17 |
| Outer workflow transaction, rollback, guarded undo | 3, 7-13, 17 |
| No implicit save/compile, PIE, project settings, or gameplay content | 7-17 |
| UE 5.6-5.8 and runtime rejection before mutation | 3, 17 |
| Offline, build, in-editor, live E2E, cleanup, liveness | 17 |

If any row cannot point to a passing named test and a concrete implementation
commit, add that missing test and implementation before the final commit.

### Unfinished-marker review

Run this against the plan and every new source file:

```powershell
rg -n -M 300 --max-filesize 1M 'T[B]D|T[O]DO|implement[ ]later|fill[ ]in[ ]details|appropriate[ ]error[ ]handling|handle[ ]edge[ ]cases|Similar[ ]to[ ]Task' Docs/superpowers/plans/2026-07-27-universal-blueprint-2.md Plugins/UnrealMCPython mcp-server/src mcp-server/tests
```

Expected: no match in the new Blueprint 2 implementation or this plan. Existing
unrelated debt must be listed by exact file/line and left unchanged.

### Type and name consistency review

Verify all 29 names exist identically in the public signature section,
`blueprint_actions.py`, `BLUEPRINT2_ACTION_SPECS`, generated catalog/registry,
named in-editor tests, and the exact C++ helper mapping. Verify the canonical
kind strings, ID prefixes, error codes, snapshot field names, query op names,
diff section names, and capability keys match their first definitions in this
plan. Then run `python validate_tools.py`, the wrapper call-shape suite, and the
contract suite one final time; all three must pass before handoff.
