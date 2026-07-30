# Blueprint Semantic Graph Editing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax for tracking.

**Goal:** Add five safe Blueprint MCP actions that suggest bridge nodes, spawn already-connected palette nodes, insert palette nodes into links, preview node replacement, and atomically apply an unchanged replacement plan.

**Architecture:** Keep Unreal Engine 5.7 native palette actions and UEdGraphSchema_K2 as the compatibility authorities. Refactor the existing palette and snapshot implementations only enough to expose focused private C++ interfaces, then place semantic filtering, connection mutation, replacement mapping, snapshot-bound capabilities, rollback verification, and response serialization in a new editor-only semantic adapter.

**Tech Stack:** Unreal Engine 5.7 editor C++, BlueprintGraph/Kismet/UnrealEd APIs, Unreal Python, Python 3.11+, FastMCP 3.2.4, Pydantic 2, JSON Schema Draft 2020-12, pytest/pytest-asyncio, UE Automation Tests, UBT.

---

## Scope and fixed public surface

This plan implements Docs/superpowers/specs/2026-07-30-blueprint-semantic-graph-editing-design.md as one cohesive phase. It adds exactly these public Blueprint actions:

1. suggest_blueprint_nodes_for_connection
2. add_blueprint_connected_action_node
3. insert_blueprint_action_node
4. preview_blueprint_action_replacement
5. replace_blueprint_node_with_action

With no unrelated catalog changes, the Blueprint domain moves from 52 to 57 actions, the full catalog moves from 294 to 299 actions, the domain count remains 22, and the exhaustive JSON live sweep moves from 293 to 298 action pairs because vision.capture_viewport remains the only typed-image exception.

The phase does not add a gameplay/base-game generator, a node-class allowlist, a generic graph-patch language, arbitrary node-class construction, Python execution, implicit compilation, implicit saving, PIE, graph creation, local variables, inherited overrides, or debugger editing.

The exact Python signatures are:

~~~python
def ue_suggest_blueprint_nodes_for_connection(
    asset_path: str = None,
    graph_id: str = None,
    source_pin_id: str = None,
    target_pin_id: str = None,
    query: str = "",
    filters: dict = None,
    allow_conversion: bool = False,
    cursor: str = "",
    limit: int = 50,
) -> str

def ue_add_blueprint_connected_action_node(
    asset_path: str = None,
    graph_id: str = None,
    pin_id: str = None,
    action_id: str = None,
    connection_binding_id: str = None,
    position: dict = None,
    allow_conversion: bool = False,
    bindings: tuple[str, ...] = (),
) -> str

def ue_insert_blueprint_action_node(
    asset_path: str = None,
    graph_id: str = None,
    source_pin_id: str = None,
    target_pin_id: str = None,
    action_id: str = None,
    input_binding_id: str = None,
    output_binding_id: str = None,
    position: dict = None,
    bindings: tuple[str, ...] = (),
) -> str

def ue_preview_blueprint_action_replacement(
    asset_path: str = None,
    graph_id: str = None,
    node_id: str = None,
    action_id: str = None,
    bindings: tuple[str, ...] = (),
    pin_mapping: tuple[dict, ...] = (),
    allow_conversion: bool = False,
    allow_loss: bool = False,
) -> str

def ue_replace_blueprint_node_with_action(
    asset_path: str = None,
    graph_id: str = None,
    replacement_plan_id: str = None,
    allow_loss: bool = False,
) -> str
~~~

All wrappers use blueprint2.call_asset_helper. Python copies caller-owned dictionaries and lists and contains no filtering, mapping, mutation, compilation, saving, or rollback logic.

## Capability and compatibility contract

Opaque public IDs retain these patterns:

~~~text
action:[0-9a-f]{40}
binding:[0-9a-f]{40}
palette-cursor:[0-9a-f]{40}
replacement-plan:[0-9a-f]{40}
~~~

Every palette action record has one exact context kind:

~~~cpp
enum class EPaletteContextKind : uint8
{
    Graph,
    Pin,
    Connection
};
~~~

- Graph actions come from search_blueprint_node_actions and may be used by add_blueprint_action_node or replacement preview.
- Pin actions come from suggest_blueprint_nodes_for_pin and may be used by add_blueprint_connected_action_node.
- Connection actions come from suggest_blueprint_nodes_for_connection and may be used by insert_blueprint_action_node.
- A connection action stores both stable pin IDs and the conversion policy. It may be described while that context is current, but it cannot be spawned by add_blueprint_action_node or replayed in a different graph, pin pair, policy, or editor session.
- Dynamic object bindings and template-pin bindings share the binding: wire prefix but have distinct internal kinds. A resolver rejects a template-pin token where a dynamic object binding is required, and vice versa.

The schema response is interpreted with this exact classification:

~~~cpp
enum class ESemanticConnectionKind : uint8
{
    Direct,
    BreakPlannedSourceLink,
    BreakPlannedTargetLink,
    BreakPlannedBothLinks,
    ConversionNode,
    Promotion,
    Disallowed
};
~~~

CONNECT_RESPONSE_MAKE is direct. BREAK_OTHERS responses are accepted only when every link that Unreal proposes to break is the exact old edge named by an insertion/replacement plan; otherwise the pair is rejected. MAKE_WITH_CONVERSION_NODE and MAKE_WITH_PROMOTION require allow_conversion=true. Conversion-node and promotion outcomes are always reported separately; neither is presented as a direct connection.

The new semantic layer always calls UBlueprintNodeSpawner::GetTemplateNode(Graph, ResolvedBindings) for read-only template inspection and UBlueprintNodeSpawner::Invoke only inside a valid FMutationScope. It re-runs UEdGraphSchema_K2::CanCreateConnection against actual spawned pins before removing an edge or node, then calls TryCreateConnection. It records any newly created auxiliary node and every final stable edge.

## Wire schemas locked by this plan

Add these reusable closed schema fragments to blueprint2_action_specs.py:

~~~python
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
            NODE_ID, maxItems=32, uniqueItems=True
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
    (
        "binding_id",
        "pin_name",
        "direction",
        "type",
        "response",
        "rank",
    ),
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
~~~

Pin suggestions gain a dedicated output card with required connection_bindings. Plain graph search and describe keep their current response shapes.

Connection suggestion success data contains asset_path, graph_id, source_pin_id, target_pin_id, allow_conversion, items, total_count, returned_count, next_cursor, and result_digest. Every item is the existing palette card plus binding_pairs.

Connected-spawn success data contains asset_path, graph_id, action_id, source_pin_id, connection_binding_id, node_id, class_path, position, pin_ids, pins, auxiliary_node_ids, connections, transaction_recorded=true, and saved=false.

Insertion success data contains the same spawn fields plus replaced_connection and exactly two requested semantic connections. Auxiliary conversion edges may add further reported connections but never disappear from the response.

Replacement preview uses these closed records:

