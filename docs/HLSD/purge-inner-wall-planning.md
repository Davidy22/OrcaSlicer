# Purge into inner walls

`flush_into_inner_walls` is an object/print boolean, disabled by default. It is
available under Multimaterial → Flush options and in the object context menu,
independently of the prime tower. `flush_inner_walls_max_extra_loops` limits
additional walls, including lower-layer support walls. Zero means automatic,
bounded by the region's geometric inset limit.

## Demand and geometry

Normal walls, fills and supports are generated before demand is collected from
`ToolOrdering`. The flush matrix and multiplier are read through
`Print::purge_volume_for_transition()`, also used by the tower-enabled allocator.
Dedicated purge objects are accounted for before requesting additional walls.
Sequential printing plans each object's interior transitions separately.
Transition predecessors come from per-nozzle resident filaments (see
Multi-head below), and mixed-color sublayers contribute one event per physical
component band (see Mixed-color sublayers below).

The geometry planner generates and measures additional insets without changing
`wall_loops` or `sparse_infill_density`. Classic and Arachne receive the local
`LayerRegion::purge_inner_wall_plan`. Their normal inset machinery updates the
fill boundaries, preventing overlap between added walls and infill. Regions
with nonzero local plans are not merged for perimeter generation. Each trial
rebuilds its entire layer, removing any previous merged island before creating
separate region geometry. With zero extra walls the original region merging
remains unchanged.

Purge capacity excludes external walls, near-visible inset one, open paths,
partly overhanging loops, deferred loops, soluble/support-material walls, and
exposed top/bottom geometry. Candidate footprints must be covered by the
configured roof layers/thickness and the lower model slice. Transparent walls
can still reveal mixed interior colors; the setting's tooltip warns about this.

Support counts propagate down through intersecting regions, including material
boundaries. The baseline retains at least the same extra count below (stricter
than X / X−1) and accounts for different normal/alternating wall counts. A
bottom-up verification checks only the added walls' extrusion centerlines
against actual lower inner-wall footprints. Exterior walls are neither purge
targets nor assumed foundations; their surface effects cannot disqualify an
unrelated interior purge wall. It does not assume that a solid slice polygon contains
printed material. Unsupported counts are reduced and alternative regions or
objects are tried before treating demand as unabsorbable. Capacity supported
only by sparse infill, same-layer bridging, or raft geometry is conservatively
excluded. Abandoned support-only trials are removed and support is propagated
again from retained demand layers.

Whole loops are indivisible: one large loop cannot absorb two separate
transitions. The measured-capacity verification simulates this allocation, not
just aggregate layer volume. Planning uses conservative flow factors; final
allocation applies the incoming filament and configured inner-wall flow ratios.
Added purge/support loops and ordinary inner loops actually needed as their
foundations use neither seam clipping nor scarfing and are protected from late
arc fitting, simplification and Z contouring. Unrelated walls retain their usual
path optimization and surface effects. Foundation metadata never authorizes a
material override on an ordinary wall.

Fuzzy skin and Z contouring remain compatible, including All Walls fuzzy skin.
Only additional purge/support insets bypass fuzzy skin; ordinary walls keep the
configured texture. The boundary is computed per surface in both generators,
including ordinary extra and alternating walls. Randomized inner foundations
are checked against their actual generated footprints, and failed trial
rollbacks are reverified. Z contouring still processes external walls, other
unrelated perimeters and surface fills, but does not deform the verified inner
purge/support stack.

The geometry rebuild invalidates dependent fill, support, contouring,
simplification, skirt and export state without invoking the worker's own cancel
callback. Cancellation checks remain active during planning. Changing either
new setting invalidates perimeters and all dependent slicing steps.

## Override isolation and emission

`ExtrusionLoop::generated_for_purge` records provenance; `purge_safe` records
successful eligibility verification. Neither an ordinary island collection nor
an unverified added loop is an override target. Verified loops are isolated in
singleton collections, retaining the existing copy-specific override keys and
extrusion roles. Normal geometry keeps its configured ordering.

