# Universal Blueprint 2.0 MCP Design

**Date:** 2026-07-27
**Status:** Approved in conversation; awaiting written-spec review
**Repository:** `pfizerboss/unreal-mcp`

## Summary

The next release expands the MCP from a workflow-safe Unreal automation server
into a broad, Blueprint-first editing surface. The model must be able to orient
itself in any supported `UBlueprint`-derived asset, request only the relevant
parts of large graphs, address objects through stable identifiers, perform
typed structural mutations, compile the result, diagnose failures, compare
before and after state, and undo a complete batch through the existing workflow
engine.

This release is deliberately generic. It does not generate a particular game,
character controller, input setup, HUD, GameMode, or project configuration.

## Goals

- Give an LLM a compact, paginated view of large Blueprints without returning
  an unbounded graph dump.
- Provide stable identifiers for graphs, members, nodes, pins, variables, and
  components so later mutations do not depend on display names or array
  indexes.
- Support broad member, graph, variable, and component authoring through
  explicit, schema-described `blueprint.*` actions.
- Resolve reflected Unreal functions, properties, classes, enums, and structs
  by full object path rather than only supporting a fixed node list.
- Return precise type, connection, compiler, health, and graph-diff
  diagnostics suitable for autonomous recovery.
- Reuse workflow planning, confirmation, editor leases, transactions,
  cancellation, rollback classification, fingerprints, and guarded undo.
- Preserve all existing Blueprint action names and compatible response fields.
- Support UE 5.6, 5.7, and 5.8 through isolated version guards and runtime
  capability metadata.

## Non-goals

- A gameplay-foundation or one-click base-game generator.
- Automatic creation of Character, GameMode, Enhanced Input, HUD, or other
  game-specific assets.
- Automatic PIE, project-setting changes, world-setting changes, or editor
  restarts.
- Implicit asset saves or compilation after every narrow mutation.
- Arbitrary Python execution as a substitute for typed Blueprint actions.
- Replacing specialized `umg`, `anim_blueprint`, or other domain actions when
  their schema-specific behavior is not safely expressible through K2 APIs.

The existing placeholder `workflow.plan_gameplay_foundation` and
`workflow.verify_gameplay_foundation` actions are outside the new product
direction. They will be removed from the public catalog in the first delivery
stage; generic `workflow.plan`, `apply`, `get`, `cancel`, and `undo` remain.

## Supported asset scope

The shared core accepts `UBlueprint` and subclasses. Standard Blueprint assets
receive the complete operation set. Widget Blueprints, Animation Blueprints,
and other subclasses receive operations only where their graph schema and
editor APIs support the requested feature. A schema-specific incompatibility
returns `UE_VERSION_UNSUPPORTED` with the Blueprint class, graph schema, and
missing capability; the MCP never coerces an unsupported asset into a standard
K2 graph.

Specialized domains remain available for UMG, animation, and other asset-type
features. Blueprint 2.0 supplies the common inspection, member, K2 graph,
variable, component, compile, and diff foundation.

## Considered approaches

### Native C++ core with thin Python and MCP wrappers

This is the selected approach. Unreal's stable editor APIs for K2 graphs,
Blueprint members, SCS components, compiler results, and transactions are
primarily available in C++. Thin Python functions load assets, perform simple
argument validation, and call reflected helper methods. Generated MCP metadata
provides strict schemas and safety information.

This costs more implementation effort than Python-first code but gives stable
identifiers, better type information, explicit version guards, reliable
transactions, and structured diagnostics.

### Python-first implementation

This would be faster for basic inspection but Unreal Python does not expose
enough of the K2, SCS, schema, and compiler APIs. The missing surface would
force fragile reflection tricks or `execute_python` fallbacks and would not
meet the safety or diagnostic requirements.

### One generic JSON mutation interpreter

A single public mutation action would reduce the tool count, but it would
create a large, difficult-to-discover command language with opaque validation
errors. The selected design keeps shared internal mutation engines while
exposing narrow public actions with explicit JSON Schemas.