~~~python
PIN_MAPPING_INPUT = _object(
    {"old_pin_id": PIN_ID, "new_binding_id": BINDING_ID},
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
~~~

The preview response requires replacement_plan_id, asset_path, graph_id, node_snapshot_digest, action_result_digest, selected_action, target_node, mappings, retained_connections, retained_defaults, unmapped_connections, unmapped_defaults, unsupported_metadata, loss_count, warnings, applicable, allow_conversion, and allow_loss. selected_action contains action_id, title, node_class_path, owner_path, and member_path. target_node contains node_id, class_path, title, position, comment, comment_bubble_visible, and enabled_state.

The apply response requires replacement_plan_id, asset_path, graph_id, action_id, old_node_id, new_node_id, class_path, position, pin_ids, pins, auxiliary_node_ids, connections, preserved_defaults, dropped_connections, dropped_defaults, preserved_metadata, transaction_recorded=true, and saved=false.

loss_count is the number of unmapped individual links plus non-empty writable input defaults. Unsupported node-class-specific metadata is reported but does not make the plan non-applicable because the design explicitly excludes copying those properties. With allow_loss=false, a positive loss_count makes preview applicable=false and apply returns CONFLICT without mutation. With allow_loss=true, preview may be applicable and apply reports every dropped item.

The implementation uses the error codes consistently:

- INVALID_INPUT for malformed IDs, wrong directions/categories, duplicate or foreign bindings, invalid explicit mappings, and non-normalizable defaults;
- PRECONDITION_FAILED for once-valid stale actions, cursors, plans, graphs, nodes, pins, bindings, snapshots, and action result sets;
- CONFLICT for strict replacement loss or a named insertion edge that changed after validation;
- UE_VERSION_UNSUPPORTED outside the validated adapter version;
- OPERATION_FAILED for a native invocation, default assignment, connection, break, destruction, or topology verification failure;
- TRANSACTION_FAILED when FMutationScope cannot begin;
- ROLLBACK_FAILED when the post-rollback selected-graph snapshot differs.

Retryable failures give one concrete re-inspect, re-suggest, or re-preview hint. Responses never expose a pointer, address, raw traceback, implicit save, or implicit compile.

## Unreal Engine 5.7 API baseline

Use the installed declarations under C:\Program Files\Epic Games\UE_5.7\Engine\Source, not inferred signatures:

~~~cpp
UEdGraphNode* UBlueprintNodeSpawner::GetTemplateNode(
    UEdGraph* TargetGraph = nullptr,
    FBindingSet const& Bindings = FBindingSet()) const;

UEdGraphNode* UBlueprintNodeSpawner::Invoke(
    UEdGraph* ParentGraph,
    FBindingSet const& Bindings,
    FVector2D const Location) const;

const FPinConnectionResponse UEdGraphSchema::CanCreateConnection(
    const UEdGraphPin* A,
    const UEdGraphPin* B) const;

bool UEdGraphSchema::TryCreateConnection(
    UEdGraphPin* A,
    UEdGraphPin* B) const;

bool UEdGraphSchema::CreateAutomaticConversionNodeAndConnections(
    UEdGraphPin* A,
    UEdGraphPin* B) const;

void UEdGraphSchema::BreakSinglePinLink(
    UEdGraphPin* SourcePin,
    UEdGraphPin* TargetPin) const;

bool UEdGraph::RemoveNode(
    UEdGraphNode* NodeToRemove,
    bool bBreakAllLinks = true,
    bool bAlwaysMarkDirty = true);

void UEdGraphNode::DestroyNode();
ENodeEnabledState UEdGraphNode::GetDesiredEnabledState() const;
void UEdGraphNode::SetEnabledState(
    ENodeEnabledState NewState,
    bool bUserAction = true);
~~~

Use DestroyNode rather than RemoveNode for replacement so node subclasses receive their destruction hook. Register Blueprint, graph, target/new/linked/auxiliary nodes, and affected pins with FMutationScope before mutation. Never invoke CompileBlueprint, SavePackage, EditorAssetLibrary.save_asset, play commands, or execute_python.

## File responsibility map

- Create Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprintPaletteInternal.h — private reusable palette candidate, filter, token-context, dynamic-binding, and template-pin interfaces.
- Create Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintSemantic.cpp — request parsing, pin-pair filtering, connected spawn, insertion, replacement preview/apply, topology verification, and structured results.
- Create Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprintSemanticTests.cpp — native semantic/token/mapping/mutation/rollback tests.
- Create Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_semantic.py — real editor acceptance, strict/lossy replacement, workflow undo, and 12/12 stress.
- Modify MCPythonHelper_BlueprintPalette.cpp — move reusable private types/functions behind MCPythonBlueprintPaletteInternal.h and enrich pin suggestions with explicit template-pin bindings.
- Modify MCPythonBlueprint2Internal.h/.cpp — context kinds, typed binding records, bounded replacement-plan records, 30-minute true-LRU registry, and test controls.
- Modify MCPythonHelper_BlueprintDiagnostics.cpp and MCPythonBlueprint2Internal.h — expose a deterministic selected-graph snapshot builder used by both public snapshots and semantic rollback checks.
- Modify MCPythonHelper.h — declare five reflected entry points.
- Modify blueprint_actions.py — add five thin wrappers and regenerated literal metadata.
- Modify tests/run_all.py — load the semantic editor suite.
- Modify blueprint2_action_specs.py — closed schemas, examples, and safety metadata.
- Modify test_blueprint2_contracts.py, test_blueprint2_action_wrappers.py, test_registry_generation.py, and test_coverage.py — contracts, forwarding, source guards, exact totals, and workflow metadata.
- Modify mcp-server/tests/test_e2e.py — dedicated semantic live workflow and complete sweep accounting.
- Regenerate _catalog.py and _registry.py.
- Modify README.md and mcp-server/README.md — 299/57 totals and semantic LLM workflows.

---

### Task 1: Define strict contracts and thin wrappers

**Files:**
- Modify: mcp-server/tests/test_blueprint2_contracts.py
- Modify: mcp-server/tests/test_blueprint2_action_wrappers.py
- Modify: mcp-server/tests/test_registry_generation.py
- Modify: mcp-server/tests/test_coverage.py
- Modify: mcp-server/src/unreal_mcp/blueprint2_action_specs.py
- Modify: Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py
- Regenerate: mcp-server/src/unreal_mcp/dispatchers/_catalog.py
- Regenerate: mcp-server/src/unreal_mcp/dispatchers/_registry.py

- [ ] **Step 1: Write failing catalog, safety, and schema tests**

Add SEMANTIC_ACTIONS with the five names. Add suggestion and preview to READ_ACTIONS, connected spawn and insertion to WRITE_ACTIONS, and replacement apply to DESTRUCTIVE_ACTIONS. Change exact assertions to 57 Blueprint, 299 total, and 298 JSON sweep pairs.

Add focused validation cases that require closed objects, reject malformed action/binding/plan IDs, reject duplicate pin mappings, cap mappings at 256, cap dynamic bindings at 32, cap returned cards at 200, and verify examples through Draft202012Validator.

~~~python
SEMANTIC_ACTIONS = {
    "suggest_blueprint_nodes_for_connection",
    "add_blueprint_connected_action_node",
    "insert_blueprint_action_node",
    "preview_blueprint_action_replacement",
    "replace_blueprint_node_with_action",
}

def test_semantic_blueprint_contracts_are_closed_bounded_and_opaque():
    specs = _new_specs()
    suggest = specs["suggest_blueprint_nodes_for_connection"]["input_schema"]
    connected = specs["add_blueprint_connected_action_node"]["input_schema"]
    insert = specs["insert_blueprint_action_node"]["input_schema"]
    preview = specs["preview_blueprint_action_replacement"]["input_schema"]
    replace = specs["replace_blueprint_node_with_action"]["input_schema"]

    for schema in (suggest, connected, insert, preview, replace):
        assert schema["additionalProperties"] is False

    assert suggest["required"] == [
        "asset_path", "graph_id", "source_pin_id", "target_pin_id"
    ]
    assert preview["properties"]["pin_mapping"]["maxItems"] == 256
    assert preview["properties"]["pin_mapping"]["uniqueItems"] is True
    assert replace["properties"]["replacement_plan_id"]["pattern"] == (
        r"^replacement-plan:[0-9a-f]{40}$"
    )

    Draft202012Validator(connected).validate({
        "asset_path": "/Game/BP.BP",
        "graph_id": "graph:11111111-1111-4111-8111-111111111111",
        "pin_id": "pin:22222222-2222-4222-8222-222222222222",
        "action_id": "action:" + "a" * 40,
        "connection_binding_id": "binding:" + "b" * 40,
        "position": {"x": 320, "y": 160},
        "allow_conversion": False,
        "bindings": [],
    })
    with pytest.raises(ValidationError):
        Draft202012Validator(replace).validate({
            "asset_path": "/Game/BP.BP",
            "graph_id": "graph:11111111-1111-4111-8111-111111111111",
            "replacement_plan_id": "replacement-plan:tampered",
            "allow_loss": False,
        })
~~~

Run:

~~~powershell
uv run --project mcp-server --extra dev pytest mcp-server/tests/test_blueprint2_contracts.py mcp-server/tests/test_registry_generation.py mcp-server/tests/test_coverage.py -q
~~~

Expected: failures name all five absent specs and old 294/52 totals.

- [ ] **Step 2: Write failing exact wrapper-forwarding tests**

For each wrapper, assert one helper call with a deep-copied request. Include a mutation-after-call assertion for filters, position, bindings, and pin_mapping.

~~~python
def test_insert_blueprint_action_node_wrapper(monkeypatch):
    module, calls = _load_variable_wrapper(monkeypatch)
    bindings = ["binding:" + "c" * 40]
    position = {"x": 600, "y": 80}

    result = module.ue_insert_blueprint_action_node(
        asset_path="/Game/BP.BP",
        graph_id="graph:11111111-1111-4111-8111-111111111111",
        source_pin_id="pin:22222222-2222-4222-8222-222222222222",
        target_pin_id="pin:33333333-3333-4333-8333-333333333333",
        action_id="action:" + "a" * 40,
        input_binding_id="binding:" + "b" * 40,
        output_binding_id="binding:" + "d" * 40,
        position=position,
        bindings=bindings,
    )

    position["x"] = -1
    bindings.append("binding:" + "e" * 40)
    assert json.loads(result)["marker"] == "native"
    assert calls[0][0:2] == ("insert_blueprint_action_node", "/Game/BP.BP")
    assert calls[0][2]["position"] == {"x": 600, "y": 80}
    assert calls[0][2]["bindings"] == ["binding:" + "c" * 40]
~~~

Run:

~~~powershell
uv run --project mcp-server --extra dev pytest mcp-server/tests/test_blueprint2_action_wrappers.py -q
~~~

Expected: five AttributeError failures for missing ue_ wrappers.

- [ ] **Step 3: Add the closed schemas and five action specs**

Add the locked fragments from the Wire schemas section. Define separate PALETTE_PIN_SUGGESTION_CARD, PALETTE_PIN_PAGE_DATA, CONNECTION_ACTION_CARD, CONNECTION_PAGE_DATA, CONNECTED_SPAWN_DATA, INSERT_DATA, REPLACEMENT_PREVIEW_DATA, and REPLACEMENT_APPLY_DATA objects; do not loosen existing plain-search schemas.

Use _read for suggestion and preview, _write for connected spawn and insertion, and _destructive for replacement apply. The exact replacement input is:

~~~python
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
        "replacement_plan_id": "replacement-plan:" + "e" * 40,
        "allow_loss": False,
    },
    success_data_schema=REPLACEMENT_APPLY_DATA,
),
~~~

