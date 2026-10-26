#include "pch.h"
#include "skycollection.h"

#include <RE/F/FormTypes.h>
#include <RE/A/AlchemyItem.h>
#include <RE/S/ScriptEventSourceHolder.h>
#include <RE/T/TESContainerChangedEvent.h>
#include <RE/T/TESFile.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESObjectARMO.h>
#include <RE/T/TESObjectBOOK.h>
#include <RE/T/TESObjectWEAP.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/B/BGSLocation.h>
#include <RE/E/ExtraMapMarker.h>
#include <RE/I/INISettingCollection.h>
#include <RE/L/LocationDiscovery.h>
#include <RE/M/MenuControls.h>
#include <RE/R/Renderer.h>
#include <RE/T/TESActorLocationChangeEvent.h>

#include <Windows.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace SkyOS {
namespace {
    constexpr std::size_t kMaxCatalogItems = 10000;
    constexpr std::size_t kMaxCatalogJsonBytes = 4 * 1024 * 1024;
    constexpr std::uint32_t kCollectionPluginID = 'SKCL';
    constexpr std::uint32_t kCollectionRecordType = 'SCAT';
    constexpr std::uint32_t kCollectionRecordVersion = 2;
    constexpr std::uint32_t kMaxCollectionRecordBytes = 32 * 1024 * 1024;

    struct ItemRecord {
        std::string plugin;
        std::string name;
        std::string category;
        std::uint32_t localFormID{};
        std::uint32_t runtimeFormID{};
        std::uint64_t totalFound{};
        bool unique{};
        std::string iconPath;
    };

    struct LocationRecord {
        std::string plugin;
        std::string name;
        std::uint32_t localFormID{};
        std::uint32_t runtimeFormID{};
        std::string screenshotPath;
    };

    struct PendingAcquisition {
        RE::FormID formID{};
        std::uint32_t count{};
        std::uint64_t saveGeneration{};
    };

    std::mutex g_catalogMutex;
    std::mutex g_queueMutex;
    std::map<std::string, ItemRecord, std::less<>> g_items;
    std::map<std::string, LocationRecord, std::less<>> g_locations;
    std::deque<PendingAcquisition> g_pending;
    SkyCounterPickupCallback g_pickupCallback{};
    SkyCollectionIconUpdateCallback g_iconCallback{};
    SkyCollectionLocationUpdateCallback g_locationCallback{};
    bool g_catalogLoaded{};
    bool g_catalogRestored{};
    bool g_drainScheduled{};
    bool g_sinkInstalled{};
    bool g_locationDiscoverySinkInstalled{};
    bool g_locationChangeSinkInstalled{};
    std::atomic<std::uint64_t> g_saveGeneration{};
    std::string g_catalogScope;

    template <class T>
    bool AppendBinary(std::vector<std::byte>& output, const T& value)
    {
        if (output.size() > kMaxCollectionRecordBytes - sizeof(T)) return false;
        const auto* begin = reinterpret_cast<const std::byte*>(std::addressof(value));
        output.insert(output.end(), begin, begin + sizeof(T));
        return true;
    }

    bool AppendString(std::vector<std::byte>& output, std::string_view value, std::size_t maxLength)
    {
        if (value.size() > maxLength || value.size() > std::numeric_limits<std::uint32_t>::max()) return false;
        const auto length = static_cast<std::uint32_t>(value.size());
        if (output.size() > kMaxCollectionRecordBytes - sizeof(length) ||
            value.size() > kMaxCollectionRecordBytes - output.size() - sizeof(length)) return false;
        if (!AppendBinary(output, length)) return false;
        const auto* begin = reinterpret_cast<const std::byte*>(value.data());
        output.insert(output.end(), begin, begin + value.size());
        return true;
    }

    template <class T>
    bool ReadBinary(const std::byte*& cursor, const std::byte* end, T& value)
    {
        if (static_cast<std::size_t>(end - cursor) < sizeof(T)) return false;
        std::memcpy(std::addressof(value), cursor, sizeof(T));
        cursor += sizeof(T);
        return true;
    }

    bool ReadString(const std::byte*& cursor, const std::byte* end, std::string& value,
        std::size_t maxLength)
    {
        std::uint32_t length{};
        if (!ReadBinary(cursor, end, length) || length > maxLength ||
            static_cast<std::size_t>(end - cursor) < length) return false;
        value.assign(reinterpret_cast<const char*>(cursor), length);
        cursor += length;
        return true;
    }

    void DrainSerializationRecord(SKSE::SerializationInterface* intfc, std::uint32_t length)
    {
        std::array<std::byte, 4096> discard{};
        while (length) {
            const auto chunk = (std::min)(length, static_cast<std::uint32_t>(discard.size()));
            if (intfc->ReadRecordData(discard.data(), chunk) != chunk) break;
            length -= chunk;
        }
    }

    std::string CleanField(std::string_view input)
    {
        std::string result;
        result.reserve(input.size());
        for (const unsigned char ch : input) {
            if (ch == '|' || ch == '\r' || ch == '\n' || ch == '\t' || ch == 0) result.push_back(' ');
            else if (ch >= 0x20) result.push_back(static_cast<char>(ch));
        }
        while (!result.empty() && result.back() == ' ') result.pop_back();
        return result;
    }