## Architecture

### C++ Blueprint core

Blueprint 2.0 is split by responsibility rather than added to one oversized
translation unit:

- `MCPythonHelper_BlueprintInspection.cpp` serializes briefs, filtered
  queries, stable IDs, and pagination.
- `MCPythonHelper_BlueprintMembers.cpp` authors functions, macros, custom
  events, dispatchers, and interfaces.
- `MCPythonHelper_BlueprintGraph.cpp` creates and mutates nodes, connects and
  disconnects pins, and performs layout.
- `MCPythonHelper_BlueprintVariables.cpp` parses canonical types and mutates
  variable definitions, defaults, metadata, and replication.
- `MCPythonHelper_BlueprintComponents.cpp` addresses SCS nodes and mutates
  component hierarchy and defaults.
- `MCPythonHelper_BlueprintDiagnostics.cpp` compiles, checks health, snapshots
  graphs, and produces diffs.
- `MCPythonBlueprint2Internal.h/.cpp` owns shared ID, type, cursor,
  serialization, target-resolution, change-record, and transaction helpers.

`MCPythonHelper.h` contains only reflected entry-point declarations. Internal
helpers remain non-public and are not included in the generated MCP catalog.

### Python wrapper layer

`UnrealMCPython/blueprint2.py` contains shared asset loading, compact JSON
construction, and compatibility adapters. `blueprint_actions.py` retains the
public `ue_*` functions so current discovery and dispatch behavior remains
stable. New functions are thin delegates and do not reimplement graph logic.

### MCP layer

Every public operation remains a separate `blueprint` action with a generated
input schema, output schema, effect, risk, preview, undo, confirmation,
supported-version, and capability declaration. Existing actions retain their
names and accepted parameters. Their internals may delegate to Blueprint 2.0,
but existing top-level compatibility fields are not removed.

The generic workflow engine remains the batching surface. Blueprint 2.0 does
not introduce a second batch executor.

## Stable identifiers

The inspection surface returns identifiers from persisted Unreal data:

- `UEdGraph::GraphGuid` for graphs and graph-backed members;
- `UEdGraphNode::NodeGuid` for nodes;
- `UEdGraphPin::PinId` for pins;
- `FBPVariableDescription::VarGuid` for Blueprint variables;
- SCS variable GUIDs for components;
- graph GUIDs and referenced interface paths for interface implementations.

Every record includes `id`, `id_kind`, and `stable`. Old assets that lack a
persisted GUID receive a deterministic qualified-name fallback with
`stable=false`. Mutations reject unstable IDs by default. A compatibility
fallback is accepted only when the request explicitly supplies
`allow_name_fallback=true`, the current owner and name, and enough type
information to resolve exactly one object. Workflow planning signs that target
description and preflight resolves it again before opening a transaction.

Display names and array indexes are never treated as stable identifiers.

## Inspection API

### Blueprint brief

`blueprint.get_blueprint_brief` returns a bounded orientation result:

- asset path and Blueprint class;
- parent/generated/skeleton class paths;
- compile status;
- implemented interfaces;
- top-level component and graph names;
- counts for variables, components, functions, macros, events, dispatchers,
  interfaces, graphs, and nodes;
- capabilities that differ for this Blueprint subclass or UE version.

It never embeds full node or pin arrays.

### Filtered inspection

`blueprint.inspect_blueprint` accepts a list of queries. Supported `op` values
are:

`overview`, `variables`, `variable_defaults`, `components`,
`component_hierarchy`, `functions`, `macros`, `events`, `dispatchers`,
`interfaces`, `nodes`, `pins`, and `connections`.

Each query may filter by graph ID, member ID, node ID, kind, name pattern, or
class path where relevant. Results sort by stable ID and then qualified name.
The default page limit is 100 and the maximum is 500.