Extend _destructive with an optional success_data_schema parameter and pass it to _spec. Keep all ue_versions values unchanged at 5.6, 5.7, 5.8 because the registry describes intended compatibility; actual release evidence remains version-specific.

- [ ] **Step 4: Add the five thin Python wrappers**

Insert the exact signatures from the scope section beside the palette wrappers. Each function constructs one request with deepcopy for dictionaries and list(...) for tuple/list sequences.

~~~python
def ue_preview_blueprint_action_replacement(
    asset_path: str = None,
    graph_id: str = None,
    node_id: str = None,
    action_id: str = None,
    bindings: tuple[str, ...] = (),
    pin_mapping: tuple[dict, ...] = (),
    allow_conversion: bool = False,
    allow_loss: bool = False,
) -> str:
    """Preview a snapshot-bound palette action replacement without mutation."""
    from UnrealMCPython import blueprint2

    request = {
        "graph_id": graph_id,
        "node_id": node_id,
        "action_id": action_id,
        "bindings": list(bindings),
        "pin_mapping": deepcopy(list(pin_mapping)),
        "allow_conversion": allow_conversion,
        "allow_loss": allow_loss,
    }
    return blueprint2.call_asset_helper(
        "preview_blueprint_action_replacement", asset_path, request
    )
~~~

Implement the other four with the same direct field-for-field pattern.

- [ ] **Step 5: Generate metadata and make the focused contract suite green**

Run:

~~~powershell
uv run --project mcp-server python mcp-server/generate_catalog.py
uv run --project mcp-server --extra dev pytest mcp-server/tests/test_blueprint2_contracts.py mcp-server/tests/test_blueprint2_action_wrappers.py mcp-server/tests/test_registry_generation.py mcp-server/tests/test_coverage.py -q
~~~

Expected: all focused tests pass; generated catalog reports 299 total, 57 Blueprint, and 22 domains.

- [ ] **Step 6: Commit the contract slice**

~~~powershell
git add -- mcp-server/src/unreal_mcp/blueprint2_action_specs.py mcp-server/tests/test_blueprint2_contracts.py mcp-server/tests/test_blueprint2_action_wrappers.py mcp-server/tests/test_registry_generation.py mcp-server/tests/test_coverage.py Plugins/UnrealMCPython/Content/Python/UnrealMCPython/blueprint_actions.py mcp-server/src/unreal_mcp/dispatchers/_catalog.py mcp-server/src/unreal_mcp/dispatchers/_registry.py
git commit -m "feat: define semantic Blueprint graph actions"
~~~

Expected: one commit containing contracts, wrappers, generated metadata, and their tests.

---

### Task 2: Expose reusable palette/snapshot primitives and bounded semantic capabilities

**Files:**
- Create: Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprintPaletteInternal.h
- Modify: Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprint2Internal.h
- Modify: Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprint2Internal.cpp
- Modify: Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintPalette.cpp
- Modify: Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintDiagnostics.cpp
- Modify: Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprintPaletteTests.cpp
- Modify: mcp-server/tests/test_blueprint2_action_wrappers.py

- [ ] **Step 1: Write failing native capability tests**

Extend MCPythonBlueprintPaletteTests.cpp with:

1. graph, pin, and connection action records produce different action IDs;
2. ResolvePaletteActionToken rejects the wrong expected context kind;
3. connection context rejects a changed target pin and changed allow_conversion;
4. object and template-pin binding records cannot cross-resolve;
5. replacement-plan tokens reject malformed/unknown IDs;
6. records expire after 30 idle minutes;
7. after 1,024 records, least-recently-used eviction retains a just-resolved oldest record and evicts the next-oldest record;
8. ResetSemanticTokenStateForTests clears every replacement plan.

Use this exact context constructor:

~~~cpp
FPaletteContext MakeConnectionContext(const bool bAllowConversion)
{
    FPaletteContext Context = MakeContext();
    Context.Kind = EPaletteContextKind::Connection;
    Context.SourcePinId =
        TEXT("pin:22222222-2222-4222-8222-222222222222");
    Context.TargetPinId =
        TEXT("pin:33333333-3333-4333-8333-333333333333");
    Context.bAllowConversion = bAllowConversion;
    return Context;
}
~~~

Run the UE automation test before implementation:

~~~powershell
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' UnrealMCPSampleEditor Win64 Development "$PWD\UnrealMCPSample.uproject" -WaitMutex -NoHotReload
~~~

Expected: compilation fails because EPaletteContextKind and replacement-plan APIs do not exist.

- [ ] **Step 2: Add context expectations, typed bindings, and primitive replacement records**

Add these types to MCPythonBlueprint2Internal.h:

~~~cpp
enum class EPaletteContextKind : uint8 { Graph, Pin, Connection };
enum class EPaletteBindingKind : uint8 { Object, TemplatePin };

struct FPaletteContext
{
    FString AssetPath;
    FString GraphId;
    FString GraphSchemaPath;
    EPaletteContextKind Kind = EPaletteContextKind::Graph;
    FString SourcePinId;
    FString TargetPinId;
    bool bAllowConversion = false;
    FString RequestDigest;
    FString ResultDigest;
    int32 Limit = 50;
};

struct FPaletteContextExpectation
{
    FString AssetPath;
    FString GraphId;
    FString GraphSchemaPath;
    TOptional<EPaletteContextKind> Kind;
    FString SourcePinId;
    FString TargetPinId;
    TOptional<bool> AllowConversion;
    FString RequestDigest;
    FString ResultDigest;
    int32 Limit = 0;
};

struct FPaletteBindingRecord
{
    EPaletteBindingKind Kind = EPaletteBindingKind::Object;
    FString BindingId;
    FString ActionId;
    FString ObjectPath;
    FString ExpectedClassPath;
    FString PinName;
    FString PinDirection;
    FString PinTypeJson;
    int32 PinOccurrence = 0;
};

struct FReplacementMappingRecord
{
    FString OldPinId;
    FString NewBindingId;
    FString Origin;
    FString Reason;
};

