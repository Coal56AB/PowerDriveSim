# Subcircuit model and flattening

The project catalog stores `Definition` objects with a shared `Schematic` body, public ports and numeric parameter bindings. An `Instance` references a definition UUID, owns its geometry and may override public parameter defaults. Definitions can contain other instances. The graph and editor model remain independent of Qt.

## Identity and connections

Object UUIDs are local to a schematic. Expanded UUIDs derive from the complete sequence of instance UUIDs and the local object UUID. Names, positions and definition UUIDs are not part of that key. Renaming, moving or detaching an instance therefore preserves its result identities. The compiler retains an origin map; compilation and solver diagnostics carry the original object and instance path.

When wires merge named nodes, the net UUID remains the stable UUID representative.
The displayed voltage name independently prefers a nonempty explicit node label at
the shallowest hierarchy level, then the lowest UUID for ties. A root bus label thus
takes precedence over an internal port label. Hierarchy depth comes from the origin
map, so slashes inside user names have no special meaning. Unnamed junctions remain
unnamed during flattening and do not suppress another node's label.

A public port aliases an existing internal terminal, including another instance's public port. Its declared domain and direction must match. Electrical ports remain conserving; directed gate and measurement ports preserve their direction. Flattening rewrites endpoints and recorded gate targets before the existing connectivity and generic compilation stages. It adds no electrical elements or stabilization.

Connection tags retain their original connection name and the exact UUID path of
their containing schematic during flattening. Names are compared exactly and only
within the same domain. A local tag joins matching tags on the same schematic path.
An `ancestors` tag additionally joins matching tags on any strict ancestor path; it
does not directly reach descendants or sibling paths. Sibling instances can still
share a group through an explicit matching tag at their common ancestor. A global
tag joins every matching tag in the model regardless of path. There is no implicit
lexical shadowing: every scope-compatible match is unioned, and conflicting directed
drivers are diagnosed after that union. The `listed` flag controls name completion
only and never changes connectivity; manual entry always remains available.
Several paths from the same gate or signal output to one tag group count as one
driver. A second, distinct output is still diagnosed as a conflicting driver.

Public numeric parameters bind to value/initial fields, inductor parallel resistance, source waveform offset/frequency/phase (radians or the degree editor field)/delay/duty, semiconductor Ron/Roff/Vf, diode charge times/initial charge, thyristor holding current, constant PWM frequency/duty/delay, or another instance's public parameter. A public phase value in degrees is converted to the source's stored radians on expansion. Unknown and duplicate overrides are rejected. Instance overrides may use expressions evaluated from the project's safe initialization program; unresolved names, non-finite results and invalid target fields are diagnosed before compilation.

Schema 28 also allows the default of a public parameter itself to be an
expression in the definition-local initialization scope. Explicit instance
overrides remain numeric and take precedence. Grouping keeps the outer expression
in its original scope and gives the new pass-through parameter a materialized
numeric default; expansion likewise materializes the scope that disappears.

Schema 21 extends the existing `PublicParameter` mask metadata with an optional
group and inclusive minimum/maximum values. The interface editor stores these
constraints with the definition, the instance inspector renders group headings,
and both direct property edits and flattening reject overrides outside the
effective intersection of every range in a nested public-parameter chain and the
atomic property range. Definitions are checked transitively even when they have
no root instance, so an invalid deep default cannot remain dormant in a saved
catalog. Older projects load with an empty group and no additional range; the
bound atomic property still performs its own physical validation.

The complete catalog is checked for missing definitions, invalid local UUIDs, incompatible ports and recursion. Expansion is limited to 64 levels and one million objects and reports a diagnostic at the limit.

## Editor transactions

`Document` provides grouping from selection, adding an instance, editing a shared definition, deep detach and replacement with editable atoms. Boundary wires become public ports when grouping. Detach copies reachable definitions and retains local object UUIDs. Each operation is one undoable transaction. Clipboard imports preserve conflicting definitions by assigning a separate catalog identity.

