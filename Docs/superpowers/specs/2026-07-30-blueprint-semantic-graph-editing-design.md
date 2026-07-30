# Blueprint Semantic Graph Editing Design

**Date:** 2026-07-30

## Summary

This phase builds safe semantic graph transformations on top of the native
Blueprint node palette. It lets an LLM create a palette node already connected
to a stable pin, insert a palette node between two pins, and replace an
existing node while preserving compatible connections and defaults.

Unreal remains the compatibility authority. The MCP does not invent node
classes, guess an action without presenting candidates, or maintain a parallel
catalog of K2 semantics. Every mutation uses stable Blueprint 2.0 identifiers,
opaque context-bound capabilities, native K2 schema checks, one editor
transaction, and exact rollback verification.

With no unrelated catalog changes, this phase adds five Blueprint actions. The
public totals move from 294 to 299 actions, from 52 to 57 Blueprint actions,
and remain at 22 domains. The exhaustive JSON live sweep moves from 293 to 298
action pairs because `vision.capture_viewport` remains the sole typed-image
exception.

## Goals

- Reduce the multi-call and error-prone work required to spawn and wire a
  palette node.
- Suggest actions that can bridge two stable pins, including actions supplied
  by enabled plugins.
- Insert a selected action into an existing connection without leaving a
  partially rewired graph.
- Preview node replacement without mutation and make every lost connection or
  default explicit.
- Replace a node while preserving compatible links, defaults, position, and
  comment metadata.
- Reject stale or ambiguous semantic edits before destructive mutation.
- Reuse palette search, stable IDs, canonical types/defaults, snapshots,
  transactions, workflow undo, and result envelopes.
- Preserve explicit compile and save behavior.

## Non-goals

- A high-level branch, loop, array, async, or gameplay graph-builder catalog.
- A generic JSON graph-patch interpreter.
- Automatic selection of one action when multiple candidates remain.
- Silent pin selection when more than one binding pair is valid.
- Arbitrary node-class construction or Python execution.
- Copying unrelated node-class-specific properties between different classes.
- Preserving debugger breakpoints, watches, editor selection, or open tabs.
- Cross-graph connections, graph creation, Blueprint inheritance editing, or
  local-variable authoring.
- Implicit compilation, saving, PIE, project changes, or asset creation.

## Considered approaches

### Atomic palette-backed transformations

This is the selected approach. It combines native palette filtering with
explicit stable pins and binding IDs. Common add, insert, and replace edits
become single transactions while arbitrary plugin-contributed actions remain
available.

The cost is that action and replacement-plan capabilities are editor-session
and graph-context bound. Models must re-run suggestion or preview after the
target graph changes. That is preferable to applying a stale rewire.

### Typed graph-pattern builders

Specialized operations for loops, branches, arrays, delegates, and async nodes
could provide excellent schemas for common gameplay patterns. They would also
create a second node inventory that grows continuously and cannot cover plugin
actions automatically. Typed builders may be added later on top of this phase.

### Generic declarative graph patch

A generic patch document could express add, remove, connect, replace, and
property operations in one request. It would duplicate the workflow planner,
make partial failure harder to explain, and greatly widen the mutation surface.
This phase instead exposes five bounded operations with explicit invariants.

## Architecture

### Native semantic editing adapter

Add `MCPythonHelper_BlueprintSemantic.cpp` for two-pin filtering, connected
spawn, insertion, replacement preview, and replacement apply. It depends on
the existing palette adapter for action enumeration and invocation, the graph
adapter for stable pin resolution and K2 connections, and the diagnostics
adapter for deterministic graph snapshots.

The existing palette context gains an optional secondary pin ID. Existing
search and single-pin tokens retain their current behavior. An action returned
for a two-pin request is bound to both pins and cannot be replayed through a
plain graph search, a different pin pair, or `add_blueprint_action_node`.