struct FReplacementConnectionRecord
{
    FString OldPinId;
    FString NewBindingId;
    FString LinkedPinId;
    FString ResponseKind;
    FString ResponseMessage;
};

struct FReplacementDefaultRecord
{
    FString OldPinId;
    FString NewBindingId;
    FString CanonicalValueJson;
};

struct FReplacementConnectionLossRecord
{
    FString OldPinId;
    FString LinkedPinId;
    FString Reason;
};

struct FReplacementDefaultLossRecord
{
    FString OldPinId;
    FString CanonicalValueJson;
    FString Reason;
};

struct FReplacementPlanRecord
{
    FString PlanId;
    FString AssetPath;
    FString GraphId;
    FString GraphSchemaPath;
    FString NodeId;
    FString NodeSnapshotDigest;
    FString ActionId;
    FString ActionResultDigest;
    TArray<FString> DynamicBindingIds;
    TArray<FReplacementMappingRecord> Mappings;
    TArray<FReplacementConnectionRecord> Connections;
    TArray<FReplacementDefaultRecord> Defaults;
    TArray<FReplacementConnectionLossRecord> LostConnections;
    TArray<FReplacementDefaultLossRecord> LostDefaults;
    int32 PositionX = 0;
    int32 PositionY = 0;
    FString Comment;
    bool bCommentBubbleVisible = false;
    uint8 EnabledState = 0;
    bool bAllowConversion = false;
    bool bAllowLoss = false;
    int32 LossCount = 0;
};
~~~

Change ResolvePaletteActionToken to accept FPaletteContextExpectation. Add RegisterPaletteTemplatePinBinding and ResolvePaletteTemplatePinBinding while keeping ResolvePaletteBindings restricted to EPaletteBindingKind::Object.

Declare:

~~~cpp
FString RegisterReplacementPlan(FReplacementPlanRecord& Record);
bool ResolveReplacementPlan(
    const FString& PlanId,
    const FString& AssetPath,
    const FString& GraphId,
    FReplacementPlanRecord& OutRecord,
    FError& OutError);
bool BuildBlueprintGraphSnapshot(
    UBlueprint* Blueprint,
    const TArray<FString>& GraphIds,
    TSharedPtr<FJsonObject>& OutSnapshot,
    FError& OutError);
~~~

- [ ] **Step 3: Implement canonical context matching and the true-LRU plan registry**

CanonicalPaletteContext must include the context-kind string, both pin IDs, and allow_conversion before the request/result digests. FPaletteContextExpectation treats empty strings and unset optionals as wildcards; when Kind or AllowConversion is set it requires equality.

RegisterReplacementPlan validates every required primitive, sorts dynamic binding IDs, canonicalizes all record fields, computes replacement-plan: plus SHA-1, stores the current editor session ID, and updates the LRU order. ResolveReplacementPlan distinguishes malformed/unknown tokens as INVALID_INPUT and stale session/asset/graph as PRECONDITION_FAILED.

Use one helper for true LRU:

~~~cpp
void TouchLru(TArray<FString>& Order, const FString& Id)
{
    Order.Remove(Id);
    Order.Add(Id);
}
~~~

Purge records idle for more than FTimespan::FromMinutes(30.0). Enforce 1,024 plans by removing Order[0]. Touch on registration and successful resolution. Do not store UObject, UEdGraph, UEdGraphNode, UEdGraphPin, or UBlueprintNodeSpawner pointers in the registry.

- [ ] **Step 4: Extract the private palette interface without behavior changes**

MCPythonBlueprintPaletteInternal.h owns FPaletteFilters, FPaletteBindingCandidate, and FPaletteCandidate plus these declarations:

~~~cpp
namespace UE::MCPython::Blueprint2::Palette
{
bool ParseStoredFilters(
    const FString& FiltersJson,
    FPaletteFilters& OutFilters,
    FError& OutError);
bool ResolveStableGraph(
    UBlueprint* Blueprint,
    const FString& GraphId,
    UEdGraph*& OutGraph,
    FError& OutError,
    bool bStaleIsPrecondition);
bool BuildCandidates(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    TConstArrayView<UEdGraphPin*> ContextPins,
    const FString& Query,
    const FPaletteFilters& Filters,
    TArray<FPaletteCandidate>& OutCandidates);
FString ResultDigest(const TArray<FPaletteCandidate>& Candidates);
const FPaletteCandidate* FindExactCandidate(
    const TArray<FPaletteCandidate>& Candidates,
    const FPaletteActionRecord& Record);
bool ResolveDynamicBindingObjects(
    const FString& ActionId,
    const TArray<FString>& BindingIds,
    TArray<FPaletteBindingRecord>& OutRecords,
    IBlueprintNodeBinder::FBindingSet& OutBindings,
    FError& OutError);
UEdGraphNode* GetBoundTemplateNode(
    const FPaletteCandidate& Candidate,
    UEdGraph* Graph,
    const IBlueprintNodeBinder::FBindingSet& Bindings);
}
~~~

Move the existing implementations from the anonymous namespace into this named private namespace. BuildCandidates adds every non-null ContextPins entry to NativeFilter.Context.Pins and derives target classes from both pins. Existing graph search passes an empty view; existing pin suggestion passes a one-element view. Keep candidate keys, scoring, ordering, filters, and result digests byte-for-byte unchanged for those existing calls.

Add a source guard to test_blueprint2_action_wrappers.py that asserts both palette and semantic sources include MCPythonBlueprintPaletteInternal.h and that the semantic source contains no GetAllActions call.

- [ ] **Step 5: Emit explicit compatible pin bindings from existing pin suggestions**

For each candidate returned by suggest_blueprint_nodes_for_pin:

1. resolve dynamic object bindings;
2. call GetBoundTemplateNode;
3. enumerate visible template pins in original pin order;
4. keep pins of opposite direction to the stable source pin;
5. classify Schema->CanCreateConnection(SourcePin, TemplatePin);
6. retain direct responses and retain conversion/promotion only for description with requires_conversion=true;
7. create a typed template-pin binding using pin name, direction, CanonicalJsonString(SerializeTypeSpec), and occurrence among identical signatures;
8. sort by direct before conversion, normalized pin name, type JSON, occurrence, binding ID;
9. emit deterministic zero-based rank.

Pin suggestion itself does not gain allow_conversion. It reports all native-compatible bindings, while add_blueprint_connected_action_node decides whether a reported conversion may execute from its explicit allow_conversion input.

Change only suggest_blueprint_nodes_for_pin to use PALETTE_PIN_PAGE_DATA. Plain search output remains PALETTE_PAGE_DATA.

- [ ] **Step 6: Extract the deterministic snapshot builder**

Move the graph selection, stable-ID validation, serialization, ordering, and digest body of SnapshotBlueprintGraph into BuildBlueprintGraphSnapshot. The public helper keeps request parsing and response-envelope creation:

~~~cpp
TSharedPtr<FJsonObject> Snapshot;
FError Error;
if (!BuildBlueprintGraphSnapshot(
        Blueprint, RequestedGraphIds, Snapshot, Error))
{
    return SerializeResult(MakeFailure(
        Error.Code, Error.Path, Error.Message, false, Error.Hint));
}
return SerializeResult(MakeSuccess(
    FString::Printf(
        TEXT("Snapshotted %d Blueprint graph(s)."),
        Snapshot->GetArrayField(TEXT("graphs")).Num()),
    Snapshot));
~~~

Add comment_bubble_visible to each node properties object so rollback comparison covers every metadata field this phase may copy. Keep snapshot_version=1 because properties is intentionally open and the wire shape does not change.

- [ ] **Step 7: Run regression, build, and native tests**

~~~powershell
uv run --project mcp-server --extra dev pytest mcp-server/tests/test_blueprint2_action_wrappers.py mcp-server/tests/test_blueprint2_contracts.py -q
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' UnrealMCPSampleEditor Win64 Development "$PWD\UnrealMCPSample.uproject" -WaitMutex -NoHotReload
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' "$PWD\UnrealMCPSample.uproject" -unattended -nop4 -nosplash -NullRHI -ExecCmds="Automation RunTests UnrealMCP.Blueprint2.Palette; Quit" -TestExit="Automation Test Queue Empty" -log
~~~

