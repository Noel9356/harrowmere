# character_customisation

The owner's architecture document as the engine's eleventh module: a
data-driven modular-character service - parts library, connector
compatibility rules, assembly profiles, parameter registry, validated
attachment-tree assemblies, versioned recipes.

## Shape

- `character_assembly.hpp` - the whole data model + assembly engine,
  std-only and header-only (the mod's unit battery compiles it directly;
  this plugin wraps it unchanged). The doc's concepts map 1:1: attachment
  tree (rooted, acyclic, one structural parent), connectors (semantic
  roles, anchors with parameter-dependent offsets), compatibility REGISTRY
  (rule pairs with orientation tolerance + scale ceilings - never species
  conditionals), assembly profiles (role-count contracts), parameters
  (global registry, per-asset bindings), recipes (versioned structs;
  serialization stays with the content owner).
- `character_capability.hpp` - the published surface: installLibrary,
  library, build (under the config knobs).
- Resolve: per-frame `resolveAssembly` produces per-part world transforms
  (two-pass ground solve: the LOWEST rest foot lands on the ground point),
  gait pose swaps and appendage yaw oscillation (tails wag around their
  connector, siblings phase-split; wings idle at 35% amplitude).

## Engine adaptations (the doc vs this renderer)

The renderer's mesh instances carry translate + rotateY + per-axis scale
(rev 34 added scale: SceneObject.scale, the descriptor, the SDF proxies,
and the exact inverse-transpose normal in scene.vert). There is NO
skeletal pipeline. Therefore, documented as deviations, not silent gaps:

| Doc concept | This engine |
| --- | --- |
| Rig fragments + skinning | pose-variant mesh SETS per part (rest/strideA/strideB) |
| Morph targets | parameter-driven axis scales + connector anchor sync |
| Retargeting | deferred (no skeletal animation to retarget) |
| Transition meshes / topology merging | seam metadata recorded; junctions are overlapping components with hidden seams |
| Physics articulation | deferred (per-part collision shapes are plumbed in the data contract) |
| Lua bridge over validated ops | deferred (the lua_scripting module exists; the surface lands when the game needs it) |

## Wiring

The game mod demands `character_customisation` (mod.toml), parses
`data/characters/parts.json` + `data/characters/recipes/*.json` (nlohmann,
the game's data mandate) into the capability, and the bridge's
`character_lab` scenario (the document's first implementation test) builds
the HYBRID (torso+pelvis core, canine head, digitigrade legs, arms, CC0
avian wings, wagging tail) beside its VARIANT (avian head, wings removed,
torso widened, legs lengthened, second tail) - the replace / remove /
proportion / second-appendage operations as data.

## Configuration

`[character.customisation]`: `max_parts_per_assembly` (48; the validation
ceiling and the scene-object budget guard), `strict_parameters` (true =
out-of-range rejects; false clamps).

## Known limits

- Parts are authored anchor-centric (torso base, pelvis top, hip top,
  shoulder top, neck bottom, wing joint, tail base) - the parts.json
  anchors and the generator's conventions must agree.
- The fully-transparent transparency class, LOD management and mesh
  merging are future work; the crowd has not migrated onto the assembly
  path yet (it keeps the rev-28 direct spawn).
- The capture rasterizer's frame cost bounds the lab to a handful of
  accumulated frames; animation is deterministic per time-of-day.