    std::string JsonEscape(std::string_view input)
    {
        std::string output;
        output.reserve(input.size() + 8);
        for (const unsigned char ch : input) {
            switch (ch) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default: if (ch >= 0x20) output.push_back(static_cast<char>(ch));
            }
        }
        return output;
    }

    std::string Hex(std::uint32_t value, int width = 8)
    {
        std::array<char, 16> buffer{};
        const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 16);
        if (error != std::errc{}) return "00000000";
        std::string result(buffer.data(), end);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        if (result.size() < static_cast<std::size_t>(width)) result.insert(result.begin(), static_cast<std::size_t>(width) - result.size(), '0');
        return result;
    }

    bool ParseHex(std::string_view value, std::uint32_t& result)
    {
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result, 16);
        return error == std::errc{} && end == value.data() + value.size();
    }

    std::uint32_t ResolveRuntimeFormID(std::uint32_t localFormID, std::string_view plugin)
    {
        if (!localFormID || plugin.empty()) return 0;
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) return 0;
        auto* file = dataHandler->LookupLoadedModByName(plugin);
        if (!file) file = dataHandler->LookupLoadedLightModByName(plugin);
        if (!file) return 0;

        const auto partialIndex = file->GetPartialIndex();
        const auto formID = file->IsLight()
            ? ((partialIndex << 12) | (localFormID & 0xFFF))
            : ((static_cast<std::uint32_t>(file->GetCompileIndex()) << 24) | (localFormID & 0xFFFFFF));
        auto* form = RE::TESForm::LookupByID(formID);
        if (form && !form->IsDeleted() && !form->IsDestroyed() && form->GetFile() == file) return formID;

        // Let the data handler resolve unusual records if the loaded-file index path did not.
        const auto lookedUpID = dataHandler->LookupFormID(localFormID, plugin);
        form = lookedUpID ? RE::TESForm::LookupByID(lookedUpID) : nullptr;
        if (form && !form->IsDeleted() && !form->IsDestroyed() && form->GetFile() == file) return lookedUpID;
        return 0;
    }

    fs::path SkyCollectionRoot()
    {
        std::wstring localAppData(32768, L'\0');
        const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData.data(), static_cast<DWORD>(localAppData.size()));
        if (!length || length >= localAppData.size()) return {};
        localAppData.resize(length);
        return fs::path(localAppData) / L"SkyOS" / L"SkyCollection";
    }

    fs::path SkyCounterUIIconRoot()
    {
        std::array<wchar_t, 32768> executable{};
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length >= executable.size()) return {};
        return fs::path(std::wstring(executable.data(), length)).parent_path() / L"Data" / L"SKSE" /
            L"Plugins" / L"SkyOS" / L"Mods" / L"SkyCounterUI" / L"Icons";
    }

    fs::path PreviousPerUserSkyCounterUIIconRoot()
    {
        std::wstring localAppData(32768, L'\0');
        const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData.data(), static_cast<DWORD>(localAppData.size()));
        if (!length || length >= localAppData.size()) return {};
        localAppData.resize(length);
        return fs::path(localAppData) / L"SkyOS" / L"SkyCounterUI" / L"Icons";
    }

    fs::path SkyrimDataLocationsRoot()
    {
        std::array<wchar_t, 32768> executable{};
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length >= static_cast<DWORD>(executable.size())) return {};
        return fs::path(std::wstring(executable.data(), length)).parent_path() / L"Data" / L"SKSE" /
            L"Plugins" / L"SkyOS" / L"Mods" / L"SkyCounterUI" / L"Locations";
    }

    std::string SafePathSegment(std::string_view value)
    {
        std::string result;
        result.reserve(value.size());
        bool previousSeparator = false;
        for (const unsigned char ch : value) {
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
                result.push_back(static_cast<char>(ch));
                previousSeparator = false;
            } else if ((ch == ' ' || ch == '.') && !result.empty() && !previousSeparator) {
                result.push_back('_');
                previousSeparator = true;
            }
        }
        while (!result.empty() && result.back() == '_') result.pop_back();
        return result;
    }

    std::string SafePluginFolder(std::string_view value)
    {
        std::string result;
        result.reserve(value.size());
        for (const unsigned char ch : value) {
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.') {
                result.push_back(static_cast<char>(ch));
            } else {
                result.push_back('_');
            }
        }
        while (!result.empty() && (result.back() == '.' || result.back() == '_')) result.pop_back();
        return result;
    }

    fs::path LocationScreenshotPathFor(const std::string& plugin, std::uint32_t localFormID,
        const std::string& name)
    {
        auto root = SkyrimDataLocationsRoot();
        if (root.empty()) return {};
        std::string normalizedPlugin = plugin;
        std::transform(normalizedPlugin.begin(), normalizedPlugin.end(), normalizedPlugin.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (!normalizedPlugin.empty() && normalizedPlugin != "skyrim.esm") {
            auto safePlugin = SafePluginFolder(plugin);
            if (safePlugin.empty()) safePlugin = "UnknownPlugin";
            root /= safePlugin;
        }
        auto safeName = SafePathSegment(name);
        if (safeName.empty()) safeName = "Location";
        auto path = root / fs::path(safeName + "_" + Hex(localFormID) + ".bmp");
        std::error_code error;
        if (fs::is_regular_file(path, error) && !error) return path;
        for (const auto* extension : { L".png", L".jpg", L".jpeg" }) {
            path.replace_extension(extension);
            error.clear();
            if (fs::is_regular_file(path, error) && !error) return path;
        }
        auto legacyRoot = SkyrimDataLocationsRoot().parent_path().parent_path() / L"SkyCollection" / L"Locations";
        if (!normalizedPlugin.empty() && normalizedPlugin != "skyrim.esm") {
            auto legacyPlugin = SafePluginFolder(plugin);
            if (!legacyPlugin.empty()) legacyRoot /= legacyPlugin;
        }
        auto legacy = legacyRoot / fs::path(safeName + ".png");
        error.clear();
        if (fs::is_regular_file(legacy, error) && !error) return legacy;
        return root / fs::path(safeName + "_" + Hex(localFormID) + ".bmp");
    }

    std::string IconFileUrl(const fs::path& path)
    {
        std::wstring wide = path.wstring();
        std::replace(wide.begin(), wide.end(), L'\\', L'/');
        if (wide.empty()) return {};
        const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
            nullptr, 0, nullptr, nullptr);
        if (utf8Length <= 0) return {};
        std::string utf8(static_cast<std::size_t>(utf8Length), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                utf8.data(), utf8Length, nullptr, nullptr) != utf8Length) return {};

        std::string result = "file:///";
        constexpr char hex[] = "0123456789ABCDEF";
        for (const unsigned char ch : utf8) {
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                ch == '/' || ch == ':' || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
                result.push_back(static_cast<char>(ch));
            } else {
                result.push_back('%');
                result.push_back(hex[ch >> 4]);
                result.push_back(hex[ch & 0x0F]);
            }
        }
        // SkyCollection reads icons written by SkyCounter UI. Keep a distinct
        // URL so PrismaUI does not retain a previous PNG decoded from disk.
        result += "?v=2";
        std::error_code stampError;
        const auto writeTime = fs::last_write_time(path, stampError);
        if (!stampError) result += "-" + std::to_string(writeTime.time_since_epoch().count());
        return result;
    }

    fs::path CatalogPath()
    {
        const auto root = SkyCollectionRoot();
        if (root.empty() || g_catalogScope.empty()) return {};
        return root / L"Saves" / fs::path(g_catalogScope) / L"catalog.tsv";
    }

    fs::path IconPathFor(const std::string& plugin, std::uint32_t localFormID)
    {
        std::string safePlugin;
        safePlugin.reserve(plugin.size());
        for (const unsigned char ch : plugin) {
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
                safePlugin.push_back(static_cast<char>(ch));
            } else if (ch == '.') {
                safePlugin.push_back('_');
            }
        }
        if (safePlugin.empty()) safePlugin = "UnknownPlugin";
        const auto root = SkyCounterUIIconRoot();
        if (root.empty()) return {};
        const auto filename = fs::path(Hex(localFormID) + ".png");
        // Keep the leaf filename to the record ID, while the plugin directory
        // prevents two plugins with the same local FormID from overwriting
        // each other's cached icons.
        const auto destination = root / safePlugin / filename;
        std::error_code error;
        if (fs::is_regular_file(destination, error) && !error) return destination;

        // Read-only fallbacks preserve icons from the temporary LocalAppData
        // cache path and from older SkyCollection releases. SkyCollection
        // never copies or writes icon files; SkyCounter UI owns that work.
        const auto perUserRoot = PreviousPerUserSkyCounterUIIconRoot();
        const auto perUserIcon = perUserRoot.empty() ? fs::path{} : perUserRoot / safePlugin / filename;
        error.clear();
        if (!perUserIcon.empty() && fs::is_regular_file(perUserIcon, error) && !error) return perUserIcon;

        const auto oldCollectionDataIcon = root.parent_path().parent_path().parent_path() /
            L"Mods" / L"SkyCollection" / L"Icons" / safePlugin / filename;
        error.clear();
        if (fs::is_regular_file(oldCollectionDataIcon, error) && !error) return oldCollectionDataIcon;

        const auto legacyRoot = SkyCollectionRoot();
        const auto legacy = legacyRoot.empty() ? fs::path{} : legacyRoot / L"Icons" /
            fs::path(safePlugin + "_" + Hex(localFormID) + ".png");
        error.clear();
        if (!legacy.empty() && fs::is_regular_file(legacy, error) && !error) return legacy;
        return destination;
    }

    bool HasCachedIcon(const std::string& plugin, std::uint32_t localFormID)
    {
        const auto path = IconPathFor(plugin, localFormID);
        if (path.empty()) return false;
        std::error_code error;
        return fs::is_regular_file(path, error) && !error;
    }

    std::string KeyFor(const std::string& plugin, std::uint32_t localFormID)
    {
        return plugin + "|" + Hex(localFormID);
    }

    bool IsCurrencyName(std::string_view name)
    {
        std::string normalized(name);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return normalized == "gold" || normalized == "septim" || normalized == "septims";
    }

    bool IsCurrencyItem(const RE::TESForm* form)
    {
        if (!form) return false;
        if (form->IsGold() || form->GetFormID() == 0x0000000F) return true;
        const auto* name = form->GetName();
        return name && IsCurrencyName(name);
    }

    std::string Lower(std::string_view value)
    {
        std::string result(value);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return result;
    }

    bool Contains(std::string_view value, std::string_view part)
    {
        return Lower(value).find(part) != std::string::npos;
    }

    bool ContainsWord(std::string_view value, std::string_view word)
    {
        const auto lowerValue = Lower(value);
        const auto lowerWord = Lower(word);
        std::size_t position = 0;
        while ((position = lowerValue.find(lowerWord, position)) != std::string::npos) {
            const bool leftBoundary = position == 0 || !std::isalnum(static_cast<unsigned char>(lowerValue[position - 1]));
            const auto end = position + lowerWord.size();
            const bool rightBoundary = end == lowerValue.size() || !std::isalnum(static_cast<unsigned char>(lowerValue[end]));
            if (leftBoundary && rightBoundary) return true;
            ++position;
        }
        return false;
    }

    bool IsFishName(std::string_view name)
    {
        return Contains(name, "fish") || Contains(name, "longfin") || Contains(name, "spadetail") ||
            Contains(name, "histcarp") || ContainsWord(name, "carp") || ContainsWord(name, "perch") ||
            ContainsWord(name, "salmon") || ContainsWord(name, "betty") || ContainsWord(name, "angler") ||
            ContainsWord(name, "anthias") || ContainsWord(name, "grayling") || ContainsWord(name, "char") ||
            ContainsWord(name, "bass") || ContainsWord(name, "trout") || ContainsWord(name, "eel") ||
            ContainsWord(name, "gurnard") || ContainsWord(name, "pogfish") || ContainsWord(name, "spadefish");
    }

    bool IsQuestItem(RE::TESForm* form)
    {
        return form && form->HasKeywordByEditorID("VendorItemQuest");
    }

    bool IsUniqueItem(RE::TESForm* form)
    {
        if (!form) return false;
        return form->HasKeywordByEditorID("VendorItemUnique") || IsQuestItem(form);
    }

    std::string CategoryFor(const RE::TESForm* form)
    {
        if (!form) return "Other";
        const auto* rawName = form->GetName();
        const std::string_view name = rawName ? rawName : "";
        if (Contains(name, "dragon claw")) return "Dragon Claws";
        if (Contains(name, "dragon priest mask") || Contains(name, "wooden mask") || Contains(name, "hevnoraak") ||
            Contains(name, "krosis") || Contains(name, "morokei") || Contains(name, "nahkriin") ||
            Contains(name, "otar") || Contains(name, "rahgot") || Contains(name, "vokun") ||
            Contains(name, "volsung") || Contains(name, "konahrik")) return "Dragon Masks";

        switch (form->GetFormType()) {
        case RE::FormType::Weapon: {
            const auto* weapon = static_cast<const RE::TESObjectWEAP*>(form);
            if (Contains(name, "warhammer")) return "Weapons/Warhammers";
            switch (weapon->GetWeaponType()) {
            case RE::WEAPON_TYPE::kOneHandSword: return "Weapons/Swords";
            case RE::WEAPON_TYPE::kTwoHandSword: return "Weapons/Greatswords";
            case RE::WEAPON_TYPE::kOneHandDagger: return "Weapons/Daggers";
            case RE::WEAPON_TYPE::kOneHandAxe: return "Weapons/War Axes";
            case RE::WEAPON_TYPE::kTwoHandAxe: return "Weapons/Battleaxes";
            case RE::WEAPON_TYPE::kOneHandMace: return "Weapons/Maces";
            case RE::WEAPON_TYPE::kBow: return "Weapons/Bows";
            case RE::WEAPON_TYPE::kCrossbow: return "Weapons/Crossbows";
            case RE::WEAPON_TYPE::kStaff: return "Weapons/Staves";
            default: return "Weapons/Other";
            }
        }
        case RE::FormType::Armor: {
            const auto* armor = static_cast<const RE::TESObjectARMO*>(form);
            const auto* editorID = form->GetFormEditorID();
            const std::string_view editorName = editorID ? editorID : "";
            if (Contains(name, "hood") || Contains(editorName, "hood")) return "Armors/Hoods";
            if (Contains(name, "robe") || Contains(editorName, "robe")) return "Armors/Robes";
            if (ContainsWord(name, "merchant") || ContainsWord(name, "chef") || ContainsWord(name, "blacksmith") ||
                Contains(name, "common clothes") || Contains(name, "common tunic") ||
                Contains(name, "clothing") || Contains(name, "clothes") || Contains(name, "outfit") ||
                Contains(name, "tunic") || Contains(name, "apron") ||
                Contains(editorName, "merchant") || Contains(editorName, "chef") || Contains(editorName, "blacksmith") ||
                Contains(editorName, "commonclothes") || Contains(editorName, "common_clothes")) return "Armors/Outfits";
            if (Contains(name, "bikini") || Contains(name, "thong")) return "Armors/Bikinis";
            if (Contains(name, "cape") || Contains(name, "cloak")) return "Armors/Capes";
            if (ContainsWord(name, "ring")) return "Armors/Rings";
            if (Contains(name, "amulet")) return "Armors/Amulets";
            if (Contains(name, "necklace")) return "Armors/Necklaces";
            if (armor->HasPartOf(RE::BIPED_MODEL::BipedObjectSlot::kRing)) return "Armors/Rings";
            if (armor->HasPartOf(RE::BIPED_MODEL::BipedObjectSlot::kAmulet)) return "Armors/Necklaces";
            if (armor->IsShield()) return "Armors/Shields";
            if (armor->IsHelmet() || armor->IsCirclet()) return "Armors/Helmets";
            if (armor->IsGauntlets()) return "Armors/Gauntlets";
            if (armor->IsBoots()) return "Armors/Boots";
            if (armor->IsChestpiece()) return "Armors/Armors";
            return "Armors/Other";
        }
        case RE::FormType::Ammo: return "Weapons/Ammo (Arrows)";
        case RE::FormType::Ingredient: return IsFishName(name) ? "Consumables/Fishes" : "Ingredients";
        case RE::FormType::AlchemyItem: {
            const auto* alchemy = static_cast<const RE::AlchemyItem*>(form);
            if (IsFishName(name)) return "Consumables/Fishes";
            if (Contains(name, "alcohol") || Contains(name, "cocktail") || Contains(name, "wine") ||
                ContainsWord(name, "ale") || ContainsWord(name, "mead") || ContainsWord(name, "brandy") ||
                ContainsWord(name, "beer") || ContainsWord(name, "liquor") || ContainsWord(name, "spirits")) return "Consumables/Alcohol";
            return alchemy->IsFood() ? "Consumables/Food" : "Consumables/Potions";
        }
        case RE::FormType::Book: {
            const auto* book = static_cast<const RE::TESObjectBOOK*>(form);
            const auto* editorID = form->GetFormEditorID();
            const std::string_view editorName = editorID ? editorID : "";
            if (book->TeachesSpell()) return "Books/Spell Tomes";
            if (book->TeachesSkill() || book->data.flags.any(RE::OBJ_BOOK::Flag::kAdvancesActorValue)) return "Books/Skill Books";
            if (Contains(name, "bounty") || Contains(editorName, "bounty")) return "Books/Bounties";
            if (Contains(name, "letter") || Contains(editorName, "letter")) return "Books/Letters";
            if (book->IsNoteScroll()) return "Books/Notes";
            if (Contains(name, "recipe")) return "Books/Recipes";
            if (Contains(name, "journal")) return "Books/Journals";
            if (Contains(name, "map")) return "Books/Maps";
            return Contains(name, "note") ? "Books/Notes" : "Books/Books";
        }
        case RE::FormType::SoulGem: return "Soul Gems";
        case RE::FormType::KeyMaster: return "Keys";
        case RE::FormType::Misc:
            if (IsFishName(name)) return "Consumables/Fishes";
            if (IsCurrencyItem(form)) return "Currency";
            if (Contains(name, "ingot")) return "Ingots";
            if (Contains(name, "hide")) return "Hides";
            if (Contains(name, "pelt")) return "Pelts";
            if (Contains(name, "ore")) return "Ores";
            if (Contains(name, "gem") || Contains(name, "ruby") || Contains(name, "emerald") ||
                Contains(name, "sapphire") || Contains(name, "diamond") || Contains(name, "amethyst")) return "Gems";
            if (Contains(name, "straw") || Contains(name, "nail") || Contains(name, "hinge") || Contains(name, "goat horn") ||
                Contains(name, "iron fitting") || Contains(name, "glass") ||
                Contains(name, "lock") || Contains(name, "sawn log") || Contains(name, "quarried stone") ||
                Contains(name, "clay")) return "Building Materials";
            return "Misc";
        default: return "Other";
        }
    }

    std::string RarityFor(const ItemRecord& item)
    {
        const auto plugin = Lower(item.plugin);
        const bool weaponOrArmor = item.category.starts_with("Weapons/") || item.category.starts_with("Armors/");
        const bool legacyOfTheDragonborn = plugin.find("legacyofthedragonborn") != std::string::npos ||
            plugin.find("legacy of the dragonborn") != std::string::npos;
        const bool questItem = item.unique || item.category == "Dragon Claws" || item.category == "Dragon Masks" ||
            item.category.starts_with("Unique/");
        if (questItem) return "Legendary";
        if (legacyOfTheDragonborn && weaponOrArmor) return "Legendary";

        if (weaponOrArmor && (plugin.find("esoimports") != std::string::npos ||
            plugin.find("ktweaponpack") != std::string::npos || plugin.find("wzoblivionartifacts") != std::string::npos)) {
            return "Rare";
        }
        if (plugin == "dragonborn.esm" || plugin == "dawnguard.esm") return "Uncommon";

        const bool standardContent = plugin.empty() || plugin == "skyrim.esm" || plugin == "update.esm" ||
            plugin == "hearthfires.esm" || plugin == "another sorting mod" || plugin.find("anothersortingmod") != std::string::npos ||
            plugin.find("unofficial skyrim special edition patch") != std::string::npos || plugin.find("unofficial skyrim special edition patch.esp") != std::string::npos ||
            plugin.find("cutting room floor") != std::string::npos || plugin.find("morehud") != std::string::npos ||
            plugin.find("remodeled armors") != std::string::npos || legacyOfTheDragonborn;
        if (!standardContent) return "Epic";

        return "Common";
    }

    std::string JsonFor(const ItemRecord& item, std::uint32_t lastPickup = 0, bool discoveredNow = false)
    {
        std::string result = "{\"plugin\":\"" + JsonEscape(item.plugin) +
            "\",\"name\":\"" + JsonEscape(item.name) +
            "\",\"category\":\"" + JsonEscape(item.category) +
            "\",\"formID\":\"" + (item.runtimeFormID ? Hex(item.runtimeFormID) : std::string{}) +
            "\",\"localFormID\":\"" + Hex(item.localFormID) +
            "\",\"totalFound\":" + std::to_string(item.totalFound) +
            ",\"lastPickup\":" + std::to_string(lastPickup) +
            ",\"unique\":" + (item.unique ? "true" : "false") +
            ",\"rarity\":\"" + RarityFor(item) + "\"" +
            ",\"discoveredNow\":" + (discoveredNow ? "true" : "false") +
            ",\"icon\":\"" + JsonEscape(item.iconPath) + "\"}";
        return result;
    }

    std::string JsonFor(const LocationRecord& location, bool discoveredNow = false)
    {
        std::string image;
        const auto path = fs::path(location.screenshotPath);
        std::error_code error;
        if (!path.empty() && fs::is_regular_file(path, error) && !error) image = IconFileUrl(path);
        return "{\"key\":\"" + JsonEscape(KeyFor(location.plugin, location.localFormID)) +
            "\",\"plugin\":\"" + JsonEscape(location.plugin) +
            "\",\"name\":\"" + JsonEscape(location.name) +
            "\",\"formID\":\"" + (location.runtimeFormID ? Hex(location.runtimeFormID) : std::string{}) +
            "\",\"localFormID\":\"" + Hex(location.localFormID) +
            "\",\"discoveredNow\":" + (discoveredNow ? "true" : "false") +
            ",\"image\":\"" + JsonEscape(image) + "\"}";
    }

    bool HasVisibleScreenshotPixels(const fs::path& path)
    {
        std::error_code error;
        if (path.empty() || !fs::is_regular_file(path, error) || error) return false;

        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool shouldUninitialize = SUCCEEDED(comResult);
        IWICImagingFactory* factory = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICBitmapScaler* scaler = nullptr;
        IWICFormatConverter* converter = nullptr;
        bool usable = false;

        do {
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(&factory)))) break;
            if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                    WICDecodeMetadataCacheOnLoad, &decoder))) break;
            if (FAILED(decoder->GetFrame(0, &frame))) break;

            UINT width{};
            UINT height{};
            if (FAILED(frame->GetSize(&width, &height)) || width < 320 || height < 180) break;
            if (FAILED(factory->CreateBitmapScaler(&scaler))) break;
            if (FAILED(scaler->Initialize(frame, 64, 36, WICBitmapInterpolationModeNearestNeighbor))) break;
            if (FAILED(factory->CreateFormatConverter(&converter))) break;
            if (FAILED(converter->Initialize(scaler, GUID_WICPixelFormat32bppBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) break;

            constexpr UINT sampleWidth = 64;
            constexpr UINT sampleHeight = 36;
            constexpr UINT stride = sampleWidth * 4;
            std::array<BYTE, sampleWidth * sampleHeight * 4> pixels{};
            if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data()))) break;

            std::array<bool, 512> colorBuckets{};
            std::size_t distinctColors{};
            std::uint8_t minLuma = 255;
            std::uint8_t maxLuma{};
            std::size_t opaquePixels{};
            for (std::size_t offset = 0; offset < pixels.size(); offset += 4) {
                const auto blue = pixels[offset];
                const auto green = pixels[offset + 1];
                const auto red = pixels[offset + 2];
                const auto alpha = pixels[offset + 3];
                if (alpha < 128) continue;
                ++opaquePixels;
                const auto bucket = static_cast<std::size_t>((red >> 5) << 6) |
                    static_cast<std::size_t>((green >> 5) << 3) | static_cast<std::size_t>(blue >> 5);
                if (!colorBuckets[bucket]) {
                    colorBuckets[bucket] = true;
                    ++distinctColors;
                }
                const auto luma = static_cast<std::uint8_t>((77U * red + 150U * green + 29U * blue) >> 8);
                minLuma = (std::min)(minLuma, luma);
                maxLuma = (std::max)(maxLuma, luma);
            }
            usable = opaquePixels > (sampleWidth * sampleHeight * 3 / 4) &&
                distinctColors >= 4 && static_cast<unsigned>(maxLuma - minLuma) >= 8;
        } while (false);

        if (converter) converter->Release();
        if (scaler) scaler->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (factory) factory->Release();
        if (shouldUninitialize) CoUninitialize();
        return usable;
    }

    bool SaveCatalogLocked();

    void LoadCatalogLocked()
    {
        if (g_catalogLoaded) return;
        g_catalogLoaded = true;
        const auto path = CatalogPath();
        if (path.empty()) return;
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return;
        std::string line;
        while (g_items.size() < kMaxCatalogItems && std::getline(stream, line)) {
            std::array<std::string, 8> fields{};
            std::size_t start = 0;
            bool valid = true;
            for (std::size_t i = 0; i < fields.size(); ++i) {
                const auto separator = line.find('|', start);
                if (separator == std::string::npos && i + 1 != fields.size()) { valid = false; break; }
                const auto end = separator == std::string::npos ? line.size() : separator;
                fields[i] = line.substr(start, end - start);
                start = end + 1;
            }
            if (!valid) continue;
            ItemRecord item{};
            std::uint32_t found{};
            if (!ParseHex(fields[1], item.localFormID) || !ParseHex(fields[2], item.runtimeFormID) ||
                std::from_chars(fields[3].data(), fields[3].data() + fields[3].size(), item.totalFound).ec != std::errc{} ||
                std::from_chars(fields[5].data(), fields[5].data() + fields[5].size(), found).ec != std::errc{}) continue;
            item.plugin = fields[0];
            item.category = fields[4];
            item.unique = found != 0;
            item.name = fields[6];
            item.iconPath = fields[7];
            if (item.plugin.empty() || item.name.empty()) continue;
            if (IsCurrencyName(item.name) || item.category == "Currency") {
                SKSE::log::info("SkyCollection excluded legacy currency catalog entry: name='{}', plugin='{}', localFormID={:08X}",
                    item.name, item.plugin, item.localFormID);
                continue;
            }
            if (item.runtimeFormID) {
                if (auto* form = RE::TESForm::LookupByID(item.runtimeFormID); form && IsCurrencyItem(form)) {
                    SKSE::log::info("SkyCollection excluded legacy currency catalog form: formID={:08X}, name='{}'",
                        form->GetFormID(), form->GetName() ? form->GetName() : item.name);
                    continue;
                }
            }
            g_items.insert_or_assign(KeyFor(item.plugin, item.localFormID), std::move(item));
        }
    }

    void ClearPendingAcquisitions()
    {
        std::scoped_lock lock(g_queueMutex);
        g_pending.clear();
        g_drainScheduled = false;
    }

    std::string SaveFileScope(std::string_view saveFileKey)
    {
        if (saveFileKey.empty()) return {};
        // Legacy catalog migration only: the key is the old physical save path.
        std::uint64_t hash = 14695981039346656037ull;
        for (unsigned char ch : saveFileKey) {
            if (ch == '/') ch = '\\';
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<unsigned char>(ch - 'A' + 'a');
            hash ^= ch;
            hash *= 1099511628211ull;
        }
        std::array<char, 17> buffer{};
        const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + 16, hash, 16);
        if (error != std::errc{}) return {};
        std::string value(buffer.data(), end);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        if (value.size() < 16) value.insert(value.begin(), 16 - value.size(), '0');
        return "Save_" + value;
    }

    std::string SaveScope(std::string_view saveFileKey)
    {
        return SaveFileScope(saveFileKey);
    }

    bool SaveCatalogLocked()
    {
        const auto path = CatalogPath();
        if (path.empty()) return false;
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        if (error) return false;
        auto temporary = path;
        temporary += L".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) return false;
            for (const auto& [key, item] : g_items) {
                (void)key;
                stream << CleanField(item.plugin) << '|' << Hex(item.localFormID) << '|' << Hex(item.runtimeFormID) << '|'
                       << item.totalFound << '|' << CleanField(item.category) << '|' << (item.unique ? 1 : 0) << '|'
                       << CleanField(item.name) << '|' << CleanField(item.iconPath) << '\n';
            }
            stream.flush();
            if (!stream) return false;
        }
        return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }

    void NotifyPickup(const ItemRecord& item, std::uint32_t count, bool discoveredNow)
    {
        auto callback = g_pickupCallback;
        if (!callback) return;
        callback("{\"item\":" + JsonFor(item, count, discoveredNow) + "}");
    }

    void DrainOnePendingAcquisition();

    void ScheduleDrain()
    {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            std::scoped_lock lock(g_queueMutex);
            g_drainScheduled = false;
            g_pending.clear();
            return;
        }
        tasks->AddTask([] { DrainOnePendingAcquisition(); });
    }

    void DrainOnePendingAcquisition()
    {
        PendingAcquisition pending{};
        {
            std::scoped_lock lock(g_queueMutex);
            if (g_pending.empty()) { g_drainScheduled = false; return; }
            pending = g_pending.front();
            g_pending.pop_front();
        }

        auto* form = RE::TESForm::LookupByID(pending.formID);
        const bool validAcquisition = pending.saveGeneration == g_saveGeneration.load(std::memory_order_acquire) &&
            form && !form->IsDeleted() && !form->IsDestroyed();
        bool acceptedForCurrentSave = false;
        if (validAcquisition && IsCurrencyItem(form)) {
            SKSE::log::info("SkyCollection ignored player currency acquisition: formID={:08X}, name='{}', count={}",
                form->GetFormID(), form->GetName() ? form->GetName() : "Gold", pending.count);
        } else
        if (validAcquisition) {
            ItemRecord item{};
            item.runtimeFormID = form->GetFormID();
            item.localFormID = form->GetLocalFormID();
            item.name = CleanField(form->GetName() ? form->GetName() : "Unknown item");
            item.category = CategoryFor(form);
            item.unique = IsUniqueItem(form);
            if (auto* file = form->GetFile()) item.plugin = CleanField(file->GetFilename());
            if (item.plugin.empty()) item.plugin = "Unknown plugin";

            bool discoveredNow = false;
            {
                std::scoped_lock lock(g_catalogMutex);
                if (pending.saveGeneration == g_saveGeneration.load(std::memory_order_acquire)) {
                    LoadCatalogLocked();
                    const auto key = KeyFor(item.plugin, item.localFormID);
                    const auto found = g_items.find(key);
                    if (found == g_items.end()) {
                        discoveredNow = true;
                        item.totalFound = pending.count;
                        if (g_items.size() >= kMaxCatalogItems) g_items.erase(g_items.begin());
                        g_items.emplace(key, item);
                    } else {
                        found->second.runtimeFormID = item.runtimeFormID;
                        found->second.name = item.name;
                        found->second.category = item.category;
                        found->second.unique = item.unique;
                        found->second.totalFound += pending.count;
                        item = found->second;
                    }
                    const auto cachedIcon = IconPathFor(item.plugin, item.localFormID);
                    std::error_code iconError;
                    if (!cachedIcon.empty() && fs::is_regular_file(cachedIcon, iconError) && !iconError) {
                        item.iconPath = IconFileUrl(cachedIcon);
                    } else {
                        item.iconPath.clear();
                    }
                    g_items[key].iconPath = item.iconPath;
                    if (!g_catalogScope.empty() && !SaveCatalogLocked()) {
                        SKSE::log::warn("SkyCollection could not save its item catalog");
                    }
                    acceptedForCurrentSave = true;
                }
            }
            if (acceptedForCurrentSave) {
                NotifyPickup(item, pending.count, discoveredNow);
            }
        }

        bool more = false;
        {
            std::scoped_lock lock(g_queueMutex);
            more = !g_pending.empty();
            if (!more) g_drainScheduled = false;
        }
        if (more) ScheduleDrain();
    }

    class SkyCollectionEventSink final :
        public RE::BSTEventSink<RE::TESContainerChangedEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* event,
            RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
        {
            if (!event || event->itemCount <= 0 || event->baseObj == 0) return RE::BSEventNotifyControl::kContinue;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || event->newContainer != player->GetFormID()) return RE::BSEventNotifyControl::kContinue;
            const auto* form = RE::TESForm::LookupByID(event->baseObj);
            if (form && IsCurrencyItem(form)) {
                SKSE::log::info("SkyCollection ignored player currency event: formID={:08X}, name='{}', count={}",
                    event->baseObj, form->GetName() ? form->GetName() : "Gold", event->itemCount);
                return RE::BSEventNotifyControl::kContinue;
            }

            bool schedule = false;
            {
                std::scoped_lock lock(g_queueMutex);
                if (g_pending.size() < 256) {
                    g_pending.push_back({ event->baseObj, static_cast<std::uint32_t>(event->itemCount),
                        g_saveGeneration.load(std::memory_order_acquire) });
                }
                if (!g_drainScheduled && !g_pending.empty()) { g_drainScheduled = true; schedule = true; }
            }
            if (schedule) ScheduleDrain();
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    RE::BGSLocation* ResolveDiscoveredMapMarkerLocation(std::string_view markerName)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* currentLocation = player ? player->GetCurrentLocation() : nullptr;
        const auto wantedName = Lower(markerName);

        // Prefer the player's active location or one of its parents when it
        // matches the marker text. This avoids choosing a same-named location
        // from another plugin or worldspace.
        for (auto* candidate = currentLocation; candidate; candidate = candidate->parentLoc) {
            const auto* name = candidate->GetName();
            if (!wantedName.empty() && name && Lower(name) == wantedName) return candidate;
        }

        // The event exposes the marker's display name rather than its form.
        // Resolve an exact loaded location record if the active location has
        // not caught up yet or the marker is represented by a child location.
        if (!wantedName.empty()) {
            if (auto* data = RE::TESDataHandler::GetSingleton()) {
                for (auto* candidate : data->GetFormArray<RE::BGSLocation>()) {
                    if (!candidate) continue;
                    const auto* name = candidate->GetName();
                    if (name && Lower(name) == wantedName) return candidate;
                }
            }
        }
        return currentLocation;
    }

    void RetryMissingLocationScreenshot(RE::BGSLocation* location)
    {
        for (auto* candidate = location; candidate; candidate = candidate->parentLoc) {
            if (!candidate->GetFormID() || !candidate->GetLocalFormID()) continue;
            std::string plugin;
            if (auto* file = candidate->GetFile()) plugin = CleanField(file->GetFilename());
            if (plugin.empty()) continue;
            const auto key = KeyFor(plugin, candidate->GetLocalFormID());
            LocationRecord existing{};
            {
                std::scoped_lock lock(g_catalogMutex);
                const auto found = g_locations.find(key);
                if (found == g_locations.end()) continue;
                existing = found->second;
            }
            const auto path = LocationScreenshotPathFor(plugin, existing.localFormID, existing.name);
            if (HasVisibleScreenshotPixels(path)) {
                if (existing.screenshotPath.empty()) {
                    UpdateSkyCollectionLocationScreenshot(key, path.string());
                }
                return;
            }
            using RequestLocationScreenshot = void(*)(const char*, std::uint32_t, std::uint32_t, const char*);
            const auto counterModule = GetModuleHandleW(L"SkyCounterUI.dll");
            const auto request = counterModule ? reinterpret_cast<RequestLocationScreenshot>(
                GetProcAddress(counterModule, "SkyCounterUI_RequestLocationScreenshot")) : nullptr;
            if (request) {
                request(plugin.c_str(), existing.runtimeFormID, existing.localFormID, existing.name.c_str());
            } else {
                SKSE::log::error("SkyCollection requires SkyCounterUI.dll's location-capture bridge; install and load SkyCounter UI");
            }
            return;
        }
    }

    class SkyCollectionLocationChangeSink final :
        public RE::BSTEventSink<RE::TESActorLocationChangeEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESActorLocationChangeEvent* event,
            RE::BSTEventSource<RE::TESActorLocationChangeEvent>*) override
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (event && player && event->actor.get() == player && event->newLoc) {
                RetryMissingLocationScreenshot(event->newLoc);
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class SkyCollectionLocationDiscoverySink final :
        public RE::BSTEventSink<RE::LocationDiscovery::Event>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::LocationDiscovery::Event* event,
            RE::BSTEventSource<RE::LocationDiscovery::Event>*) override
        {
            if (!event || !event->mapMarkerData) return RE::BSEventNotifyControl::kContinue;

            const auto markerType = event->mapMarkerData->type.get();
            if (markerType == RE::MARKER_TYPE::kNone ||
                static_cast<std::uint32_t>(markerType) >= static_cast<std::uint32_t>(RE::MARKER_TYPE::kTotalLocationTypes)) {
                return RE::BSEventNotifyControl::kContinue;
            }

            const char* markerName = event->mapMarkerData->locationName.GetFullName();
            if (!markerName || !*markerName) return RE::BSEventNotifyControl::kContinue;

            auto* location = ResolveDiscoveredMapMarkerLocation(markerName);
            if (!location) {
                SKSE::log::warn("SkyCollection received discovery for map marker '{}' but could not resolve its location record", markerName);
                return RE::BSEventNotifyControl::kContinue;
            }

            SKSE::log::info("SkyCollection received map-marker discovery for '{}' in worldspace '{}'",
                markerName, event->worldspaceID ? event->worldspaceID : "unknown");
            DiscoverSkyCollectionLocation(location, markerName);
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    SkyCollectionEventSink g_eventSink;
    SkyCollectionLocationDiscoverySink g_locationDiscoverySink;
    SkyCollectionLocationChangeSink g_locationChangeSink;
}

