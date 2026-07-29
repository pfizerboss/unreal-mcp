# Blueprint Node Palette MCP Design

**Date:** 2026-07-30
**Status:** Approved in conversation; awaiting written-spec review
**Repository:** `pfizerboss/unreal-mcp`

## Summary

This phase adds a palette-first node discovery and spawning surface to the
Universal Blueprint 2.0 API. An LLM will be able to ask Unreal Engine which
nodes are valid in a particular Blueprint graph, inspect a selected action,
spawn it through Unreal's native node-spawner machinery, and request actions
compatible with an existing pin.

The palette is the source of truth. The MCP will not maintain a parallel list
of node classes or accept arbitrary class names as a fallback. This allows the
surface to cover built-in, reflected, macro, event, latent, asynchronous, and
plugin-defined node families when Unreal exposes them for the current graph.

## Goals

- Let an LLM discover valid Blueprint nodes without knowing Unreal class names.
- Respect the active Blueprint, graph schema, graph type, and pin context.
- Return bounded, deterministic, paginated search results.
- Expose enough metadata for an LLM to choose between similar actions.
- Spawn the selected action through `UBlueprintNodeSpawner::Invoke`.
- Return the stable node and pin identifiers used by Blueprint 2.0.
- Reject stale, tampered, or context-mismatched action references before a
  mutation begins.
- Reuse existing Blueprint transactions, workflow undo, result envelopes, and
  explicit compile and save operations.

## Non-goals

- A base-game, gameplay-foundation, or one-click project generator.
- A hard-coded catalog that attempts to mirror the Unreal palette.
- Arbitrary Python execution or arbitrary node-class construction.
- Automatic wiring from a suggested pin in this phase.
- Automatic Blueprint compilation, asset saving, PIE, or project changes.
- Bypassing Unreal's graph compatibility filters.
- Exposing internal, template-only, deprecated, or otherwise unsafe actions.

## Considered approaches

### Palette-first discovery and spawning

This is the selected approach. It uses `FBlueprintActionDatabase` as the
inventory, `FBlueprintActionFilter` for graph and pin compatibility, and
`UBlueprintNodeSpawner` for metadata and invocation. It provides broad
coverage, including plugin-contributed nodes, while preserving Unreal's own
compatibility rules.

The cost is that action references are session-scoped and must be refreshed
when the action database or target graph changes. That is preferable to
accepting stale or invented node descriptions.

### Typed graph semantics

This approach would add more specialized commands such as loop, branch, array,
delegate, and async builders. It can provide excellent schemas for common
patterns but grows a second abstraction over K2 and cannot automatically cover
plugin nodes. It remains a possible later layer built on top of palette-first
discovery.

### Specialized Blueprint subclasses

This approach would prioritize dedicated surfaces for Animation, Widget, and
other Blueprint subclasses. Those domains still need specialized operations,
but they do not solve general node discovery. Palette-first support provides
the common graph-level foundation first.

## Unreal Engine integration

The implementation uses the editor-only BlueprintGraph APIs already linked by
the plugin:

- `FBlueprintActionDatabase::GetAllActions()` supplies node spawners.
- `FBlueprintActionFilter::IsFiltered(...)` removes actions incompatible with
  the target Blueprint, graph, schema, and optional source pin.
- `UBlueprintNodeSpawner::PrimeDefaultUiSpec(...)` and `GetUiSpec(...)` supply
  display metadata.
- `UBlueprintNodeSpawner::GetSpawnerSignature()` supplies a native action
  identity component.
- `UBlueprintNodeSpawner::Invoke(...)` creates the selected node.

UE 5.7 local headers are the authoritative API reference for this design
because current online documentation lookup was unavailable. Version-specific
differences must be isolated behind existing compatibility guards. UE 5.6 and
5.8 must remain reported as unverified until those engines are available for
real build and editor gates.

## Architecture

### C++ palette adapter

`MCPythonHelper_BlueprintPalette.cpp` owns the four reflected entry points and
the Unreal-specific enumeration, filtering, metadata extraction, and spawning
logic. It remains focused on palette behavior rather than absorbing unrelated
Blueprint graph mutations.