Each query result has its own opaque `next_cursor`. A cursor binds to the asset
path, query digest, editor session, and last returned stable ID. Reusing it for
a different query, asset, or editor session returns `INVALID_INPUT`. A
top-level cursor is accepted only for a single-query compatibility request.

Compact mode returns IDs, kinds, names, type summaries, and counts. Detailed
mode adds node titles and positions, typed pins and defaults, member metadata,
component defaults, and linked node/pin IDs only for the requested records.

## Public mutation surface

### Members

- `create_blueprint_function`
- `rename_blueprint_function`
- `set_blueprint_function_signature`
- `delete_blueprint_function`
- `create_blueprint_macro`
- `delete_blueprint_macro`
- `create_custom_event`
- `delete_custom_event`
- `add_event_dispatcher`
- `remove_event_dispatcher`
- `add_blueprint_interface`
- `remove_blueprint_interface`

Function signatures submit the complete desired ordered input/output list plus
pure, const, access, category, and description fields. Interface addition
returns the IDs of generated implementation graphs.

### Graphs and pins

Existing `add_blueprint_node`, `remove_blueprint_node`,
`set_blueprint_node_position`, `connect_blueprint_pins`,
`auto_layout_graph`, and `build_blueprint_graph` remain compatible and delegate
to the new target and type infrastructure where possible.

New actions are:

- `add_reflected_blueprint_node`
- `set_blueprint_node_properties`
- `disconnect_blueprint_pins`

The existing common-node action expands to cover branch, sequence, cast,
variable get/set, arithmetic and comparison operators, events, select, switch,
reroute, comment, and make/break struct families. The reflected-node action
requires a full Unreal object path for functions, properties, classes, enums,
or structs. Node property mutation uses a per-node-class allowlist.

Connections use stable pin IDs when available and accept pin names only as the
explicit compatibility fallback. The response lists any conversion nodes
inserted by Unreal.

### Variables

Existing `add_variable`, `list_blueprint_variables`, and `set_variable_flags`
remain. New actions are:

- `rename_blueprint_variable`
- `remove_blueprint_variable`
- `set_blueprint_variable_default`
- `set_blueprint_variable_metadata`
- `set_blueprint_variable_replication`

Variable mutations target the variable GUID. Metadata covers category,
tooltip, visibility, instance editability, `ExposeOnSpawn`, `SaveGame`,
cinematic exposure, and supported replication settings.

### Components

Existing component list/add/remove/property actions remain. New actions are:

- `rename_blueprint_component`
- `reparent_blueprint_component`
- `reorder_blueprint_component`
- `set_blueprint_component_transform`

Component mutations target SCS variable GUIDs. Rename preserves references,
reparent rejects cycles, reorder uses an explicit sibling index, and property
updates reject read-only or unknown reflected fields.

## Canonical type and default-value model

New APIs use structured type specifications instead of ambiguous display
strings. Representative forms are:

```json
{"kind":"bool"}
{"kind":"real","precision":"double"}
{"kind":"enum","type_path":"/Script/Engine.ECollisionChannel"}
{"kind":"struct","type_path":"/Script/CoreUObject.Vector"}
{"kind":"object","class_path":"/Script/Engine.Actor"}
{"kind":"soft_object","class_path":"/Script/Engine.Texture2D"}
{"kind":"array","item":{"kind":"name"}}
{"kind":"set","item":{"kind":"object","class_path":"/Script/Engine.Actor"}}
{"kind":"map","key":{"kind":"name"},"value":{"kind":"int"}}
```

Supported scalar kinds include bool, byte, int, int64, real, string, name,
text, enum, struct, object, class, interface, soft object, and soft class.
Containers include array, set, and map when supported by the target UE
version. Legacy string aliases remain accepted by existing actions and are
normalized to the structured model.

Defaults are JSON values validated against the canonical type. Object and
class references use full Unreal object paths. Enum defaults use canonical
names. Struct and container imports validate the entire value before mutation.
Unsupported or lossy conversions are rejected.