void InstallSkyCollectionEventSink(SkyCounterPickupCallback callback, SkyCollectionIconUpdateCallback iconCallback,
    SkyCollectionLocationUpdateCallback locationCallback)
{
    g_pickupCallback = callback;
    g_iconCallback = iconCallback;
    g_locationCallback = locationCallback;
    if (!g_sinkInstalled) {
        auto* events = RE::ScriptEventSourceHolder::GetSingleton();
        if (!events) {
            SKSE::log::error("SkyCollection could not access Skyrim's script event sources");
        } else {
            events->AddEventSink<RE::TESContainerChangedEvent>(&g_eventSink);
            g_sinkInstalled = true;
        }
    }
    if (!g_locationDiscoverySinkInstalled) {
        if (auto* discoveries = RE::LocationDiscovery::GetEventSource()) {
            discoveries->AddEventSink(&g_locationDiscoverySink);
            g_locationDiscoverySinkInstalled = true;
            SKSE::log::info("SkyCollection subscribed to Skyrim map-marker discovery events");
        } else {
            SKSE::log::error("SkyCollection could not access Skyrim's map-marker discovery event source");
        }
    }
    if (!g_locationChangeSinkInstalled) {
        if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
            events->AddEventSink<RE::TESActorLocationChangeEvent>(&g_locationChangeSink);
            g_locationChangeSinkInstalled = true;
            SKSE::log::info("SkyCollection subscribed to player location changes to request missing captures from SkyCounterUI");
        } else {
            SKSE::log::error("SkyCollection could not subscribe to player location changes for screenshot cache recovery");
        }
    }
    if (SkyCounterUIIconRoot().empty()) {
        SKSE::log::error("SkyCollection could not resolve SkyCounter UI's Skyrim Data icon cache");
    } else {
        SKSE::log::info("SkyCollection reads SkyCounter UI icons from Data\\SKSE\\Plugins\\SkyOS\\Mods\\SkyCounterUI\\Icons");
    }
    {
        std::scoped_lock lock(g_catalogMutex);
        LoadCatalogLocked();
    }
    SKSE::log::info("SkyCollection is listening for inventory additions and first-time map-marker discoveries");
}

