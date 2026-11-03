# example_species_mod

A minimal native data package used to demonstrate Harrowmere's Phase 4 data-mod loader. It contributes one species (`mossling`) and one recipe (`craft_mossling_wrap`).

## Merge behavior

The engine discovers this package by `mod.toml`. `harrowmere_core` applies its JSON overlays after base data in manifest-id order. `species.json` is a D-MOD-2 entity array, so the `mossling` row is added without replacing the other species. `recipes.json` is not a D-MOD-2 entity array and therefore replaces wholesale; the sample includes the complete base recipe set plus its new recipe.

The native library is intentionally a minimal component with no extra capabilities. Content remains in JSON; no simulation implementation is copied into this mod.
