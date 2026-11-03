# harrowmere_ui

The UI mod owns the ImGui overlay, simulation panels, Options screen, and the frame-coupled input mapper. It consumes `harrowmere.core` exclusively through its published views and queued commands.

## Options and keybinds (task 7.6)

Press **F2** to open Options. The panel persists preferences through the engine `settings` capability in its own `settings.toml` file, separate from campaign saves. It exposes master volume, a graphics quality preference, UI scale, verbose UI details, and remappable gameplay/overlay keys. Master volume applies immediately when the optional `audio` capability is available; UI scale applies to ImGui immediately. Gameplay key remaps are read by the input mapper at startup and therefore take effect after restart. The current renderer capability has no runtime quality setter, so the chosen graphics preset is persisted for a future renderer configuration hook rather than being claimed as a live renderer change.

The UI watches the configured settings file through `hot_reload`; hand edits are loaded on the main-thread event and update the overlay key and audio level. Engine assembly defaults such as `harrowmere_ui.visible_on_start` and `key` are separately observed through `ConfigurationWatchHub`.