void UpdateSkyCollectionIcon(std::uint32_t formID, const std::string& iconUrl)
{
    if (iconUrl.empty()) return;
    std::string payload;
    {
        std::scoped_lock lock(g_catalogMutex);
        LoadCatalogLocked();
        for (auto& [key, item] : g_items) {
            (void)key;
            if (item.runtimeFormID != formID) continue;
            item.iconPath = iconUrl;
            if (!g_catalogScope.empty() && !SaveCatalogLocked()) {
                SKSE::log::warn("SkyCollection could not save the rendered item icon path");
            }
            payload = JsonFor(item);
            break;
        }
    }
    if (!payload.empty() && g_iconCallback) g_iconCallback(payload);
}

std::string GetSkyCollectionCatalogJSON()
{
    std::string result = "[";
    std::size_t emitted = 0;
    bool catalogChanged = false;
    {
        std::scoped_lock lock(g_catalogMutex);
        LoadCatalogLocked();
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        for (auto itemIt = g_items.begin(); itemIt != g_items.end();) {
            auto& item = itemIt->second;
            if (item.category == "Currency" || IsCurrencyName(item.name)) {
                itemIt = g_items.erase(itemIt);
                catalogChanged = true;
                continue;
            }
            if (dataHandler) {
                item.runtimeFormID = ResolveRuntimeFormID(item.localFormID, item.plugin);
                if (item.runtimeFormID) {
                    if (auto* form = RE::TESForm::LookupByID(item.runtimeFormID)) {
                        if (IsCurrencyItem(form)) {
                            SKSE::log::info("SkyCollection purged legacy currency item from its catalog: formID={:08X}, name='{}'",
                                form->GetFormID(), form->GetName() ? form->GetName() : "Gold");
                            itemIt = g_items.erase(itemIt);
                            catalogChanged = true;
                            continue;
                        }
                        const auto resolvedName = CleanField(form->GetName() ? form->GetName() : item.name);
                        const auto resolvedCategory = CategoryFor(form);
                        const auto resolvedUnique = IsUniqueItem(form);
                        if (item.name != resolvedName || item.category != resolvedCategory || item.unique != resolvedUnique) {
                            item.name = resolvedName;
                            item.category = resolvedCategory;
                            item.unique = resolvedUnique;
                            catalogChanged = true;
                        }
                    }
                }
            }
            const auto iconPath = IconPathFor(item.plugin, item.localFormID);
            std::error_code error;
            const bool iconExists = !iconPath.empty() && fs::is_regular_file(iconPath, error) && !error;
            if (iconExists) {
                const auto iconUrl = IconFileUrl(iconPath);
                if (!iconUrl.empty() && item.iconPath != iconUrl) {
                    item.iconPath = iconUrl;
                    catalogChanged = true;
                }
            } else if (!item.iconPath.empty()) {
                item.iconPath.clear();
                catalogChanged = true;
            }
            const auto entry = JsonFor(item);
            if (result.size() + entry.size() + 2 > kMaxCatalogJsonBytes) break;
            if (emitted++) result.push_back(',');
            result += entry;
            ++itemIt;
        }
        if (catalogChanged && !g_catalogScope.empty() && !SaveCatalogLocked()) {
            SKSE::log::warn("SkyCollection could not update cached icon paths in its item catalog");
        }
    }
    result.push_back(']');
    return result;
}

