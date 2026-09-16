# Subcircuit model and flattening

The project catalog stores `Definition` objects with a shared `Schematic` body, public ports and numeric parameter bindings. An `Instance` references a definition UUID, owns its geometry and may override public parameter defaults. Definitions can contain other instances. The graph and editor model remain independent of Qt.

## Identity and connections

Object UUIDs are local to a schematic. Expanded UUIDs derive from the complete sequence of instance UUIDs and the local object UUID. Names, positions and definition UUIDs are not part of that key. Renaming, moving or detaching an instance therefore preserves its result identities. The compiler retains an origin map; compilation and solver diagnostics carry the original object and instance path.

A public port aliases an existing internal terminal, including another instance's public port. Its declared domain and direction must match. Electrical ports remain conserving; directed gate and measurement ports preserve their direction. Flattening rewrites endpoints and recorded gate targets before the existing connectivity and generic compilation stages. It adds no electrical elements or stabilization.

Public numeric parameters bind to value/initial fields, source waveform offset/frequency/phase/delay/duty, constant PWM frequency/duty/delay, or another instance's public parameter. Unknown and duplicate overrides are rejected. Symbolic parameter expressions are not implemented by this step.

The complete catalog is checked for missing definitions, invalid local UUIDs, incompatible ports and recursion. Expansion is limited to 64 levels and one million objects and reports a diagnostic at the limit.

## Editor transactions

`Document` provides grouping from selection, adding an instance, editing a shared definition, deep detach and replacement with editable atoms. Boundary wires become public ports when grouping. Detach copies reachable definitions and retains local object UUIDs. Each operation is one undoable transaction. Clipboard imports preserve conflicting definitions by assigning a separate catalog identity.

The desktop uses the same transactions for grouping, insertion, detach and expansion. Breadcrumbs and the hierarchy tree navigate between levels. Opening internals initially shows a read-only shared definition; Edit definition enables changes affecting all linked instances. Public ports and numeric parameter bindings are editable in a dialog. Instance parameter overrides are shown in the Inspector. Run, save and autosave always operate on the complete root project, including when an internal level is open. Undo/redo restores the corresponding navigation level.

Grouping inside a definition preserves its enclosing public ports and parameter bindings. Expanded UUIDs compose from the leaf outward, so replacing a nested instance with atoms preserves result identities. Recording subscriptions are remapped for all linked instances when grouping changes object paths.

Graph windows use expanded plot UUIDs and remain open across level changes. Root-level `ViewOptions` can override a particular expanded plot, including its time viewport and cursors, without changing a shared definition. Definition settings remain defaults. Grouping, copying and expansion remap plot/channel identities together; unnamed electrical nets are resolved through terminals rather than by prefixing the old net UUID. Deleting an instance removes its overrides; undo restores them. Public graph inputs retain their electrical/gate tap behavior through nested aliases.

The public parameter dialog derives numeric bindings and display scales from component property configurations, including constant PWM. Nested instance parameters inherit primitive editor constraints. The hierarchy tree rebuilds only after structural/name changes and preserves collapsed branches; text editing suppresses hierarchy keyboard shortcuts.

## Schema 7

The loader migrates schema 1–6 to 7. Existing flat records retain their meaning. New `instance` records include a definition UUID and parameter overrides. Each `definition` record contains public-port/parameter metadata, a `body` using the same project record parser, and `end_definition`. Catalog definitions cannot be nested textually; nested instances reference entries in the project catalog. `x-view` optionally ends with a viewport override flag, begin/end and A/B cursor times; records without this tail retain their existing defaults. A view target can be a local plot or an expanded plot in a nested instance. Unknown `x-` extensions in definition bodies survive save/load. No external files are needed to reconstruct a project.

## Verification

The hierarchy test compares two parameterized RC instances with the analytical transient, checks independent initial states, nested source mapping and order independence, then exercises serialization, cycles, missing references, port mismatches, addressed diagnostics, deep detach, shared edits, expansion, grouping, clipboard conflicts and undo/redo. Public gate fanout and external graph taps are checked separately. Desktop tests exercise grouping, internal navigation, shared editing, simulation/save from an internal level, detach/expand, the public interface dialog and instance overrides. The complete Windows Release suite passed 14/14 groups on 16 September 2026 (5.55 s). The two new desktop scenarios also passed using the native Windows platform at 100% and 200% DPI; captured root-level windows were visually checked.
