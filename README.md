# SkyCollection

SkyCollection is a save-specific archive of discovered Skyrim items and locations. It records items as they enter the player's inventory and restores each collection from that save's SKSE co-save.

## Features

- Track discovered items and first-time map-marker discoveries in the active save.
- Browse the catalog by item category, plugin, name, FormID, discovery recency, and estimated rarity.
- Configure the collection cards, filters, and default sort order in SkyViewer.
- Reset the active save's collection without deleting shared image-cache files.
- Show item icons and location screenshots when SkyCounter UI has captured and cached them.

Rarity is an estimate based on available record flags, equipment tiers, and naming patterns; it is not a universal Skyrim record property.

## Requirements

- Windows x64 and Visual Studio 2022 with the C++ desktop workload.
- CMake 3.24 or newer.
- Git, for fetching CommonLibSSE-NG.
- vcpkg with `VCPKG_ROOT` set, using the `x64-windows-static-md` triplet.
- Skyrim SE/AE, SKSE, PrismaUI, and the matching SkyOS core at runtime.
- SkyCounter UI (`SkyCounterUI.dll`) for item icons and location screenshot capture.

SkyCounter UI owns image rendering, capture, and cache files. SkyCollection only reads and displays those cached images. Its app manifest and native location-capture bridge both require SkyCounter UI.

## Build

From this directory, configure and build the Release preset:

```powershell
cmake --preset release
cmake --build --preset release --parallel
```

Create an archive of the install tree:

```powershell
cpack --config build/CPackConfig.cmake -G 7Z -C Release
```

The install tree contains the DLL and SkyCollection-owned resources under Skyrim's `Data` directory structure. CPack defaults to ZIP; the command above selects 7-Zip when the CPack 7Z generator is available on the machine.

## SkyOS integration

SkyCollection is an optional native app module. `SkyOS.dll` owns the shared PrismaUI catalog page, request listeners, and settings bridge. It calls the functions exported by `SkyCollection.dll` for save serialization, item/location discovery, and catalog snapshots. The module's SKSE entry checks for `SkyOS.dll` at data load and remains inactive if the core is missing.

Building or installing this repository alone does not provide the shared SkyOS shell or SkyCounter UI. Install the matching SkyOS core, SkyCounter UI, and PrismaUI as well.

## Repository layout

```text
src/                         Native collection and serialization plugin
data/PrismaUI/views/          SkyCollection logo and banner
data/SKSE/Plugins/SkyOS/Mods/ SkyCollection app metadata and SkyDocuments guides
                             SkyViewer module page, settings, and assets
```

The SkyDocuments source guides are:

- `data/SKSE/Plugins/SkyOS/Mods/SkyCollection/SkyDocuments/SkyCollection Guide.html`
- `data/SKSE/Plugins/SkyOS/Mods/SkyCollection/SkyDocuments/SkyCollection Module.md`

CMake installs both guides to `Data/SKSE/Plugins/SkyOS/Mods/SkyCollection/SkyDocuments/`.

## Settings and saved data

SkyViewer → Modules → SkyCollection provides the catalog display options, default sort order, plugin filter visibility, and **Reset Collection** action. Per-user module settings are stored at `%LOCALAPPDATA%/SkyOS/SkyViewer/SkyCollection/settings.json`.

Item and location discoveries are saved with the active Skyrim save. SkyCounter UI owns the shared image caches:

- Item icons: `Data/SKSE/Plugins/SkyOS/Mods/SkyCounterUI/Icons/`
- Location screenshots: `Data/SKSE/Plugins/SkyOS/Mods/SkyCounterUI/Locations/`

Resetting the collection clears that save's discovery records and preserves the cached images. Save the game afterward to persist the reset in the active save.
