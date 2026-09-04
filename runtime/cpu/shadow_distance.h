// Shadow-distance control (part 93) — push the cascaded-shadow reach out so objects
// are shadowed correctly from farther away (the operator's request: "you have to be
// close to an object for it to be shadowed correctly").
//
// WHY THE FIRST APPROACH FAILED, AND WHAT WORKS. The cascade split distances are the
// data-driven named properties Start_/End_CascadeDist (bound through sub_82375518,
// exactly like FOV). The first version WROTE the scaled values back into that object
// field — and it did nothing on screen, because the game reads those fields ONCE per
// time-of-day update, blends Start->End, and caches the result into seven scattered
// GLOBALS that the cascade builder actually reads. Writing the field after that cache
// is built is ignored (operator-confirmed: 4x looked identical to 1x).
//
// The consumer is `sub_823C1CC8` (the env-lighting time-of-day interpolator; the exact
// analog of the FOV camera getter sub_8246BF48). It is the SOLE writer of the seven
// active-cascade-distance globals, and the cascade builder sub_825A89A8 re-reads them
// every render. So the fix hooks sub_823C1CC8, lets it compute the unscaled blend, then
// multiplies the seven globals by the shadow-distance factor — the game then builds
// wider cascade PROJECTIONS and culls farther, all consistent, the "render and cull
// follow" property that made the FOV substitution correct. Recomputed unscaled on every
// time-of-day update and re-scaled, so it never compounds.
//
// The scale is Settings_ShadowDist() (1.0 = stock, the bit-identical control),
// overridable for measurement by CZ_SHADOW_DIST=<factor>.
#pragma once

// The factor the sub_823C1CC8 hook multiplies the active cascade distances by. Env
// override (CZ_SHADOW_DIST) wins over the persisted setting; 1.0 = stock. Exposed so the
// hook and any diagnostic read the same value.
float ShadowDistFactor();