std::string GetSkyCollectionLocationsJSON()
{
    std::string result = "[";
    std::size_t emitted = 0;
    std::scoped_lock lock(g_catalogMutex);
    for (const auto& [key, location] : g_locations) {
        (void)key;
        const auto entry = JsonFor(location);
        if (result.size() + entry.size() + 2 > kMaxCatalogJsonBytes) break;
        if (emitted++) result.push_back(',');
        result += entry;
    }
    result.push_back(']');
    return result;
}

void DiscoverSkyCollectionLocation(RE::BGSLocation* location, std::string_view discoveredName)
{
    if (!location) return;
    LocationRecord record{};
    record.runtimeFormID = location->GetFormID();
    record.localFormID = location->GetLocalFormID();
    const auto* formName = location->GetName();
    record.name = CleanField(!discoveredName.empty() ? discoveredName : (formName ? formName : ""));
    if (record.name.empty()) {
        const auto* editorID = location->GetFormEditorID();
        record.name = CleanField(editorID && *editorID ? editorID : "Unknown location");
    }
    if (auto* file = location->GetFile()) record.plugin = CleanField(file->GetFilename());
    if (record.plugin.empty()) record.plugin = "Unknown plugin";
    if (!record.runtimeFormID || !record.localFormID) return;

    const auto key = KeyFor(record.plugin, record.localFormID);
    const auto screenshotPath = LocationScreenshotPathFor(record.plugin, record.localFormID, record.name);
    const bool screenshotCached = HasVisibleScreenshotPixels(screenshotPath);
    if (screenshotCached) {
        record.screenshotPath = screenshotPath.string();
    }

    std::string payload;
    bool discovered = false;
    bool captureMissingScreenshot = false;
    bool cachedImageRestored = false;
    {
        std::scoped_lock lock(g_catalogMutex);
        auto found = g_locations.find(key);
        if (found == g_locations.end()) {
            discovered = true;
            if (g_locations.size() >= kMaxCatalogItems) g_locations.erase(g_locations.begin());
            g_locations.emplace(key, record);
            payload = JsonFor(record, true);
            captureMissingScreenshot = !screenshotCached;
        } else if (screenshotCached && found->second.screenshotPath.empty()) {
            found->second.screenshotPath = screenshotPath.string();
            payload = JsonFor(found->second);
            cachedImageRestored = true;
        } else if (!screenshotCached && !found->second.screenshotPath.empty()) {
            found->second.screenshotPath.clear();
            payload = JsonFor(found->second);
            captureMissingScreenshot = true;
        } else if (!screenshotCached) {
            captureMissingScreenshot = true;
        }
    }

    auto* tasks = SKSE::GetTaskInterface();
    if (discovered) {
        SKSE::log::info("SkyCollection discovered location '{}' from {}", record.name, record.plugin);
        const auto saveGeneration = g_saveGeneration.load(std::memory_order_acquire);
        if (tasks) {
            tasks->AddTask([payload = std::move(payload), saveGeneration] {
                if (saveGeneration == g_saveGeneration.load(std::memory_order_acquire) && g_locationCallback) {
                    g_locationCallback(payload, true);
                }
            });
        } else if (g_locationCallback) {
            g_locationCallback(payload, true);
        }
    } else if (cachedImageRestored && tasks) {
        tasks->AddUITask([payload = std::move(payload)] {
            if (g_locationCallback) g_locationCallback(payload, false);
        });
    } else if (!payload.empty() && tasks) {
        tasks->AddUITask([payload = std::move(payload)] {
            if (g_locationCallback) g_locationCallback(payload, false);
        });
    }
    if (!discovered && !captureMissingScreenshot) return;
    if (captureMissingScreenshot && screenshotPath.empty()) {
        SKSE::log::warn("SkyCollection could not resolve a screenshot path for location '{}'", record.name);
        return;
    }
    // SkyCounterUI owns screenshot capture and writes the shared cache. This
    // catalog only reads that path and persists the location metadata.
}

