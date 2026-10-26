# SkyCollection

## Features

- Builds a save-specific catalog of discovered items and locations as the player explores and collects.
- Filters and sorts the catalog by category, subcategory, plugin, rarity, and collection count.
- SkyCollection only reads and displays item icons and first-visit location screenshots already written to SkyCounter UI's caches. SkyCounter UI owns all artwork rendering and capture; SkyCollection records the returned paths with its save-backed discoveries.
- Resets discoveries for the active save without deleting the cached artwork.

## Changelog

- **Standalone app module:** collection tracking, save serialization, location discovery, and catalog services ship in `SkyCollection.dll`; the module page and guide ship in its own package.
- **Existing behavior retained:** discovery state remains save-specific, while image caches and module preferences stay outside the save.
- **Cache and control ownership:** SkyCounter UI controls item rendering and location screenshot capture, and owns `Data/SKSE/Plugins/SkyOS/Mods/SkyCounterUI/Icons/` and `Data/SKSE/Plugins/SkyOS/Mods/SkyCounterUI/Locations/`. SkyCollection only reads the cached images and displays them in its catalog.

## Requirements

- Skyrim Special Edition / Anniversary Edition Latest Version (supported runtime).
- SKSE Latest Version.
- PrismaUI Latest Version.
- SkyOS Latest Version.
- `SkyCounter UI` Latest Version.

## Files and saved data

- Module page and settings schema: `Data/SKSE/Plugins/SkyOS/Mods/SkyViewer/SkyCollection/`.
- SkyDocuments guide and branding: `Data/SKSE/Plugins/SkyOS/Mods/SkyCollection/`.
- Per-user module settings: `%LOCALAPPDATA%/SkyOS/SkyViewer/SkyCollection/settings.json`.
- Discovery catalog: the active save's SKSE co-save. Item-icon cache: `Data/SKSE/Plugins/SkyOS/Mods/SkyCounterUI/Icons/`. Location screenshot cache: `Data/SKSE/Plugins/SkyOS/Mods/SkyCounterUI/Locations/`.

