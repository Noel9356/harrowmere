# localization_toml module

The engine's string-table service: user-facing text addressed by key, translated through an active language with a fallback, shipped as TOML files a translator can edit. The module provides the `LocalizationCapability` (family `localization`, implementation-bound member `localization.toml`, published in `localization_capability.hpp` next to this module) and requires nothing but logging plus `filesystem.read`. Before it existed every UI string was a literal in whoever drew it - the ui module's overlay, a script's log lines, a mod's dialogs - so nothing was translatable and nothing was consistent. This capability is the one lookup: a mod ships its own tables under `assets/`, every consumer addresses text by key, and a missing translation degrades visibly instead of blanking a screen.

## The file format

One `<code>.toml` per language in the configured directory (the file stem IS the code: `en.toml` loads as `en`). Rows are strings, written in whatever TOML idiom reads best - both spellings address the same dotted key:

```toml
# mods/my_mod/assets/languages/en.toml
"menu.start" = "Start game"        # quoted keys: ONE flat key with a literal dot

[menu]                             # nested tables: the same dotted keys
settings = "Settings"
quit = "Quit game"

[hud]
score = "Score: {0}"               # formatting is the consumer's business
```

A value that is not a string is skipped with one precise log (a language file is text by definition; a mistyped row must not fail the whole table). A structurally broken file refuses with file and line, leaving that language's previous rows untouched. Re-loading a code REPLACES its rows wholesale - the hot-reload watcher's `HotReloadEvent` for a changed language file is the natural trigger for a live-reloaded translation.

## The lookup chain

`translate(key)` walks active language, then the configured fallback, then the key itself. The three outcomes are deliberate: the active table answers (the normal case); the fallback answers (a language missing a freshly added key still shows the fallback text instead of a hole); the key returns (a missing translation is VISIBLE in the UI - a screen that suddenly shows raw keys is a screen someone will fix - and logged once per key, so a per-frame lookup of a typo'd key cannot spam). `setLanguage` loads the target first when needed and KEEPS the previous language on failure, so a failed switch never leaves the UI untranslated.

## Shipping translations

The engine ships no content - translations are mod content, and they reach the runtime through the existing mod packaging: a package puts its language files under `assets/languages/` (copied wholesale into `runtime/mods/<id>/assets/`) and points the module's directory key at them in its own shipped defaults (`mods/<id>/config.toml`):

```toml
[localization]
directory = "mods/my_mod/assets/languages"
language = "en"
fallback = "en"
```

An assembly that wants one language for every mod sets the same keys in `config/engine.toml`, which wins over every package's defaults (the layer stack in `docs/configuration.md`). The startup scan loads every `*.toml` in the directory, so `loadedLanguages()` is the honest list a settings screen's language picker iterates.

## Wiring

| Manifest field | Value |
|---|---|
| provided capabilities | `localization.toml`, `scripting.lua_bridge.localization` |
| required capabilities | `logging` |
| required permissions | `capability.logging`, `filesystem.read` |

The module holds no subscriptions and touches no backend; a run pulls it in the moment one mod names `localization` and never loads it otherwise. Denying `filesystem.read` degrades the module honestly: lookups return keys, and one warning names the directory that could not be read.

## Lua bridge

The module ships the `scripting.lua_bridge.localization` extension, which installs four functions into every script state's `engine` table:

```lua
engine.log_information(engine.localize("menu.start"))   -- the one lookup
engine.set_language("de")     -- loads de.toml on demand, returns false
                              -- (and keeps the old language) when it cannot
local code = engine.active_language()                  -- "de"
for _, code in ipairs(engine.loaded_languages()) do    -- sorted codes
    engine.log_debug(code)
end
```

## Configuration

The module reads its `[localization]` section during startup. Unknown keys are ignored, and a missing section means the defaults below.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `directory` | string | `"languages"` | where the `<code>.toml` files live, resolved against the executable directory; a mod points it at its own packaged `assets/languages` |
| `language` | string | `"en"` | the language activated at startup |
| `fallback` | string | `"en"` | the second lookup in the chain; a key missing from both degrades to the key itself, logged once |

## Implementation

Standard library plus toml++ - the same parser the kernel and the persistence module carry, so the tree keeps exactly one TOML implementation. Languages live in one `std::map` keyed by code (sorted iteration gives `loadedLanguages` its stable order for free); one language's rows live in an `unordered_map`, because `translate` is the per-frame path (a UI drawing N localized labels calls it N times per frame) and one hash answers where a red-black tree walked the key with string compares. The startup scan is one `directory_iterator` pass filtering on the `.toml` extension, and a re-load parses into a fresh map before swapping, so a hot-reloaded translation never shows half old half new rows. Language codes are held to letters, digits, `_` and `-` (the mod-id charset): the code names a file, and the restricted charset is the directory-injection guard. Everything is main-thread by the engine's usual contract; a `translate` call is one hash lookup in the common case (a hit in the active table), and only a miss walks the fallback row-map - both against the row-map pointers cached between mutations.

## Files

- `localization_capability.hpp` — the published capability surface every consumer includes
- `localization.hpp` / `localization.cpp` — the module, the language tables and the Lua bridge extension
- `module_plugin.cpp` — the static plugin descriptor (name, factory, provided capabilities)
- `config.toml` — shipped defaults (`[localization]`)
- `CMakeLists.txt` — target and option wiring (`ENGINE_BUILD_MODULE_LOCALIZATION_TOML`)
