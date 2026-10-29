# settings_toml module

The run's persisted key-value store: player options, a mod's unlocks, any scalar value a game wants to survive a restart. The module provides the `SettingsCapability` (family `settings`, implementation-bound member `settings.toml`, published in `settings_capability.hpp` next to this module) and requires nothing but logging plus the two file permissions. It is deliberately NOT the persistence module's job: persistence saves the scene registry (the world), settings save the options AROUND the world - and a script mod had no persistent storage at all before this capability, because a mod's `config.toml` is authored defaults, not runtime state.

## The file format

One flat TOML file of scalar rows, hand-editable like every TOML surface in this engine:

```toml
# settings.toml
master_volume = 0.8
fullscreen = true
nickname = "player"
"keybind.forward" = "W"
```

Nested tables load as dotted names, so both spellings are the same stored value: `[audio] volume = 0.8` and `"audio.volume" = 0.8` both address `audio.volume` (quoted keys keep their literal dots; a save writes them back quoted). Integers and floats land in one `number` kind, widened to double. Anything the store cannot represent (arrays, dates, arrays-of-tables) is skipped with one precise log instead of failing the file. The save writes rows in deterministic order with a short header, so the file diffs cleanly across runs.

## Merge semantics, not replace

`load` MERGES over the current store: rows the file defines are written, rows it does not define keep their value - a reset is the caller's explicit `removeValue` job. That makes layering trivial: a mod can load a base file and let a user's file layer on top, and a mid-run `load` of a partial file cannot wipe what the user already set. A structurally broken file refuses with a precise message (file, line, parse error) and changes nothing; a first run with no file at all starts empty and logs the fact - a missing settings file is a fresh install, not an error.

## Reads degrade, never throw

Every typed read takes a caller-supplied fallback and returns it when the name is unset. A name that IS set but holds a different kind (a string where a number was expected - usually a hand-edit) returns the fallback and logs the mismatch ONCE per name, so a per-frame read of a mistyped key cannot spam the log and cannot hand wrong-typed data to code that expected one. `kindOf` and `contains` let generic tooling (a settings screen that renders one editor per row) work without knowing the types; `valueCount` is the cheap health probe.

## Wiring

| Manifest field | Value |
|---|---|
| provided capabilities | `settings.toml`, `scripting.lua_bridge.settings` |
| required capabilities | `logging` |
| required permissions | `capability.logging`, `filesystem.read`, `filesystem.write` |

The start phase auto-loads the configured default file; the module holds no subscriptions and touches no backend, so a run pulls it in the moment one mod names `settings` and never loads it otherwise. Denying `filesystem.write` degrades the module honestly: the store works in memory and every save reports precisely why it did not.

## Lua bridge

The module ships the `scripting.lua_bridge.settings` extension, which installs five functions into every script state's `engine` table - a script mod's first persistent storage:

```lua
engine.set_setting("level_completed." .. level_id, true)  -- number, boolean or string
local volume = engine.get_setting("master_volume", 0.8)   -- the stored kind wins;
                                                          -- the fallback answers
                                                          -- only when unset
engine.remove_setting("obsolete_key")
engine.save_settings()        -- explicit write to the configured default file
engine.load_settings("mods/my_mod/base.toml")  -- optional path argument
```

The bridge reads through the same kind-preserving path the capability exposes, so a value set from Lua comes back to Lua with its type intact without either side naming it.

## Configuration

The module reads its `[settings]` section during startup. Unknown keys are ignored, and a missing section means the defaults below.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `file` | string | `"settings.toml"` | the default file for auto-load, autosave and every empty-path load/save; resolved against the executable directory (the engine's path convention for state the RUN owns, the same convention the log file keeps) |
| `autosave` | boolean | `false` | rewrite the default file after every mutation; off keeps writes explicit (a caller saves when it knows the values are good), on gives crash-durable settings at the cost of one file write per mutation |

## Files

- `settings_capability.hpp` — the published capability surface every consumer includes
- `settings.hpp` / `settings.cpp` — the module, the store and the TOML reader/writer
- `module_plugin.cpp` — the static plugin descriptor (name, factory, provided capabilities)
- `config.toml` — shipped defaults (`[settings]`)
- `CMakeLists.txt` — target and option wiring (`ENGINE_BUILD_MODULE_SETTINGS_TOML`)
