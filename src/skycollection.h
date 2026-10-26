#pragma once

#include <string>
#include <string_view>
#include <cstdint>

#include <SKSE/Interfaces.h>

#if defined(SKYOS_SKYCOLLECTION_EXPORTS)
#define SKYOS_SKYCOLLECTION_API __declspec(dllexport)
#else
#define SKYOS_SKYCOLLECTION_API __declspec(dllimport)
#endif

namespace RE { class BGSLocation; }

namespace SkyOS {
    using SkyCounterPickupCallback = void(*)(const std::string& payload);
    using SkyCollectionIconUpdateCallback = void(*)(const std::string& payload);
    using SkyCollectionLocationUpdateCallback = void(*)(const std::string& payload, bool discoveredNow);

    // Listen for items entering the player inventory and add newly seen forms
    // to the persistent SkyCollection catalog. The event sink only enqueues
    // form IDs/counts; form resolution, persistence, and UI notification happen
    // later through Skyrim's task interface.
    SKYOS_SKYCOLLECTION_API void InstallSkyCollectionEventSink(SkyCounterPickupCallback callback, SkyCollectionIconUpdateCallback iconCallback,
        SkyCollectionLocationUpdateCallback locationCallback);

    // Record a first-time map-marker discovery in the save-backed catalog and
    // read any screenshot already captured by SkyCounterUI.
    SKYOS_SKYCOLLECTION_API void DiscoverSkyCollectionLocation(RE::BGSLocation* location, std::string_view discoveredName = {});
    SKYOS_SKYCOLLECTION_API void UpdateSkyCollectionLocationScreenshot(const std::string& key, const std::string& imagePath);

    // Called after the background renderer has written a PNG to disk.
    SKYOS_SKYCOLLECTION_API void UpdateSkyCollectionIcon(std::uint32_t formID, const std::string& iconUrl);

    // JSON snapshot used by the SkyCollection page.
    [[nodiscard]] SKYOS_SKYCOLLECTION_API std::string GetSkyCollectionCatalogJSON();
    [[nodiscard]] SKYOS_SKYCOLLECTION_API std::string GetSkyCollectionLocationsJSON();

    // Clear only the active save's discoveries. Icon-cache files are retained.
    [[nodiscard]] SKYOS_SKYCOLLECTION_API bool ClearSkyCollectionForActiveSave();

    // The catalog is stored in the save's SKSE co-save record, so loading an
    // older or different save restores that save's own discoveries.
    SKYOS_SKYCOLLECTION_API void RegisterSkyCollectionSerialization();
    SKYOS_SKYCOLLECTION_API void ResetSkyCollectionForSaveLoad();
    SKYOS_SKYCOLLECTION_API void BeginSkyCollectionNewGame();
    SKYOS_SKYCOLLECTION_API void LoadSkyCollectionLegacyCatalog(const std::string& physicalSavePath);
    SKYOS_SKYCOLLECTION_API void SaveSkyCollectionSerializationRecord(SKSE::SerializationInterface* intfc);
    SKYOS_SKYCOLLECTION_API bool LoadSkyCollectionSerializationRecord(SKSE::SerializationInterface* intfc,
        std::uint32_t type, std::uint32_t version, std::uint32_t length);
}
