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
extrusion role.

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

The supported configuration uses one physical nozzle and ordinary planar
layers. Spiral vase, mixed-color sublayers and multiple physical nozzles are
explicitly rejected rather than silently generating unverified purge geometry.