Allocation prefers dedicated purge objects, then verified added inner walls
(sorted innermost first), then existing infill/support options. Soluble and
support-material transition exclusions remain in effect. The regular tool
ordering and preview therefore see the incoming material's color without a new
extrusion role. Added-wall capacity and the credits for dedicated objects,
infill, support body and support interface all use one shared effective-volume
model: nominal entity volume times the incoming filament flow ratio, the
region print flow ratio, the role flow ratio (when "other flow ratios" are
enabled), and — for mixed slots — the target component's sub-layer band
fraction. Support-body and support-interface overrides are keyed by
(object, copy), so sequential copies allocate their own support purge.

G-code emission has four passes when this feature has overrides: dedicated
purge objects, added purge walls, other purge overrides, and normal geometry. The added-wall pass precedes
both infill-first and outer-first normal wall sequences. Sequential allocation
is restricted to the current object copy. A filament change performed while
starting that copy carries its previous filament into the first layer's
allocator rather than losing the transition.

## Overflow and accounting

With a tower, remaining demand uses the existing tower/printer purge path,
including mandatory minimum tower priming. Without a tower, unabsorbed demand is
explicitly recorded as reduced purge. A critical slicing/export warning reports
requested and absorbed volume, reduced volume, maximum shortfall, affected
layers and transition pairs, and strongly recommends a prime tower.

Towerless tool changes set the normal flush-volume/length placeholders to zero:
the effective purge is emitted by the assigned object paths, not duplicated in
the tool-change macro. Hard-coded extrusion commands in custom printer macros
are not rewritten; mandatory loading, cutting and other firmware-controlled
extrusions remain the responsibility of the printer profile.

The preview displays requested volume, inner-wall absorption, other-object /
infill / support absorption, tower fallback and reduced volume. G-code comments
include the same accounting. Export reconciles allocations with the actual
starting filament on each layer and updates records instead of double-counting
preliminary slicing allocations.

The supported configuration excludes spiral vase: a single continuous outer
wall leaves no hidden inner wall to purge into, so that combination is a
truthful validation conflict. Multi-head/multi-nozzle printers and mixed-color
sublayers are supported (below).

## Multi-head / multi-nozzle

A transition's predecessor is the material resident in the destination
physical nozzle, never the globally active filament. Per-nozzle occupancy is
tracked with `MultiNozzleUtils::NozzleStatusRecorder`, keyed by nozzle slot;
nozzle and extruder resolution reuses `LayeredNozzleGroupResult` (the
`ToolOrdering`'s own layered result first, then the print-wide result, then the
static filament map). Switching to an already-loaded head creates no
contamination purge; a first use into an empty nozzle is loading/priming, not
contamination. The flush matrix and multiplier are indexed by the destination
extruder (`Print::purge_volume_for_transition(old, new, extruder_id)`), matching
`GCode::set_extruder` and the tower path. Type2 towers prime every extruder up
front; demand collection seeds each nozzle with its last-primed filament, which
reproduces the single-nozzle behavior exactly. Sequential export keeps its own
per-nozzle recorder (`Print::m_inner_wall_purge_residents`, reset per export,
re-seeded from the writer's active filament). The transition identity recorded
in the statistics is `(object, copy, z, old filament, new filament, destination
extruder)`, so repeated same-pair events on different nozzles are not merged.

## Mixed-color sublayers

Demand collection iterates the physical components after
`ToolOrdering::resolve_mixed_filaments` expands `LayerTools::extruders`, so every
component band is a transition event with the same per-nozzle predecessor model.
Capacity uses the shared component heights: an entity of a mixed slot is
credited only its target component's sub-layer band
(`total_volume * sub_heights[k] / layer_height * flow factors`), the same
structure the sublayer emitter reads. A nominal loop can serve one transition
per component (one band each), never one component twice; the allocation is
recorded in `WipingExtrusions::mixed_purge_map` keyed by (entity, object, copy)
and component. Mixed-slot entities only serve transitions into their own slot's
components. Grouping keeps mixed-slot entities under the slot even when
allocated, so nothing is emitted twice and no component portion is erased. The
sublayer emitter splits each instance copy's entities per component: the
allocated loops print first (innermost first) in that component's
`; PURGE INNER WALLS` pass at the component's actual sub-Z and flow, before the
component's visible geometry. Towerless component tool changes zero the
macro/chute purge so it is not duplicated. Gradients ramp the band ratios per
layer without splitting the slot; the demand structure is unchanged.