void UpdateSkyCollectionLocationScreenshot(const std::string& key, const std::string& imagePath)
{
    if (key.empty() || imagePath.empty()) return;
    std::string payload;
    {
        std::scoped_lock lock(g_catalogMutex);
        const auto found = g_locations.find(key);
        if (found == g_locations.end()) return;
        found->second.screenshotPath = imagePath;
        payload = JsonFor(found->second);
    }
    if (g_locationCallback) g_locationCallback(payload, false);
}

void ResetSkyCollectionForSaveLoad()
{
    ClearPendingAcquisitions();
    std::scoped_lock lock(g_catalogMutex);
    ++g_saveGeneration;
    g_catalogScope.clear();
    g_items.clear();
    g_locations.clear();
    g_catalogLoaded = true;
    g_catalogRestored = false;
}

bool ClearSkyCollectionForActiveSave()
{
    ClearPendingAcquisitions();
    std::scoped_lock lock(g_catalogMutex);
    ++g_saveGeneration;
    g_items.clear();
    g_locations.clear();
    g_catalogLoaded = true;
    g_catalogRestored = true;
    if (!g_catalogScope.empty() && !SaveCatalogLocked()) {
        SKSE::log::warn("SkyCollection cleared in memory, but could not update its legacy catalog file");
        return false;
    }
    SKSE::log::info("SkyCollection reset the active save collection; cached icons and location screenshots were preserved");
    return true;
}