Expected: focused Python tests pass, UE 5.7 builds, all pre-existing palette tests pass, and the new context/binding/replacement-registry tests pass.

- [ ] **Step 8: Commit the shared-core slice**

~~~powershell
git add -- Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprintPaletteInternal.h Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprint2Internal.h Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonBlueprint2Internal.cpp Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintPalette.cpp Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintDiagnostics.cpp Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprintPaletteTests.cpp mcp-server/tests/test_blueprint2_action_wrappers.py
git commit -m "refactor: expose safe Blueprint palette primitives"
~~~

Expected: one behavior-preserving core commit plus tested typed semantic capability storage.

---

### Task 3: Implement two-pin suggestions and atomic connected spawn

**Files:**
- Create: Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintSemantic.cpp
- Create: Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprintSemanticTests.cpp
- Modify: Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h
- Modify: mcp-server/tests/test_blueprint2_action_wrappers.py

- [ ] **Step 1: Write failing native suggestion tests**

Create UnrealMCP.Blueprint2.Semantic.Suggestions tests with a transient Actor Blueprint fixture containing stable exec and typed data pins. Cover:

- source must be output, target must be input, both in graph_id;
- exec may bridge exec and data may bridge compatible data, but exec/data mixing is INVALID_INPUT;
- cross-graph, reversed, duplicate, zero-GUID, tampered cursor, and changed pin IDs fail;
- direct candidates precede conversion candidates;
- allow_conversion=false removes conversion/promotion pairs;
- allow_conversion=true keeps and marks them;
- wildcard candidates remain only when both schema checks pass;
- every pair has two action-owned typed binding tokens and deterministic rank;
- pagination produces no gaps/duplicates and a changed pair/policy/query/filter/limit rejects the cursor;
- repeated requests produce the same result digest, order, and token IDs in one editor session.

Run the build and expect missing UMCPythonHelper::SuggestBlueprintNodesForConnection.

- [ ] **Step 2: Declare the two public helper entry points**

Add:

~~~cpp
UFUNCTION(BlueprintCallable, Category="Editor|MCPython")
static FString SuggestBlueprintNodesForConnection(
    UBlueprint* Blueprint,
    const FString& RequestJson);

UFUNCTION(BlueprintCallable, Category="Editor|MCPython")
static FString AddBlueprintConnectedActionNode(
    UBlueprint* Blueprint,
    const FString& RequestJson);
~~~

Both implementations use the same UE 5.7 version guard as the palette adapter and return UE_VERSION_UNSUPPORTED outside 5.7 until another engine is validated.

- [ ] **Step 3: Implement strict parsers and template-pin identities**

The semantic source begins with closed parsers that enforce:

- request JSON is one object;
- graph/source/target/pin IDs have the exact persisted prefix and non-zero GUID;
- query at most 256 chars, limit 1..200, filters reuse ParseStoredFilters;
- action and binding IDs have exact opaque shapes;
- position coordinates are finite and within plus/minus 1,000,000,000;
- dynamic binding IDs are unique and at most 32.

Use this template-pin key:

~~~cpp
FString TemplatePinKey(const UEdGraphPin* Pin, const int32 Occurrence)
{
    const FString Direction =
        Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output");
    const FString TypeJson = CanonicalJsonString(
        MakeShared<FJsonValueObject>(SerializeTypeSpec(Pin->PinType)));
    return Direction + TEXT("\n") +
        Pin->PinName.ToString() + TEXT("\n") +
        TypeJson + TEXT("\n") +
        FString::FromInt(Occurrence);
}
~~~

Actual spawned-pin resolution recomputes the same key over visible pins in original order and requires exactly one match.

- [ ] **Step 4: Implement two-pin filtering and pagination**

Resolve both stable pins and validate direction/category. Build candidates with the two-pin context view. For each candidate, inspect the bound template node and form every ordered input/output pair. Retain a pair only when source-to-input and output-to-target are both allowed by the native schema and conversion policy.

Pair rank is deterministic:

~~~text
0 conversions on either edge
1 conversion/promotion on one edge
2 conversion/promotion on both edges
then normalized input name
then normalized output name
then input canonical type JSON
then output canonical type JSON
then input occurrence
then output occurrence
~~~

Discard a candidate with no pair. Append the best pair rank and all binding-pair keys to its candidate sort key before ordering. The result digest covers every candidate key and every ordered binding-pair key before pagination.

Register EPaletteContextKind::Connection action records with both pin IDs and allow_conversion. Cursor context includes the same fields.

- [ ] **Step 5: Write failing connected-spawn mutation tests**

Cover:

- only EPaletteContextKind::Pin action tokens are accepted;
- selected connection_binding_id must belong to action_id and be present in its returned connection_bindings;
- action result digest, source pin, graph schema, dynamic bindings, and template shape are revalidated;
- direct exec and scalar connections return actual stable IDs;
- conversion is rejected before mutation when allow_conversion=false;
- conversion succeeds and reports every auxiliary node/edge when true;
- singleton/out-of-graph node returns PRECONDITION_FAILED;
- invocation failure, missing actual pin, connection rejection, and injected TryCreateConnection failure restore the exact pre-snapshot;
- response has transaction_recorded=true and saved=false;
- no compile/save/PIE call occurs.

- [ ] **Step 6: Implement connected spawn with exact rollback verification**

Before the transaction:

1. capture BuildBlueprintGraphSnapshot for graph_id;
2. resolve the pin-context action and all dynamic bindings;
3. re-enumerate candidates and verify result digest;
4. resolve the chosen template binding and classify preflight compatibility.

Inside one FMutationScope:

1. Modify Blueprint, graph, source node, source pin owner, and every linked node;
2. invoke the action;
3. reject singleton/out-of-graph/unexpected-class results;
4. find the actual pin by template key;
5. call CanCreateConnection again;
6. reject conversion when false;
7. call TryCreateConnection;
8. collect graph nodes added since the pre-snapshot;
9. verify source and actual pin are linked through the reported direct or auxiliary topology;
10. mark the Blueprint modified once.

Every failure calls Scope.Rollback, captures the same selected-graph snapshot again, and compares canonical JSON. Equal snapshots return the original error. A mismatch returns ROLLBACK_FAILED with before_digest, after_digest, and stable IDs from the changed graph.

Do not call CreateAutomaticConversionNodeAndConnections directly; TryCreateConnection is the K2 authority and chooses the same conversion behavior as the editor.

- [ ] **Step 7: Run focused tests and build**

~~~powershell
uv run --project mcp-server --extra dev pytest mcp-server/tests/test_blueprint2_contracts.py mcp-server/tests/test_blueprint2_action_wrappers.py -q
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' UnrealMCPSampleEditor Win64 Development "$PWD\UnrealMCPSample.uproject" -WaitMutex -NoHotReload
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' "$PWD\UnrealMCPSample.uproject" -unattended -nop4 -nosplash -NullRHI -ExecCmds="Automation RunTests UnrealMCP.Blueprint2.Semantic.Suggestions; Automation RunTests UnrealMCP.Blueprint2.Semantic.ConnectedSpawn; Quit" -TestExit="Automation Test Queue Empty" -log
~~~

Expected: Python contracts pass, UE 5.7 builds, and every suggestion/connected-spawn native case passes.

- [ ] **Step 8: Commit the connected semantic slice**

~~~powershell
git add -- Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintSemantic.cpp Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprintSemanticTests.cpp Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h mcp-server/tests/test_blueprint2_action_wrappers.py
git commit -m "feat: add semantic Blueprint connected spawn"
~~~

---

### Task 4: Implement atomic insertion into one existing edge

**Files:**
- Modify: Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintSemantic.cpp
- Modify: Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprintSemanticTests.cpp
- Modify: Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h

- [ ] **Step 1: Write failing insertion tests**

Add UnrealMCP.Blueprint2.Semantic.Insert tests for:

- source/target still have the exact direct link;
- other legal links on either pin remain unchanged;
- only the named old edge is removed;
- action must have EPaletteContextKind::Connection and exact source/target/policy;
- selected input/output binding IDs must be one returned pair, in the correct order;
- direct exec and data insertion produce source-to-new and new-to-target;
- conversion policy cannot be widened at mutation time;
- template/actual pin mismatch refuses before old-edge removal;
- stale graph/action/cursor/pair/direct-edge state refuses without mutation;
- injected invoke, first connect, break, second connect, topology verification, and rollback failures;
- workflow undo restores the byte-identical graph snapshot.