### Shared Blueprint 2.0 infrastructure

`MCPythonBlueprint2Internal.h/.cpp` owns reusable palette context records,
opaque action and cursor encoding, digest validation, stable graph/node/pin
resolution, transaction integration, and structured failure helpers.

Session-scoped state stores only the minimum information needed to resolve a
previous result safely. The native action database remains the source of
truth; cached records never become an independent catalog.

### Python and MCP layers

`blueprint_actions.py` contains thin public wrappers. Strict input and output
schemas live in `blueprint2_action_specs.py`. The generated registry and
catalog expose all four operations in the existing `blueprint` domain with
their effects, undo behavior, version support, and capability metadata.

Read operations are side-effect free. The spawn operation uses the existing
Blueprint editor mutation and workflow machinery rather than adding a second
batch or transaction system.

## Public actions

### `blueprint.search_blueprint_node_actions`

Required inputs:

- `asset_path`: target Blueprint asset path;
- `graph_id`: stable Blueprint 2.0 graph identifier.

Optional inputs:

- `query`: case-insensitive title, category, keyword, owner, and member search;
- `filters.action_kinds`: any of `function`, `event`, `variable`, `macro`,
  `delegate`, `cast`, `async`, `flow_control`, `operator`, `struct`, or
  `other`;
- `filters.categories`: case-insensitive category prefixes;
- `filters.owner_paths`: exact Unreal owner object paths;
- `filters.pure_only`: when true, retain only actions positively identified as
  pure rather than treating unknown actions as pure;
- `cursor`: opaque continuation cursor;
- `limit`: page size, default 50 and maximum 200.

The result contains compact action cards with `action_id`, title, category,
keywords, action kind, node class path when safely available, owner/member
identity when applicable, compatibility summary, and binding requirements. It
also contains `next_cursor` when another page is available.

Results first pass Unreal's native context filter. Text matching then produces
a normalized score. Ordering is deterministic by descending score followed by
normalized category, title, native spawner signature, and action ID. Empty
queries return the valid palette in the same deterministic order.

### `blueprint.describe_blueprint_node_action`

The action accepts an `action_id` returned by search or pin suggestion. It
returns the full UI metadata, node class, owner/member paths, action kind,
bindings, compatibility facts, and known restrictions.

Template or expected pins are returned only when the spawner exposes them
without invoking the action or mutating a graph. Otherwise the response
explicitly reports that pin preview is unavailable. Describe never invokes a
spawner to obtain speculative metadata.

### `blueprint.add_blueprint_action_node`

Required inputs are `asset_path`, `graph_id`, `action_id`, and graph position.
Optional bindings must use opaque `binding:<sha1>` references returned by
search or describe; arbitrary object paths are not accepted as binding
substitutes. A binding reference is tied to its action, editor session, target
Blueprint and graph, binding object identity, and expected binding class.

Preflight re-resolves the Blueprint and graph, validates every context digest,
finds the matching native spawner, re-applies the graph filter, and resolves
bindings. Only then does the operation open the existing Blueprint transaction
and call `UBlueprintNodeSpawner::Invoke`.

Success returns the stable node ID, the node's stable pin records, the action
ID used, the graph ID, and any non-fatal warnings. The operation creates
exactly one primary palette node. Any auxiliary nodes created by Unreal are
reported explicitly. The operation marks the Blueprint modified according to
existing Blueprint 2.0 rules but does not compile or save it.

If invocation fails or produces an invalid result, the transaction is
cancelled and no partial success response is returned.

### `blueprint.suggest_blueprint_nodes_for_pin`

Required inputs are `asset_path`, `graph_id`, and a stable `pin_id`. Optional
inputs are `query`, `cursor`, and `limit` with the same semantics as palette
search.

The implementation resolves the pin and adds it to the native action-filter
context. Results use the same card, action-ID, ordering, and pagination format
as general search. An action returned here is compatible according to Unreal's
context filtering, but the API does not claim that every possible set of
bindings or subsequent connection will succeed.

This action does not create or connect a node. The caller uses
`add_blueprint_action_node` and the existing stable-pin connection operations.