## Mutation semantics and data flow

The recommended model flow is:

`brief -> filtered inspect -> workflow.plan -> workflow.apply -> compile -> health/diff -> optional workflow.undo`

Every mutation:

1. loads and class-checks the Blueprint;
2. resolves every target and validates IDs, names, types, collisions, and pin
   compatibility before changing the asset;
3. enters the existing workflow atomic-step wrapper when workflow-owned;
4. calls `Modify()` on every affected UObject;
5. applies one narrow operation;
6. marks the Blueprint modified or structurally modified as appropriate;
7. returns stable IDs and normalized change records;
8. does not save or compile the asset.

Structural actions return a suggested `compile_blueprint` next action. A model
should perform a batch of related mutations and compile once, then request
health or a graph diff.

## Transaction behavior

When the workflow lease is active, Blueprint mutations participate in the
single outer workflow transaction and never create a nested scoped
transaction. A direct compatible-mode invocation creates one local scoped
transaction so the editor undo stack still receives a coherent entry.

Validation completes before `Modify()` wherever Unreal APIs permit it. A
multi-part mutation that fails after modification is reported as failed and is
rolled back through the owning transaction. Recovery results include residual
changes when Unreal cannot fully restore state.

No mutation saves an asset, compiles implicitly, starts PIE, changes project or
world settings, or restarts the editor.

## Result and error contract

New actions use the existing structured tool result envelope. Successful
mutations include:

- `data` with created or resolved stable IDs;
- `changes` with normalized create/update/delete records;
- `warnings` for non-fatal compiler or compatibility conditions;
- `next_actions` for compile, inspect, health, or undo.

Important error behavior is fixed:

- missing or drifted stable target: `PRECONDITION_FAILED`;
- duplicate name or non-equivalent existing member: `CONFLICT`;
- invalid type, default, cursor, node property, or pin target: `INVALID_INPUT`;
- unsupported Blueprint subclass, graph schema, node family, type, or UE API:
  `UE_VERSION_UNSUPPORTED`;
- incompatible connection: `INVALID_INPUT` containing source and target type
  summaries;
- compile failure: `COMPILE_FAILED` with graph, node GUID, severity, normalized
  diagnostic code, message, and recovery hint;
- incomplete transaction recovery: `ROLLBACK_FAILED` with residual changes.

The implementation never silently switches from a missing GUID to an object
with the same display name and never silently coerces unsupported types.

## Compile, health, snapshot, and diff

The existing `compile_blueprint` action keeps its compatible fields and gains
structured diagnostics and stable node references.

`get_blueprint_health` compiles explicitly and checks:

- compiler warnings and errors;
- disconnected required pins;
- unresolved members and stale references;
- missing interface implementations;
- duplicate member names;
- invalid class and variable defaults;
- invalid component hierarchy.

Health is a write/medium action because compilation can mutate generated
classes and editor state.

`snapshot_blueprint_graph` returns a deterministic compact graph snapshot
ordered by stable IDs. `diff_blueprint_graphs` compares two snapshots and
reports added, removed, and changed nodes, pins, connections, properties, and
positions. Diff output defaults to counts and IDs, supports independent
pagination, and returns detailed records only when requested.

## Version compatibility and capabilities

The target matrix is UE 5.6, 5.7, and 5.8. Signature differences are isolated
inside the C++ core with `ENGINE_MINOR_VERSION` guards. Public wrappers do not
contain scattered version branches.

Generated action metadata declares coarse version support. Runtime discovery
adds Blueprint-specific capability details for node families, type kinds,
containers, graph schemas, compiler diagnostics, and component operations. A
missing feature is rejected during planning or validation rather than after a
partial mutation.

## Backward compatibility

- All 18 existing Blueprint actions remain discoverable.
- Existing required parameters and representative response fields remain.
- Existing string type aliases continue to work for old actions.
- Current golden namespace schemas remain unchanged except for additive
  optional fields.