- [ ] **Step 2: Declare and parse the insertion entry point**

~~~cpp
UFUNCTION(BlueprintCallable, Category="Editor|MCPython")
static FString InsertBlueprintActionNode(
    UBlueprint* Blueprint,
    const FString& RequestJson);
~~~

The parser accepts exactly graph_id, source_pin_id, target_pin_id, action_id, input_binding_id, output_binding_id, position, and bindings. There is no allow_conversion field because the action capability already fixes the policy.

- [ ] **Step 3: Implement preflight without destructive mutation**

Before FMutationScope:

1. resolve stable K2 graph and ordered pins;
2. require TargetPin in SourcePin->LinkedTo and SourcePin in TargetPin->LinkedTo;
3. resolve the connection-context action with the exact pin pair;
4. rebuild candidates/result digest and selected template pair;
5. resolve dynamic bindings and bound template;
6. classify both schema responses while treating only the named old edge as a planned break;
7. reject any response that would break another link;
8. capture the selected-graph snapshot.

Preflight does not invoke the action, break a link, compile, save, or mark the Blueprint.

- [ ] **Step 4: Apply the insertion in one transaction**

Inside FMutationScope:

1. Modify Blueprint, graph, source/target owners, every linked node, and pins;
2. invoke and validate the new node;
3. resolve actual selected pins and re-run both CanCreateConnection checks;
4. require both checks still satisfy the capability policy;
5. call BreakSinglePinLink(SourcePin, TargetPin);
6. call TryCreateConnection(SourcePin, ActualInput);
7. call TryCreateConnection(ActualOutput, TargetPin);
8. verify the old edge is absent, both replacement paths exist, and all unrelated pre-existing edges remain;
9. collect and sort auxiliary nodes/edges;
10. mark modified and return one create change plus one disconnect and two connect changes.

Any false return or topology mismatch uses the exact snapshot rollback path from Task 3. The response includes replaced_connection as the old stable pair and saved=false.

- [ ] **Step 5: Run insertion tests and all semantic native tests**

~~~powershell
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' UnrealMCPSampleEditor Win64 Development "$PWD\UnrealMCPSample.uproject" -WaitMutex -NoHotReload
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' "$PWD\UnrealMCPSample.uproject" -unattended -nop4 -nosplash -NullRHI -ExecCmds="Automation RunTests UnrealMCP.Blueprint2.Semantic; Quit" -TestExit="Automation Test Queue Empty" -log
~~~

Expected: all semantic tests pass, including exact snapshot equality after every injected failure.

- [ ] **Step 6: Commit insertion**

~~~powershell
git add -- Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintSemantic.cpp Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprintSemanticTests.cpp Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h
git commit -m "feat: insert palette nodes into Blueprint links"
~~~

---

### Task 5: Build read-only, snapshot-bound replacement previews

**Files:**
- Modify: Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintSemantic.cpp
- Modify: Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprintSemanticTests.cpp
- Modify: Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h

- [ ] **Step 1: Write failing replacement-mapping and preview tests**

Add UnrealMCP.Blueprint2.Semantic.ReplacementPreview cases for:

- preview never changes graph snapshot, dirty state, transaction queue, compile state, or saved package;
- action must be graph-context bound to the same graph;
- target node and every considered pin require persisted stable IDs;
- explicit mapping wins over inference;
- duplicate old pin, duplicate new binding, wrong action binding, wrong direction, exec/data mismatch, invalid default, and incompatible link are INVALID_INPUT;
- exact type plus exact normalized name is inferred;
- exact type beats convertible type;
- direct compatibility beats conversion compatibility;
- exact container/reference qualifiers beat mismatches;
- equal semantic scores remain unmapped even when lexical order differs;
- allow_conversion=false leaves conversion-only links unmapped;
- every non-empty writable unmapped input default and every individual unmapped link increments loss_count;
- allow_loss=false with loss_count greater than zero returns applicable=false;
- allow_loss=true returns applicable=true and the same complete loss lists;
- unsupported node-class-specific properties are reported but excluded from loss_count;
- replacement_plan_id changes when node snapshot, action result, bindings, mapping, conversion policy, or loss policy changes;
- the response validates against REPLACEMENT_PREVIEW_DATA.

- [ ] **Step 2: Declare and parse preview**

~~~cpp
UFUNCTION(BlueprintCallable, Category="Editor|MCPython")
static FString PreviewBlueprintActionReplacement(
    UBlueprint* Blueprint,
    const FString& RequestJson);
~~~

The parser accepts exactly graph_id, node_id, action_id, bindings, pin_mapping, allow_conversion, and allow_loss. pin_mapping has at most 256 unique objects. Enforce uniqueness by old_pin_id and new_binding_id after JSON parsing because JSON Schema uniqueItems does not catch two objects that reuse one side with another side changed.

- [ ] **Step 3: Capture the focused target-node snapshot digest**

Build a canonical JSON object containing:

~~~json
{
  "node_id": "node:...",
  "class_path": "/Script/...",
  "position": {"x": 0, "y": 0},
  "comment": "",
  "comment_bubble_visible": false,
  "enabled_state": "enabled",
  "pins": [
    {
      "pin_id": "pin:...",
      "name": "Value",
      "direction": "input",
      "type": {"kind": "int"},
      "default": 0,
      "linked_pin_ids": []
    }
  ]
}
~~~

Sort pins by stable pin ID and linked_pin_ids lexically. The digest is sha1: plus Sha1Hex(CanonicalJsonString(...)). This digest is narrower than the full graph snapshot and is the exact stale-plan precondition at apply time.

- [ ] **Step 4: Implement explicit mapping validation**

Resolve the graph-context action, exact dynamic bindings, current result digest, and bound template node. Register template-pin binding records for all visible pins.

For every explicit mapping:

1. resolve old_pin_id on node_id;
2. resolve new_binding_id for action_id;
3. require equal direction;
4. require both exec or both non-exec;
5. validate every old external link with CanCreateConnection against the new template pin and allow_conversion;
6. if the old pin is a writable unlinked input with a non-empty default, call NormalizeDefaultValue using the new template pin type;
7. reject mappings that fail any check;
8. mark old and new pins consumed.

Do not mutate the cached template pin default while checking normalization.

- [ ] **Step 5: Implement unique-best inference without lexical guessing**

Normalize pin names with TrimStartAndEnd, lowercase, then remove spaces and underscores. For each remaining old pin, calculate this semantic tuple for every eligible unused new template pin:

~~~text
type_cost:
  0 exact canonical type JSON
  1 native direct-compatible type
  2 conversion/promotion-compatible type
name_cost:
  0 normalized names equal
  1 normalized names differ
container_cost:
  0 container kind equal
  1 container kind differs but native schema permits it
qualifier_cost:
  0 reference/const qualifiers equal
  1 qualifiers differ but native schema permits it
~~~

Direction and exec/data category are hard eligibility gates. Conversion cost 2 is eligible only with allow_conversion=true. Links and writable defaults must all validate.

Sort candidates for response determinism by semantic tuple, normalized name, canonical type JSON, and occurrence. Infer only when exactly one candidate has the best semantic tuple. If two candidates share that tuple, leave the old pin unmapped and report ambiguous_best_mapping; lexical fields never break the inference tie.

- [ ] **Step 6: Materialize losses, metadata, applicability, and the plan token**

For each mapped pin, emit one retained connection per external link and one retained default for each non-empty writable unlinked input default. For each unmapped pin, emit one loss per external link and one loss for its non-empty writable input default.

Report old node property keys other than enabled_state as unsupported_metadata when they are not represented by position, comment, comment bubble, enabled state, links, or defaults. Include the exact reason node_class_specific_property_is_not_copied.

Register FReplacementPlanRecord only after the complete proposal is deterministic. Store canonical default JSON, mapping origin/reason, every external linked pin ID, position, comment, comment-bubble flag, enabled state, conversion/loss policies, node snapshot digest, and action result digest.

Preview returns success even when applicable=false; CONFLICT is reserved for apply or for a topology race discovered after preview construction.

