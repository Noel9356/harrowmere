# animation_tween module - tween drivers over the scene registry

## What this module provides

Capability `animation` (implementation-bound member `animation.tweens`, published in `animation_capability.hpp`): time-based interpolation ("tweens") for scene objects and point lights.

A tween is a driver, not a parallel representation: it writes the new values through the consumed capabilities every frame - the scene registry stays the one source of truth for objects, the lighting state for lights. The lighting capability is an optional declaration (`optionalCapabilities`): a build without the pbr module still animates objects, and light tweens degrade to a precise refusal; when a light provider IS active, the resolver orders it before this module, so the first-use resolve is a property of the assembly, never scan order. Because object writes flow through the same `setObjectTransform` / `setObjectMaterial` surface a script uses, the renderer's incremental rebuild picks animated objects up with no animation-specific renderer path.

## The contract

```cpp
TweenDescription tween;
tween.channel     = animation_channel::position;
tween.fromPosition = {0.0f, 1.0f, 0.0f};
tween.toPosition   = {0.0f, 1.0f, 2.0f};
tween.durationSeconds = 2.5;
tween.easing      = animation_easing::ease_in_out;
tween.mode        = animation_mode::ping_pong;
const auto handle = animation->animateObject(objectHandle, tween);
```

- **channels**: `position` / `rotation` / `emissive` for scene objects; `lightIntensity` / `lightRadius` for point lights. A channel/target mismatch is refused with `invalidTween` and one precise log line.
- **easing**: `linear`, `ease_in` (t²), `ease_out`, `ease_in_out` (smoothstep). One pure evaluator; new curves are one case there.
- **modes**: `once` (plays, publishes `TweenFinishedEvent`, drops), `loop` (wraps), `ping_pong` (triangle wave - reverses at each end).
- **validation at start**: the target must be live when the tween is created. A tween on a typo'd handle is a programming error the caller hears about, not a tween that silently moves nothing.
- **vanishing targets**: an object removed under an active tween drops the tween silently (the removal published its own event); a point light removal RENUMBERS handles (the lighting capability's contract), which invalidates light tweens - they check the handle every write and drop.

Position tweens preserve the object's rotation; rotation tweens preserve the position; the emissive tween only touches the emissive strength. Material edits never teleport or reorient anything - the same rule the scene registry's `setObjectMaterial` enforces.

## Timing

The module subscribes to the kernel's `EngineFrameEvent` loop heartbeat and advances every active tween by the event's `deltaSeconds` - the one delta the composition root measured for the whole iteration (the tick source is the loop, not the renderer - a headless run tweens exactly like a rendered one, and the module reads no clock of its own: one clock read per frame in the composition root serves the entire gameplay layer, docs/architecture.md, "The frame clock"). Repeating tweens accumulate the frame delta and wrap their curve parameter, so a lagged frame advances the phase once by the delta it actually carried - no instantaneous refire burst, and a tween's position stays a pure function of elapsed time. `startDelaySeconds` defers the first step.

## Consumers

| consumer      | uses                                                    |
| ------------- | ------------------------------------------------------- |
| lua_scripting | `engine.animate_object` and `engine.animate_light` (the Lua bridge) |
| native mods   | the capability directly                                 |

## Configuration

None.

## Files

- `animation_capability.hpp` - the published capability surface
- `animation.hpp` / `animation.cpp` - the module and the tween evaluator
- `module_plugin.cpp` - the static plugin descriptor
- `CMakeLists.txt` - target wiring (`ENGINE_BUILD_MODULE_ANIMATION_TWEEN`)