`MCPythonBlueprint2Internal.h/.cpp` owns a bounded replacement-plan registry.
Plans use the same 30-minute idle lifetime as palette records and a maximum of
1,024 records. Least-recently-used records are evicted first. The registry is
cleared with the editor session and stores no raw pointer as public identity.

### Python and MCP layers

`blueprint_actions.py` adds five thin wrappers that load the Blueprint, encode
JSON, and call the C++ helper. `blueprint2_action_specs.py` defines closed input
and output schemas, safety metadata, effects, asset-path parameters, and
workflow undo support. Catalog and registry files remain generated artifacts.

No semantic filtering, pin mapping, connection mutation, or rollback logic is
implemented in Python.

## Stable contexts and identities

The phase reuses:

- `graph:<guid>`, `node:<guid>`, and `pin:<guid>` stable targets;
- `action:<opaque>` palette actions;
- `binding:<opaque>` action-template pins;
- `palette-cursor:<opaque>` bounded result cursors.

Replacement preview adds `replacement-plan:<opaque>`. A plan is bound to:

- editor session;
- Blueprint asset and K2 graph;
- target node ID and its deterministic node snapshot digest;
- selected action ID, action result digest, and requested dynamic bindings;
- explicit and inferred old-pin-to-new-binding mapping;
- proposed position, retained defaults, retained connections, and allowed-loss
  policy;
- relevant action-database result digest.

Malformed, unknown, or tampered capabilities use `INVALID_INPUT`. A capability
that was once valid but no longer matches session, graph, node, pins, palette,
or snapshot state uses `PRECONDITION_FAILED` and instructs the caller to repeat
suggestion or preview.

## Public actions

### `blueprint.suggest_blueprint_nodes_for_connection`

Returns native palette actions that can bridge an output pin to an input pin.

Required inputs:

- `asset_path`;
- `graph_id`;
- `source_pin_id`: a non-exec or exec output pin;
- `target_pin_id`: a non-exec or exec input pin in the same graph.

Optional inputs reuse palette-search semantics:

- `query`;
- `filters.action_kinds`, `filters.categories`, `filters.owner_paths`, and
  `filters.pure_only`;
- `allow_conversion`, default `false`;
- `cursor`;
- `limit`, default 50 and maximum 200.

The pins do not need to be connected. Their exec/data category must match. For
each native action, Unreal invokes the same action filter used by the editor,
then evaluates its template pins. A result is retained only when at least one
ordered pair can accept `source -> action input` and `action output -> target`
under the K2 schema. By default both edges must be direct. When
`allow_conversion=true`, pairs requiring a K2 automatic conversion node are
retained and marked explicitly. Wildcard pins are retained only when their
direction and schema permit both sides; the actual spawned node is revalidated
before mutation. Conversion policy is part of the action and cursor context.

Each item contains normal palette metadata plus `binding_pairs`. Each pair has
an `input_binding_id`, `output_binding_id`, canonical types, connection
responses, whether a conversion may be inserted, and a deterministic rank.
The result never silently selects a pair. Ordering is deterministic and pages
have no gaps or duplicates.

### `blueprint.add_blueprint_connected_action_node`

Spawns one action returned by `suggest_blueprint_nodes_for_pin` and connects it
to that source pin in one transaction.

Inputs:

- `asset_path`, `graph_id`, and `pin_id`;
- the pin-bound `action_id`;
- `connection_binding_id`, explicitly selecting the new node pin;
- `position`;
- optional `allow_conversion`, default `false`;
- optional dynamic `bindings` already supported by palette spawning.

The operation invokes the action, resolves the actual spawned binding, and
uses the graph schema to connect the existing pin and new pin in the only
direction allowed by their types. It returns the new node/pin IDs and the exact
connection result. An automatic conversion node is rejected unless
`allow_conversion=true`; when allowed, every auxiliary node and edge is
returned. A failed invocation, missing binding, or rejected connection
restores the pre-operation snapshot.

### `blueprint.insert_blueprint_action_node`

Replaces one existing direct connection with a selected palette action.

Inputs:

- `asset_path`, `graph_id`, `source_pin_id`, and `target_pin_id`;
- a two-pin-bound `action_id` returned by
  `suggest_blueprint_nodes_for_connection`;
- explicit `input_binding_id` and `output_binding_id` from one returned pair;
- `position`;
- optional dynamic `bindings`.

The source and target must still be directly linked to each other. Other legal
links on either pin do not invalidate the request; the operation removes only
the named source-to-target edge. The conversion policy stored in the two-pin
action capability cannot be widened at mutation time. The adapter spawns the
actual node, validates both selected actual pins, checks both proposed native
connections, removes the original edge, creates the two new edges, and
verifies the final local topology. Any permitted automatic conversion nodes
are returned explicitly. All steps run inside one transaction. Any failure
restores the exact prior graph snapshot.

### `blueprint.preview_blueprint_action_replacement`

Builds a read-only replacement proposal for one existing node.

Inputs:

- `asset_path`, `graph_id`, and `node_id`;
- a graph-bound `action_id` from palette search;
- optional dynamic `bindings`;
- optional `pin_mapping` from old stable pin IDs to new action binding IDs;
- optional `allow_conversion`, default `false`;
- optional `allow_loss`, default `false`.

The proposed mapping uses explicit entries first. Remaining pins are mapped
only when one candidate is uniquely best by direction, exec/data category,
canonical type compatibility, normalized pin name, and container/reference
qualifiers. A tie remains unmapped rather than being guessed.

The response contains:

- `replacement_plan_id`;
- selected action and target-node summaries;
- retained connections and defaults;
- inferred and explicit mappings with reasons;
- unmapped connections, writable defaults, and unsupported metadata;
- `loss_count`, warnings, and whether the plan is currently applicable.

Preview never mutates, compiles, or saves. With `allow_loss=false`, any
unmapped connection or non-empty writable input default makes the plan
non-applicable. Node-class-specific properties that cannot be expressed as pin
defaults are reported as unsupported metadata and are not copied.

### `blueprint.replace_blueprint_node_with_action`

Applies an unchanged replacement preview.

Inputs:

- `asset_path`, `graph_id`, and `replacement_plan_id`;
- optional `allow_loss`, which must equal the value used for preview.

Before mutation, the adapter verifies the target-node snapshot, graph context,
action record, mappings, and loss policy. It invokes the replacement action at
the old node position, resolves actual binding pins, and revalidates every
planned connection and default. It preserves:

- all mapped incoming and outgoing connections;
- all mapped writable input defaults;
- node position;
- comment text, comment-bubble visibility, and enabled state when the
  replacement node supports the corresponding property.

It then removes the old node and verifies the planned local topology. The
default path refuses any lost connection or non-empty writable default.
`allow_loss=true` is a high-risk, explicitly confirmed operation and returns
every dropped item in `warnings` and `changes`.

## Transaction and rollback behavior

Mutation actions capture a deterministic graph snapshot before beginning.
They register the Blueprint, graph, affected nodes, pins, and linked nodes with
one `FMutationScope`. Native palette invocation and K2 connection calls occur
inside that scope.

Preflight uses template pins to reject obvious incompatibility. Template
success is not sufficient: actual spawned pins are resolved and validated
again before an existing connection or node is removed.

On failure, the operation cancels or reverses its transaction and compares a
new snapshot to the original. Exact restoration returns the original failure.
Residual differences return `ROLLBACK_FAILED` with the graph diff and stable
affected IDs. Successful mutations report `transaction_recorded=true`, an undo
token when called through workflow, `saved=false`, and suggested next actions
for compile, health, snapshot, or undo.

## Error contract

- `INVALID_INPUT`: malformed IDs, direction mismatch, ambiguous or invalid
  binding selection, invalid explicit mapping, or incompatible defaults;
- `PRECONDITION_FAILED`: stale action/cursor/plan, changed graph or node,
  missing direct edge, or mismatched context;
- `CONFLICT`: a replacement would lose data while `allow_loss=false`, or a
  target topology has changed incompatibly;