- [ ] **Step 7: Run preview tests and prove read-only source guards**

Add Python source guards asserting the preview function body contains no FMutationScope, Invoke, TryCreateConnection, BreakSinglePinLink, DestroyNode, MarkBlueprintAsModified, CompileBlueprint, or SavePackage.

Run:

~~~powershell
uv run --project mcp-server --extra dev pytest mcp-server/tests/test_blueprint2_action_wrappers.py mcp-server/tests/test_blueprint2_contracts.py -q
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' UnrealMCPSampleEditor Win64 Development "$PWD\UnrealMCPSample.uproject" -WaitMutex -NoHotReload
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' "$PWD\UnrealMCPSample.uproject" -unattended -nop4 -nosplash -NullRHI -ExecCmds="Automation RunTests UnrealMCP.Blueprint2.Semantic.ReplacementPreview; Quit" -TestExit="Automation Test Queue Empty" -log
~~~

Expected: contracts and source guards pass, UE builds, preview tests pass, and every read-only assertion observes no mutation.

- [ ] **Step 8: Commit replacement preview**

~~~powershell
git add -- Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintSemantic.cpp Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprintSemanticTests.cpp Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h mcp-server/tests/test_blueprint2_action_wrappers.py
git commit -m "feat: preview semantic Blueprint node replacement"
~~~

---

### Task 6: Apply unchanged replacement plans atomically

**Files:**
- Modify: Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintSemantic.cpp
- Modify: Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprintSemanticTests.cpp
- Modify: Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h
- Modify: mcp-server/tests/test_blueprint2_action_wrappers.py

- [ ] **Step 1: Write failing strict, lossy, stale, and rollback tests**

Add UnrealMCP.Blueprint2.Semantic.ReplacementApply cases for:

- strict plan with any loss returns CONFLICT before FMutationScope;
- request allow_loss must exactly equal the preview plan policy;
- malformed/unknown/expired/evicted plan is INVALID_INPUT;
- changed node digest, graph schema, action result, dynamic binding object, template pin shape, external linked pin, or default is PRECONDITION_FAILED;
- strict success preserves every mapped incoming/outgoing link, writable default, position, comment, bubble visibility, and enabled state;
- lossy success reports every dropped link/default in both warnings and changes;
- actual spawned-pin mismatch refuses before destroying the old node;
- conversion is created only when the preview policy allowed it and every auxiliary node/edge is returned;
- old node DestroyNode hook runs after successful rewiring;
- new node gets a different stable node ID and all visible pins get stable IDs;
- invocation, default assignment, each connection, metadata copy, destroy, final topology, and rollback failures restore exact full graph snapshot;
- workflow undo restores the pre-replacement digest;
- transaction_recorded=true and saved=false;
- compile and save remain explicit.

Expose deterministic failure injection only under WITH_DEV_AUTOMATION_TESTS:

~~~cpp
enum class ESemanticFailurePoint : uint8
{
    None,
    AfterInvoke,
    AfterDefaults,
    AfterFirstConnection,
    BeforeDestroy,
    AfterDestroy,
    BeforeFinalVerification
};

void SetSemanticFailurePointForTests(ESemanticFailurePoint Point);
~~~

- [ ] **Step 2: Declare and parse replacement apply**

~~~cpp
UFUNCTION(BlueprintCallable, Category="Editor|MCPython")
static FString ReplaceBlueprintNodeWithAction(
    UBlueprint* Blueprint,
    const FString& RequestJson);
~~~

The parser accepts exactly graph_id, replacement_plan_id, and allow_loss. Resolve the plan against Blueprint->GetPathName and graph_id before any graph mutation.

- [ ] **Step 3: Revalidate the complete plan before opening a transaction**

Repeat all mutable checks:

1. resolve graph and target node by stable IDs;
2. recompute focused node snapshot digest;
3. resolve graph-context action and require stored action result digest;
4. resolve the exact stored dynamic bindings;
5. obtain the current bound template;
6. resolve every stored new binding by pin key;
7. resolve every stored external linked pin;
8. rerun CanCreateConnection and default normalization;
9. require allow_loss equality;
10. return CONFLICT if LossCount is positive and false;
11. capture the full selected-graph snapshot.

Do not rely on pointer identity from preview. The registry contains primitives only.

- [ ] **Step 4: Spawn, preserve defaults/metadata, and rewire before destruction**

Inside one valid FMutationScope:

1. Modify Blueprint, graph, old node, all linked nodes, and their owners;
2. invoke the selected action at stored PositionX/PositionY;
3. reject null, singleton, old-node, out-of-graph, or unexpected-class results;
4. resolve actual pins by the stored template keys;
5. apply each retained default through NormalizeDefaultValue followed by TrySetDefaultValue, TrySetDefaultObject, or TrySetDefaultText as appropriate;
6. reserialize each actual default and require equality with stored canonical JSON;
7. copy NodePosX, NodePosY, NodeComment, bCommentBubbleVisible, and SetEnabledState;
8. for every retained external link, call CanCreateConnection and TryCreateConnection on the actual new pin;
9. permit a BREAK_OTHERS response only when the broken link is the planned old-node edge;
10. verify every mapped external pin is now connected to the new actual pin;
11. call OldNode->DestroyNode();
12. verify the old node/pins no longer resolve, new and auxiliary IDs are stable, planned connections/defaults exist, unrelated topology matches the pre-snapshot, and losses equal the stored loss lists;
13. call FBlueprintEditorUtils::MarkBlueprintAsModified once.

The output preserved_metadata array contains position, comment, comment_bubble_visible, and enabled_state. dropped_connections and dropped_defaults are empty for strict success and exactly equal the preview lists for lossy success.

- [ ] **Step 5: Implement exact rollback and structured failure details**

All post-scope errors go through one function:

~~~cpp
FString RollbackSemanticFailure(
    FMutationScope& Scope,
    UBlueprint* Blueprint,
    const FString& GraphId,
    const TSharedPtr<FJsonObject>& BeforeSnapshot,
    const FError& OriginalError,
    const TArray<FString>& AffectedIds);
~~~

After Scope.Rollback, recapture BuildBlueprintGraphSnapshot. If canonical JSON equals BeforeSnapshot, return OriginalError. Otherwise return ROLLBACK_FAILED with before_digest, after_digest, sorted unique affected_ids, and a compact diff generated from the two snapshots. Never include pointers, object addresses, or a raw C++/Python traceback.

- [ ] **Step 6: Add safety and workflow contract tests**

In test_blueprint2_action_wrappers.py assert:

- all three semantic mutation bodies contain FMutationScope and BuildBlueprintGraphSnapshot;
- preview contains neither;
- all five bodies contain no CompileBlueprint, SavePackage, execute_python, or PIE command;
- replacement uses DestroyNode and does not call UEdGraph::RemoveNode;
- MarkBlueprintAsModified occurs after final topology verification;
- every mutation response serializer sets saved=false.

In test_blueprint2_contracts.py construct one workflow plan for connected spawn, insertion, and replacement. Assert all three can be planned/applied, supports_undo=true, asset_path is locked, and workflow undo is offered. Assert suggestion/preview remain read-only and cannot create an undo step.

- [ ] **Step 7: Run the full native semantic gate**

~~~powershell
uv run --project mcp-server --extra dev pytest mcp-server/tests/test_blueprint2_contracts.py mcp-server/tests/test_blueprint2_action_wrappers.py mcp-server/tests/test_workflow_planner.py mcp-server/tests/test_workflow_executor.py -q
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' UnrealMCPSampleEditor Win64 Development "$PWD\UnrealMCPSample.uproject" -WaitMutex -NoHotReload
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' "$PWD\UnrealMCPSample.uproject" -unattended -nop4 -nosplash -NullRHI -ExecCmds="Automation RunTests UnrealMCP.Blueprint2.Semantic; Quit" -TestExit="Automation Test Queue Empty" -log
~~~

Expected: all focused Python tests and every native semantic test pass with zero errors.

- [ ] **Step 8: Commit replacement apply**