void BeginSkyCollectionNewGame()
{
    ResetSkyCollectionForSaveLoad();
    SKSE::log::info("SkyCollection started a new empty save catalog");
}

void LoadSkyCollectionLegacyCatalog(const std::string& physicalSavePath)
{
    if (physicalSavePath.empty()) return;
    std::scoped_lock lock(g_catalogMutex);
    if (g_catalogRestored) return;
    const auto legacyScope = SaveScope("path:" + physicalSavePath);
    if (legacyScope.empty()) return;
    g_items.clear();
    g_locations.clear();
    g_catalogScope = legacyScope;
    g_catalogLoaded = false;
    LoadCatalogLocked();
    g_catalogScope.clear();
    g_catalogLoaded = true;
    g_catalogRestored = true;
    SKSE::log::info("SkyCollection imported {} items from this save's legacy catalog", g_items.size());
}

void SaveSkyCollectionSerializationRecord(SKSE::SerializationInterface* intfc)
{
    if (!intfc) {
        SKSE::log::error("SkyCollection could not save discoveries: serialization interface is unavailable");
        return;
    }

    std::vector<ItemRecord> snapshot;
    std::vector<LocationRecord> locationSnapshot;
    {
        std::scoped_lock lock(g_catalogMutex);
        LoadCatalogLocked();
        snapshot.reserve(g_items.size());
        locationSnapshot.reserve(g_locations.size());
        for (const auto& [key, item] : g_items) {
            (void)key;
            snapshot.push_back(item);
        }
        for (const auto& [key, location] : g_locations) {
            (void)key;
            locationSnapshot.push_back(location);
        }
    }

    std::vector<std::byte> record;
    record.reserve(4 + snapshot.size() * 128);
    const auto count = static_cast<std::uint32_t>(snapshot.size());
    bool valid = AppendBinary(record, count);
    for (const auto& item : snapshot) {
        const auto localFormID = item.localFormID;
        const auto foundCount = item.totalFound;
        const std::uint8_t unique = item.unique ? 1 : 0;
        valid = valid && AppendBinary(record, localFormID) && AppendBinary(record, foundCount) &&
            AppendBinary(record, unique) && AppendString(record, item.plugin, 512) &&
            AppendString(record, item.name, 2048) && AppendString(record, item.category, 128) &&
            AppendString(record, item.iconPath, 4096);
        if (!valid) break;
    }
    const auto locationCount = static_cast<std::uint32_t>(locationSnapshot.size());
    valid = valid && AppendBinary(record, locationCount);
    for (const auto& location : locationSnapshot) {
        valid = valid && AppendBinary(record, location.localFormID) &&
            AppendString(record, location.plugin, 512) && AppendString(record, location.name, 2048);
        if (!valid) break;
    }
    if (!valid || record.size() > kMaxCollectionRecordBytes) {
        SKSE::log::error("SkyCollection could not serialize its {} items and {} locations within the save-record limits",
            snapshot.size(), locationSnapshot.size());
        return;
    }
    if (!intfc->OpenRecord(kCollectionRecordType, kCollectionRecordVersion) ||
        !intfc->WriteRecordData(record.data(), static_cast<std::uint32_t>(record.size()))) {
        SKSE::log::error("SkyCollection failed to write {} discoveries into the save", snapshot.size());
        return;
    }
    SKSE::log::info("SkyCollection saved {} discovered items and {} locations in the active save",
        snapshot.size(), locationSnapshot.size());
}