- New strict schemas apply to new actions.
- Legacy actions may delegate to the new core only after golden and in-editor
  compatibility tests prove equivalent behavior.

## Delivery stages

### Stage 1: Compact inspection

Implement shared stable IDs and cursors, Blueprint brief, filtered inspection,
runtime capabilities, and removal of the two gameplay-foundation placeholder
actions from the public workflow catalog.

### Stage 2: Members and interfaces

Implement functions and complete signatures, macros, custom events,
dispatchers, and Blueprint interfaces. Verify all created members through the
inspection API before and after rename or deletion.

### Stage 3: Graphs, variables, and components

Implement reflected and common node families, property mutation,
connect/disconnect diagnostics, the canonical type/default model, variable
metadata and replication, and component rename/reparent/reorder/transform.

### Stage 4: Diagnostics and diff

Enhance compilation, implement health, graph snapshots, paginated diffs, and
the complete live workflow E2E.

Each stage is independently test-driven, committed, and required to pass its
offline and applicable in-editor gates before the next stage begins.

## Testing strategy

### Test-first requirement

Every new behavior begins with a focused failing test. Implementation is the
minimum needed to pass, followed by refactoring while the test remains green.
Generated files are regenerated only after source metadata tests define the
desired action contract.

### In-editor tests

- One named test for every public action.
- Stable-ID and cursor tests, including stale and query-mismatched cursors.
- Function/member lifecycle and ordered signature tests.
- Common and reflected node-family tests.
- Compatible and incompatible pin connection tests, including inserted
  conversion nodes.
- Scalar, enum, struct, object/class, soft-reference, array, set, and map
  default tests.
- Variable metadata and replication tests.
- Component lifecycle, hierarchy, cycle rejection, ordering, transform, and
  default-property tests.
- Valid, warning, and intentionally broken Blueprint compile/health tests.
- Compact and detailed graph diff pagination tests.

Temporary assets live only under `/Game/__MCPTests/Blueprint2_<uuid>` and are
removed in `finally`. Retained test assets fail the gate and are listed
explicitly.

### Offline tests

- Registry generation and stale generated-file checks.
- Complete JSON Schema and safety metadata coverage.
- Named coverage for every new action.
- Legacy golden schema and response compatibility.
- Result normalization for success, warning, validation, compile, version,
  precondition, and rollback errors.
- Workflow planning tests for all new mutation actions.
- Cursor, canonical type, and graph-diff contract tests without Unreal where
  deterministic pure helpers exist.

### Build and live gates

Each C++ stage requires a closed-editor UE build and the focused in-editor
suite. The locally installed complete gate runs on UE 5.7. The code and tests
retain version guards and capability expectations for UE 5.6 and 5.8; those
versions run when matching workers are available.

The final live E2E performs the complete MCP/TCP/C++ path:

1. create a unique test Blueprint;
2. inspect its brief and stable IDs;
3. plan and apply member, variable, component, node, and pin mutations;
4. compile and assert healthy status;
5. snapshot and verify the expected graph diff;
6. use the workflow undo token;
7. verify the pre-state is restored;
8. clean the asset and run an editor-liveness canary.

## Acceptance criteria

- The model can inspect a large Blueprint without an unbounded response.
- All inspectable objects use persisted stable IDs or an explicitly marked
  non-stable fallback.
- The model can create and edit members, common/reflected graph nodes, pin
  connections, variables, and components without arbitrary Python.
- All mutations validate targets and types before modification and integrate
  with workflow rollback and undo.
- Compilation and health errors point to stable graph and node IDs and include
  actionable recovery hints.
- Graph snapshots and diffs are deterministic and paginated.
- Existing Blueprint actions and representative schemas remain compatible.
- No Blueprint 2.0 action saves assets, starts PIE, changes project settings,
  or generates game-specific content.
- Catalog validation, the full offline suite, UE build, in-editor suite, live
  E2E, cleanup, and editor-liveness gates all pass.