The desktop uses the same transactions for grouping, insertion, detach and expansion. Breadcrumbs and the hierarchy tree navigate between levels. Opening internals initially shows a read-only shared definition; Edit definition enables changes affecting all linked instances. Public ports and numeric parameter bindings are editable in a dialog. Instance parameter overrides are shown in the Inspector. Run, save and autosave always operate on the complete root project, including when an internal level is open. Browsing levels does not create undo entries or discard redo. Undo/redo reverses an edit and restores the level where that edit was made.

Grouping inside a definition preserves its enclosing public ports and parameter bindings. Expanded UUIDs compose from the leaf outward, so replacing a nested instance with atoms preserves result identities. Recording subscriptions are remapped for all linked instances when grouping changes object paths.

Deletion is dependency-aware across hierarchy levels. Removing an atom or an
instance clears Scope points, active recording channels, experiment channels,
sweep axes and scenario overrides for every affected expanded instance in the
same undoable transaction. An internal object referenced by a public port or
public parameter cannot be deleted until that interface binding is redirected
or removed; the editor returns an addressed diagnostic instead of leaving an
invalid definition.

Graph windows use expanded plot UUIDs and remain open across level changes. Root-level `ViewOptions` can override a particular expanded plot, including its time viewport and cursors, without changing a shared definition. Definition settings remain defaults. Grouping, copying and expansion remap plot/channel identities together; unnamed electrical nets are resolved through terminals rather than by prefixing the old net UUID. Deleting an instance removes its overrides; undo restores them. Public graph inputs retain their electrical/gate tap behavior through nested aliases.

The public parameter dialog derives numeric bindings and display scales from component property configurations, including constant PWM. New bindings follow the same mode-dependent visibility as the Inspector; existing bindings remain selectable if their underlying field is currently hidden. A newly exposed binding starts with the effective value of its target, including nested instance overrides and values resolved from definition-local initialization expressions. The source expressions remain in the project. Nested instance parameters inherit primitive editor constraints. The hierarchy tree rebuilds only after structural/name changes and preserves collapsed branches; text editing suppresses hierarchy keyboard shortcuts.

The same public-interface dialog stores a `DefinitionAppearance`. A definition may
select any symbol from the centralized schematic icon set or keep automatic
library/name detection. It may also embed a raster image; the desktop converts
the selected file to a PNG of at most 512×512 and stores its base64 bytes in the
definition metadata. The embedded image takes precedence over the symbol and is
drawn with fixed aspect ratio, while the block frame and real ports remain normal
schematic geometry. No absolute source path is serialized.

## Schema 7

The loader migrates schema 1–6 to 7. Existing flat records retain their meaning. New `instance` records include a definition UUID and parameter overrides. Each `definition` record contains public-port/parameter metadata, a `body` using the same project record parser, and `end_definition`. Catalog definitions cannot be nested textually; nested instances reference entries in the project catalog. `x-view` optionally ends with a viewport override flag, begin/end and A/B cursor times; records without this tail retain their existing defaults. A view target can be a local plot or an expanded plot in a nested instance. Unknown `x-` extensions in definition bodies survive save/load. No external files are needed to reconstruct a project.

## Verification

The hierarchy test compares two parameterized RC instances with the analytical transient, checks independent initial states, nested source mapping and order independence, then exercises serialization, cycles, missing references, port mismatches, addressed diagnostics, deep detach, shared edits, expansion, grouping, dependency cleanup, clipboard conflicts and undo/redo. Public gate fanout and external graph taps are checked separately. Desktop tests exercise grouping, internal navigation, shared editing, simulation/save from an internal level, detach/expand, the public interface dialog and instance overrides. The complete Windows Release suite passed 14/14 groups on 16 September 2026 (5.55 s). The two new desktop scenarios also passed using the native Windows platform at 100% and 200% DPI; captured root-level windows were visually checked.