bool LoadSkyCollectionSerializationRecord(SKSE::SerializationInterface* intfc,
    std::uint32_t type, std::uint32_t version, std::uint32_t length)
{
    if (type != kCollectionRecordType) return false;
    if (!intfc) return true;
    if ((version != 1 && version != kCollectionRecordVersion) || length < sizeof(std::uint32_t) ||
        length > kMaxCollectionRecordBytes) {
        DrainSerializationRecord(intfc, length);
        SKSE::log::warn("SkyCollection skipped unsupported save record version {} ({} bytes)", version, length);
        return true;
    }

    std::vector<std::byte> record(length);
    if (intfc->ReadRecordData(record.data(), length) != length) {
        SKSE::log::error("SkyCollection could not read its save catalog record");
        return true;
    }
    const std::byte* cursor = record.data();
    const auto* end = cursor + record.size();
    std::uint32_t count{};
    if (!ReadBinary(cursor, end, count) || count > kMaxCatalogItems) {
        SKSE::log::warn("SkyCollection rejected invalid discovered-item count in save record");
        return true;
    }

    std::map<std::string, ItemRecord, std::less<>> restored;
    for (std::uint32_t index = 0; index < count; ++index) {
        ItemRecord item{};
        std::uint8_t unique{};
        if (!ReadBinary(cursor, end, item.localFormID) || !ReadBinary(cursor, end, item.totalFound) ||
            !ReadBinary(cursor, end, unique) || !ReadString(cursor, end, item.plugin, 512) ||
            !ReadString(cursor, end, item.name, 2048) || !ReadString(cursor, end, item.category, 128) ||
            !ReadString(cursor, end, item.iconPath, 4096) || item.plugin.empty() || item.name.empty()) {
            SKSE::log::warn("SkyCollection stopped reading a malformed item in its save record at entry {}", index);
            return true;
        }
        item.unique = unique != 0;
        if (IsCurrencyName(item.name) || item.category == "Currency") {
            SKSE::log::info("SkyCollection excluded legacy currency save entry: name='{}', plugin='{}', localFormID={:08X}",
                item.name, item.plugin, item.localFormID);
            continue;
        }
        item.runtimeFormID = ResolveRuntimeFormID(item.localFormID, item.plugin);
        if (item.runtimeFormID) {
            if (auto* form = RE::TESForm::LookupByID(item.runtimeFormID)) {
                if (IsCurrencyItem(form)) {
                    SKSE::log::info("SkyCollection excluded legacy currency save form: formID={:08X}, name='{}'",
                        form->GetFormID(), form->GetName() ? form->GetName() : item.name);
                    continue;
                }
                item.name = CleanField(form->GetName() ? form->GetName() : item.name);
                item.category = CategoryFor(form);
                item.unique = IsUniqueItem(form);
            }
        }
        restored.insert_or_assign(KeyFor(item.plugin, item.localFormID), std::move(item));
    }
    std::map<std::string, LocationRecord, std::less<>> restoredLocations;
    if (version >= 2) {
        std::uint32_t locationCount{};
        if (!ReadBinary(cursor, end, locationCount) || locationCount > kMaxCatalogItems) {
            SKSE::log::warn("SkyCollection rejected invalid discovered-location count in save record");
            return true;
        }
        for (std::uint32_t index = 0; index < locationCount; ++index) {
            LocationRecord location{};
            if (!ReadBinary(cursor, end, location.localFormID) ||
                !ReadString(cursor, end, location.plugin, 512) || !ReadString(cursor, end, location.name, 2048) ||
                !location.localFormID || location.plugin.empty() || location.name.empty()) {
                SKSE::log::warn("SkyCollection stopped reading a malformed location in its save record at entry {}", index);
                return true;
            }
            location.runtimeFormID = ResolveRuntimeFormID(location.localFormID, location.plugin);
            const auto screenshotPath = LocationScreenshotPathFor(location.plugin,
                location.localFormID, location.name);
            if (HasVisibleScreenshotPixels(screenshotPath)) {
                location.screenshotPath = screenshotPath.string();
            }
            restoredLocations.insert_or_assign(KeyFor(location.plugin, location.localFormID), std::move(location));
        }
    }
    if (cursor != end) {
        SKSE::log::warn("SkyCollection ignored {} trailing bytes in its save record", end - cursor);
    }

    std::size_t restoredCount{};
    std::size_t restoredLocationCount{};
    {
        std::scoped_lock lock(g_catalogMutex);
        g_items = std::move(restored);
        g_locations = std::move(restoredLocations);
        g_catalogScope.clear();
        g_catalogLoaded = true;
        g_catalogRestored = true;
        restoredCount = g_items.size();
        restoredLocationCount = g_locations.size();
    }
    SKSE::log::info("SkyCollection loaded {} discovered items and {} locations from the active save",
        restoredCount, restoredLocationCount);
    return true;
}

namespace {
    void SaveSkyCollection(SKSE::SerializationInterface* intfc)
    {
        SaveSkyCollectionSerializationRecord(intfc);
    }

    void LoadSkyCollection(SKSE::SerializationInterface* intfc)
    {
        if (!intfc) {
            SKSE::log::error("SkyCollection could not load its catalog: serialization interface is unavailable");
            return;
        }

        std::uint32_t type{}, version{}, length{};
        while (intfc->GetNextRecordInfo(type, version, length)) {
            if (LoadSkyCollectionSerializationRecord(intfc, type, version, length)) continue;
            DrainSerializationRecord(intfc, length);
            SKSE::log::warn("SkyCollection skipped unrecognized save record {}", type);
        }
    }

    void RevertSkyCollection(SKSE::SerializationInterface*)
    {
        ResetSkyCollectionForSaveLoad();
    }
}

void RegisterSkyCollectionSerialization()
{
    const auto* serialization = SKSE::GetSerializationInterface();
    if (!serialization) {
        SKSE::log::error("SkyCollection could not access SKSE serialization");
        return;
    }
    serialization->SetUniqueID(kCollectionPluginID);
    serialization->SetSaveCallback(SaveSkyCollection);
    serialization->SetLoadCallback(LoadSkyCollection);
    serialization->SetRevertCallback(RevertSkyCollection);
}
}

extern "C" __declspec(dllexport) bool SkyCollection_LoadLegacySaveRecord(
    SKSE::SerializationInterface* intfc, std::uint32_t type, std::uint32_t version, std::uint32_t length)
{
    return SkyOS::LoadSkyCollectionSerializationRecord(intfc, type, version, length);
}