~~~powershell
git add -- Plugins/UnrealMCPython/Source/UnrealMCPython/Private/MCPythonHelper_BlueprintSemantic.cpp Plugins/UnrealMCPython/Source/UnrealMCPython/Private/Tests/MCPythonBlueprintSemanticTests.cpp Plugins/UnrealMCPython/Source/UnrealMCPython/Public/MCPythonHelper.h mcp-server/tests/test_blueprint2_action_wrappers.py mcp-server/tests/test_blueprint2_contracts.py
git commit -m "feat: replace Blueprint nodes from semantic plans"
~~~

---

### Task 7: Prove editor, workflow, live MCP, documentation, and release gates

**Files:**
- Create: Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_semantic.py
- Modify: Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/run_all.py
- Modify: mcp-server/tests/test_e2e.py
- Modify: mcp-server/tests/test_blueprint2_contracts.py
- Modify: README.md
- Modify: mcp-server/README.md
- Regenerate: mcp-server/src/unreal_mcp/dispatchers/_catalog.py
- Regenerate: mcp-server/src/unreal_mcp/dispatchers/_registry.py

- [ ] **Step 1: Add real editor acceptance tests**

The semantic editor suite creates unique /Game/__MCPTests fixtures and deletes them in tearDownClass even after failure. Add tests for:

1. pin suggestion emits explicit connection_bindings;
2. connected spawn for exec and scalar pins;
3. two-pin suggestion and exec insertion;
4. data insertion with a plugin-contributed palette action when one is installed, otherwise a capability skip with a concrete reason;
5. replacement preserves links, defaults, position, comment, bubble visibility, and enabled state;
6. strict preview refuses loss;
7. explicit lossy preview/apply reports every loss;
8. stable returned IDs match inspect_blueprint and snapshot_blueprint_graph;
9. explicit compile_blueprint and get_blueprint_health succeed after each mutation;
10. no mutation reports saved=true;
11. workflow apply/undo restores exact pre-operation snapshot for connected spawn, insertion, and replacement;
12. test_semantic_workflow_stress_12_of_12 executes four add, four insert, and four replacement/undo cycles and prints Semantic workflow stress: 12/12.

Use class cleanup assertions:

~~~python
@classmethod
def tearDownClass(cls):
    for asset_path in reversed(cls.created_assets):
        cls.delete_asset(asset_path)
    leftovers = unreal.EditorAssetLibrary.list_assets(
        "/Game/__MCPTests", recursive=True, include_folder=False
    )
    cls.assertEqual(leftovers, [], f"Semantic test assets remain: {leftovers}")
~~~

Register the module in tests/run_all.py beside test_blueprint2_palette.

- [ ] **Step 2: Add the dedicated live semantic MCP workflow**

Add test_blueprint_semantic_graph_editing_round_trip to test_e2e.py. It must:

1. create an unsaved Actor Blueprint;
2. inspect EventGraph stable IDs;
3. construct one direct exec edge with existing safe graph actions;
4. call suggest_blueprint_nodes_for_connection;
5. insert an explicit returned pair;
6. use suggest_blueprint_nodes_for_pin and connected spawn on a second pin;
7. snapshot;
8. search a replacement action;
9. preview strict replacement and apply only if applicable;
10. run an explicitly lossy preview in a disposable branch and verify warnings;
11. explicitly compile and run health;
12. run one mutation through workflow.plan/apply/undo;
13. diff and require exact restoration;
14. delete every created asset in finally;
15. check editor reachability after cleanup.

Every dispatcher result goes through _assert_not_connection_error. Assert saved is false for all three semantic mutations.

- [ ] **Step 3: Update documentation and exact totals**

Change 294 to 299 and Blueprint 52 to 57 in both READMEs. Document these LLM paths:

~~~text
Connected spawn:
inspect_blueprint -> suggest_blueprint_nodes_for_pin
-> add_blueprint_connected_action_node
-> snapshot_blueprint_graph -> compile_blueprint -> get_blueprint_health

Insertion:
inspect_blueprint -> suggest_blueprint_nodes_for_connection
-> insert_blueprint_action_node
-> snapshot_blueprint_graph -> compile_blueprint -> get_blueprint_health

Replacement:
inspect_blueprint -> search_blueprint_node_actions
-> preview_blueprint_action_replacement
-> replace_blueprint_node_with_action
-> diff_blueprint_graphs -> compile_blueprint -> get_blueprint_health
~~~

Explain action/binding/cursor/plan session scope, explicit pair selection, stale-token recovery, allow_conversion, strict allow_loss behavior, workflow undo, and explicit saving.

Update remaining exact assertions to 299 total, 57 Blueprint, 298 JSON sweep pairs, and 22 domains. Regenerate catalog and registry.

- [ ] **Step 4: Run the complete offline gate**

~~~powershell
uv run --project mcp-server python mcp-server/generate_catalog.py
git diff --exit-code -- mcp-server/src/unreal_mcp/dispatchers/_catalog.py mcp-server/src/unreal_mcp/dispatchers/_registry.py
uv run --project mcp-server --extra dev pytest mcp-server/tests -q
~~~

Expected: the prior 596-passed baseline plus new semantic tests passes; environment-gated skips remain visible and generated files are deterministic.

- [ ] **Step 5: Run UE 5.7 build and all native Blueprint2 tests**

~~~powershell
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat' UnrealMCPSampleEditor Win64 Development "$PWD\UnrealMCPSample.uproject" -WaitMutex -NoHotReload
& 'C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' "$PWD\UnrealMCPSample.uproject" -unattended -nop4 -nosplash -NullRHI -ExecCmds="Automation RunTests UnrealMCP.Blueprint2; Quit" -TestExit="Automation Test Queue Empty" -log
~~~

Expected: UBT succeeds and every Blueprint2 native test passes with zero failures.

- [ ] **Step 6: Run full in-editor and stress acceptance**

Open UE 5.7 and execute:

~~~python
import runpy
runpy.run_module("UnrealMCPython.tests.run_all", run_name="__main__")
~~~

Expected: the prior 416-test editor baseline plus semantic tests has zero failures/errors, only explicit capability skips, remaining_assets=0, and Semantic workflow stress: 12/12.

- [ ] **Step 7: Run the live MCP E2E gate**

With the UE 5.7 editor TCP server reachable on 127.0.0.1:12029:

~~~powershell
uv run --project mcp-server --extra dev pytest mcp-server/tests/test_e2e.py -v
~~~

Expected: all 299 catalog actions are accounted for, all 298 JSON action pairs return unwrapped results, the semantic round trip passes, the editor remains reachable, and /Game/__MCPTests is empty.

- [ ] **Step 8: Record compatibility gates honestly**

UE 5.7 is the mandatory release target. Run UE 5.6 and UE 5.8 build/native/editor gates only when the complete engine, headers, and runnable editor are available. Record each unavailable or unexecuted version as not run with the concrete reason; never infer a pass from schemas, installed directory names, or preprocessor guards.

- [ ] **Step 9: Commit release proof and push only the user fork**

~~~powershell
git add -- Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/test_blueprint2_semantic.py Plugins/UnrealMCPython/Content/Python/UnrealMCPython/tests/run_all.py mcp-server/tests/test_e2e.py mcp-server/tests/test_blueprint2_contracts.py README.md mcp-server/README.md mcp-server/src/unreal_mcp/dispatchers/_catalog.py mcp-server/src/unreal_mcp/dispatchers/_registry.py
git commit -m "test: prove semantic Blueprint graph workflows"
git status --short
git remote -v
git push origin codex/llm-friendly-expansion
~~~

Expected: clean worktree, origin is the only remote, and the branch push succeeds. Do not add or use an upstream remote.

## Final evidence checklist

Before claiming this phase complete, preserve exact outputs for:

- 299 actions across 22 domains and 57 Blueprint actions;
- complete offline pytest result;
- deterministic catalog generation;
- UE 5.7 UBT result;
- all UnrealMCP.Blueprint2 native automation results;
- full in-editor count, zero failures/errors, and remaining_assets=0;
- Semantic workflow stress: 12/12;
- live MCP E2E count and editor reachability;
- strict and explicitly lossy replacement proof;
- snapshot equality after workflow undo;
- git status, only-origin remote list, pushed commit SHA;
- explicit not-run reasons for every unavailable engine gate.

Do not infer success from source inspection when a runnable gate exists. A failed, skipped, or unavailable editor gate remains visible in the delivery handoff.