- `UE_VERSION_UNSUPPORTED`: the active Blueprint/graph schema or engine
  version cannot expose required palette/template capabilities;
- `OPERATION_FAILED`: native action invocation or K2 connection failure with
  action, graph, and stable pin context;
- `TRANSACTION_FAILED`: editor transaction could not begin or commit;
- `ROLLBACK_FAILED`: exact pre-operation topology could not be restored.

Failures do not expose pointers or raw Python tracebacks. Retryable failures
include a concrete hint to re-inspect, re-suggest, or re-preview.

## LLM workflows

Connected spawn:

1. Inspect a graph and obtain a stable pin ID.
2. Request pin-compatible suggestions.
3. Select an action and explicit connection binding.
4. Call `add_blueprint_connected_action_node`.
5. Inspect or snapshot, compile, run health, and save only when wanted.

Insertion:

1. Inspect an existing source-to-target connection.
2. Request two-pin suggestions.
3. Select an action and explicit input/output binding pair.
4. Call `insert_blueprint_action_node`.
5. Verify the returned topology and explicitly compile/save when wanted.

Replacement:

1. Inspect the target node, pins, links, defaults, and current snapshot.
2. Search the native palette and select an action.
3. Preview replacement, adding explicit pin mappings when inference is
   ambiguous.
4. Review all retained and lost items.
5. Apply the opaque replacement plan.
6. Diff, compile, run health, and save only when wanted.

## Verification

### Native C++ tests

- deterministic two-pin filtering, ordering, pagination, and binding pairs;
- exec, scalar, struct, object, container, wildcard, and conversion cases;
- cross-graph, reversed-direction, tampered, stale, and changed-pin rejection;
- connected spawn returns the actual node, pin, and edge IDs;
- insertion replaces exactly one direct edge with two planned edges;
- template/actual pin-shape mismatch rejects without residual mutation;
- replacement mapping prefers explicit, then unique deterministic mappings;
- ambiguous mappings remain unmapped;
- strict replacement refuses connection/default loss;
- explicitly lossy replacement reports every dropped item;
- injected invocation, connection, removal, and rollback failures;
- exact snapshot restoration after every failed mutation;
- 12/12 add, insert, replace, and undo stress cycles.

### Offline Python and contract tests

- five closed input/output JSON schemas and representative validation cases;
- thin wrappers forward every field and preserve structured native errors;
- action IDs, binding IDs, cursors, and replacement plans remain opaque;
- safety metadata classifies read-only suggestion/preview separately from
  transactional mutations and high-risk lossy replacement;
- workflow planning and undo coverage for all three mutations;
- generated catalog/registry totals: 299 actions, 57 Blueprint actions, 22
  domains, and 298 JSON live-sweep pairs;
- source guards prohibit implicit compile, save, PIE, Python escape, and raw
  node-class construction.

### In-editor and live MCP tests

- connected spawn for exec and typed data pins;
- insertion for exec flow and data flow, including an available
  plugin-contributed action;
- replacement preserves actual links, defaults, position, and comment;
- strict and explicitly lossy replacement behavior;
- stable returned IDs match filtered inspection and graph snapshots;
- successful explicit compile and health checks;
- workflow apply/undo restores the exact pre-operation snapshot;
- mutation responses never report implicit save;
- complete live MCP E2E leaves no `/Game/__MCPTests` assets and the editor
  remains reachable.

The release gate remains the full offline suite, deterministic catalog
generation, UE 5.7 build, native Blueprint2 automation, full in-editor
acceptance, workflow stress, and complete live MCP E2E. UE 5.6 and UE 5.8
results are reported only when those engines and sources are available.

## Delivery boundary

This phase is complete when the five public actions are catalogued,
documented, verified through real UE 5.7 editor and MCP sessions, and pushed to
the project branch with all release gates green. Typed pattern builders,
specialized Blueprint-subclass semantics, local variables, inherited function
overrides, and debugger editing require separate designs.