## Opaque identities and pagination

Public action IDs have the form `action:<sha1>`. The digest and its
session-scoped record bind the action to:

- the editor session;
- asset path;
- graph GUID and graph schema;
- search or suggestion context, including a source pin when present;
- query and filter digest;
- native spawner signature and owner identity;
- a digest of the relevant action-database result set.

Action IDs are capabilities, not permanent Blueprint identifiers. Restarting
the editor, changing the target context, or changing the relevant palette can
require a new search.

Cursors bind to the same context plus the complete deterministic sort key of
the last returned item and the result-set digest. A cursor cannot be reused
with different request parameters. Pagination never relies on an unsorted
container index.

Hash verification is followed by record lookup and native re-resolution. A
matching digest alone never authorizes a spawn. No public fallback accepts a
node class string, display title, array index, or raw pointer.

## Safety and failure behavior

The native graph filter is mandatory for search, describe revalidation, and
spawn preflight. Additional policy excludes internal, template-only,
deprecated, hidden, abstract, invalid, or explicitly unsafe actions. The
public API has no flag that bypasses these exclusions.

Failures use the existing structured result envelope:

- malformed, unknown, tampered, cross-context tokens and invalid bindings use
  `INVALID_INPUT`;
- once-valid tokens invalidated by session, graph, Blueprint, pin, or palette
  changes use `PRECONDITION_FAILED` and instruct the caller to search again;
- unsupported graph schemas or engine-specific capabilities use
  `UE_VERSION_UNSUPPORTED` with capability details;
- a native invocation failure uses the existing operation-failure code and
  includes the action, graph, and native diagnostic context without exposing
  pointers.

Every rejection before invocation is mutation-free. A successful spawn has no
implicit compile, save, PIE, asset creation, or project-setting side effects.
Existing workflow apply and guarded undo must restore the pre-operation graph
snapshot.

## LLM workflow

The intended sequence is:

1. Inspect the Blueprint and obtain a stable graph or pin ID.
2. Search the graph palette or request pin-compatible suggestions.
3. Describe ambiguous candidates when additional metadata is needed.
4. Spawn the selected opaque action at a specified position.
5. Connect returned stable pins with existing Blueprint 2.0 actions.
6. Explicitly compile and run health checks.
7. Explicitly save only when the caller wants persistence.

Search results remain compact so a model can broaden or narrow its query
without receiving the complete Unreal palette in one response.

## Verification

### Native C++ tests

- deterministic search and stable ordering;
- pagination without gaps or duplicates;
- different graph, Blueprint, and pin contexts produce correctly filtered
  results;
- action and cursor tampering is rejected;
- stale session, graph, pin, and palette records fail before mutation;
- successful invocation returns node and pin IDs matching native objects;
- failed invocation leaves the graph unchanged.

### Offline Python and contract tests

- strict input and output schema validation for all four actions;
- thin-wrapper argument forwarding and structured error preservation;
- generated registry, catalog, safety, and capability accounting;
- default and maximum page limits;
- no implicit compile or save calls.

With no unrelated action changes, the expected public totals move from 290 to
294 actions, from 48 to 52 Blueprint actions, and remain at 22 domains.

### In-editor and live MCP tests

- search, describe, and spawn common function, event, macro, cast, reflected,
  async/latent, and available plugin-defined node families;
- pin suggestions differ appropriately by type, direction, and graph schema;
- stable IDs match `inspect_blueprint` output;
- returned pins can be connected through existing actions;
- explicit compile and Blueprint health checks succeed for valid fixtures;
- workflow apply and undo restore the exact pre-operation snapshot;
- assets remain unsaved unless an explicit save action is called.

The complete existing offline, native UE 5.7 build, native automation,
in-editor automation, workflow stress, and live MCP E2E suites remain release
gates. UE 5.6 and UE 5.8 results must not be claimed without running those
engines.

## Delivery boundary

This phase is complete when all four actions are catalogued, documented,
covered by the tests above, and verified through a real UE 5.7 editor and MCP
session. Further typed graph semantics and specialized Blueprint-subclass
features require separate designs after this foundation is complete.
