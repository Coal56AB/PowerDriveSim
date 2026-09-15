# Subcircuit model and flattening

The project catalog stores `Definition` objects with a shared `Schematic` body, public ports and numeric parameter bindings. An `Instance` references a definition UUID, owns its geometry and may override public parameter defaults. Definitions can contain other instances. The graph and editor model remain independent of Qt.

## Identity and connections

Object UUIDs are local to a schematic. Expanded UUIDs derive from the complete sequence of instance UUIDs and the local object UUID. Names, positions and definition UUIDs are not part of that key. Renaming, moving or detaching an instance therefore preserves its result identities. The compiler retains an origin map; compilation and solver diagnostics carry the original object and instance path.

A public port aliases an existing internal terminal, including another instance's public port. Its declared domain and direction must match. Electrical ports remain conserving; directed gate and measurement ports preserve their direction. Flattening rewrites endpoints and recorded gate targets before the existing connectivity and generic compilation stages. It adds no electrical elements or stabilization.

Public numeric parameters bind to value/initial fields, constant PWM frequency/duty/delay, or another instance's public parameter. Unknown and duplicate overrides are rejected. Symbolic parameter expressions are not implemented by this step.

The complete catalog is checked for missing definitions, invalid local UUIDs, incompatible ports and recursion. Expansion is limited to 64 levels and one million objects and reports a diagnostic at the limit.

## Editor transactions

`Document` provides grouping from selection, adding an instance, editing a shared definition, deep detach and replacement with editable atoms. Boundary wires become public ports when grouping. Detach copies reachable definitions and retains local object UUIDs. Each operation is one undoable transaction. Clipboard imports preserve conflicting definitions by assigning a separate catalog identity.

These APIs are the model layer; desktop navigation and public-port editing are the next integration step.

## Schema 7

The loader migrates schema 1–6 to 7. Existing flat records retain their meaning. New `instance` records include a definition UUID and parameter overrides. Each `definition` record contains public-port/parameter metadata, a `body` using the same project record parser, and `end_definition`. Catalog definitions cannot be nested textually; nested instances reference entries in the project catalog. Unknown `x-` extensions in definition bodies survive save/load. No external files are needed to reconstruct a project.

## Verification

The hierarchy test compares two parameterized RC instances with the analytical transient, checks independent initial states, nested source mapping and order independence, then exercises serialization, cycles, missing references, port mismatches, addressed diagnostics, deep detach, shared edits, expansion, grouping, clipboard conflicts and undo/redo. Public gate fanout and external graph taps are checked separately. The complete Windows Release suite passed 14/14 groups on 16 September 2026.
