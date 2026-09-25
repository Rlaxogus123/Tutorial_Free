#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/GJAccountManager.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/MusicDownloadManager.hpp>
#include <Geode/binding/SFXInfoObject.hpp>
#include <Geode/binding/SimplePlayer.hpp>
#include <Geode/binding/FLAlertLayer.hpp>
#include <Geode/binding/FLAlertLayerProtocol.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/utils/Keyboard.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/binding/Slider.hpp>
#include <Geode/binding/SliderThumb.hpp>

#include "FirebaseConfig.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <unordered_map>

using namespace geode::prelude;

static constexpr char const* FIREBASE_DATABASE_URL =
    firebase_config::DATABASE_URL;

static float clampLevelPercent(float percent) {
    if (!std::isfinite(percent)) {
        return 0.f;
    }

    return std::clamp(percent, 0.f, 100.f);
}

static float getCurrentLevelPercent(PlayLayer* playLayer) {
    if (!playLayer || !playLayer->m_level || !playLayer->m_player1) {
        return 0.f;
    }

    return clampLevelPercent(playLayer->getCurrentPercent());
}

static std::string formatPlatformerTime(int64_t milliseconds) {
    milliseconds = std::max<int64_t>(0, milliseconds);

    auto const hours = milliseconds / 3'600'000;
    auto const minutes = (milliseconds / 60'000) % 60;
    auto const seconds = (milliseconds / 1'000) % 60;
    auto const millis = milliseconds % 1'000;

    if (hours > 0) {
        return fmt::format(
            "{}:{:02}:{:02}.{:03}",
            hours,
            minutes,
            seconds,
            millis
        );
    }
    return fmt::format("{}:{:02}.{:03}", minutes, seconds, millis);
}

struct SectionData {
    float startPercent;
    int difficulty;
    std::string partName;
    int faces;
    std::string customImage;
    bool deathSoundOverride = false;
    std::string deathSound = "explode_11.ogg";
    bool deathSoundCustom = false;
    float deathSoundVolume = 1.f;
};

enum class FlagPercentSource {
    Fixed,
    PersonalBest,
};

struct FlagData {
    std::string id;
    std::string label;
    float percent = 0.f;
    FlagPercentSource source = FlagPercentSource::Fixed;
    std::string iconFrame = "GJ_arrow_03_001.png";
    int passedColor = 0x66FF38;
    float offsetX = 0.f;
    float offsetY = 0.f;
    float scale = 1.f;
    float opacity = 1.f;
};

static constexpr float FLAG_DETAIL_OFFSET_MIN = -30.f;
static constexpr float FLAG_DETAIL_OFFSET_MAX = 30.f;
static constexpr float FLAG_DETAIL_SCALE_MIN = 0.5f;
static constexpr float FLAG_DETAIL_SCALE_MAX = 2.f;
static constexpr float FLAG_DETAIL_OPACITY_MIN = 0.f;
static constexpr float FLAG_DETAIL_OPACITY_MAX = 1.f;

static float sanitizeFlagDetailValue(
    float value,
    float minimum,
    float maximum,
    float fallback
) {
    return std::isfinite(value)
        ? std::clamp(value, minimum, maximum)
        : fallback;
}

class FaceSelectPopup;
class FlagDataListPopup;
class DownloadMenu;
class ServerMapListPopup;
class MyDataListPopup;
class SectionDifficultyGraphPopup;
class DeathSoundPopup;

static void setPopupControlsEnabled(CCNode* node, bool enabled) {
    if (!node) return;

    if (auto input = typeinfo_cast<TextInput*>(node)) {
        input->setEnabled(enabled);
    }
    if (auto menu = typeinfo_cast<CCMenu*>(node)) {
        menu->setTouchEnabled(enabled);
    }
    if (auto scroll = typeinfo_cast<ScrollLayer*>(node)) {
        scroll->setTouchEnabled(enabled);
    }

    for (auto child : node->getChildrenExt()) {
        setPopupControlsEnabled(child, enabled);
    }
}

static void syncActiveFlagProgressBar(
    std::vector<FlagData> const& flags
);

// JSON

static matjson::Value sectionToJson(SectionData const& s) {
    auto obj = matjson::Value::object();

    obj["start"] = s.startPercent;
    obj["difficulty"] = s.difficulty;
    obj["partName"] = s.partName;
    obj["faces"] = s.faces;
    obj["customImage"] = s.customImage;
    obj["deathSoundOverride"] = s.deathSoundOverride;
    obj["deathSound"] = s.deathSound;
    obj["deathSoundCustom"] = s.deathSoundCustom;
    obj["deathSoundVolume"] = std::clamp(s.deathSoundVolume, 0.f, 1.f);

    return obj;
}

static SectionData sectionFromJson(matjson::Value const& v) {
    return {
        static_cast<float>(v["start"].asDouble().unwrapOr(0.0)),
        static_cast<int>(v["difficulty"].asInt().unwrapOr(0)),
        v["partName"].asString().unwrapOr(""),
        static_cast<int>(v["faces"].asInt().unwrapOr(0)),
        v["customImage"].asString().unwrapOr(""),
        v["deathSoundOverride"].asBool().unwrapOr(false),
        v["deathSound"].asString().unwrapOr("explode_11.ogg"),
        v["deathSoundCustom"].asBool().unwrapOr(false),
        static_cast<float>(std::clamp(
            v["deathSoundVolume"].asDouble().unwrapOr(1.0),
            0.0,
            1.0
        ))
    };
}

// Level name key

static std::string trimWhitespace(std::string value) {
    while (
        !value.empty() &&
        std::isspace(static_cast<unsigned char>(value.front()))
    ) {
        value.erase(value.begin());
    }
    while (
        !value.empty() &&
        std::isspace(static_cast<unsigned char>(value.back()))
    ) {
        value.pop_back();
    }
    return value;
}

static std::string normalizeLevelName(std::string name) {
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) {
        name.erase(name.begin());
    }

    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) {
        name.pop_back();
    }

    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        if (c >= 'A' && c <= 'Z') return static_cast<char>(c + ('a' - 'A'));
        return static_cast<char>(c);
    });

    return name;
}

static constexpr std::string_view SECTION_DATA_KEY_PREFIX =
    "sections-name-";

static constexpr std::string_view SECTION_GO_KEY_PREFIX =
    "section-go-percent-name-";

static constexpr std::string_view FLAG_DATA_KEY_PREFIX =
    "flag-data-name-";

static constexpr std::string_view LEGACY_SECTION_DATA_KEY_PREFIX =
    "sections-";

static constexpr std::string_view LEGACY_GO_KEY_SUFFIX =
    "-go-percent";

static constexpr char const* SECTION_MAP_NAMES_KEY =
    "section-data-map-names-v1";

static constexpr char const* PROGRESS_DATA_UPGRADE_BACKUP_KEY =
    "progress-data-upgrade-backup-v102-v1";

static std::string getFlagKeyForSectionKey(std::string_view sectionKey);

static bool isLegacyUnassignedSectionKey(std::string_view key) {
    return
        key.starts_with(LEGACY_SECTION_DATA_KEY_PREFIX) &&
        !key.starts_with(SECTION_DATA_KEY_PREFIX);
}

// v1.0.2 already uses the same `sections-name-*` arrays as this version. Keep
// one exact, read-only snapshot anyway, before any later migration or explicit
// edit can touch progress data. Unknown keys are intentionally preserved.
static void ensureProgressDataUpgradeBackup() {
    auto& saved = Mod::get()->getSaveContainer();
    if (
        !saved.isObject() ||
        saved.contains(PROGRESS_DATA_UPGRADE_BACKUP_KEY)
    ) {
        return;
    }

    auto backup = matjson::Value::object();
    std::size_t entryCount = 0;
    for (auto const& item : saved) {
        auto const key = item.getKey().value_or("");
        auto const isProgressData =
            key.starts_with(LEGACY_SECTION_DATA_KEY_PREFIX) ||
            key.starts_with(SECTION_GO_KEY_PREFIX) ||
            key.starts_with(FLAG_DATA_KEY_PREFIX) ||
            key == SECTION_MAP_NAMES_KEY ||
            key == "difficulty-user-images";
        if (!isProgressData) continue;

        backup[key] = item;
        entryCount++;
    }

    if (entryCount == 0) return;
    saved[PROGRESS_DATA_UPGRADE_BACKUP_KEY] = std::move(backup);
    log::info(
        "Preserved {} progress-data entries in the v1.0.2 upgrade backup",
        entryCount
    );
}

static std::string getSectionKeyForMapName(std::string mapName) {
    auto normalized = normalizeLevelName(std::move(mapName));
    if (normalized.empty()) return "";
    return fmt::format("{}{}", SECTION_DATA_KEY_PREFIX, normalized);
}

static std::string getCurrentMapName() {
    auto playLayer = PlayLayer::get();
    if (!playLayer || !playLayer->m_level) return "";
    return trimWhitespace(std::string(playLayer->m_level->m_levelName));
}

static std::string getNameBasedLevelKey() {
    auto mapName = getCurrentMapName();
    if (mapName.empty()) {
        auto pl = PlayLayer::get();
        if (pl && pl->m_level) {
            return "sections-name-empty";
        }
        return "sections-name-unknown";
    }

    return getSectionKeyForMapName(std::move(mapName));
}

// Save / Load

static void sortSections(std::vector<SectionData>& sections) {
    std::sort(sections.begin(), sections.end(), [](auto const& a, auto const& b) {
        return a.startPercent < b.startPercent;
    });
}

static void syncActiveSectionProgressBar(
    std::vector<SectionData> const& sections
);

static matjson::Value sectionsToJson(
    std::vector<SectionData> const& sections
) {
    auto array = matjson::Value::array();
    for (auto const& section : sections) {
        array.push(sectionToJson(section));
    }
    return array;
}

static std::vector<SectionData> sectionsFromJson(
    matjson::Value const& value
) {
    std::vector<SectionData> result;
    if (!value.isArray()) return result;

    for (auto const& item : value) {
        result.push_back(sectionFromJson(item));
    }

    sortSections(result);
    return result;
}

static bool isSupportedStoredSectionArray(matjson::Value const& value) {
    if (!value.isArray()) return false;
    for (auto const& item : value) {
        if (
            !item.isObject() ||
            !item.contains("start") ||
            !item["start"].isNumber() ||
            !item.contains("difficulty") ||
            !item["difficulty"].isNumber() ||
            !item.contains("partName") ||
            !item["partName"].isString() ||
            !item.contains("faces") ||
            !item["faces"].isNumber() ||
            (
                item.contains("customImage") &&
                !item["customImage"].isNull() &&
                !item["customImage"].isString()
            )
        ) {
            return false;
        }
    }
    return true;
}

static std::string getGoPercentKeyForSectionKey(
    std::string_view sectionKey
) {
    if (!sectionKey.starts_with(SECTION_DATA_KEY_PREFIX)) return "";
    return fmt::format(
        "{}{}",
        SECTION_GO_KEY_PREFIX,
        sectionKey.substr(SECTION_DATA_KEY_PREFIX.size())
    );
}

static std::string getLegacyGoPercentKeyForSectionKey(
    std::string_view sectionKey
) {
    return fmt::format("{}{}", sectionKey, LEGACY_GO_KEY_SUFFIX);
}

// A legacy Go key for map "foo" is identical to the SectionData key for map
// "foo-go-percent". Move the numeric legacy value before using that key for
// SectionData so neither dataset can overwrite the other.
static void makeRoomForSectionKey(std::string const& sectionKey) {
    auto& saved = Mod::get()->getSaveContainer();
    if (
        !saved.isObject() ||
        !saved.contains(sectionKey) ||
        !saved[sectionKey].isNumber() ||
        !sectionKey.starts_with(SECTION_DATA_KEY_PREFIX) ||
        !sectionKey.ends_with(LEGACY_GO_KEY_SUFFIX)
    ) {
        return;
    }

    auto ownerSectionKey = sectionKey.substr(
        0,
        sectionKey.size() - LEGACY_GO_KEY_SUFFIX.size()
    );
    auto migratedKey = getGoPercentKeyForSectionKey(ownerSectionKey);
    if (migratedKey.empty()) return;

    if (!saved.contains(migratedKey) || !saved[migratedKey].isNumber()) {
        saved[migratedKey] = saved[sectionKey];
    }
    saved.erase(sectionKey);
}

static void setSectionMapDisplayName(
    std::string const& sectionKey,
    std::string mapName
) {
    mapName = trimWhitespace(std::move(mapName));
    if (
        mapName.empty() ||
        getSectionKeyForMapName(mapName) != sectionKey
    ) {
        return;
    }

    auto& saved = Mod::get()->getSaveContainer();
    if (!saved.isObject()) return;
    if (
        !saved.contains(SECTION_MAP_NAMES_KEY) ||
        !saved[SECTION_MAP_NAMES_KEY].isObject()
    ) {
        saved[SECTION_MAP_NAMES_KEY] = matjson::Value::object();
    }
    saved[SECTION_MAP_NAMES_KEY][sectionKey] = std::move(mapName);
}

static void removeSectionMapDisplayName(std::string const& sectionKey) {
    auto& saved = Mod::get()->getSaveContainer();
    if (
        saved.isObject() &&
        saved.contains(SECTION_MAP_NAMES_KEY) &&
        saved[SECTION_MAP_NAMES_KEY].isObject()
    ) {
        saved[SECTION_MAP_NAMES_KEY].erase(sectionKey);
    }
}

static std::string getSectionMapDisplayName(
    std::string const& sectionKey
) {
    auto& saved = Mod::get()->getSaveContainer();
    if (
        saved.isObject() &&
        saved.contains(SECTION_MAP_NAMES_KEY) &&
        saved[SECTION_MAP_NAMES_KEY].isObject() &&
        saved[SECTION_MAP_NAMES_KEY].contains(sectionKey)
    ) {
        auto name = trimWhitespace(
            saved[SECTION_MAP_NAMES_KEY][sectionKey]
                .asString()
                .unwrapOr("")
        );
        if (!name.empty() && getSectionKeyForMapName(name) == sectionKey) {
            return name;
        }
    }

    if (isLegacyUnassignedSectionKey(sectionKey)) {
        return fmt::format(
            "Legacy / Unassigned ({})",
            sectionKey.substr(LEGACY_SECTION_DATA_KEY_PREFIX.size())
        );
    }
    if (sectionKey.starts_with(SECTION_DATA_KEY_PREFIX)) {
        return sectionKey.substr(SECTION_DATA_KEY_PREFIX.size());
    }
    return sectionKey;
}

static std::vector<SectionData> loadSectionsForKey(
    std::string const& sectionKey
) {
    ensureProgressDataUpgradeBackup();
    makeRoomForSectionKey(sectionKey);

    auto& saved = Mod::get()->getSaveContainer();
    if (
        !saved.isObject() ||
        !saved.contains(sectionKey) ||
        !saved[sectionKey].isArray()
    ) {
        return {};
    }
    return sectionsFromJson(saved[sectionKey]);
}

static bool saveSectionsForKey(
    std::string const& sectionKey,
    std::vector<SectionData> const& sections,
    std::string const& mapName
) {
    if (sectionKey.empty()) return false;
    ensureProgressDataUpgradeBackup();
    makeRoomForSectionKey(sectionKey);

    auto& saved = Mod::get()->getSaveContainer();
    if (!saved.isObject()) return false;
    if (
        saved.contains(sectionKey) &&
        !saved[sectionKey].isNull() &&
        !isSupportedStoredSectionArray(saved[sectionKey])
    ) {
        log::error(
            "Refusing to overwrite unsupported SectionData at key '{}'",
            sectionKey
        );
        return false;
    }

    if (sections.empty()) {
        saved.erase(sectionKey);
        auto const flagKey = getFlagKeyForSectionKey(sectionKey);
        if (
            flagKey.empty() ||
            !saved.contains(flagKey) ||
            !saved[flagKey].isArray() ||
            saved[flagKey].size() == 0
        ) {
            removeSectionMapDisplayName(sectionKey);
        }
        return true;
    }

    saved[sectionKey] = sectionsToJson(sections);
    setSectionMapDisplayName(sectionKey, mapName);
    return true;
}

static std::optional<float> loadStoredGoPercent(
    std::string const& sectionKey
) {
    auto& saved = Mod::get()->getSaveContainer();
    if (!saved.isObject()) return std::nullopt;

    auto key = getGoPercentKeyForSectionKey(sectionKey);
    if (
        !key.empty() &&
        saved.contains(key) &&
        saved[key].isNumber()
    ) {
        auto value = static_cast<float>(
            saved[key].asDouble().unwrapOr(0.0)
        );
        return std::isfinite(value)
            ? std::optional<float>(std::clamp(value, 0.f, 100.f))
            : std::optional<float>(0.f);
    }

    auto legacyKey = getLegacyGoPercentKeyForSectionKey(sectionKey);
    if (saved.contains(legacyKey) && saved[legacyKey].isNumber()) {
        auto value = static_cast<float>(
            saved[legacyKey].asDouble().unwrapOr(0.0)
        );
        value = std::isfinite(value)
            ? std::clamp(value, 0.f, 100.f)
            : 0.f;
        if (!key.empty()) saved[key] = value;
        saved.erase(legacyKey);
        return value;
    }
    return std::nullopt;
}

static void saveStoredGoPercent(
    std::string const& sectionKey,
    std::optional<float> value
) {
    auto& saved = Mod::get()->getSaveContainer();
    if (!saved.isObject()) return;

    auto key = getGoPercentKeyForSectionKey(sectionKey);
    if (!key.empty()) {
        if (value.has_value()) {
            saved[key] = std::clamp(value.value(), 0.f, 100.f);
        }
        else {
            saved.erase(key);
        }
    }

    auto legacyKey = getLegacyGoPercentKeyForSectionKey(sectionKey);
    if (saved.contains(legacyKey) && saved[legacyKey].isNumber()) {
        saved.erase(legacyKey);
    }
}

static std::vector<SectionData> loadSections() {
    auto key = getNameBasedLevelKey();
    auto result = loadSectionsForKey(key);
    if (!result.empty()) {
        setSectionMapDisplayName(key, getCurrentMapName());
    }
    return result;
}

static bool saveSections(std::vector<SectionData> const& sections) {
    auto key = getNameBasedLevelKey();
    if (!saveSectionsForKey(key, sections, getCurrentMapName())) {
        return false;
    }
    syncActiveSectionProgressBar(sections);
    return true;
}

struct LocalSectionDataset {
    std::string sectionKey;
    std::string mapName;
    std::vector<SectionData> sections;
};

static std::vector<LocalSectionDataset> loadLocalSectionDatasets() {
    std::vector<LocalSectionDataset> datasets;
    ensureProgressDataUpgradeBackup();
    auto& saved = Mod::get()->getSaveContainer();
    if (!saved.isObject()) return datasets;

    std::vector<std::string> sectionKeys;
    for (auto const& item : saved) {
        auto key = item.getKey().value_or("");
        std::string sectionKey;
        if (
            key.starts_with(SECTION_DATA_KEY_PREFIX) &&
            key.size() > SECTION_DATA_KEY_PREFIX.size() &&
            item.isArray() &&
            item.size() > 0
        ) {
            sectionKey = key;
        }
        else if (
            isLegacyUnassignedSectionKey(key) &&
            item.isArray() &&
            item.size() > 0
        ) {
            sectionKey = key;
        }
        else if (
            key.starts_with(FLAG_DATA_KEY_PREFIX) &&
            key.size() > FLAG_DATA_KEY_PREFIX.size() &&
            item.isArray() &&
            item.size() > 0
        ) {
            sectionKey = fmt::format(
                "{}{}",
                SECTION_DATA_KEY_PREFIX,
                key.substr(FLAG_DATA_KEY_PREFIX.size())
            );
        }
        if (
            sectionKey.empty() ||
            std::find(sectionKeys.begin(), sectionKeys.end(), sectionKey) !=
                sectionKeys.end()
        ) continue;
        sectionKeys.push_back(std::move(sectionKey));
    }

    for (auto const& sectionKey : sectionKeys) {
        datasets.push_back({
            sectionKey,
            getSectionMapDisplayName(sectionKey),
            loadSectionsForKey(sectionKey)
        });
    }

    std::sort(
        datasets.begin(),
        datasets.end(),
        [](auto const& a, auto const& b) {
            auto left = normalizeLevelName(a.mapName);
            auto right = normalizeLevelName(b.mapName);
            if (left != right) return left < right;
            return a.mapName < b.mapName;
        }
    );
    return datasets;
}

static constexpr char const* SETTING_SHOW_FLAGS =
    "show-flag-data";

static constexpr char const* SETTING_SHOW_PROGRESS_HUD =
    "show-progress-hud";

static constexpr char const* SETTING_SHOW_BEST =
    "show-best-hud";

static constexpr char const* SETTING_SHOW_DIFFICULTY =
    "show-difficulty-hud";

static constexpr char const* SETTING_SHOW_LABEL =
    "show-label-hud";

static constexpr char const* SETTING_SHOW_TOTAL_PERCENT =
    "show-total-percent-hud";

static constexpr char const* SETTING_SHOW_PART_PERCENT =
    "show-part-percent-hud";

static constexpr char const* SETTING_SHOW_PART_INDEX =
    "show-part-index-hud";

static constexpr char const* SETTING_PERCENT_DECIMAL_PLACES =
    "percent-decimal-places";

static constexpr char const* SETTING_HUD_SCALE =
    "progress-hud-scale";

static constexpr char const* SETTING_HUD_OFFSET_X =
    "progress-hud-offset-x";

static constexpr char const* SETTING_HUD_OFFSET_Y =
    "progress-hud-offset-y";

static constexpr char const* SETTING_HUD_OPACITY =
    "progress-hud-opacity";

static constexpr char const* SETTING_PROGRESS_COLOR_NORMAL =
    "progress-color-normal";

static constexpr char const* SETTING_PROGRESS_COLOR_GO =
    "progress-color-go";

static constexpr char const* SETTING_PROGRESS_COLOR_BEST =
    "progress-color-best";

static constexpr char const* SETTING_DIFFICULTY_FONT =
    "difficulty-number-font";

static constexpr char const* SETTING_DIFFICULTY_FONT_OFFSET_X =
    "difficulty-number-offset-x";

static constexpr char const* SETTING_DIFFICULTY_FONT_OFFSET_Y =
    "difficulty-number-offset-y";

static constexpr char const* SETTING_DIFFICULTY_FACE_SCALE =
    "difficulty-face-scale";

static constexpr char const* SETTING_DIFFICULTY_FONT_SCALE =
    "difficulty-number-scale";

static constexpr char const* SETTING_DIFFICULTY_FACE_OFFSET_X =
    "difficulty-face-offset-x";

static constexpr char const* SETTING_DIFFICULTY_FACE_OFFSET_Y =
    "difficulty-face-offset-y";

static constexpr char const* SETTING_PART_NAME_SCALE =
    "part-name-scale";

static constexpr char const* SETTING_PART_NAME_FONT =
    "part-name-font";

static constexpr char const* SETTING_PART_NAME_OFFSET_X =
    "part-name-offset-x";

static constexpr char const* SETTING_PART_NAME_OFFSET_Y =
    "part-name-offset-y";

static constexpr float PROGRESS_HUD_SCALE_MIN = 0.3f;
static constexpr float PROGRESS_HUD_SCALE_MAX = 3.f;
static constexpr float PROGRESS_HUD_OFFSET_X_MIN = -400.f;
static constexpr float PROGRESS_HUD_OFFSET_X_MAX = 400.f;
static constexpr float PROGRESS_HUD_OFFSET_Y_MIN = -200.f;
static constexpr float PROGRESS_HUD_OFFSET_Y_MAX = 100.f;

static constexpr float PROGRESS_ELEMENT_SCALE_MIN = 0.25f;
static constexpr float PROGRESS_ELEMENT_SCALE_MAX = 4.f;
static constexpr float PROGRESS_ELEMENT_OFFSET_MIN = -200.f;
static constexpr float PROGRESS_ELEMENT_OFFSET_MAX = 200.f;
static constexpr float PART_NAME_OFFSET_X_MIN = -400.f;
static constexpr float PART_NAME_OFFSET_X_MAX = 400.f;

static constexpr int PERCENT_DECIMAL_PLACES_MIN = 0;
static constexpr int PERCENT_DECIMAL_PLACES_MAX = 4;
static constexpr int DEFAULT_PERCENT_DECIMAL_PLACES = 1;

static constexpr int DEFAULT_PROGRESS_COLOR_NORMAL = 0xFFFFFF;
static constexpr int DEFAULT_PROGRESS_COLOR_GO = 0x66FF38;
static constexpr int DEFAULT_PROGRESS_COLOR_BEST = 0xF5FF64;

struct FlagIconOption {
    char const* name;
    char const* frame;
};

static std::vector<FlagIconOption> const& flagIconOptions() {
    static std::vector<FlagIconOption> const options = {
        {"Arrow", "GJ_arrow_03_001.png"},
        {"Best", "rankIcon_top10_001.png"},
        {"Star", "GJ_starsIcon_001.png"},
        {"Check", "GJ_checkOn_001.png"},
        {"Demon", "GJ_demonIcon_001.png"},
        {"Time", "GJ_timeIcon_001.png"},
        {"Done", "GJ_completesIcon_001.png"},
        {"Magic", "GJ_sMagicIcon_001.png"},
    };
    return options;
}

static bool isAllowedFlagIcon(std::string_view frame) {
    auto const& options = flagIconOptions();
    return std::any_of(
        options.begin(),
        options.end(),
        [frame](FlagIconOption const& option) {
            return frame == option.frame;
        }
    );
}

static CCSprite* createFlagIconSprite(
    std::string const& requestedFrame,
    float maximumSide
) {
    auto const frame = isAllowedFlagIcon(requestedFrame)
        ? requestedFrame
        : std::string(flagIconOptions().front().frame);
    auto sprite = CCSprite::createWithSpriteFrameName(frame.c_str());
    if (!sprite) {
        sprite = CCSprite::createWithSpriteFrameName(
            "GJ_unknownIcon_001.png"
        );
    }
    if (!sprite) return nullptr;

    auto const size = sprite->getContentSize();
    auto const largestSide = std::max(size.width, size.height);
    if (largestSide > 0.f) {
        sprite->setScale(maximumSide / largestSide);
    }
    sprite->setRotation(
        frame == "GJ_arrow_03_001.png" ? -90.f : 0.f
    );
    return sprite;
}

static constexpr char const* DEFAULT_DIFFICULTY_FONT =
    "gjFont54.fnt";

static constexpr char const* DEFAULT_PART_NAME_FONT =
    "bigFont.fnt";

static constexpr char const* USER_IMAGES_KEY =
    "difficulty-user-images";

struct DifficultyFontOption {
    std::string name;
    std::string file;
};

static std::vector<DifficultyFontOption> const& difficultyFontOptions() {
    static auto const options = [] {
        std::vector<DifficultyFontOption> result = {
            {"Default", DEFAULT_DIFFICULTY_FONT},
            {"Big", "bigFont.fnt"},
            {"Gold", "goldFont.fnt"},
            {"Chat", "chatFont.fnt"},
        };
        for (int index = 1; index <= 59; ++index) {
            if (index == 54) {
                continue;
            }
            result.push_back({
                fmt::format("Font {:02}", index),
                fmt::format("gjFont{:02}.fnt", index),
            });
        }
        return result;
    }();
    return options;
}

static size_t difficultyFontIndex(std::string const& file) {
    auto const& options = difficultyFontOptions();
    auto const found = std::find_if(
        options.begin(),
        options.end(),
        [&file](DifficultyFontOption const& option) {
            return option.file == file;
        }
    );
    return found == options.end()
        ? 0
        : static_cast<size_t>(std::distance(options.begin(), found));
}

static std::string getDifficultyHUDFont() {
    auto const saved = Mod::get()->getSavedValue<std::string>(
        SETTING_DIFFICULTY_FONT,
        DEFAULT_DIFFICULTY_FONT
    );
    auto const& options = difficultyFontOptions();
    return options[difficultyFontIndex(saved)].file;
}

static std::string getFlagKeyForSectionKey(std::string_view sectionKey) {
    if (!sectionKey.starts_with(SECTION_DATA_KEY_PREFIX)) return "";
    return fmt::format(
        "{}{}",
        FLAG_DATA_KEY_PREFIX,
        sectionKey.substr(SECTION_DATA_KEY_PREFIX.size())
    );
}

static std::string getPartNameHUDFont() {
    auto const saved = Mod::get()->getSavedValue<std::string>(
        SETTING_PART_NAME_FONT,
        DEFAULT_PART_NAME_FONT
    );
    auto const& options = difficultyFontOptions();
    auto const index = difficultyFontIndex(saved);
    return options[index].file == saved
        ? options[index].file
        : DEFAULT_PART_NAME_FONT;
}

static std::vector<std::string> loadUserImages() {
    std::vector<std::string> images;
    auto value = Mod::get()->getSavedValue<matjson::Value>(
        USER_IMAGES_KEY,
        matjson::Value::array()
    );

    if (!value.isArray()) return images;

    for (auto const& item : value) {
        auto path = item.asString().unwrapOr("");
        if (!path.empty() && std::filesystem::exists(path)) {
            images.push_back(path);
        }
    }
    return images;
}

static void saveUserImages(std::vector<std::string> const& images) {
    auto value = matjson::Value::array();
    for (auto const& path : images) value.push(path);
    Mod::get()->setSavedValue(USER_IMAGES_KEY, value);
}

static CCSize defaultFaceDisplaySize(float scale) {
    auto reference = CCSprite::create("1.png"_spr);
    if (!reference) return {40.f, 40.f};
    auto size = reference->getContentSize();
    return {size.width * scale, size.height * scale};
}

// Fits inside the same box as 1.png without ever stretching either axis.
static void scaleFaceToReference(CCSprite* sprite, float referenceScale) {
    if (!sprite) return;
    auto source = sprite->getContentSize();
    auto target = defaultFaceDisplaySize(referenceScale);
    if (source.width <= 0.f || source.height <= 0.f) return;

    sprite->setScale(std::min(
        target.width / source.width,
        target.height / source.height
    ));
}

static CCSprite* createFaceSprite(int faceID, std::string const& customImage = "") {
    CCSprite* sprite = nullptr;
    if (!customImage.empty() && std::filesystem::exists(customImage)) {
        sprite = CCSprite::create(customImage.c_str());
    }
    if (!sprite) {
        faceID = std::clamp(faceID, 0, 32);
        sprite = CCSprite::create(fmt::format("{}.png"_spr, faceID).c_str());
    }
    if (!sprite) {
        sprite = CCSprite::createWithSpriteFrameName("GJ_unknownIcon_001.png");
    }
    return sprite;
}


static constexpr char const* CUSTOM_DEATH_SOUNDS_KEY =
    "difficulty-custom-death-sounds";

struct DeathSoundOption {
    std::string label;
    std::string path;
};

static bool parseSfxDeathSoundKey(std::string const& key, int& id) {
    if (!key.starts_with("sfx:") || key.size() <= 4) return false;
    try {
        size_t parsed = 0;
        auto value = std::stoi(key.substr(4), &parsed);
        if (parsed != key.size() - 4 || value <= 0 || value > 99999) {
            return false;
        }
        id = value;
        return true;
    }
    catch (...) {
        return false;
    }
}

static std::vector<DeathSoundOption> geometryDashDeathSounds() {
    std::vector<DeathSoundOption> options = {
        {"Default Explosion", "explode_11.ogg"},
        {"Magic Explosion", "magicExplosion.ogg"},
        {"Achievement", "achievement_01.ogg"},
        {"Buy Item 1", "buyItem01.ogg"},
        {"Buy Item 3", "buyItem03.ogg"},
        {"Chest Open", "chestOpen01.ogg"},
        {"Chest Land", "chestLand.ogg"},
        {"Chest 7", "chest07.ogg"},
        {"Chest 8", "chest08.ogg"},
        {"Counter", "counter003.ogg"},
        {"Crystal", "crystal01.ogg"},
        {"Door Heavy", "door001.ogg"},
        {"Door 1", "door01.ogg"},
        {"Door 2", "door02.ogg"},
        {"End Start", "endStart_02.ogg"},
        {"Gold 1", "gold01.ogg"},
        {"Gold 2", "gold02.ogg"},
        {"Grunt 1", "grunt01.ogg"},
        {"Grunt 2", "grunt02.ogg"},
        {"Grunt 3", "grunt03.ogg"},
        {"High Score", "highscoreGet02.ogg"},
        {"Play", "playSound_01.ogg"},
        {"Quit", "quitSound_01.ogg"},
        {"Reward", "reward01.ogg"},
        {"Secret Key", "secretKey.ogg"},
        {"Unlock Gauntlet", "unlockGauntlet.ogg"},
        {"Unlock Path", "unlockPath.ogg"},
        {"Jumpscare", "jumpscareAudio.mp3"},
        {"Fire In The Hole", "sfx:4451"},
        {"Fire In The Hole 2", "sfx:4450"},
        {"I See You 01", "sfx:4821"},
        {"I See You 02", "sfx:5062"},
        {"Arr Matey", "sfx:4467"},
        {"Intrusion Detected", "sfx:8386"},
        {"So It Has Come To This", "sfx:5107"},
        {"Your Power Is Meaningless", "sfx:5170"},
        {"You Should Not Be Here", "sfx:10271"},
        {"Why Are You Here", "sfx:22589"},
        {"You Again", "sfx:22607"},
        {"There Is No Way Out", "sfx:22587"},
        {"Time Is Running Out", "sfx:22588"},
        {"Fire In The Hole 01", "sfx:14278"},
        {"Fire In The Hole 02", "sfx:14279"},
        {"Fire In The Hole 03", "sfx:14280"},
        {"Fire In The Hole 04", "sfx:14281"},
        {"Fire In The Hole 05", "sfx:14282"},
    };

    auto manager = MusicDownloadManager::sharedState();
    if (!manager || !manager->m_sfxObjects) return options;

    std::vector<DeathSoundOption> downloaded;
    for (
        auto [id, object] :
        CCDictionaryExt<int, SFXInfoObject*>(manager->m_sfxObjects)
    ) {
        if (
            !object ||
            object->m_folder ||
            !manager->isSFXDownloaded(object->m_sfxID)
        ) continue;

        auto key = fmt::format("sfx:{}", object->m_sfxID);
        auto duplicate = std::any_of(
            options.begin(),
            options.end(),
            [&key](DeathSoundOption const& option) {
                return option.path == key;
            }
        );
        if (!duplicate) {
            downloaded.push_back({
                std::string(object->m_name.c_str()),
                std::move(key)
            });
        }
    }
    std::sort(
        downloaded.begin(),
        downloaded.end(),
        [](DeathSoundOption const& left, DeathSoundOption const& right) {
            return left.label < right.label;
        }
    );
    options.insert(options.end(), downloaded.begin(), downloaded.end());
    return options;
}

static bool isGeometryDashDeathSound(std::string const& path) {
    int sfxID = 0;
    if (parseSfxDeathSoundKey(path, sfxID)) return true;
    auto const& options = geometryDashDeathSounds();
    return std::any_of(
        options.begin(),
        options.end(),
        [&path](DeathSoundOption const& option) {
            return option.path == path;
        }
    );
}

static std::string resolveDeathSoundPath(std::string const& key) {
    int sfxID = 0;
    if (!parseSfxDeathSoundKey(key, sfxID)) return key;
    auto manager = MusicDownloadManager::sharedState();
    if (!manager || !manager->isSFXDownloaded(sfxID)) return "";
    return std::string(manager->pathForSFX(sfxID).c_str());
}

static bool ensureDeathSoundAvailable(std::string const& key) {
    int sfxID = 0;
    if (!parseSfxDeathSoundKey(key, sfxID)) return true;
    auto manager = MusicDownloadManager::sharedState();
    if (!manager) return false;
    if (manager->isSFXDownloaded(sfxID)) return true;
    manager->downloadSFX(sfxID);
    return false;
}

static std::vector<std::string> loadCustomDeathSounds() {
    std::vector<std::string> sounds;
    auto value = Mod::get()->getSavedValue<matjson::Value>(
        CUSTOM_DEATH_SOUNDS_KEY,
        matjson::Value::array()
    );
    if (!value.isArray()) return sounds;

    for (auto const& item : value) {
        auto path = item.asString().unwrapOr("");
        if (!path.empty() && std::filesystem::exists(path)) {
            sounds.push_back(std::move(path));
        }
    }
    return sounds;
}

static void saveCustomDeathSounds(std::vector<std::string> const& sounds) {
    auto value = matjson::Value::array();
    for (auto const& path : sounds) value.push(path);
    Mod::get()->setSavedValue(CUSTOM_DEATH_SOUNDS_KEY, value);
}

static std::string customDeathSoundName(std::string const& path) {
    auto name = std::filesystem::path(path).filename().string();
    return name.empty() ? "Custom Sound" : name;
}

static void previewDeathSound(
    std::string const& path,
    float volume
) {
    if (path.empty() || volume <= 0.f) return;
    if (!ensureDeathSoundAvailable(path)) {
        Notification::create("Downloading SFX...", NotificationIcon::Info)
            ->show();
        return;
    }
    auto const resolvedPath = resolveDeathSoundPath(path);
    if (resolvedPath.empty()) return;
    auto engine = FMODAudioEngine::sharedEngine();
    if (!engine) return;
    engine->playEffect(
        gd::string(resolvedPath),
        1.f,
        0.f,
        std::clamp(volume, 0.f, 1.f)
    );
}

struct ActiveDeathSoundOverride {
    bool enabled = false;
    std::string path;
    float volume = 1.f;
};

static thread_local ActiveDeathSoundOverride s_activeDeathSoundOverride;

static ActiveDeathSoundOverride deathSoundForCurrentSection(
    PlayLayer* playLayer
) {
    auto sections = loadSections();
    sortSections(sections);

    SectionData const* current = nullptr;
    auto const percent = getCurrentLevelPercent(playLayer);
    for (auto const& section : sections) {
        if (percent < section.startPercent) break;
        current = &section;
    }
    if (!current || !current->deathSoundOverride) return {};

    std::string resolvedSound;
    if (current->deathSoundCustom) {
        if (!std::filesystem::exists(current->deathSound)) return {};
        resolvedSound = current->deathSound;
    }
    else {
        if (!isGeometryDashDeathSound(current->deathSound)) return {};
        resolvedSound = resolveDeathSoundPath(current->deathSound);
        if (resolvedSound.empty()) {
            ensureDeathSoundAvailable(current->deathSound);
            return {};
        }
    }

    return {
        true,
        std::move(resolvedSound),
        std::clamp(current->deathSoundVolume, 0.f, 1.f)
    };
}

static bool isDefaultDeathSound(gd::string const& value) {
    auto path = std::string(value.c_str());
    std::replace(path.begin(), path.end(), '\\', '/');
    auto const separator = path.find_last_of('/');
    if (separator != std::string::npos) path.erase(0, separator + 1);
    std::transform(
        path.begin(),
        path.end(),
        path.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        }
    );
    return path == "explode_11.ogg";
}

static CCNode* createCompactSoundIcon(bool enabled) {
    auto holder = CCNode::create();
    holder->setContentSize({22.f, 22.f});
    holder->setAnchorPoint({0.5f, 0.5f});
    holder->ignoreAnchorPointForPosition(false);

    auto sprite = CCSprite::createWithSpriteFrameName(
        "edit_eSFXBtn_001.png"
    );
    if (sprite) {
        auto const size = sprite->getContentSize();
        if (size.width > 0.f && size.height > 0.f) {
            sprite->setScale(std::min(20.f / size.width, 20.f / size.height));
        }
        sprite->setPosition({11.f, 11.f});
        sprite->setOpacity(enabled ? 255 : 190);
        holder->addChild(sprite);
    }
    return holder;
}


static bool isFlagHUDEnabled() {
    return Mod::get()->getSavedValue<bool>(
        SETTING_SHOW_FLAGS,
        true
    );
}

static bool isProgressHUDEnabled() {
    return Mod::get()->getSavedValue<bool>(
        SETTING_SHOW_PROGRESS_HUD,
        true
    );
}

static bool isBestHUDEnabled() {
    return Mod::get()->getSavedValue<bool>(
        SETTING_SHOW_BEST,
        true
    );
}

static bool isDifficultyHUDEnabled() {
    return Mod::get()->getSavedValue<bool>(
        SETTING_SHOW_DIFFICULTY,
        true
    );
}

static bool isLabelHUDEnabled() {
    return Mod::get()->getSavedValue<bool>(
        SETTING_SHOW_LABEL,
        true
    );
}

static bool isTotalPercentHUDEnabled() {
    return Mod::get()->getSavedValue<bool>(
        SETTING_SHOW_TOTAL_PERCENT,
        true
    );
}

static bool isPartPercentHUDEnabled() {
    return Mod::get()->getSavedValue<bool>(
        SETTING_SHOW_PART_PERCENT,
        true
    );
}

static bool isPartIndexHUDEnabled() {
    return Mod::get()->getSavedValue<bool>(
        SETTING_SHOW_PART_INDEX,
        true
    );
}

static int getPercentDecimalPlaces() {
    return std::clamp(
        Mod::get()->getSavedValue<int>(
            SETTING_PERCENT_DECIMAL_PLACES,
            DEFAULT_PERCENT_DECIMAL_PLACES
        ),
        PERCENT_DECIMAL_PLACES_MIN,
        PERCENT_DECIMAL_PLACES_MAX
    );
}

static std::string formatHUDPercent(float percent, int decimalPlaces) {
    return fmt::format(
        "{:.{}f}%",
        percent,
        std::clamp(
            decimalPlaces,
            PERCENT_DECIMAL_PLACES_MIN,
            PERCENT_DECIMAL_PLACES_MAX
        )
    );
}

static float loadProgressHUDSetting(
    char const* key,
    float defaultValue,
    float minimum,
    float maximum
) {
    auto value = static_cast<float>(
        Mod::get()->getSavedValue<double>(key, defaultValue)
    );
    if (!std::isfinite(value)) {
        value = defaultValue;
    }
    return std::clamp(value, minimum, maximum);
}

static float getProgressHUDScale() {
    return loadProgressHUDSetting(
        SETTING_HUD_SCALE,
        1.f,
        PROGRESS_HUD_SCALE_MIN,
        PROGRESS_HUD_SCALE_MAX
    );
}

static float getProgressHUDOffsetX() {
    return loadProgressHUDSetting(
        SETTING_HUD_OFFSET_X,
        0.f,
        PROGRESS_HUD_OFFSET_X_MIN,
        PROGRESS_HUD_OFFSET_X_MAX
    );
}

static float getProgressHUDOffsetY() {
    return loadProgressHUDSetting(
        SETTING_HUD_OFFSET_Y,
        0.f,
        PROGRESS_HUD_OFFSET_Y_MIN,
        PROGRESS_HUD_OFFSET_Y_MAX
    );
}

static float getProgressHUDOpacity() {
    return loadProgressHUDSetting(SETTING_HUD_OPACITY, 1.f, 0.3f, 1.f);
}

static int packProgressColor(ccColor3B const& color) {
    return
        (static_cast<int>(color.r) << 16) |
        (static_cast<int>(color.g) << 8) |
        static_cast<int>(color.b);
}

static ccColor3B unpackProgressColor(int color) {
    color = std::clamp(color, 0, 0xFFFFFF);
    return ccc3(
        static_cast<GLubyte>((color >> 16) & 0xFF),
        static_cast<GLubyte>((color >> 8) & 0xFF),
        static_cast<GLubyte>(color & 0xFF)
    );
}

static ccColor3B loadProgressColor(char const* key, int defaultColor) {
    auto color = Mod::get()->getSavedValue<int>(key, defaultColor);
    if (color < 0 || color > 0xFFFFFF) {
        color = defaultColor;
    }
    return unpackProgressColor(color);
}

static ccColor3B getNormalProgressColor() {
    return loadProgressColor(
        SETTING_PROGRESS_COLOR_NORMAL,
        DEFAULT_PROGRESS_COLOR_NORMAL
    );
}

static ccColor3B getGoProgressColor() {
    return loadProgressColor(
        SETTING_PROGRESS_COLOR_GO,
        DEFAULT_PROGRESS_COLOR_GO
    );
}

static ccColor3B getBestProgressColor() {
    return loadProgressColor(
        SETTING_PROGRESS_COLOR_BEST,
        DEFAULT_PROGRESS_COLOR_BEST
    );
}

static float getDifficultyFontOffsetX() {
    return loadProgressHUDSetting(
        SETTING_DIFFICULTY_FONT_OFFSET_X,
        0.f,
        PROGRESS_ELEMENT_OFFSET_MIN,
        PROGRESS_ELEMENT_OFFSET_MAX
    );
}

static float getDifficultyFontOffsetY() {
    return loadProgressHUDSetting(
        SETTING_DIFFICULTY_FONT_OFFSET_Y,
        0.f,
        PROGRESS_ELEMENT_OFFSET_MIN,
        PROGRESS_ELEMENT_OFFSET_MAX
    );
}

static float getDifficultyFaceScale() {
    return loadProgressHUDSetting(
        SETTING_DIFFICULTY_FACE_SCALE,
        1.f,
        PROGRESS_ELEMENT_SCALE_MIN,
        PROGRESS_ELEMENT_SCALE_MAX
    );
}

static float getDifficultyFontScale() {
    return loadProgressHUDSetting(
        SETTING_DIFFICULTY_FONT_SCALE,
        1.f,
        PROGRESS_ELEMENT_SCALE_MIN,
        PROGRESS_ELEMENT_SCALE_MAX
    );
}

static float getDifficultyFaceOffsetX() {
    return loadProgressHUDSetting(
        SETTING_DIFFICULTY_FACE_OFFSET_X,
        0.f,
        PROGRESS_ELEMENT_OFFSET_MIN,
        PROGRESS_ELEMENT_OFFSET_MAX
    );
}

static float getDifficultyFaceOffsetY() {
    return loadProgressHUDSetting(
        SETTING_DIFFICULTY_FACE_OFFSET_Y,
        0.f,
        PROGRESS_ELEMENT_OFFSET_MIN,
        PROGRESS_ELEMENT_OFFSET_MAX
    );
}

static float getPartNameScale() {
    return loadProgressHUDSetting(
        SETTING_PART_NAME_SCALE,
        1.f,
        PROGRESS_ELEMENT_SCALE_MIN,
        PROGRESS_ELEMENT_SCALE_MAX
    );
}

static float getPartNameOffsetX() {
    return loadProgressHUDSetting(
        SETTING_PART_NAME_OFFSET_X,
        0.f,
        PART_NAME_OFFSET_X_MIN,
        PART_NAME_OFFSET_X_MAX
    );
}

static float getPartNameOffsetY() {
    return loadProgressHUDSetting(
        SETTING_PART_NAME_OFFSET_Y,
        0.f,
        PROGRESS_ELEMENT_OFFSET_MIN,
        PROGRESS_ELEMENT_OFFSET_MAX
    );
}

static std::string getFirebaseResponseError(web::WebResponse const& response) {
    if (auto jsonResult = response.json()) {
        auto const& json = jsonResult.unwrap();
        if (json.isObject() && json.contains("error")) {
            auto const& error = json["error"];
            if (error.isString()) {
                return error.asString().unwrapOr("Unknown Firebase error");
            }
            if (error.isObject() && error.contains("message")) {
                return error["message"].asString().unwrapOr("Unknown Firebase error");
            }
        }
    }

    auto message = std::string(response.errorMessage());
    if (!message.empty()) return message;
    return response.string().unwrapOr("Unknown network error");
}

static std::string firebaseSafeKey(std::string value) {
    static constexpr char HEX[] = "0123456789ABCDEF";

    std::string key;
    key.reserve(value.size());
    for (auto raw : value) {
        auto c = static_cast<unsigned char>(raw);
        if (c >= 'A' && c <= 'Z') {
            key.push_back(static_cast<char>(c + ('a' - 'A')));
        }
        else if (
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-'
        ) {
            key.push_back(static_cast<char>(c));
        }
        else {
            key.push_back('_');
            key.push_back(HEX[c >> 4]);
            key.push_back(HEX[c & 0x0f]);
        }
    }
    return key.empty() ? "unnamed" : key;
}

static std::string getLoggedInGDUsername() {
    auto account = GJAccountManager::sharedState();
    if (!account || account->m_accountID <= 0 || account->m_username.empty()) {
        return "";
    }
    return trimWhitespace(std::string(account->m_username.c_str()));
}

static std::string truncateUtf8(std::string value, std::size_t maxBytes) {
    if (value.size() <= maxBytes) return value;

    auto end = maxBytes;
    while (
        end > 0 &&
        (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80
    ) {
        end--;
    }
    value.resize(end);
    return value;
}

static std::string makeFlagID() {
    static std::uint64_t sequence = 0;
    auto const stamp = std::chrono::steady_clock::now()
        .time_since_epoch()
        .count();
    return fmt::format("flag-{}-{}", stamp, ++sequence);
}

static void sortFlags(std::vector<FlagData>& flags) {
    std::stable_sort(
        flags.begin(),
        flags.end(),
        [](FlagData const& left, FlagData const& right) {
            if (left.source != right.source) {
                return left.source == FlagPercentSource::Fixed;
            }
            if (left.percent != right.percent) {
                return left.percent < right.percent;
            }
            return left.id < right.id;
        }
    );
}

static matjson::Value flagToJson(FlagData const& flag) {
    auto object = matjson::Value::object();
    object["id"] = flag.id;
    object["label"] = flag.label;
    object["percent"] = clampLevelPercent(flag.percent);
    object["source"] = flag.source == FlagPercentSource::PersonalBest
        ? "best"
        : "fixed";
    object["icon"] = flag.iconFrame;
    object["color"] = std::clamp(flag.passedColor, 0, 0xFFFFFF);
    object["offsetX"] = sanitizeFlagDetailValue(
        flag.offsetX,
        FLAG_DETAIL_OFFSET_MIN,
        FLAG_DETAIL_OFFSET_MAX,
        0.f
    );
    object["offsetY"] = sanitizeFlagDetailValue(
        flag.offsetY,
        FLAG_DETAIL_OFFSET_MIN,
        FLAG_DETAIL_OFFSET_MAX,
        0.f
    );
    object["scale"] = sanitizeFlagDetailValue(
        flag.scale,
        FLAG_DETAIL_SCALE_MIN,
        FLAG_DETAIL_SCALE_MAX,
        1.f
    );
    object["opacity"] = sanitizeFlagDetailValue(
        flag.opacity,
        FLAG_DETAIL_OPACITY_MIN,
        FLAG_DETAIL_OPACITY_MAX,
        1.f
    );
    return object;
}

static matjson::Value flagsToJson(std::vector<FlagData> const& flags) {
    auto array = matjson::Value::array();
    for (auto const& flag : flags) {
        array.push(flagToJson(flag));
    }
    return array;
}

static std::vector<FlagData> flagsFromJson(matjson::Value const& value) {
    std::vector<FlagData> flags;
    if (!value.isArray()) return flags;

    for (auto const& item : value) {
        if (!item.isObject() || flags.size() >= 100) continue;

        auto id = item["id"].asString().unwrapOr("");
        auto label = trimWhitespace(
            truncateUtf8(item["label"].asString().unwrapOr("Flag"), 48)
        );
        auto icon = item["icon"].asString().unwrapOr(
            flagIconOptions().front().frame
        );
        auto percent = static_cast<float>(
            item["percent"].asDouble().unwrapOr(0.0)
        );
        auto color = static_cast<int>(
            item["color"].asInt().unwrapOr(DEFAULT_PROGRESS_COLOR_GO)
        );
        auto source = item["source"].asString().unwrapOr("fixed") == "best"
            ? FlagPercentSource::PersonalBest
            : FlagPercentSource::Fixed;
        auto offsetX = static_cast<float>(
            item["offsetX"].asDouble().unwrapOr(0.0)
        );
        auto offsetY = static_cast<float>(
            item["offsetY"].asDouble().unwrapOr(0.0)
        );
        auto scale = static_cast<float>(
            item["scale"].asDouble().unwrapOr(1.0)
        );
        auto opacity = static_cast<float>(
            item["opacity"].asDouble().unwrapOr(1.0)
        );

        if (id.empty()) id = makeFlagID();
        if (label.empty()) label = "Flag";
        if (!std::isfinite(percent)) percent = 0.f;
        if (!isAllowedFlagIcon(icon)) {
            icon = flagIconOptions().front().frame;
        }

        flags.push_back({
            truncateUtf8(std::move(id), 64),
            std::move(label),
            clampLevelPercent(percent),
            source,
            std::move(icon),
            std::clamp(color, 0, 0xFFFFFF),
            sanitizeFlagDetailValue(
                offsetX,
                FLAG_DETAIL_OFFSET_MIN,
                FLAG_DETAIL_OFFSET_MAX,
                0.f
            ),
            sanitizeFlagDetailValue(
                offsetY,
                FLAG_DETAIL_OFFSET_MIN,
                FLAG_DETAIL_OFFSET_MAX,
                0.f
            ),
            sanitizeFlagDetailValue(
                scale,
                FLAG_DETAIL_SCALE_MIN,
                FLAG_DETAIL_SCALE_MAX,
                1.f
            ),
            sanitizeFlagDetailValue(
                opacity,
                FLAG_DETAIL_OPACITY_MIN,
                FLAG_DETAIL_OPACITY_MAX,
                1.f
            ),
        });
    }

    sortFlags(flags);
    return flags;
}

static void saveFlagsForSectionKey(
    std::string const& sectionKey,
    std::vector<FlagData> flags
) {
    auto const flagKey = getFlagKeyForSectionKey(sectionKey);
    auto& saved = Mod::get()->getSaveContainer();
    if (flagKey.empty() || !saved.isObject()) return;

    sortFlags(flags);
    saved[flagKey] = flagsToJson(flags);
}

static void removeFlagsForSectionKey(std::string const& sectionKey) {
    auto const flagKey = getFlagKeyForSectionKey(sectionKey);
    auto& saved = Mod::get()->getSaveContainer();
    if (!flagKey.empty() && saved.isObject()) {
        saved.erase(flagKey);
    }
    saveStoredGoPercent(sectionKey, std::nullopt);
}

static std::vector<FlagData> loadFlagsForSectionKey(
    std::string const& sectionKey
) {
    ensureProgressDataUpgradeBackup();
    auto const flagKey = getFlagKeyForSectionKey(sectionKey);
    auto& saved = Mod::get()->getSaveContainer();
    if (flagKey.empty() || !saved.isObject()) return {};

    if (saved.contains(flagKey) && !saved[flagKey].isNull()) {
        if (saved[flagKey].isArray()) {
            return flagsFromJson(saved[flagKey]);
        }
        log::error(
            "Refusing to replace unsupported FlagData at key '{}'",
            flagKey
        );
        return {};
    }

    std::vector<FlagData> migrated;
    migrated.push_back({
        "legacy-go",
        "Go!",
        loadStoredGoPercent(sectionKey).value_or(0.f),
        FlagPercentSource::Fixed,
        "GJ_arrow_03_001.png",
        packProgressColor(getGoProgressColor()),
    });
    migrated.push_back({
        "legacy-best",
        "Best!",
        0.f,
        FlagPercentSource::PersonalBest,
        "rankIcon_top10_001.png",
        packProgressColor(getBestProgressColor()),
    });

    saveFlagsForSectionKey(sectionKey, migrated);
    saveStoredGoPercent(sectionKey, std::nullopt);
    return migrated;
}

static std::vector<FlagData> loadFlags() {
    return loadFlagsForSectionKey(getNameBasedLevelKey());
}

static void saveFlags(std::vector<FlagData> flags) {
    sortFlags(flags);
    auto const sectionKey = getNameBasedLevelKey();
    saveFlagsForSectionKey(sectionKey, flags);
    if (!flags.empty()) {
        setSectionMapDisplayName(sectionKey, getCurrentMapName());
    }
    else if (loadSectionsForKey(sectionKey).empty()) {
        removeSectionMapDisplayName(sectionKey);
    }
    syncActiveFlagProgressBar(flags);
}

static std::string formatCompactUploadAge(double timestampMs) {
    if (!std::isfinite(timestampMs) || timestampMs <= 0.0) return "--";

    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    auto ageSeconds = static_cast<long long>(
        std::max(0.0, static_cast<double>(nowMs) - timestampMs) / 1000.0
    );

    if (ageSeconds < 60) return fmt::format("{}s", ageSeconds);
    if (ageSeconds < 3600) return fmt::format("{}m", ageSeconds / 60);
    if (ageSeconds < 86400) return fmt::format("{}h", ageSeconds / 3600);
    if (ageSeconds < 604800) return fmt::format("{}d", ageSeconds / 86400);
    if (ageSeconds < 31536000) {
        return fmt::format("{}w", ageSeconds / 604800);
    }
    return fmt::format("{}y", ageSeconds / 31536000);
}

static bool readServerInteger(
    matjson::Value const& value,
    int minimum,
    int maximum,
    int& output
) {
    if (!value.isNumber()) return false;
    auto number = value.asDouble().unwrapOr(-1.0);
    if (
        !std::isfinite(number) ||
        std::floor(number) != number ||
        number < minimum ||
        number > maximum
    ) {
        return false;
    }
    output = static_cast<int>(number);
    return true;
}

static matjson::Value sectionsToServerJson(
    std::vector<SectionData> const& sections
) {
    auto array = matjson::Value::array();
    for (auto const& section : sections) {
        auto object = matjson::Value::object();
        object["start"] = clampLevelPercent(section.startPercent);
        object["difficulty"] = std::clamp(section.difficulty, 0, 999);
        object["partName"] = truncateUtf8(section.partName, 96);
        object["faces"] = std::clamp(section.faces, 0, 32);
        auto const shareOverride =
            section.deathSoundOverride &&
            !section.deathSoundCustom &&
            isGeometryDashDeathSound(section.deathSound);
        object["deathSoundOverride"] = shareOverride;
        object["deathSound"] = shareOverride
            ? section.deathSound
            : std::string("explode_11.ogg");
        object["deathSoundVolume"] = std::clamp(
            section.deathSoundVolume,
            0.f,
            1.f
        );
        array.push(std::move(object));
    }
    return array;
}

static bool parseServerSectionArray(
    matjson::Value const& value,
    std::vector<SectionData>& sections,
    std::string& error
) {
    if (!value.isArray()) {
        error = "sections must be a JSON array.";
        return false;
    }
    if (value.size() > 500) {
        error = "A preset cannot contain more than 500 sections.";
        return false;
    }

    sections.clear();
    int index = 0;
    for (auto const& item : value) {
        if (
            !item.isObject() ||
            !item.contains("start") ||
            !item.contains("difficulty") ||
            !item.contains("faces") ||
            !item.contains("partName") ||
            !item["start"].isNumber() ||
            !item["difficulty"].isNumber() ||
            !item["faces"].isNumber() ||
            !item["partName"].isString()
        ) {
            error = fmt::format(
                "SectionData item {} has an invalid field.",
                index
            );
            return false;
        }

        auto start = item["start"].asDouble().unwrapOr(-1.0);
        auto difficulty = item["difficulty"].asDouble().unwrapOr(-1.0);
        auto faces = item["faces"].asDouble().unwrapOr(-1.0);
        auto const hasDeathOverride = item.contains("deathSoundOverride");
        auto const hasDeathSound = item.contains("deathSound");
        auto const hasDeathVolume = item.contains("deathSoundVolume");
        if (
            (hasDeathOverride && !item["deathSoundOverride"].isBool()) ||
            (hasDeathSound && !item["deathSound"].isString()) ||
            (hasDeathVolume && !item["deathSoundVolume"].isNumber())
        ) {
            error = fmt::format(
                "SectionData item {} has an invalid death sound field.",
                index
            );
            return false;
        }
        auto const deathSoundOverride = hasDeathOverride
            ? item["deathSoundOverride"].asBool().unwrapOr(false)
            : false;
        auto const deathSound = hasDeathSound
            ? item["deathSound"].asString().unwrapOr("explode_11.ogg")
            : std::string("explode_11.ogg");
        auto const deathSoundVolume = hasDeathVolume
            ? item["deathSoundVolume"].asDouble().unwrapOr(1.0)
            : 1.0;
        if (
            !std::isfinite(start) ||
            !std::isfinite(difficulty) ||
            !std::isfinite(faces) ||
            start < 0.0 || start > 100.0 ||
            difficulty < 0.0 || difficulty > 999.0 ||
            faces < 0.0 || faces > 32.0 ||
            std::floor(difficulty) != difficulty ||
            std::floor(faces) != faces ||
            deathSound.size() > 64 ||
            !isGeometryDashDeathSound(deathSound) ||
            !std::isfinite(deathSoundVolume) ||
            deathSoundVolume < 0.0 || deathSoundVolume > 1.0
        ) {
            error = fmt::format(
                "SectionData item {} has a value outside the allowed range.",
                index
            );
            return false;
        }

        SectionData section {
            static_cast<float>(start),
            static_cast<int>(difficulty),
            truncateUtf8(
                item["partName"].asString().unwrapOr(""),
                96
            ),
            static_cast<int>(faces),
            "",
            deathSoundOverride,
            deathSound,
            false,
            static_cast<float>(deathSoundVolume)
        };
        sections.push_back(std::move(section));
        index++;
    }

    if (sections.empty()) {
        error = "The preset contains no SectionData.";
        return false;
    }

    sortSections(sections);
    return true;
}

static matjson::Value flagsToServerJson(
    std::vector<FlagData> const& flags
) {
    auto flagData = matjson::Value::object();
    flagData["version"] = 1;
    flagData["items"] = flagsToJson(flags);
    return flagData;
}

static bool parseServerFlagArray(
    matjson::Value const& value,
    std::vector<FlagData>& flags,
    std::string& error
) {
    if (!value.isArray()) {
        error = "flagData.items must be a JSON array.";
        return false;
    }
    if (value.size() > 100) {
        error = "A preset cannot contain more than 100 FlagData items.";
        return false;
    }

    flags.clear();
    int index = 0;
    for (auto const& item : value) {
        if (
            !item.isObject() ||
            !item.contains("id") ||
            !item.contains("label") ||
            !item.contains("percent") ||
            !item.contains("source") ||
            !item.contains("icon") ||
            !item.contains("color") ||
            !item.contains("offsetX") ||
            !item.contains("offsetY") ||
            !item.contains("scale") ||
            !item.contains("opacity") ||
            !item["id"].isString() ||
            !item["label"].isString() ||
            !item["percent"].isNumber() ||
            !item["source"].isString() ||
            !item["icon"].isString() ||
            !item["color"].isNumber() ||
            !item["offsetX"].isNumber() ||
            !item["offsetY"].isNumber() ||
            !item["scale"].isNumber() ||
            !item["opacity"].isNumber()
        ) {
            error = fmt::format(
                "FlagData item {} has an invalid field.",
                index
            );
            return false;
        }

        auto id = item["id"].asString().unwrapOr("");
        auto label = item["label"].asString().unwrapOr("");
        auto sourceName = item["source"].asString().unwrapOr("");
        auto icon = item["icon"].asString().unwrapOr("");
        auto percent = item["percent"].asDouble().unwrapOr(-1.0);
        auto offsetX = item["offsetX"].asDouble().unwrapOr(0.0);
        auto offsetY = item["offsetY"].asDouble().unwrapOr(0.0);
        auto scale = item["scale"].asDouble().unwrapOr(1.0);
        auto opacity = item["opacity"].asDouble().unwrapOr(1.0);
        int color = 0;

        auto const duplicateID = std::any_of(
            flags.begin(),
            flags.end(),
            [&id](FlagData const& flag) {
                return flag.id == id;
            }
        );
        if (
            id.empty() || id.size() > 64 ||
            label.size() > 48 ||
            (sourceName != "fixed" && sourceName != "best") ||
            !isAllowedFlagIcon(icon) ||
            !std::isfinite(percent) || percent < 0.0 || percent > 100.0 ||
            !std::isfinite(offsetX) ||
                offsetX < FLAG_DETAIL_OFFSET_MIN ||
                offsetX > FLAG_DETAIL_OFFSET_MAX ||
            !std::isfinite(offsetY) ||
                offsetY < FLAG_DETAIL_OFFSET_MIN ||
                offsetY > FLAG_DETAIL_OFFSET_MAX ||
            !std::isfinite(scale) ||
                scale < FLAG_DETAIL_SCALE_MIN ||
                scale > FLAG_DETAIL_SCALE_MAX ||
            !std::isfinite(opacity) ||
                opacity < FLAG_DETAIL_OPACITY_MIN ||
                opacity > FLAG_DETAIL_OPACITY_MAX ||
            !readServerInteger(item["color"], 0, 0xFFFFFF, color) ||
            duplicateID
        ) {
            error = fmt::format(
                "FlagData item {} has a value outside the allowed range.",
                index
            );
            return false;
        }

        flags.push_back({
            std::move(id),
            std::move(label),
            static_cast<float>(percent),
            sourceName == "best"
                ? FlagPercentSource::PersonalBest
                : FlagPercentSource::Fixed,
            std::move(icon),
            color,
            static_cast<float>(offsetX),
            static_cast<float>(offsetY),
            static_cast<float>(scale),
            static_cast<float>(opacity),
        });
        index++;
    }

    sortFlags(flags);
    return true;
}

static bool parseServerFlagData(
    matjson::Value const& value,
    std::vector<FlagData>& flags,
    std::string& error
) {
    if (!value.isObject()) {
        error = "flagData must be a JSON object.";
        return false;
    }

    int version = 0;
    if (
        !value.contains("version") ||
        !readServerInteger(value["version"], 1, 1, version)
    ) {
        error = "flagData.version must be 1.";
        return false;
    }

    flags.clear();
    if (!value.contains("items") || value["items"].isNull()) {
        return true;
    }
    return parseServerFlagArray(value["items"], flags, error);
}

struct ServerSectionPreset {
    std::string userName;
    std::vector<SectionData> sections;
    std::vector<FlagData> flags;
    bool hasFlagData = false;
    double updatedAt = 0.0;
    int playerIcon = 1;
    int playerColor1 = 0;
    int playerColor2 = 3;
    bool playerGlow = false;
    int playerGlowColor = 0;
    bool hasPlayerIcon = false;
};

struct ServerMapEntry {
    std::string mapName;
    std::string mapKey;
    std::size_t uploadCount = 0;
};

// Main Popup

class SectionListPopup : public geode::Popup {
protected:
    std::vector<SectionData> m_sections;
    ScrollLayer* m_scroll = nullptr;
    bool m_writeProtectionNoticeShown = false;

    bool persistSections(std::vector<SectionData> const& sections) {
        if (saveSections(sections)) return true;
        if (!m_writeProtectionNoticeShown) {
            m_writeProtectionNoticeShown = true;
            FLAlertLayer::create(
                "SectionData Protected",
                "This stored SectionData uses an unsupported format. "
                "The original data was not overwritten and is also kept "
                "in the upgrade backup.",
                "OK"
            )->show();
        }
        return false;
    }

    void deferInlineEditReload() {
        WeakRef<SectionListPopup> self(this);
        geode::queueInMainThread([self] {
            if (auto owner = self.lock()) {
                owner->reloadList(true);
            }
        });
    }

    bool init() {
        if (!Popup::init(430.f, 270.f)) {
            return false;
        }

        this->setTitle("Sections");

        m_sections = loadSections();

        m_scroll = ScrollLayer::create({420.f, 175.f});
        m_scroll->setPosition({5.f, 55.f});
        m_mainLayer->addChild(m_scroll);

        // Add button
        auto addMenu = CCMenu::create();
        addMenu->setPosition({95.f, 25.f});
        m_mainLayer->addChild(addMenu);

        auto addSpr = ButtonSprite::create("Add");
        addSpr->setScale(0.6f);

        auto addBtn = CCMenuItemSpriteExtra::create(
            addSpr,
            this,
            menu_selector(SectionListPopup::onAddSection)
        );

        addMenu->addChild(addBtn);

        auto graphMenu = CCMenu::create();
        graphMenu->setPosition({35.f, 25.f});
        m_mainLayer->addChild(graphMenu);

        auto graphSprite = ButtonSprite::create("Graph");
        graphSprite->setScale(0.52f);
        auto graphButton = CCMenuItemSpriteExtra::create(
            graphSprite,
            this,
            menu_selector(SectionListPopup::onOpenDifficultyGraph)
        );
        graphButton->setID("difficulty-graph-button"_spr);
        graphMenu->addChild(graphButton);

        auto flagMenu = CCMenu::create();
        flagMenu->setPosition({210.f, 25.f});
        m_mainLayer->addChild(flagMenu);

        auto flagSprite = CCSprite::createWithSpriteFrameName(
            "GJ_createLinesBtn_001.png"
        );
        CCNode* flagVisual = flagSprite
            ? static_cast<CCNode*>(flagSprite)
            : static_cast<CCNode*>(ButtonSprite::create("Flags"));
        flagVisual->setScale(flagSprite ? 0.65f : 0.5f);
        auto flagButton = CCMenuItemSpriteExtra::create(
            flagVisual,
            this,
            menu_selector(SectionListPopup::onOpenFlagData)
        );
        flagButton->setID("flag-data-button"_spr);
        flagMenu->addChild(flagButton);

        // Opens the per-map Firebase upload/download browser.
        auto serverMenu = CCMenu::create();
        serverMenu->setPosition({340.f, 25.f});
        m_mainLayer->addChild(serverMenu);

        auto serverSprite = ButtonSprite::create("Server Download");
        serverSprite->setScale(0.45f);

        auto serverDownloadButton = CCMenuItemSpriteExtra::create(
            serverSprite,
            this,
            menu_selector(SectionListPopup::onServerDownload)
        );

        serverMenu->addChild(serverDownloadButton);

        auto settingsSprite = CCSprite::createWithSpriteFrameName(
            "GJ_optionsBtn_001.png"
        );

        if (settingsSprite) {
            settingsSprite->setScale(0.65f);

            auto settingsButton = CCMenuItemSpriteExtra::create(
                settingsSprite,
                this,
                menu_selector(SectionListPopup::onOpenCustomizationSettings)
            );

            auto size = m_mainLayer->getContentSize();
            settingsButton->setPosition({
                size.width - 22.f,
                size.height - 22.f
            });
            settingsButton->setID("progress-settings-button"_spr);
            m_buttonMenu->addChild(settingsButton);
        }

        reloadList(false);

        return true;
    }

    // SectionListPopup customization entry point
    void onOpenCustomizationSettings(CCObject*);

    void reloadList(bool keepScroll = true) {
        float oldY = 0.f;

        if (keepScroll && m_scroll && m_scroll->m_contentLayer) {
            oldY = m_scroll->m_contentLayer->getPositionY();
        }

        sortSections(m_sections);

        m_scroll->m_contentLayer->removeAllChildren();

        float contentWidth = 420.f;

        float contentHeight = std::max(
            175.f,
            38.f * static_cast<float>(m_sections.size()) + 20.f
        );

        m_scroll->m_contentLayer->setContentSize({contentWidth, contentHeight});

        float y = contentHeight - 24.f;

        for (int i = 0; i < static_cast<int>(m_sections.size()); i++) {
            auto startInput = TextInput::create(52.f, "Start");
            startInput->setString(fmt::format("{:.1f}", m_sections[i].startPercent));
            startInput->setScale(0.58f);
            startInput->setPosition({28.f, y});
            startInput->setCommonFilter(CommonFilter::Float);

            startInput->setCallback([this, i](std::string const& str) {
                if (str.empty()) return;

                try {
                    float value = std::stof(str);
                    value = std::clamp(value, 0.f, 100.f);

                    if (i >= 0 && i < static_cast<int>(m_sections.size())) {
                        auto const previous = m_sections[i].startPercent;
                        m_sections[i].startPercent = value;
                        if (!persistSections(m_sections)) {
                            m_sections[i].startPercent = previous;
                            deferInlineEditReload();
                        }
                    }
                }
                catch (...) {}
            });

            m_scroll->m_contentLayer->addChild(startInput);

            auto percentLabel = CCLabelBMFont::create("%", "bigFont.fnt");
            percentLabel->setScale(0.3f);
            percentLabel->setAnchorPoint({0.f, 0.5f});
            percentLabel->setPosition({47.f, y});
            m_scroll->m_contentLayer->addChild(percentLabel);

            auto diffInput = TextInput::create(42.f, "0");
            diffInput->setString(fmt::format("{}", m_sections[i].difficulty));
            diffInput->setScale(0.58f);
            diffInput->setPosition({76.f, y});
            diffInput->setCommonFilter(CommonFilter::Uint);

            diffInput->setCallback([this, i](std::string const& str) {
                if (str.empty()) return;

                try {
                    int value = std::stoi(str);
                    value = std::clamp(value, 0, 999);

                    if (i >= 0 && i < static_cast<int>(m_sections.size())) {
                        auto const previous = m_sections[i].difficulty;
                        m_sections[i].difficulty = value;
                        if (!persistSections(m_sections)) {
                            m_sections[i].difficulty = previous;
                            deferInlineEditReload();
                        }
                    }
                }
                catch (...) {}
            });

            m_scroll->m_contentLayer->addChild(diffInput);

            auto diffLabel = CCLabelBMFont::create("Diff", "bigFont.fnt");
            diffLabel->setScale(0.2f);
            diffLabel->setAnchorPoint({0.f, 0.5f});
            diffLabel->setColor(ccc3(190, 210, 195));
            diffLabel->setPosition({92.f, y});
            m_scroll->m_contentLayer->addChild(diffLabel);

            auto nameInput = TextInput::create(220.f, "PartName");
            nameInput->setString(m_sections[i].partName);
            nameInput->setScale(0.58f);
            nameInput->setCommonFilter(CommonFilter::Any);
            nameInput->setPosition({194.f, y});

            nameInput->setCallback([this, i](std::string const& str) {
                if (i >= 0 && i < static_cast<int>(m_sections.size())) {
                    auto previous = m_sections[i].partName;
                    m_sections[i].partName = str;
                    if (!persistSections(m_sections)) {
                        m_sections[i].partName = std::move(previous);
                        deferInlineEditReload();
                    }
                }
            });

            m_scroll->m_contentLayer->addChild(nameInput);

            auto faceMenu = CCMenu::create();
            faceMenu->setPosition({286.f, y});
            m_scroll->m_contentLayer->addChild(faceMenu);

            int faceID = std::clamp(m_sections[i].faces, 0, 32);

            auto faceSpr = createFaceSprite(faceID, m_sections[i].customImage);
            scaleFaceToReference(faceSpr, 0.2f);

            auto faceBtn = CCMenuItemSpriteExtra::create(
                faceSpr,
                this,
                menu_selector(SectionListPopup::onOpenFaceSelect)
            );

            faceBtn->setTag(i);
            faceBtn->setPosition({0.f, 0.f});
            faceMenu->addChild(faceBtn);

            auto soundMenu = CCMenu::create();
            soundMenu->setPosition({314.f, y});
            m_scroll->m_contentLayer->addChild(soundMenu);

            auto soundButton = CCMenuItemSpriteExtra::create(
                createCompactSoundIcon(m_sections[i].deathSoundOverride),
                this,
                menu_selector(SectionListPopup::onOpenDeathSound)
            );
            soundButton->setTag(i);
            soundButton->setID(fmt::format("section-death-sound-{}", i));
            soundMenu->addChild(soundButton);

            auto deleteMenu = CCMenu::create();
            deleteMenu->setPosition({390.f, y});
            m_scroll->m_contentLayer->addChild(deleteMenu);

            auto deleteSpr = ButtonSprite::create("X");
            deleteSpr->setScale(0.45f);

            auto deleteBtn = CCMenuItemSpriteExtra::create(
                deleteSpr,
                this,
                menu_selector(SectionListPopup::onDeleteSection)
            );

            deleteBtn->setTag(i);
            deleteBtn->setPosition({0.f, 0.f});
            deleteMenu->addChild(deleteBtn);

            y -= 38.f;
        }

        if (keepScroll) {
            float minY = std::min(0.f, 175.f - contentHeight);
            float maxY = 0.f;

            oldY = std::clamp(oldY, minY, maxY);
            m_scroll->m_contentLayer->setPositionY(oldY);
        }
        else {
            m_scroll->moveToTop();
        }
    }

    void onAddSection(CCObject*) {
        sortSections(m_sections);

        float newStart = 0.f;

        if (!m_sections.empty()) {
            newStart = std::clamp(
                m_sections.back().startPercent + 10.f,
                0.f,
                100.f
            );
        }

        m_sections.push_back({
            newStart,
            0,
            "",
            0,
            ""
        });

        sortSections(m_sections);
        if (!persistSections(m_sections)) {
            m_sections = loadSections();
        }
        reloadList(true);
    }

    void onDeleteSection(CCObject* sender) {
        int index = static_cast<CCNode*>(sender)->getTag();

        if (index < 0 || index >= static_cast<int>(m_sections.size())) {
            return;
        }

        m_sections.erase(m_sections.begin() + index);

        sortSections(m_sections);
        if (!persistSections(m_sections)) {
            m_sections = loadSections();
        }
        reloadList(true);
    }

    void onServerDownload(CCObject*);

    void onOpenFlagData(CCObject*);

    void onOpenDifficultyGraph(CCObject*);

    void onOpenFaceSelect(CCObject* sender);

    void onOpenDeathSound(CCObject* sender);

public:
    void setSectionControlsEnabled(bool enabled) {
        this->setTouchEnabled(enabled);
        this->setKeypadEnabled(enabled);
        setPopupControlsEnabled(this, enabled);
    }

    std::vector<SectionData> const& getSections() const {
        return m_sections;
    }

    bool applyServerData(
        std::vector<SectionData> sections,
        std::vector<FlagData> flags,
        bool hasFlagData
    ) {
        sortSections(sections);
        if (!persistSections(sections)) return false;
        m_sections = std::move(sections);
        if (hasFlagData) {
            saveFlags(std::move(flags));
        }
        reloadList(false);
        return true;
    }

    void setFace(int index, int face) {
        if (index < 0 || index >= static_cast<int>(m_sections.size())) {
            return;
        }

        m_sections[index].faces = std::clamp(face, 0, 32);
        m_sections[index].customImage.clear();

        if (!persistSections(m_sections)) {
            m_sections = loadSections();
        }
        reloadList(true);
    }

    void setCustomFace(int index, std::string const& path) {
        if (index < 0 || index >= static_cast<int>(m_sections.size())) {
            return;
        }

        m_sections[index].customImage = path;

        if (!persistSections(m_sections)) {
            m_sections = loadSections();
        }
        reloadList(true);
    }

    void removeCustomFaceReferences(std::string const& path) {
        bool changed = false;
        for (auto& section : m_sections) {
            if (section.customImage == path) {
                section.customImage.clear();
                changed = true;
            }
        }

        if (changed) {
            if (!persistSections(m_sections)) {
                m_sections = loadSections();
            }
            reloadList(true);
        }
    }

    void setDeathSound(int index, SectionData const& updated) {
        if (index < 0 || index >= static_cast<int>(m_sections.size())) {
            return;
        }

        auto const previous = m_sections[index];
        m_sections[index].deathSoundOverride = updated.deathSoundOverride;
        m_sections[index].deathSound = updated.deathSound;
        m_sections[index].deathSoundCustom = updated.deathSoundCustom;
        m_sections[index].deathSoundVolume = std::clamp(
            updated.deathSoundVolume,
            0.f,
            1.f
        );
        if (!persistSections(m_sections)) {
            m_sections[index] = previous;
        }
    }

    void removeCustomDeathSoundReferences(std::string const& path) {
        auto const previous = m_sections;
        bool changed = false;
        for (auto& section : m_sections) {
            if (section.deathSoundCustom && section.deathSound == path) {
                section.deathSoundOverride = false;
                section.deathSound = "explode_11.ogg";
                section.deathSoundCustom = false;
                changed = true;
            }
        }
        if (changed && !persistSections(m_sections)) {
            m_sections = previous;
        }
    }

    void refreshSectionRows() {
        reloadList(true);
    }

    static SectionListPopup* create() {
        auto ret = new SectionListPopup();

        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }

        delete ret;
        return nullptr;
    }
};

class SectionDifficultyGraphPopup : public geode::Popup {
protected:
    WeakRef<SectionListPopup> m_parent;
    std::vector<SectionData> m_sections;
    std::vector<CCPoint> m_pointPositions;
    CCNode* m_detailCard = nullptr;
    int m_visibleIndex = -1;
    int m_pinnedIndex = -1;

    static constexpr float GRAPH_LEFT = 56.f;
    static constexpr float GRAPH_BOTTOM = 52.f;
    static constexpr float GRAPH_WIDTH = 372.f;
    static constexpr float GRAPH_HEIGHT = 154.f;

    static std::string difficultyText(int difficulty) {
        return fmt::format("{:.1f}", difficulty / 10.f);
    }

    void hideDetails() {
        if (m_detailCard) {
            m_detailCard->removeFromParent();
            m_detailCard = nullptr;
        }
        m_visibleIndex = -1;
    }

    void showDetails(int index) {
        if (
            index < 0 ||
            index >= static_cast<int>(m_sections.size()) ||
            index >= static_cast<int>(m_pointPositions.size())
        ) {
            return;
        }
        if (m_visibleIndex == index && m_detailCard) return;

        hideDetails();
        m_visibleIndex = index;

        auto const& section = m_sections[index];
        auto const point = m_pointPositions[index];
        constexpr float cardWidth = 140.f;
        constexpr float cardHeight = 48.f;
        constexpr float cardPadding = 8.f;

        float cardX = point.x + cardPadding;
        if (cardX + cardWidth > 442.f) {
            cardX = point.x - cardWidth - cardPadding;
        }
        cardX = std::clamp(cardX, 8.f, 442.f - cardWidth);

        float cardY = point.y + cardPadding;
        if (cardY + cardHeight > 222.f) {
            cardY = point.y - cardHeight - cardPadding;
        }
        cardY = std::clamp(cardY, 40.f, 222.f - cardHeight);

        m_detailCard = CCNode::create();
        m_detailCard->setPosition({cardX, cardY});
        m_detailCard->setZOrder(100);
        m_mainLayer->addChild(m_detailCard);

        auto background = CCLayerColor::create(
            ccc4(0, 0, 0, 245),
            cardWidth,
            cardHeight
        );
        m_detailCard->addChild(background);

        auto border = CCDrawNode::create();
        auto const green = ccc4f(0.f, 1.f, 0.f, 1.f);
        border->drawSegment({0.f, 0.f}, {cardWidth, 0.f}, 0.6f, green);
        border->drawSegment(
            {cardWidth, 0.f},
            {cardWidth, cardHeight},
            0.6f,
            green
        );
        border->drawSegment(
            {cardWidth, cardHeight},
            {0.f, cardHeight},
            0.6f,
            green
        );
        border->drawSegment({0.f, cardHeight}, {0.f, 0.f}, 0.6f, green);
        m_detailCard->addChild(border);

        auto face = createFaceSprite(
            std::clamp(section.faces, 0, 32),
            section.customImage
        );
        if (face) {
            scaleFaceToReference(face, 0.22f);
            face->setPosition({20.f, 24.f});
            m_detailCard->addChild(face);
        }

        auto const partName = section.partName.empty()
            ? fmt::format("Part {}", index + 1)
            : section.partName;
        auto partLabel = CCLabelBMFont::create(
            partName.c_str(),
            "bigFont.fnt"
        );
        partLabel->setAnchorPoint({0.f, 0.5f});
        partLabel->setScale(0.28f);
        partLabel->limitLabelWidth(92.f, 0.28f, 0.17f);
        partLabel->setPosition({40.f, 33.f});
        partLabel->setColor(ccc3(225, 255, 232));
        m_detailCard->addChild(partLabel);

        auto detailLabel = CCLabelBMFont::create(
            fmt::format(
                "Difficulty {}  |  {:.1f}%",
                difficultyText(section.difficulty),
                section.startPercent
            ).c_str(),
            "bigFont.fnt"
        );
        detailLabel->setAnchorPoint({0.f, 0.5f});
        detailLabel->setScale(0.18f);
        detailLabel->setColor(ccc3(0, 255, 0));
        detailLabel->setPosition({40.f, 14.f});
        m_detailCard->addChild(detailLabel);
    }

    void onGraphBackgroundPressed(CCObject*) {
        m_pinnedIndex = -1;
        hideDetails();
    }

    void onPointPressed(CCObject* sender) {
        auto const index = static_cast<CCNode*>(sender)->getTag();
        m_pinnedIndex = index;
        showDetails(index);
    }

    void handleMouseMove(int32_t x, int32_t y) {
        if (!this->isRunning() || !m_mainLayer) return;

        auto const world = CCDirector::sharedDirector()->convertToGL({
            static_cast<float>(x),
            static_cast<float>(y)
        });
        auto const local = m_mainLayer->convertToNodeSpace(world);

        int hoveredIndex = -1;
        float closestDistanceSquared = 12.f * 12.f;
        for (int index = 0; index < static_cast<int>(m_pointPositions.size()); ++index) {
            auto const delta = local - m_pointPositions[index];
            auto const distanceSquared = delta.x * delta.x + delta.y * delta.y;
            if (distanceSquared <= closestDistanceSquared) {
                closestDistanceSquared = distanceSquared;
                hoveredIndex = index;
            }
        }

        if (hoveredIndex >= 0) {
            showDetails(hoveredIndex);
        }
        else if (m_pinnedIndex >= 0) {
            showDetails(m_pinnedIndex);
        }
        else {
            hideDetails();
        }
    }

    void addAxisLabel(
        std::string const& text,
        CCPoint const& position,
        float scale = 0.22f
    ) {
        auto label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
        label->setScale(scale);
        label->setColor(ccc3(145, 173, 157));
        label->setPosition(position);
        m_mainLayer->addChild(label);
    }

    bool init(
        SectionListPopup* parent,
        std::vector<SectionData> sections
    ) {
        if (!Popup::init(450.f, 275.f)) return false;

        m_parent = parent;
        m_sections = std::move(sections);
        sortSections(m_sections);
        this->setID("section-difficulty-graph-popup"_spr);
        this->setTitle("Difficulty Graph");

        auto graphBackground = CCLayerColor::create(
            ccc4(0, 0, 0, 255),
            GRAPH_WIDTH,
            GRAPH_HEIGHT
        );
        graphBackground->setPosition({GRAPH_LEFT, GRAPH_BOTTOM});
        m_mainLayer->addChild(graphBackground);

        auto grid = CCDrawNode::create();
        auto const gridColor = ccc4f(0.18f, 0.34f, 0.25f, 0.55f);
        auto const axisColor = ccc4f(0.47f, 0.68f, 0.54f, 0.9f);
        for (int step = 0; step <= 4; ++step) {
            auto const xPos = GRAPH_LEFT + GRAPH_WIDTH * step / 4.f;
            grid->drawSegment(
                {xPos, GRAPH_BOTTOM},
                {xPos, GRAPH_BOTTOM + GRAPH_HEIGHT},
                step == 0 ? 0.9f : 0.45f,
                step == 0 ? axisColor : gridColor
            );
            addAxisLabel(
                fmt::format("{}", step * 25),
                {xPos, GRAPH_BOTTOM - 10.f}
            );
        }
        for (int step = 0; step <= 4; ++step) {
            auto const yPos = GRAPH_BOTTOM + GRAPH_HEIGHT * step / 4.f;
            grid->drawSegment(
                {GRAPH_LEFT, yPos},
                {GRAPH_LEFT + GRAPH_WIDTH, yPos},
                step == 0 ? 0.9f : 0.45f,
                step == 0 ? axisColor : gridColor
            );
        }
        m_mainLayer->addChild(grid);

        auto xTitle = CCLabelBMFont::create("Progress (%)", "bigFont.fnt");
        xTitle->setScale(0.27f);
        xTitle->setColor(ccc3(180, 205, 188));
        xTitle->setPosition({GRAPH_LEFT + GRAPH_WIDTH / 2.f, 24.f});
        m_mainLayer->addChild(xTitle);

        auto yTitle = CCLabelBMFont::create("Difficulty", "bigFont.fnt");
        yTitle->setScale(0.27f);
        yTitle->setRotation(-90.f);
        yTitle->setColor(ccc3(180, 205, 188));
        yTitle->setPosition({12.f, GRAPH_BOTTOM + GRAPH_HEIGHT / 2.f});
        m_mainLayer->addChild(yTitle);

        if (m_sections.empty()) {
            auto emptyLabel = CCLabelBMFont::create(
                "No SectionData yet.",
                "bigFont.fnt"
            );
            emptyLabel->setScale(0.42f);
            emptyLabel->setColor(ccc3(128, 180, 145));
            emptyLabel->setPosition({
                GRAPH_LEFT + GRAPH_WIDTH / 2.f,
                GRAPH_BOTTOM + GRAPH_HEIGHT / 2.f
            });
            m_mainLayer->addChild(emptyLabel);
            return true;
        }

        auto [minimumIt, maximumIt] = std::minmax_element(
            m_sections.begin(),
            m_sections.end(),
            [](SectionData const& left, SectionData const& right) {
                return left.difficulty < right.difficulty;
            }
        );
        auto const minimumDifficulty = minimumIt->difficulty;
        auto const maximumDifficulty = maximumIt->difficulty;
        auto const difficultySpan = maximumDifficulty - minimumDifficulty;

        if (difficultySpan == 0) {
            addAxisLabel(
                difficultyText(minimumDifficulty),
                {GRAPH_LEFT - 14.f, GRAPH_BOTTOM + GRAPH_HEIGHT / 2.f}
            );
        }
        else {
            addAxisLabel(
                difficultyText(minimumDifficulty),
                {GRAPH_LEFT - 14.f, GRAPH_BOTTOM}
            );
            addAxisLabel(
                difficultyText(minimumDifficulty + difficultySpan / 2),
                {GRAPH_LEFT - 14.f, GRAPH_BOTTOM + GRAPH_HEIGHT / 2.f}
            );
            addAxisLabel(
                difficultyText(maximumDifficulty),
                {GRAPH_LEFT - 14.f, GRAPH_BOTTOM + GRAPH_HEIGHT}
            );
        }

        m_pointPositions.reserve(m_sections.size());
        for (auto const& section : m_sections) {
            auto const xRatio = clampLevelPercent(section.startPercent) / 100.f;
            auto const yRatio = difficultySpan == 0
                ? 0.5f
                : static_cast<float>(section.difficulty - minimumDifficulty) /
                    static_cast<float>(difficultySpan);
            m_pointPositions.push_back({
                GRAPH_LEFT + GRAPH_WIDTH * xRatio,
                GRAPH_BOTTOM + GRAPH_HEIGHT * yRatio
            });
        }

        auto line = CCDrawNode::create();
        auto const lineGreen = ccc4f(0.f, 1.f, 0.f, 1.f);
        for (size_t index = 1; index < m_pointPositions.size(); ++index) {
            line->drawSegment(
                m_pointPositions[index - 1],
                m_pointPositions[index],
                0.7f,
                lineGreen
            );
        }
        m_mainLayer->addChild(line);

        auto pointMenu = CCMenu::create();
        pointMenu->setPosition({0.f, 0.f});
        pointMenu->setZOrder(10);
        m_mainLayer->addChild(pointMenu);

        auto addDismissRegion = [this, pointMenu](
            float left,
            float right,
            float bottom,
            float top
        ) {
            if (right - left < 1.f || top - bottom < 1.f) return;
            auto hitArea = CCLayerColor::create(
                ccc4(0, 0, 0, 0),
                right - left,
                top - bottom
            );
            hitArea->setAnchorPoint({0.5f, 0.5f});
            hitArea->ignoreAnchorPointForPosition(false);
            auto button = CCMenuItemSpriteExtra::create(
                hitArea,
                this,
                menu_selector(
                    SectionDifficultyGraphPopup::onGraphBackgroundPressed
                )
            );
            button->setPosition({
                (left + right) / 2.f,
                (bottom + top) / 2.f
            });
            pointMenu->addChild(button);
        };

        // The dismiss surface is split into strips with holes around points.
        // This keeps background taps from stealing the point buttons' touches.
        constexpr float rowHeight = 14.f;
        constexpr float pointHoleRadius = 11.f;
        for (
            float rowBottom = GRAPH_BOTTOM;
            rowBottom < GRAPH_BOTTOM + GRAPH_HEIGHT;
            rowBottom += rowHeight
        ) {
            auto const rowTop = std::min(
                rowBottom + rowHeight,
                GRAPH_BOTTOM + GRAPH_HEIGHT
            );
            std::vector<std::pair<float, float>> blocked;
            for (auto const& point : m_pointPositions) {
                if (
                    point.y + pointHoleRadius <= rowBottom ||
                    point.y - pointHoleRadius >= rowTop
                ) continue;
                blocked.push_back({
                    std::max(GRAPH_LEFT, point.x - pointHoleRadius),
                    std::min(
                        GRAPH_LEFT + GRAPH_WIDTH,
                        point.x + pointHoleRadius
                    )
                });
            }
            std::sort(blocked.begin(), blocked.end());

            float cursor = GRAPH_LEFT;
            for (auto const& interval : blocked) {
                if (interval.first > cursor) {
                    addDismissRegion(
                        cursor,
                        interval.first,
                        rowBottom,
                        rowTop
                    );
                }
                cursor = std::max(cursor, interval.second);
            }
            addDismissRegion(
                cursor,
                GRAPH_LEFT + GRAPH_WIDTH,
                rowBottom,
                rowTop
            );
        }

        for (int index = 0; index < static_cast<int>(m_pointPositions.size()); ++index) {
            auto pointVisual = CCDrawNode::create();
            pointVisual->setContentSize({22.f, 22.f});
            pointVisual->setAnchorPoint({0.5f, 0.5f});
            pointVisual->drawDot({11.f, 11.f}, 3.5f, lineGreen);

            auto pointButton = CCMenuItemSpriteExtra::create(
                pointVisual,
                this,
                menu_selector(SectionDifficultyGraphPopup::onPointPressed)
            );
            pointButton->setTag(index);
            pointButton->setPosition(m_pointPositions[index]);
            pointButton->setID(fmt::format("difficulty-point-{}", index));
            pointMenu->addChild(pointButton);
        }

        this->addEventListener(
            MouseMoveEvent(),
            [this](int32_t x, int32_t y) {
                handleMouseMove(x, y);
            }
        );
        return true;
    }

public:
    void onClose(CCObject* sender) override {
        if (auto parent = m_parent.lock()) {
            parent->setSectionControlsEnabled(true);
        }
        Popup::onClose(sender);
    }

    static SectionDifficultyGraphPopup* create(
        SectionListPopup* parent,
        std::vector<SectionData> sections
    ) {
        auto ret = new SectionDifficultyGraphPopup();
        if (ret && ret->init(parent, std::move(sections))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

void SectionListPopup::onOpenDifficultyGraph(CCObject*) {
    if (auto popup = SectionDifficultyGraphPopup::create(this, m_sections)) {
        setSectionControlsEnabled(false);
        popup->show();
    }
}

// Local SectionData browser

class MapNameInputPopup : public geode::Popup {
protected:
    TextInput* m_nameInput = nullptr;
    geode::Function<bool(std::string const&)> m_onSubmit;

    bool init(
        std::string title,
        std::string initialName,
        std::string actionText,
        geode::Function<bool(std::string const&)> onSubmit
    ) {
        if (!Popup::init(340.f, 175.f)) return false;

        m_onSubmit = std::move(onSubmit);
        this->setTitle(title.c_str());

        m_nameInput = TextInput::create(280.f, "Map name...");
        m_nameInput->setCommonFilter(CommonFilter::Any);
        m_nameInput->setMaxCharCount(100);
        m_nameInput->setString(initialName);
        m_nameInput->setPosition({170.f, 91.f});
        m_mainLayer->addChild(m_nameInput);

        auto note = CCLabelBMFont::create(
            "Names match maps without case differences.",
            "goldFont.fnt"
        );
        note->setScale(0.25f);
        note->setPosition({170.f, 62.f});
        m_mainLayer->addChild(note);

        auto actionSprite = ButtonSprite::create(actionText.c_str());
        actionSprite->setScale(0.55f);
        auto actionButton = CCMenuItemSpriteExtra::create(
            actionSprite,
            this,
            menu_selector(MapNameInputPopup::onApply)
        );
        actionButton->setPosition({170.f, 29.f});
        m_buttonMenu->addChild(actionButton);
        return true;
    }

    void onApply(CCObject*) {
        if (!m_nameInput) return;
        auto mapName = trimWhitespace(
            std::string(m_nameInput->getString().c_str())
        );
        if (mapName.empty()) {
            FLAlertLayer::create(
                "Invalid Map Name",
                "Enter a non-empty map name.",
                "OK"
            )->show();
            return;
        }

        if (m_onSubmit(mapName)) {
            this->onClose(nullptr);
        }
    }

public:
    static MapNameInputPopup* create(
        std::string title,
        std::string initialName,
        std::string actionText,
        geode::Function<bool(std::string const&)> onSubmit
    ) {
        auto ret = new MapNameInputPopup();
        if (
            ret &&
            ret->init(
                std::move(title),
                std::move(initialName),
                std::move(actionText),
                std::move(onSubmit)
            )
        ) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class LocalSectionPreviewPopup : public geode::Popup {
protected:
    bool init(LocalSectionDataset dataset) {
        if (!Popup::init(390.f, 270.f)) return false;
        this->setTitle("Local SectionData Preview");

        auto mapLabel = CCLabelBMFont::create(
            fmt::format(
                "{} - {} section{}",
                dataset.mapName,
                dataset.sections.size(),
                dataset.sections.size() == 1 ? "" : "s"
            ).c_str(),
            "goldFont.fnt"
        );
        mapLabel->setScale(0.34f);
        mapLabel->limitLabelWidth(350.f, 0.34f, 0.18f);
        mapLabel->setPosition({195.f, 216.f});
        m_mainLayer->addChild(mapLabel);

        auto scroll = ScrollLayer::create({370.f, 180.f});
        scroll->setStealingTouches(true);
        scroll->setPosition({10.f, 32.f});
        m_mainLayer->addChild(scroll);

        float contentHeight = std::max(
            180.f,
            static_cast<float>(dataset.sections.size()) * 29.f + 10.f
        );
        scroll->m_contentLayer->setContentSize({370.f, contentHeight});

        float y = contentHeight - 18.f;
        for (int i = 0; i < static_cast<int>(dataset.sections.size()); i++) {
            auto const& section = dataset.sections[i];
            auto line = CCLabelBMFont::create(
                fmt::format(
                    "#{}  {:.1f}%  Diff {:.1f}  Face {}  {}",
                    i + 1,
                    section.startPercent,
                    section.difficulty / 10.f,
                    section.faces,
                    section.partName
                ).c_str(),
                "bigFont.fnt"
            );
            line->setAnchorPoint({0.f, 0.5f});
            line->setScale(0.27f);
            line->limitLabelWidth(350.f, 0.27f, 0.16f);
            line->setPosition({10.f, y});
            scroll->m_contentLayer->addChild(line);
            y -= 29.f;
        }
        scroll->moveToTop();
        return true;
    }

public:
    static LocalSectionPreviewPopup* create(LocalSectionDataset dataset) {
        auto ret = new LocalSectionPreviewPopup();
        if (ret && ret->init(std::move(dataset))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class MyDataListPopup : public geode::Popup, public FLAlertLayerProtocol {
protected:
    std::vector<LocalSectionDataset> m_datasets;
    ScrollLayer* m_scroll = nullptr;
    CCLabelBMFont* m_statusLabel = nullptr;
    std::string m_pendingDeleteKey;

    bool init() override {
        if (!Popup::init(440.f, 285.f)) return false;
        this->setTitle("My Data List");

        m_statusLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_statusLabel->setScale(0.27f);
        m_statusLabel->setPosition({220.f, 229.f});
        m_mainLayer->addChild(m_statusLabel);

        m_scroll = ScrollLayer::create({420.f, 180.f});
        m_scroll->setStealingTouches(true);
        m_scroll->setPosition({10.f, 40.f});
        m_mainLayer->addChild(m_scroll);

        reloadList(false);
        return true;
    }

    void showError(std::string const& message) {
        FLAlertLayer::create(
            "Local SectionData",
            message,
            "OK"
        )->show();
    }

    bool canUseTargetKey(
        std::string const& sourceKey,
        std::string const& targetKey
    ) {
        if (targetKey.empty()) {
            showError("Enter a non-empty map name.");
            return false;
        }
        if (targetKey == sourceKey) return true;

        makeRoomForSectionKey(targetKey);
        auto& saved = Mod::get()->getSaveContainer();
        if (!saved.isObject()) return true;

        auto const hasSections =
            saved.contains(targetKey) &&
            !saved[targetKey].isNull() &&
            (!saved[targetKey].isArray() || saved[targetKey].size() > 0);
        auto const flagKey = getFlagKeyForSectionKey(targetKey);
        auto const hasFlags =
            !flagKey.empty() &&
            saved.contains(flagKey) &&
            !saved[flagKey].isNull() &&
            (!saved[flagKey].isArray() || saved[flagKey].size() > 0);
        if (!hasSections && !hasFlags) {
            return true;
        }

        showError(
            "That map already has SectionData or FlagData."
        );
        return false;
    }

    void syncAfterMutation(
        std::string const& removedKey,
        std::string const& writtenKey,
        std::vector<SectionData> const& writtenSections,
        std::vector<FlagData> const& writtenFlags
    ) {
        if (!PlayLayer::get()) return;
        auto activeKey = getNameBasedLevelKey();
        if (!writtenKey.empty() && activeKey == writtenKey) {
            syncActiveSectionProgressBar(writtenSections);
            syncActiveFlagProgressBar(writtenFlags);
        }
        else if (!removedKey.empty() && activeKey == removedKey) {
            syncActiveSectionProgressBar({});
            syncActiveFlagProgressBar({});
        }
    }

    bool renameDataset(
        std::string const& sourceKey,
        std::string targetName
    ) {
        auto& saved = Mod::get()->getSaveContainer();
        auto const keepUnsupportedSource =
            saved.isObject() &&
            saved.contains(sourceKey) &&
            !saved[sourceKey].isNull() &&
            !isSupportedStoredSectionArray(saved[sourceKey]);
        auto const keepSource =
            isLegacyUnassignedSectionKey(sourceKey) ||
            keepUnsupportedSource;
        targetName = trimWhitespace(std::move(targetName));
        auto targetKey = getSectionKeyForMapName(targetName);
        auto sections = loadSectionsForKey(sourceKey);
        auto flags = loadFlagsForSectionKey(sourceKey);
        if (sections.empty() && flags.empty()) {
            showError("The source data no longer exists.");
            reloadList(true);
            return false;
        }

        if (targetKey == sourceKey) {
            setSectionMapDisplayName(sourceKey, targetName);
            reloadList(true);
            return true;
        }
        if (!canUseTargetKey(sourceKey, targetKey)) return false;

        if (!saveSectionsForKey(targetKey, sections, targetName)) {
            showError(
                "Recovery stopped because the target data is protected."
            );
            return false;
        }
        saveFlagsForSectionKey(targetKey, flags);
        setSectionMapDisplayName(targetKey, targetName);

        if (!keepSource) {
            saved.erase(sourceKey);
            removeSectionMapDisplayName(sourceKey);
            removeFlagsForSectionKey(sourceKey);
        }

        syncAfterMutation(
            keepSource ? "" : sourceKey,
            targetKey,
            sections,
            flags
        );
        reloadList(true);
        return true;
    }

    bool copyDataset(
        std::string const& sourceKey,
        std::string targetName
    ) {
        targetName = trimWhitespace(std::move(targetName));
        auto targetKey = getSectionKeyForMapName(targetName);
        if (targetKey == sourceKey) {
            showError("A copy needs a different map name.");
            return false;
        }

        auto sections = loadSectionsForKey(sourceKey);
        auto flags = loadFlagsForSectionKey(sourceKey);
        if (sections.empty() && flags.empty()) {
            showError("The source data no longer exists.");
            reloadList(true);
            return false;
        }
        if (!canUseTargetKey(sourceKey, targetKey)) return false;

        if (!saveSectionsForKey(targetKey, sections, targetName)) {
            showError(
                "Copy stopped because the target data is protected."
            );
            return false;
        }
        saveFlagsForSectionKey(targetKey, flags);
        setSectionMapDisplayName(targetKey, targetName);
        syncAfterMutation("", targetKey, sections, flags);
        reloadList(true);
        return true;
    }

    std::string getSuggestedCopyName(LocalSectionDataset const& dataset) {
        auto base = fmt::format("{} Copy", dataset.mapName);
        auto& saved = Mod::get()->getSaveContainer();
        for (int copyIndex = 1; copyIndex <= 9999; copyIndex++) {
            auto candidate = copyIndex == 1
                ? base
                : fmt::format("{} {}", base, copyIndex);
            auto key = getSectionKeyForMapName(candidate);
            if (
                !saved.isObject() ||
                !saved.contains(key) ||
                saved[key].isNull() ||
                (saved[key].isArray() && saved[key].size() == 0) ||
                (saved[key].isNumber() && key.ends_with(LEGACY_GO_KEY_SUFFIX))
            ) {
                return candidate;
            }
        }
        return fmt::format("{} Copy New", dataset.mapName);
    }

    void deleteDataset(std::string const& sectionKey) {
        auto& saved = Mod::get()->getSaveContainer();
        if (saved.isObject()) saved.erase(sectionKey);
        removeSectionMapDisplayName(sectionKey);
        removeFlagsForSectionKey(sectionKey);
        syncAfterMutation(sectionKey, "", {}, {});
        reloadList(true);
    }

    void reloadList(bool keepScroll = true) {
        float oldY = 0.f;
        if (keepScroll && m_scroll && m_scroll->m_contentLayer) {
            oldY = m_scroll->m_contentLayer->getPositionY();
        }

        m_datasets = loadLocalSectionDatasets();
        if (m_statusLabel) {
            m_statusLabel->setString(fmt::format(
                "{} local map{} - tap a map to preview",
                m_datasets.size(),
                m_datasets.size() == 1 ? "" : "s"
            ).c_str());
        }
        if (!m_scroll || !m_scroll->m_contentLayer) return;
        m_scroll->m_contentLayer->removeAllChildren();

        float contentHeight = std::max(
            180.f,
            static_cast<float>(m_datasets.size()) * 40.f + 12.f
        );
        m_scroll->m_contentLayer->setContentSize({420.f, contentHeight});

        if (m_datasets.empty()) {
            auto empty = CCLabelBMFont::create(
                "No local SectionData yet.",
                "bigFont.fnt"
            );
            empty->setScale(0.32f);
            empty->setPosition({210.f, contentHeight / 2.f});
            m_scroll->m_contentLayer->addChild(empty);
            return;
        }

        auto menu = CCMenu::create();
        menu->setContentSize({420.f, contentHeight});
        menu->setAnchorPoint({0.f, 0.f});
        menu->ignoreAnchorPointForPosition(false);
        menu->setPosition({0.f, 0.f});
        m_scroll->m_contentLayer->addChild(menu);

        float y = contentHeight - 21.f;
        for (int i = 0; i < static_cast<int>(m_datasets.size()); i++) {
            auto const& dataset = m_datasets[i];

            auto rowSprite = ButtonSprite::create(
                " ",
                235,
                true,
                "bigFont.fnt",
                "GJ_button_04.png",
                31.f,
                0.42f
            );
            auto nameLabel = CCLabelBMFont::create(
                dataset.mapName.c_str(),
                "bigFont.fnt"
            );
            nameLabel->setAnchorPoint({0.f, 0.5f});
            nameLabel->limitLabelWidth(165.f, 0.34f, 0.18f);
            nameLabel->setPosition({10.f, 15.5f});
            rowSprite->addChild(nameLabel, 2);

            auto countLabel = CCLabelBMFont::create(
                fmt::format("{}x", dataset.sections.size()).c_str(),
                "goldFont.fnt"
            );
            countLabel->setAnchorPoint({1.f, 0.5f});
            countLabel->setScale(0.28f);
            countLabel->setPosition({225.f, 15.5f});
            rowSprite->addChild(countLabel, 2);

            auto rowButton = CCMenuItemSpriteExtra::create(
                rowSprite,
                this,
                menu_selector(MyDataListPopup::onPreview)
            );
            rowButton->setTag(i);
            rowButton->m_scaleMultiplier = 1.04f;
            rowButton->setAnchorPoint({0.1f, 0.5f});
            rowButton->setPosition({
                10.f + rowButton->getContentSize().width * 0.1f,
                y
            });
            menu->addChild(rowButton);

            constexpr float actionStartX = 296.f;
            constexpr float actionSpacing = 44.f;

            auto renameSprite = ButtonSprite::create(
                isLegacyUnassignedSectionKey(dataset.sectionKey)
                    ? "Recover"
                    : "Rename"
            );
            renameSprite->setScale(0.39f);
            auto renameButton = CCMenuItemSpriteExtra::create(
                renameSprite,
                this,
                menu_selector(MyDataListPopup::onRename)
            );
            renameButton->setTag(i);
            renameButton->setPosition({actionStartX, y});
            menu->addChild(renameButton);

            auto copySprite = ButtonSprite::create("Copy");
            copySprite->setScale(0.39f);
            auto copyButton = CCMenuItemSpriteExtra::create(
                copySprite,
                this,
                menu_selector(MyDataListPopup::onCopy)
            );
            copyButton->setTag(i);
            copyButton->setPosition({actionStartX + actionSpacing, y});
            menu->addChild(copyButton);

            auto deleteSprite = ButtonSprite::create("X");
            deleteSprite->setScale(0.43f);
            auto deleteButton = CCMenuItemSpriteExtra::create(
                deleteSprite,
                this,
                menu_selector(MyDataListPopup::onDelete)
            );
            deleteButton->setTag(i);
            deleteButton->setPosition({
                actionStartX + actionSpacing * 2.f,
                y
            });
            menu->addChild(deleteButton);
            y -= 40.f;
        }

        if (keepScroll) {
            auto minY = std::min(0.f, 180.f - contentHeight);
            m_scroll->m_contentLayer->setPositionY(
                std::clamp(oldY, minY, 0.f)
            );
        }
        else {
            m_scroll->moveToTop();
        }
    }

    void onPreview(CCObject* sender) {
        int index = static_cast<CCNode*>(sender)->getTag();
        if (index < 0 || index >= static_cast<int>(m_datasets.size())) return;
        if (auto preview = LocalSectionPreviewPopup::create(m_datasets[index])) {
            preview->show();
        }
    }

    void onRename(CCObject* sender) {
        int index = static_cast<CCNode*>(sender)->getTag();
        if (index < 0 || index >= static_cast<int>(m_datasets.size())) return;
        auto key = m_datasets[index].sectionKey;
        auto const isLegacy = isLegacyUnassignedSectionKey(key);
        auto initialName = isLegacy ? "" : m_datasets[index].mapName;
        WeakRef<MyDataListPopup> self(this);
        if (auto popup = MapNameInputPopup::create(
            isLegacy
                ? "Recover Legacy SectionData"
                : "Rename Map Association",
            std::move(initialName),
            isLegacy ? "Recover" : "Rename",
            [self, key](std::string const& name) {
                auto owner = self.lock();
                return !owner || owner->renameDataset(key, name);
            }
        )) {
            popup->show();
        }
    }

    void onCopy(CCObject* sender) {
        int index = static_cast<CCNode*>(sender)->getTag();
        if (index < 0 || index >= static_cast<int>(m_datasets.size())) return;
        auto key = m_datasets[index].sectionKey;
        auto suggestedName = getSuggestedCopyName(m_datasets[index]);
        WeakRef<MyDataListPopup> self(this);
        if (auto popup = MapNameInputPopup::create(
            "Copy SectionData",
            std::move(suggestedName),
            "Copy",
            [self, key](std::string const& name) {
                auto owner = self.lock();
                return !owner || owner->copyDataset(key, name);
            }
        )) {
            popup->show();
        }
    }

    void onDelete(CCObject* sender) {
        int index = static_cast<CCNode*>(sender)->getTag();
        if (index < 0 || index >= static_cast<int>(m_datasets.size())) return;
        m_pendingDeleteKey = m_datasets[index].sectionKey;
        FLAlertLayer::create(
            this,
            "Delete Local SectionData",
            fmt::format(
                "Delete all local SectionData linked to <cy>{}</c>?",
                m_datasets[index].mapName
            ),
            "Cancel",
            "Delete"
        )->show();
    }

    void FLAlert_Clicked(FLAlertLayer*, bool btn2) override {
        auto key = std::move(m_pendingDeleteKey);
        m_pendingDeleteKey.clear();
        if (btn2 && !key.empty()) deleteDataset(key);
    }

public:
    static MyDataListPopup* create() {
        auto ret = new MyDataListPopup();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// Face Select Popup

class FaceSelectPopup : public geode::Popup {
protected:
    SectionListPopup* m_parent = nullptr;
    int m_index = -1;
    bool m_userSection = false;
    bool m_deleteMode = false;
    CCNode* m_content = nullptr;
    CCLabelBMFont* m_sectionLabel = nullptr;
    CCLabelBMFont* m_deleteModeLabel = nullptr;

    bool init(SectionListPopup* parent, int index) {
        if (!Popup::init(320.f, 260.f)) {
            return false;
        }

        m_parent = parent;
        m_index = index;

        this->setTitle("Difficulty Image");

        m_sectionLabel = CCLabelBMFont::create("Default", "goldFont.fnt");
        m_sectionLabel->setScale(0.45f);
        m_sectionLabel->setPosition({160.f, 215.f});
        m_mainLayer->addChild(m_sectionLabel);

        auto leftSprite = CCSprite::createWithSpriteFrameName("GJ_arrow_03_001.png");
        auto rightSprite = CCSprite::createWithSpriteFrameName("GJ_arrow_03_001.png");
        if (leftSprite && rightSprite) {
            leftSprite->setScale(0.55f);
            rightSprite->setScale(0.55f);
            rightSprite->setFlipX(true);

            auto left = CCMenuItemSpriteExtra::create(
                leftSprite, this, menu_selector(FaceSelectPopup::onChangeSection)
            );
            auto right = CCMenuItemSpriteExtra::create(
                rightSprite, this, menu_selector(FaceSelectPopup::onChangeSection)
            );
            left->setPosition({95.f, 215.f});
            right->setPosition({225.f, 215.f});
            m_buttonMenu->addChild(left);
            m_buttonMenu->addChild(right);
        }

        reloadContent();
        return true;
    }

    void reloadContent() {
        if (m_content) m_content->removeFromParent();
        m_content = CCNode::create();
        m_content->setPosition({0.f, 0.f});
        m_mainLayer->addChild(m_content);

        m_sectionLabel->setString(m_userSection ? "User Image" : "Default");

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        m_content->addChild(menu);

        float startX = 45.f;
        float startY = 190.f;
        float gapX = 38.f;
        float gapY = 34.f;

        if (!m_userSection) {
            // Only 0.png through 32.png are difficulty faces. The remaining
            // packaged images (bar, fill, and Mark) are HUD assets.
            for (int i = 0; i < 33; i++) {
                auto spr = createFaceSprite(i);
                scaleFaceToReference(spr, 0.2f);

                auto btn = CCMenuItemSpriteExtra::create(
                    spr, this, menu_selector(FaceSelectPopup::onSelectFace)
                );

                btn->setTag(i);

                int col = i % 7;
                int row = i / 7;

                btn->setPosition({startX + col * gapX, startY - row * gapY});
                menu->addChild(btn);
            }
            return;
        }

        auto images = loadUserImages();
        int rows = std::max(1, (static_cast<int>(images.size()) + 5) / 6);
        float contentHeight = std::max(175.f, rows * gapY + 10.f);

        // Keep the image list scrollable while reserving a fixed tool column
        // in the lower-right corner for Add and Delete Mode.
        auto scroll = ScrollLayer::create({235.f, 175.f});
        scroll->setPosition({15.f, 15.f});
        scroll->m_contentLayer->setContentSize({235.f, contentHeight});
        m_content->addChild(scroll);

        auto userMenu = CCMenu::create();
        userMenu->setPosition({0.f, 0.f});
        scroll->m_contentLayer->addChild(userMenu);

        auto importSprite = CCSprite::createWithSpriteFrameName("GJ_downloadBtn_001.png");
        CCNode* importVisual = importSprite
            ? static_cast<CCNode*>(importSprite)
            : static_cast<CCNode*>(ButtonSprite::create("Import"));
        importVisual->setScale(importSprite ? 0.65f : 0.5f);
        auto importButton = CCMenuItemSpriteExtra::create(
            importVisual, this, menu_selector(FaceSelectPopup::onImportImage)
        );
        importButton->setPosition({282.f, 172.f});
        menu->addChild(importButton);

        auto addLabel = CCLabelBMFont::create("Add", "bigFont.fnt");
        addLabel->setScale(0.28f);
        addLabel->setPosition({282.f, 145.f});
        m_content->addChild(addLabel);

        auto deleteToggle = CCMenuItemToggler::createWithStandardSprites(
            this,
            menu_selector(FaceSelectPopup::onToggleDeleteMode),
            0.65f
        );
        deleteToggle->toggle(m_deleteMode);
        deleteToggle->setPosition({282.f, 97.f});
        menu->addChild(deleteToggle);

        m_deleteModeLabel = CCLabelBMFont::create("Delete\nMode", "bigFont.fnt");
        m_deleteModeLabel->setAlignment(kCCTextAlignmentCenter);
        m_deleteModeLabel->setScale(0.25f);
        m_deleteModeLabel->setColor(
            m_deleteMode ? ccc3(255, 90, 90) : ccc3(255, 255, 255)
        );
        m_deleteModeLabel->setPosition({282.f, 63.f});
        m_content->addChild(m_deleteModeLabel);

        for (int i = 0; i < static_cast<int>(images.size()); i++) {
            auto spr = createFaceSprite(0, images[i]);
            scaleFaceToReference(spr, 0.2f);
            auto btn = CCMenuItemSpriteExtra::create(
                spr, this, menu_selector(FaceSelectPopup::onSelectUserImage)
            );
            btn->setTag(i);

            int col = i % 6;
            int row = i / 6;
            btn->setPosition({20.f + col * gapX, contentHeight - 20.f - row * gapY});
            userMenu->addChild(btn);
        }
        scroll->moveToTop();
    }

    void onChangeSection(CCObject*) {
        m_userSection = !m_userSection;
        reloadContent();
    }

    void onToggleDeleteMode(CCObject*) {
        m_deleteMode = !m_deleteMode;
        if (m_deleteModeLabel) {
            m_deleteModeLabel->setColor(
                m_deleteMode ? ccc3(255, 90, 90) : ccc3(255, 255, 255)
            );
        }
    }

    void onImportImage(CCObject*) {
        WeakRef<FaceSelectPopup> self(this);
        async::spawn(file::pick(file::PickMode::OpenFile, {
            .defaultPath = std::nullopt,
            .filters = {{
                .description = "Images",
                .files = {"*.png", "*.jpg", "*.jpeg"}
            }}
        }), [self](Result<std::optional<std::filesystem::path>> result) {
            auto popup = self.lock();
            if (!popup) return;
            if (result.isErr()) {
                FLAlertLayer::create("Import Failed", result.unwrapErr(), "OK")->show();
                return;
            }
            if (!result.unwrap().has_value()) return;

            auto source = result.unwrap().value();
            auto directory = Mod::get()->getSaveDir() / "difficulty-images";
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            auto destination = directory / fmt::format(
                "user-{}{}", stamp, source.extension().string()
            );
            std::filesystem::copy_file(
                source, destination,
                std::filesystem::copy_options::overwrite_existing, error
            );
            if (error) {
                FLAlertLayer::create(
                    "Import Failed", fmt::format("Could not copy image: {}", error.message()), "OK"
                )->show();
                return;
            }

            auto images = loadUserImages();
            images.push_back(destination.string());
            saveUserImages(images);
            popup->reloadContent();
        });
    }

    void onSelectUserImage(CCObject* sender) {
        auto images = loadUserImages();
        int index = static_cast<CCNode*>(sender)->getTag();
        if (index < 0 || index >= static_cast<int>(images.size())) return;

        if (m_deleteMode) {
            auto path = images[index];
            std::error_code error;
            if (std::filesystem::exists(path)) {
                std::filesystem::remove(path, error);
            }
            if (error) {
                FLAlertLayer::create(
                    "Delete Failed",
                    fmt::format("Could not delete image: {}", error.message()),
                    "OK"
                )->show();
                return;
            }

            images.erase(images.begin() + index);
            saveUserImages(images);
            if (m_parent) m_parent->removeCustomFaceReferences(path);
            reloadContent();
            return;
        }

        if (m_parent) m_parent->setCustomFace(m_index, images[index]);
        this->onClose(nullptr);
    }

    void onSelectFace(CCObject* sender) {
        int face = static_cast<CCNode*>(sender)->getTag();

        if (m_parent) {
            m_parent->setFace(m_index, face);
        }

        this->onClose(nullptr);
    }

public:
    static FaceSelectPopup* create(SectionListPopup* parent, int index) {
        auto ret = new FaceSelectPopup();

        if (ret && ret->init(parent, index)) {
            ret->autorelease();
            return ret;
        }

        delete ret;
        return nullptr;
    }
};

// Implement functions that depend on FaceSelectPopup after its definition.

#include "DeathSoundPopup.inl"

void SectionListPopup::onOpenFaceSelect(CCObject* sender) {
    int index = static_cast<CCNode*>(sender)->getTag();

    if (index < 0 || index >= static_cast<int>(m_sections.size())) {
        return;
    }

    FaceSelectPopup::create(this, index)->show();
}

class FlagDataListPopup : public geode::Popup {
protected:
    WeakRef<SectionListPopup> m_parent;
    std::vector<FlagData> m_flags;
    ScrollLayer* m_scroll = nullptr;
    CCLabelBMFont* m_statusLabel = nullptr;

    static CCNode* createColorSwatch(
        ccColor3B color,
        float size
    ) {
        auto holder = CCNode::create();
        holder->setContentSize({size, size});
        holder->setAnchorPoint({0.5f, 0.5f});
        holder->ignoreAnchorPointForPosition(false);

        auto border = CCLayerColor::create(
            ccc4(235, 235, 235, 255),
            size,
            size
        );
        holder->addChild(border);
        auto fill = CCLayerColor::create(
            ccc4(color.r, color.g, color.b, 255),
            size - 5.f,
            size - 5.f
        );
        fill->setPosition({2.5f, 2.5f});
        holder->addChild(fill);
        return holder;
    }

    bool init(SectionListPopup* parent) {
        if (!Popup::init(440.f, 290.f)) return false;

        m_parent = parent;

        this->setTitle("Flag Data");
        m_flags = loadFlags();

        auto normalLabel = CCLabelBMFont::create(
            "Normal Color",
            "bigFont.fnt"
        );
        normalLabel->setScale(0.32f);
        normalLabel->setAnchorPoint({0.f, 0.5f});
        normalLabel->setPosition({25.f, 225.f});
        m_mainLayer->addChild(normalLabel);

        auto normalColorSwatch = createColorSwatch(
            getNormalProgressColor(),
            28.f
        );
        normalColorSwatch->setPosition({145.f, 225.f});
        m_mainLayer->addChild(normalColorSwatch);

        auto freeEditionLabel = CCLabelBMFont::create(
            "Fixed percentages are editable",
            "goldFont.fnt"
        );
        freeEditionLabel->setScale(0.32f);
        freeEditionLabel->setPosition({330.f, 225.f});
        m_mainLayer->addChild(freeEditionLabel);

        m_scroll = ScrollLayer::create({420.f, 165.f});
        m_scroll->setStealingTouches(true);
        m_scroll->setPosition({10.f, 45.f});
        m_mainLayer->addChild(m_scroll);

        m_statusLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_statusLabel->setScale(0.28f);
        m_statusLabel->setPosition({220.f, 24.f});
        m_mainLayer->addChild(m_statusLabel);

        reloadList(false);
        return true;
    }

    void reloadList(bool keepScroll = true) {
        auto oldY = 0.f;
        if (keepScroll && m_scroll && m_scroll->m_contentLayer) {
            oldY = m_scroll->m_contentLayer->getPositionY();
        }

        if (m_statusLabel) {
            m_statusLabel->setString(fmt::format(
                "{} flag{}",
                m_flags.size(),
                m_flags.size() == 1 ? "" : "s"
            ).c_str());
        }
        if (!m_scroll || !m_scroll->m_contentLayer) return;
        m_scroll->m_contentLayer->removeAllChildren();

        auto const contentHeight = std::max(
            165.f,
            static_cast<float>(m_flags.size()) * 42.f + 12.f
        );
        m_scroll->m_contentLayer->setContentSize({420.f, contentHeight});

        if (m_flags.empty()) {
            auto empty = CCLabelBMFont::create(
                "No FlagData to display.",
                "bigFont.fnt"
            );
            empty->setScale(0.31f);
            empty->setPosition({210.f, contentHeight / 2.f});
            m_scroll->m_contentLayer->addChild(empty);
            return;
        }

        auto y = contentHeight - 22.f;
        for (int index = 0; index < static_cast<int>(m_flags.size()); index++) {
            auto const& flag = m_flags[index];

            if (auto icon = createFlagIconSprite(flag.iconFrame, 24.f)) {
                icon->setPosition({24.f, y});
                m_scroll->m_contentLayer->addChild(icon);
            }

            auto const id = flag.id;
            auto nameInput = TextInput::create(190.f, "Flag text...");
            nameInput->setCommonFilter(CommonFilter::Any);
            nameInput->setMaxCharCount(24);
            nameInput->setString(flag.label);
            nameInput->setScale(0.55f);
            nameInput->setPosition({112.f, y});
            nameInput->setEnabled(false);
            m_scroll->m_contentLayer->addChild(nameInput);

            auto percentInput = TextInput::create(65.f, "0~100");
            percentInput->setCommonFilter(CommonFilter::Float);
            percentInput->setString(fmt::format("{:.1f}", flag.percent));
            percentInput->setScale(0.58f);
            percentInput->setPosition({194.f, y});
            percentInput->setVisible(
                flag.source != FlagPercentSource::PersonalBest
            );
            percentInput->setCallback([this, id](std::string const& value) {
                if (value.empty()) return;
                try {
                    auto percent = std::stof(value);
                    if (!std::isfinite(percent)) return;
                    auto found = std::find_if(
                        m_flags.begin(),
                        m_flags.end(),
                        [&id](FlagData const& flag) {
                            return flag.id == id;
                        }
                    );
                    if (found == m_flags.end()) return;
                    found->percent = clampLevelPercent(percent);
                    saveFlags(m_flags);
                }
                catch (...) {}
            });
            m_scroll->m_contentLayer->addChild(percentInput);

            auto pbSprite = CCSprite::createWithSpriteFrameName(
                flag.source == FlagPercentSource::PersonalBest
                    ? "GJ_checkOn_001.png"
                    : "GJ_checkOff_001.png"
            );
            if (pbSprite) {
                pbSprite->setScale(0.45f);
                pbSprite->setPosition({230.f, y});
                m_scroll->m_contentLayer->addChild(pbSprite);
            }

            auto pbLabel = CCLabelBMFont::create("PB", "bigFont.fnt");
            pbLabel->setScale(0.22f);
            pbLabel->setPosition({248.f, y});
            m_scroll->m_contentLayer->addChild(pbLabel);

            auto colorSwatch = createColorSwatch(
                unpackProgressColor(flag.passedColor),
                22.f
            );
            colorSwatch->setPosition({274.f, y});
            m_scroll->m_contentLayer->addChild(colorSwatch);

            y -= 42.f;
        }

        if (keepScroll) {
            auto const minimumY = std::min(0.f, 165.f - contentHeight);
            m_scroll->m_contentLayer->setPositionY(
                std::clamp(oldY, minimumY, 0.f)
            );
        }
        else {
            m_scroll->moveToTop();
        }
    }

public:
    void onClose(CCObject* sender) override {
        if (auto parent = m_parent.lock()) {
            parent->setSectionControlsEnabled(true);
        }
        Popup::onClose(sender);
    }

    static FlagDataListPopup* create(SectionListPopup* parent) {
        auto ret = new FlagDataListPopup();
        if (ret && ret->init(parent)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

void SectionListPopup::onOpenFlagData(CCObject*) {
    if (auto popup = FlagDataListPopup::create(this)) {
        setSectionControlsEnabled(false);
        popup->show();
    }
}

class SectionPreviewPopup : public geode::Popup {
protected:
    WeakRef<SectionListPopup> m_parent;
    ServerSectionPreset m_preset;

    bool init(SectionListPopup* parent, ServerSectionPreset preset);
    void onApply(CCObject*);

public:
    static SectionPreviewPopup* create(
        SectionListPopup* parent,
        ServerSectionPreset preset
    );
};

class ServerMapScrollLayer : public ScrollLayer {
protected:
    using TapCallback = geode::Function<void(int)>;

    TapCallback m_tapCallback;
    std::vector<std::pair<CCNode*, int>> m_tapTargets;
    CCPoint m_touchStart;
    bool m_trackingTouch = false;
    bool m_dragged = false;

    ServerMapScrollLayer(CCRect const& rect, TapCallback callback)
      : ScrollLayer(rect, true, true),
        m_tapCallback(std::move(callback)) {}

    bool ccTouchBegan(CCTouch* touch, CCEvent* event) override {
        m_trackingTouch = ScrollLayer::ccTouchBegan(touch, event);
        if (m_trackingTouch) {
            m_touchStart = touch->getLocation();
            m_dragged = false;
        }
        return m_trackingTouch;
    }

    void ccTouchMoved(CCTouch* touch, CCEvent* event) override {
        if (m_trackingTouch) {
            auto const delta = touch->getLocation() - m_touchStart;
            if (delta.x * delta.x + delta.y * delta.y > 64.f) {
                m_dragged = true;
            }
        }
        ScrollLayer::ccTouchMoved(touch, event);
    }

    void ccTouchEnded(CCTouch* touch, CCEvent* event) override {
        auto const releaseLocation = touch->getLocation();
        auto const shouldOpen = m_trackingTouch && !m_dragged;

        ScrollLayer::ccTouchEnded(touch, event);
        m_trackingTouch = false;

        if (!shouldOpen || !m_tapCallback) return;

        auto const viewportPoint = this->convertToNodeSpace(releaseLocation);
        if (!CCRect({0.f, 0.f}, this->getContentSize()).containsPoint(
            viewportPoint
        )) {
            return;
        }

        for (auto const& [target, index] : m_tapTargets) {
            if (!target || !target->isVisible() || !target->getParent()) {
                continue;
            }
            auto const parentPoint = target->getParent()->convertToNodeSpace(
                releaseLocation
            );
            if (target->boundingBox().containsPoint(parentPoint)) {
                m_tapCallback(index);
                return;
            }
        }
    }

    void ccTouchCancelled(CCTouch* touch, CCEvent* event) override {
        m_trackingTouch = false;
        m_dragged = true;
        ScrollLayer::ccTouchCancelled(touch, event);
    }

public:
    static ServerMapScrollLayer* create(CCSize const& size, TapCallback callback) {
        auto ret = new ServerMapScrollLayer(
            {0.f, 0.f, size.width, size.height},
            std::move(callback)
        );
        ret->autorelease();
        return ret;
    }

    void clearTapTargets() {
        m_tapTargets.clear();
    }

    void addTapTarget(CCNode* target, int index) {
        if (target) m_tapTargets.emplace_back(target, index);
    }
};

class ServerMapListPopup : public geode::Popup {
protected:
    WeakRef<SectionListPopup> m_parent;
    std::vector<ServerMapEntry> m_maps;
    std::string m_searchQuery;

    TextInput* m_searchInput = nullptr;
    ServerMapScrollLayer* m_scroll = nullptr;
    CCLabelBMFont* m_statusLabel = nullptr;
    bool m_requestInFlight = false;

    bool init(SectionListPopup* parent);
    void setStatus(char const* text);
    void showRequestError(std::string const& message);
    void reloadMapList();
    void loadMaps();
    void openMap(int index);
    void onRefresh(CCObject*);

public:
    static ServerMapListPopup* create(SectionListPopup* parent = nullptr);
};

class DownloadMenu : public geode::Popup, public FLAlertLayerProtocol {
protected:
    enum class PendingConfirmation {
        None,
        Upload,
        Delete,
    };

    WeakRef<SectionListPopup> m_parent;
    std::string m_databaseUrl;
    std::string m_mapName;
    std::string m_mapKey;
    std::vector<ServerSectionPreset> m_presets;
    bool m_canManageCurrentMap = false;

    ScrollLayer* m_scroll = nullptr;
    CCLabelBMFont* m_statusLabel = nullptr;
    bool m_requestInFlight = false;
    PendingConfirmation m_pendingConfirmation = PendingConfirmation::None;

    std::string getMapEndpoint() const {
        return fmt::format(
            "{}/section-data/map-{}",
            m_databaseUrl,
            m_mapKey
        );
    }

    std::string getUserDataPath(std::string const& userName) const {
        return fmt::format(
            "section-data/map-{}/user-{}",
            m_mapKey,
            firebaseSafeKey(userName)
        );
    }

    std::string getUserIndexPath(std::string const& userName) const {
        return fmt::format(
            "map-index/map-{}/user-{}",
            m_mapKey,
            firebaseSafeKey(userName)
        );
    }

    void setStatus(char const* text) {
        if (m_statusLabel) m_statusLabel->setString(text);
    }

    void showRequestError(std::string const& message) {
        m_requestInFlight = false;
        setStatus("Request failed");
        FLAlertLayer::create("Server Error", message, "OK")->show();
    }

    bool init(SectionListPopup* parent, std::string mapName) {
        if (!Popup::init(410.f, 285.f)) return false;

        m_parent = parent;
        m_databaseUrl = FIREBASE_DATABASE_URL;
        m_mapName = trimWhitespace(std::move(mapName));
        if (m_mapName.empty()) {
            m_mapName = getCurrentMapName();
        }
        m_mapKey = firebaseSafeKey(normalizeLevelName(m_mapName));
        m_canManageCurrentMap =
            parent &&
            m_mapKey == firebaseSafeKey(
                normalizeLevelName(getCurrentMapName())
            );

        this->setTitle("Download Menu");

        auto listIcon = CCDrawNode::create();
        listIcon->setContentSize({26.f, 22.f});
        listIcon->setAnchorPoint({0.5f, 0.5f});

        ccColor4F outline = {0.08f, 0.08f, 0.08f, 1.f};
        ccColor4F foreground = {1.f, 1.f, 1.f, 1.f};
        for (float y : {5.f, 11.f, 17.f}) {
            listIcon->drawDot({4.f, y}, 2.8f, outline);
            listIcon->drawSegment({10.f, y}, {24.f, y}, 2.2f, outline);
            listIcon->drawDot({4.f, y}, 1.65f, foreground);
            listIcon->drawSegment({10.f, y}, {24.f, y}, 1.15f, foreground);
        }

        auto mapListSprite = CircleButtonSprite::create(
            listIcon,
            CircleBaseColor::Green,
            CircleBaseSize::Small
        );
        mapListSprite->setTopRelativeScale(0.78f);
        mapListSprite->setScale(0.58f);
        auto mapListButton = CCMenuItemSpriteExtra::create(
            mapListSprite,
            this,
            menu_selector(DownloadMenu::onOpenMapList)
        );
        mapListButton->setID("server-map-list-button"_spr);
        auto popupSize = m_mainLayer->getContentSize();
        mapListButton->setPosition({
            popupSize.width - 22.f,
            popupSize.height - 22.f
        });
        m_buttonMenu->addChild(mapListButton);

        auto mapLabel = CCLabelBMFont::create(
            fmt::format("Map: {}", m_mapName).c_str(),
            "goldFont.fnt"
        );
        mapLabel->setScale(0.34f);
        mapLabel->limitLabelWidth(350.f, 0.34f, 0.18f);
        mapLabel->setPosition({205.f, 228.f});
        m_mainLayer->addChild(mapLabel);

        m_statusLabel = CCLabelBMFont::create("Loading...", "bigFont.fnt");
        m_statusLabel->setScale(0.25f);
        m_statusLabel->setPosition({205.f, 207.f});
        m_mainLayer->addChild(m_statusLabel);

        m_scroll = ScrollLayer::create({390.f, 155.f});
        m_scroll->setPosition({10.f, 48.f});
        m_mainLayer->addChild(m_scroll);

        if (m_canManageCurrentMap) {
            auto uploadSprite = ButtonSprite::create("Upload / Overwrite");
            uploadSprite->setScale(0.48f);
            auto uploadButton = CCMenuItemSpriteExtra::create(
                uploadSprite,
                this,
                menu_selector(DownloadMenu::onUpload)
            );
            uploadButton->setPosition({105.f, 25.f});
            m_buttonMenu->addChild(uploadButton);

            auto deleteSprite = ButtonSprite::create("Delete Mine");
            deleteSprite->setScale(0.48f);
            auto deleteButton = CCMenuItemSpriteExtra::create(
                deleteSprite,
                this,
                menu_selector(DownloadMenu::onDeleteMine)
            );
            deleteButton->setPosition({220.f, 25.f});
            m_buttonMenu->addChild(deleteButton);
        }

        auto refreshSprite = ButtonSprite::create("Refresh");
        refreshSprite->setScale(0.48f);
        auto refreshButton = CCMenuItemSpriteExtra::create(
            refreshSprite,
            this,
            menu_selector(DownloadMenu::onRefresh)
        );
        refreshButton->setPosition({
            m_canManageCurrentMap ? 325.f : 205.f,
            25.f
        });
        m_buttonMenu->addChild(refreshButton);

        reloadPresetList();
        loadPresets();
        return true;
    }

    void reloadPresetList() {
        if (!m_scroll || !m_scroll->m_contentLayer) return;
        m_scroll->m_contentLayer->removeAllChildren();

        float contentHeight = std::max(
            155.f,
            static_cast<float>(m_presets.size()) * 38.f + 12.f
        );
        m_scroll->m_contentLayer->setContentSize({390.f, contentHeight});

        if (m_presets.empty()) {
            auto empty = CCLabelBMFont::create(
                "No uploads for this map.",
                "bigFont.fnt"
            );
            empty->setScale(0.3f);
            empty->setPosition({195.f, contentHeight / 2.f});
            m_scroll->m_contentLayer->addChild(empty);
            return;
        }

        auto menu = CCMenu::create();
        menu->setContentSize({390.f, contentHeight});
        menu->setAnchorPoint({0.f, 0.f});
        menu->ignoreAnchorPointForPosition(false);
        menu->setPosition({0.f, 0.f});
        m_scroll->m_contentLayer->addChild(menu);

        auto gameManager = GameManager::sharedState();
        auto loggedInName = normalizeLevelName(getLoggedInGDUsername());
        float y = contentHeight - 21.f;
        for (int i = 0; i < static_cast<int>(m_presets.size()); i++) {
            auto const& preset = m_presets[i];
            auto rowSprite = ButtonSprite::create(
                " ",
                350,
                true,
                "bigFont.fnt",
                "GJ_button_04.png",
                30.f,
                0.42f
            );

            int iconID = preset.playerIcon;
            int color1 = preset.playerColor1;
            int color2 = preset.playerColor2;
            int glowColor = preset.playerGlowColor;
            bool glow = preset.playerGlow;
            bool useCurrentAppearance =
                !preset.hasPlayerIcon &&
                !loggedInName.empty() &&
                normalizeLevelName(preset.userName) == loggedInName;

            if (gameManager && useCurrentAppearance) {
                iconID = gameManager->getPlayerFrame();
                color1 = gameManager->getPlayerColor();
                color2 = gameManager->getPlayerColor2();
                glow = gameManager->getPlayerGlow();
                glowColor = gameManager->getPlayerGlowColor();
            }

            if (gameManager) {
                iconID = std::clamp(
                    iconID,
                    1,
                    std::max(
                        1,
                        gameManager->countForType(IconType::Cube)
                    )
                );
            }
            else {
                iconID = std::max(1, iconID);
            }

            if (auto playerIcon = SimplePlayer::create(iconID)) {
                if (gameManager) {
                    playerIcon->setColors(
                        gameManager->colorForIdx(color1),
                        gameManager->colorForIdx(color2)
                    );
                    if (glow) {
                        playerIcon->setGlowOutline(
                            gameManager->colorForIdx(glowColor)
                        );
                    }
                    else {
                        playerIcon->disableGlowOutline();
                    }
                }
                playerIcon->setScale(0.52f);
                playerIcon->setPosition({20.f, 15.f});
                rowSprite->addChild(playerIcon, 2);
            }

            auto nameLabel = CCLabelBMFont::create(
                preset.userName.c_str(),
                "bigFont.fnt"
            );
            nameLabel->setAnchorPoint({0.f, 0.5f});
            nameLabel->limitLabelWidth(235.f, 0.38f, 0.18f);
            nameLabel->setPosition({39.f, 15.f});
            rowSprite->addChild(nameLabel, 2);

            auto ageLabel = CCLabelBMFont::create(
                formatCompactUploadAge(preset.updatedAt).c_str(),
                "goldFont.fnt"
            );
            ageLabel->setAnchorPoint({1.f, 0.5f});
            ageLabel->setScale(0.3f);
            ageLabel->setPosition({340.f, 15.f});
            rowSprite->addChild(ageLabel, 2);

            auto rowButton = CCMenuItemSpriteExtra::create(
                rowSprite,
                this,
                menu_selector(DownloadMenu::onOpenPreset)
            );
            rowButton->setTag(i);
            rowButton->setPosition({195.f, y});
            menu->addChild(rowButton);
            y -= 38.f;
        }
        m_scroll->moveToTop();
    }

    void loadPresets() {
        if (m_requestInFlight) return;
        m_requestInFlight = true;
        setStatus("Downloading map uploads...");

        auto request = web::WebRequest();
        request.timeout(std::chrono::seconds(15));

        WeakRef<DownloadMenu> self(this);
        async::spawn(
            request.get(getMapEndpoint() + ".json"),
            [self](web::WebResponse response) {
                auto popup = self.lock();
                if (!popup) return;

                if (!response.ok()) {
                    popup->showRequestError(fmt::format(
                        "Download failed (HTTP {}).\n{}",
                        response.code(),
                        getFirebaseResponseError(response)
                    ));
                    return;
                }

                auto jsonResult = response.json();
                if (!jsonResult) {
                    popup->showRequestError(
                        "Firebase returned invalid JSON."
                    );
                    return;
                }

                std::vector<ServerSectionPreset> presets;
                auto const& root = jsonResult.unwrap();
                if (!root.isNull() && !root.isObject()) {
                    popup->showRequestError(
                        "The map node must be a JSON object."
                    );
                    return;
                }

                if (root.isObject()) {
                    for (auto const& item : root) {
                        auto key = item.getKey().value_or("unknown");
                        if (
                            presets.size() >= 200 ||
                            !item.isObject() ||
                            !item.contains("userName") ||
                            !item.contains("sections") ||
                            !item["userName"].isString()
                        ) {
                            continue;
                        }

                        ServerSectionPreset preset;
                        preset.userName = truncateUtf8(
                            item["userName"].asString().unwrapOr(key),
                            64
                        );
                        preset.updatedAt =
                            item["updatedAt"].asDouble().unwrapOr(0.0);

                        int playerIcon = 1;
                        int playerColor1 = 0;
                        int playerColor2 = 3;
                        int playerGlowColor = 0;
                        if (
                            item.contains("playerIcon") &&
                            item.contains("playerColor1") &&
                            item.contains("playerColor2") &&
                            item.contains("playerGlow") &&
                            item.contains("playerGlowColor") &&
                            readServerInteger(
                                item["playerIcon"],
                                1,
                                2000,
                                playerIcon
                            ) &&
                            readServerInteger(
                                item["playerColor1"],
                                0,
                                255,
                                playerColor1
                            ) &&
                            readServerInteger(
                                item["playerColor2"],
                                0,
                                255,
                                playerColor2
                            ) &&
                            item["playerGlow"].isBool() &&
                            readServerInteger(
                                item["playerGlowColor"],
                                0,
                                255,
                                playerGlowColor
                            )
                        ) {
                            preset.playerIcon = playerIcon;
                            preset.playerColor1 = playerColor1;
                            preset.playerColor2 = playerColor2;
                            preset.playerGlow =
                                item["playerGlow"].asBool().unwrapOr(false);
                            preset.playerGlowColor = playerGlowColor;
                            preset.hasPlayerIcon = true;
                        }

                        std::string error;
                        auto valid = parseServerSectionArray(
                            item["sections"],
                            preset.sections,
                            error
                        );
                        if (valid && item.contains("flagData")) {
                            valid = parseServerFlagData(
                                item["flagData"],
                                preset.flags,
                                error
                            );
                            preset.hasFlagData = valid;
                        }

                        if (valid) {
                            presets.push_back(std::move(preset));
                        }
                        else {
                            log::warn(
                                "Skipping invalid server preset {}: {}",
                                key,
                                error
                            );
                        }
                    }
                }

                std::sort(
                    presets.begin(),
                    presets.end(),
                    [](auto const& a, auto const& b) {
                        return a.userName < b.userName;
                    }
                );

                popup->m_presets = std::move(presets);
                popup->m_requestInFlight = false;
                popup->setStatus(fmt::format(
                    "{} upload(s) found",
                    popup->m_presets.size()
                ).c_str());
                popup->reloadPresetList();
            }
        );
    }

    void onOpenPreset(CCObject* sender) {
        int index = static_cast<CCNode*>(sender)->getTag();
        if (index < 0 || index >= static_cast<int>(m_presets.size())) return;
        auto parent = m_parent.lock();
        if (auto preview = SectionPreviewPopup::create(
            parent ? parent.data() : nullptr,
            m_presets[index]
        )) {
            preview->show();
        }
    }

    void onOpenMapList(CCObject*);

    void onRefresh(CCObject*) {
        loadPresets();
    }

    void onUpload(CCObject*) {
        if (m_requestInFlight || !m_canManageCurrentMap) return;

        auto userName = getLoggedInGDUsername();
        if (userName.empty()) {
            FLAlertLayer::create(
                "GD Login Required",
                "Log in to your Geometry Dash account before uploading.",
                "OK"
            )->show();
            return;
        }

        auto parent = m_parent.lock();
        if (!parent || parent->getSections().empty()) {
            FLAlertLayer::create(
                "Upload Failed",
                "There is no local SectionData to upload.",
                "OK"
            )->show();
            return;
        }
        if (parent->getSections().size() > 500) {
            FLAlertLayer::create(
                "Upload Failed",
                "A server upload can contain at most 500 SectionData items.",
                "OK"
            )->show();
            return;
        }
        auto flags = loadFlags();
        if (flags.size() > 100) {
            FLAlertLayer::create(
                "Upload Failed",
                "A server upload can contain at most 100 FlagData items.",
                "OK"
            )->show();
            return;
        }

        m_pendingConfirmation = PendingConfirmation::Upload;
        FLAlertLayer::create(
            this,
            "Share Preset Online",
            "Upload your <cy>GD username</c>, <cg>map name</c>, icon colors, "
            "SectionData, and FlagData to the public preset database? "
            "Anyone can view and download it. Cancel to keep the data local.",
            "Cancel",
            "Upload"
        )->show();
    }

    void uploadOwnPreset() {
        if (m_requestInFlight || !m_canManageCurrentMap) return;

        auto userName = getLoggedInGDUsername();
        auto parent = m_parent.lock();
        if (userName.empty() || !parent || parent->getSections().empty()) {
            return;
        }

        auto flags = loadFlags();
        if (parent->getSections().size() > 500 || flags.size() > 100) {
            return;
        }

        auto body = matjson::Value::object();
        body["mapName"] = m_mapName;
        body["userName"] = userName;
        body["sections"] = sectionsToServerJson(parent->getSections());
        body["flagData"] = flagsToServerJson(flags);
        if (auto gameManager = GameManager::sharedState()) {
            body["playerIcon"] = std::clamp(
                gameManager->getPlayerFrame(),
                1,
                2000
            );
            body["playerColor1"] = std::clamp(
                gameManager->getPlayerColor(),
                0,
                255
            );
            body["playerColor2"] = std::clamp(
                gameManager->getPlayerColor2(),
                0,
                255
            );
            body["playerGlow"] = gameManager->getPlayerGlow();
            body["playerGlowColor"] = std::clamp(
                gameManager->getPlayerGlowColor(),
                0,
                255
            );
        }
        auto timestamp = matjson::Value::object();
        timestamp[".sv"] = "timestamp";
        body["updatedAt"] = std::move(timestamp);

        m_requestInFlight = true;
        setStatus("Uploading SectionData and FlagData...");

        auto updates = matjson::Value::object();
        updates[getUserDataPath(userName)] = std::move(body);
        updates[getUserIndexPath(userName)] = m_mapName;

        auto request = web::WebRequest();
        request.bodyJSON(updates);
        request.timeout(std::chrono::seconds(15));

        WeakRef<DownloadMenu> self(this);
        async::spawn(
            request.patch(m_databaseUrl + "/.json?print=silent"),
            [self](web::WebResponse response) {
                auto popup = self.lock();
                if (!popup) return;

                popup->m_requestInFlight = false;
                if (!response.ok()) {
                    popup->showRequestError(fmt::format(
                        "Upload failed (HTTP {}).\n{}",
                        response.code(),
                        getFirebaseResponseError(response)
                    ));
                    return;
                }

                FLAlertLayer::create(
                    "Upload Complete",
                    "Your SectionData, FlagData, and map-list entry were updated successfully.",
                    "OK"
                )->show();
                popup->loadPresets();
            }
        );
    }

    void onDeleteMine(CCObject*) {
        if (m_requestInFlight || !m_canManageCurrentMap) return;
        auto userName = getLoggedInGDUsername();
        if (userName.empty()) {
            FLAlertLayer::create(
                "GD Login Required",
                "Log in to your Geometry Dash account before deleting.",
                "OK"
            )->show();
            return;
        }

        m_pendingConfirmation = PendingConfirmation::Delete;
        FLAlertLayer::create(
            this,
            "Delete Server Data",
            fmt::format(
                "Delete <cy>{}</c>'s SectionData for <cg>{}</c>?",
                userName,
                m_mapName
            ),
            "Cancel",
            "Delete"
        )->show();
    }

    void deleteOwnPreset() {
        auto userName = getLoggedInGDUsername();
        if (userName.empty()) return;

        m_requestInFlight = true;
        setStatus("Deleting your upload...");

        auto updates = matjson::Value::object();
        updates[getUserDataPath(userName)] = nullptr;
        updates[getUserIndexPath(userName)] = nullptr;

        auto request = web::WebRequest();
        request.bodyJSON(updates);
        request.timeout(std::chrono::seconds(15));

        WeakRef<DownloadMenu> self(this);
        async::spawn(
            request.patch(m_databaseUrl + "/.json?print=silent"),
            [self](web::WebResponse response) {
                auto popup = self.lock();
                if (!popup) return;

                popup->m_requestInFlight = false;
                if (!response.ok()) {
                    popup->showRequestError(fmt::format(
                        "Delete failed (HTTP {}).\n{}",
                        response.code(),
                        getFirebaseResponseError(response)
                    ));
                    return;
                }

                FLAlertLayer::create(
                    "Delete Complete",
                    "Your server SectionData was deleted.",
                    "OK"
                )->show();
                popup->loadPresets();
            }
        );
    }

    void FLAlert_Clicked(FLAlertLayer*, bool btn2) override {
        auto const confirmation = m_pendingConfirmation;
        m_pendingConfirmation = PendingConfirmation::None;
        if (!btn2) return;

        if (confirmation == PendingConfirmation::Upload) {
            uploadOwnPreset();
        }
        else if (confirmation == PendingConfirmation::Delete) {
            deleteOwnPreset();
        }
    }

public:
    static DownloadMenu* create(
        SectionListPopup* parent,
        std::string mapName = ""
    ) {
        auto ret = new DownloadMenu();
        if (ret && ret->init(parent, std::move(mapName))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

bool SectionPreviewPopup::init(
    SectionListPopup* parent,
    ServerSectionPreset preset
) {
    if (!Popup::init(380.f, 270.f)) return false;

    m_parent = parent;
    m_preset = std::move(preset);
    this->setTitle("Server Data Preview");

    auto userLabel = CCLabelBMFont::create(
        (m_preset.hasFlagData
            ? fmt::format(
                "{} - {} section(s), {} flag(s)",
                m_preset.userName,
                m_preset.sections.size(),
                m_preset.flags.size()
            )
            : fmt::format(
                "{} - {} section(s), legacy upload",
                m_preset.userName,
                m_preset.sections.size()
            )).c_str(),
        "goldFont.fnt"
    );
    userLabel->setScale(0.34f);
    userLabel->limitLabelWidth(330.f, 0.34f, 0.18f);
    userLabel->setPosition({190.f, 215.f});
    m_mainLayer->addChild(userLabel);

    auto scroll = ScrollLayer::create({360.f, 165.f});
    scroll->setPosition({10.f, 43.f});
    m_mainLayer->addChild(scroll);

    auto const previewLineCount =
        m_preset.sections.size() +
        (m_preset.hasFlagData ? m_preset.flags.size() + 1 : 1);
    float contentHeight = std::max(
        165.f,
        static_cast<float>(previewLineCount) * 29.f + 10.f
    );
    scroll->m_contentLayer->setContentSize({360.f, contentHeight});

    float y = contentHeight - 18.f;
    for (int i = 0; i < static_cast<int>(m_preset.sections.size()); i++) {
        auto const& section = m_preset.sections[i];
        auto line = CCLabelBMFont::create(
            fmt::format(
                "#{}  {:.1f}%  Diff {:.1f}  Face {}  {}",
                i + 1,
                section.startPercent,
                section.difficulty / 10.f,
                section.faces,
                section.partName
            ).c_str(),
            "bigFont.fnt"
        );
        line->setAnchorPoint({0.f, 0.5f});
        line->setScale(0.27f);
        line->limitLabelWidth(340.f, 0.27f, 0.16f);
        line->setPosition({10.f, y});
        scroll->m_contentLayer->addChild(line);
        y -= 29.f;
    }

    auto flagHeader = CCLabelBMFont::create(
        m_preset.hasFlagData
            ? fmt::format("FlagData ({})", m_preset.flags.size()).c_str()
            : "Legacy upload - local FlagData will be kept",
        "goldFont.fnt"
    );
    flagHeader->setAnchorPoint({0.f, 0.5f});
    flagHeader->setScale(0.27f);
    flagHeader->limitLabelWidth(340.f, 0.27f, 0.16f);
    flagHeader->setPosition({10.f, y});
    scroll->m_contentLayer->addChild(flagHeader);
    y -= 29.f;

    if (m_preset.hasFlagData) {
        for (int i = 0; i < static_cast<int>(m_preset.flags.size()); i++) {
            auto const& flag = m_preset.flags[i];
            auto line = CCLabelBMFont::create(
                fmt::format(
                    "#{}  {}  {}",
                    i + 1,
                    flag.source == FlagPercentSource::PersonalBest
                        ? "PB"
                        : fmt::format("{:.1f}%", flag.percent),
                    flag.label
                ).c_str(),
                "bigFont.fnt"
            );
            line->setAnchorPoint({0.f, 0.5f});
            line->setScale(0.27f);
            line->limitLabelWidth(340.f, 0.27f, 0.16f);
            line->setPosition({10.f, y});
            scroll->m_contentLayer->addChild(line);
            y -= 29.f;
        }
    }
    scroll->moveToTop();

    if (parent) {
        auto applySprite = ButtonSprite::create("Apply This Data");
        applySprite->setScale(0.52f);
        auto applyButton = CCMenuItemSpriteExtra::create(
            applySprite,
            this,
            menu_selector(SectionPreviewPopup::onApply)
        );
        applyButton->setPosition({190.f, 23.f});
        m_buttonMenu->addChild(applyButton);
    }
    else {
        auto browseOnly = CCLabelBMFont::create(
            "Browse only - open this map to apply",
            "goldFont.fnt"
        );
        browseOnly->setScale(0.27f);
        browseOnly->setPosition({190.f, 23.f});
        m_mainLayer->addChild(browseOnly);
    }
    return true;
}

void SectionPreviewPopup::onApply(CCObject*) {
    if (auto parent = m_parent.lock()) {
        if (!parent->applyServerData(
            m_preset.sections,
            m_preset.flags,
            m_preset.hasFlagData
        )) {
            return;
        }
        FLAlertLayer::create(
            "Server Data Applied",
            m_preset.hasFlagData
                ? "The selected SectionData and FlagData are now active for this map."
                : "The selected legacy SectionData is active. Local FlagData was kept.",
            "OK"
        )->show();
    }
    this->onClose(nullptr);
}

SectionPreviewPopup* SectionPreviewPopup::create(
    SectionListPopup* parent,
    ServerSectionPreset preset
) {
    auto ret = new SectionPreviewPopup();
    if (ret && ret->init(parent, std::move(preset))) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool ServerMapListPopup::init(SectionListPopup* parent) {
    if (!Popup::init(410.f, 285.f)) return false;

    m_parent = parent;
    this->setTitle("Server Map List");

    m_searchInput = TextInput::create(320.f, "Search map name...");
    m_searchInput->setScale(0.72f);
    m_searchInput->setPosition({160.f, 230.f});
    m_searchInput->setCommonFilter(CommonFilter::Any);
    m_searchInput->setCallback([this](std::string const& value) {
        m_searchQuery = normalizeLevelName(value);
        reloadMapList();
    });
    m_mainLayer->addChild(m_searchInput);

    auto refreshSprite = ButtonSprite::create("Refresh");
    refreshSprite->setScale(0.45f);
    auto refreshButton = CCMenuItemSpriteExtra::create(
        refreshSprite,
        this,
        menu_selector(ServerMapListPopup::onRefresh)
    );
    refreshButton->setPosition({340.f, 230.f});
    m_buttonMenu->addChild(refreshButton);

    m_statusLabel = CCLabelBMFont::create("Loading...", "bigFont.fnt");
    m_statusLabel->setScale(0.25f);
    m_statusLabel->setPosition({205.f, 205.f});
    m_mainLayer->addChild(m_statusLabel);

    m_scroll = ServerMapScrollLayer::create(
        {390.f, 150.f},
        [this](int index) {
            this->openMap(index);
        }
    );
    m_scroll->setPosition({10.f, 48.f});
    m_mainLayer->addChild(m_scroll);

    loadMaps();
    return true;
}

void ServerMapListPopup::setStatus(char const* text) {
    if (m_statusLabel) m_statusLabel->setString(text);
}

void ServerMapListPopup::showRequestError(std::string const& message) {
    m_requestInFlight = false;
    setStatus("Request failed");
    FLAlertLayer::create("Server Error", message, "OK")->show();
}

void ServerMapListPopup::reloadMapList() {
    if (!m_scroll || !m_scroll->m_contentLayer) return;
    m_scroll->clearTapTargets();
    m_scroll->m_contentLayer->removeAllChildren();

    std::vector<int> visibleMaps;
    visibleMaps.reserve(m_maps.size());
    for (int i = 0; i < static_cast<int>(m_maps.size()); i++) {
        if (
            m_searchQuery.empty() ||
            normalizeLevelName(m_maps[i].mapName).find(m_searchQuery) !=
                std::string::npos
        ) {
            visibleMaps.push_back(i);
        }
    }

    float contentHeight = std::max(
        150.f,
        static_cast<float>(visibleMaps.size()) * 38.f + 12.f
    );
    m_scroll->m_contentLayer->setContentSize({390.f, contentHeight});

    if (visibleMaps.empty()) {
        auto empty = CCLabelBMFont::create(
            m_maps.empty()
                ? "No indexed maps yet."
                : "No matching map names.",
            "bigFont.fnt"
        );
        empty->setScale(0.3f);
        empty->setPosition({195.f, contentHeight / 2.f});
        m_scroll->m_contentLayer->addChild(empty);
        return;
    }

    float y = contentHeight - 21.f;
    for (auto index : visibleMaps) {
        auto const& entry = m_maps[index];
        auto rowSprite = ButtonSprite::create(
            " ",
            350,
            true,
            "bigFont.fnt",
            "GJ_button_04.png",
            30.f,
            0.42f
        );

        auto nameLabel = CCLabelBMFont::create(
            entry.mapName.c_str(),
            "bigFont.fnt"
        );
        nameLabel->setAnchorPoint({0.f, 0.5f});
        nameLabel->limitLabelWidth(260.f, 0.38f, 0.18f);
        nameLabel->setPosition({12.f, 15.f});
        rowSprite->addChild(nameLabel, 2);

        auto countLabel = CCLabelBMFont::create(
            fmt::format("{} upload{}", entry.uploadCount,
                entry.uploadCount == 1 ? "" : "s").c_str(),
            "goldFont.fnt"
        );
        countLabel->setAnchorPoint({1.f, 0.5f});
        countLabel->setScale(0.28f);
        countLabel->setPosition({340.f, 15.f});
        rowSprite->addChild(countLabel, 2);

        rowSprite->setPosition({195.f, y});
        m_scroll->m_contentLayer->addChild(rowSprite);
        m_scroll->addTapTarget(rowSprite, index);
        y -= 38.f;
    }
    m_scroll->moveToTop();
}

void ServerMapListPopup::loadMaps() {
    if (m_requestInFlight) return;
    m_requestInFlight = true;
    setStatus("Downloading map index...");

    auto request = web::WebRequest();
    request.timeout(std::chrono::seconds(15));

    WeakRef<ServerMapListPopup> self(this);
    async::spawn(
        request.get(
            std::string(FIREBASE_DATABASE_URL) + "/map-index.json"
        ),
        [self](web::WebResponse response) {
            auto popup = self.lock();
            if (!popup) return;

            if (!response.ok()) {
                popup->showRequestError(fmt::format(
                    "Map list download failed (HTTP {}).\n{}",
                    response.code(),
                    getFirebaseResponseError(response)
                ));
                return;
            }

            auto jsonResult = response.json();
            if (!jsonResult) {
                popup->showRequestError(
                    "Firebase returned an invalid map index."
                );
                return;
            }

            auto const& root = jsonResult.unwrap();
            if (!root.isNull() && !root.isObject()) {
                popup->showRequestError(
                    "The map-index node must be a JSON object."
                );
                return;
            }

            std::vector<ServerMapEntry> maps;
            if (root.isObject()) {
                for (auto const& mapNode : root) {
                    if (maps.size() >= 2000) break;

                    auto databaseKey = mapNode.getKey().value_or("");
                    if (
                        !databaseKey.starts_with("map-") ||
                        !mapNode.isObject()
                    ) {
                        continue;
                    }

                    std::unordered_map<std::string, std::size_t> nameCounts;
                    for (auto const& userNode : mapNode) {
                        if (!userNode.isString()) continue;
                        auto name = trimWhitespace(truncateUtf8(
                            userNode.asString().unwrapOr(""),
                            100
                        ));
                        if (name.empty()) continue;
                        nameCounts[name]++;
                    }
                    if (nameCounts.empty()) continue;

                    std::string mapName;
                    std::size_t bestCount = 0;
                    std::size_t uploadCount = 0;
                    for (auto const& [candidate, count] : nameCounts) {
                        auto expectedKey = fmt::format(
                            "map-{}",
                            firebaseSafeKey(normalizeLevelName(candidate))
                        );
                        if (databaseKey != expectedKey) continue;

                        uploadCount += count;
                        if (
                            count > bestCount ||
                            (
                                count == bestCount &&
                                normalizeLevelName(candidate) <
                                    normalizeLevelName(mapName)
                            )
                        ) {
                            mapName = candidate;
                            bestCount = count;
                        }
                    }

                    if (mapName.empty()) {
                        log::warn(
                            "Skipping inconsistent map-index entry {}",
                            databaseKey
                        );
                        continue;
                    }

                    maps.push_back({
                        std::move(mapName),
                        databaseKey.substr(4),
                        uploadCount
                    });
                }
            }

            std::sort(
                maps.begin(),
                maps.end(),
                [](auto const& a, auto const& b) {
                    auto left = normalizeLevelName(a.mapName);
                    auto right = normalizeLevelName(b.mapName);
                    if (left != right) return left < right;
                    return a.mapName < b.mapName;
                }
            );

            popup->m_maps = std::move(maps);
            popup->m_requestInFlight = false;
            popup->setStatus(fmt::format(
                "{} map(s) indexed - A to Z",
                popup->m_maps.size()
            ).c_str());
            popup->reloadMapList();
        }
    );
}

void ServerMapListPopup::openMap(int index) {
    if (index < 0 || index >= static_cast<int>(m_maps.size())) return;

    auto parent = m_parent.lock();
    SectionListPopup* applicableParent = nullptr;
    if (
        parent &&
        m_maps[index].mapKey ==
            firebaseSafeKey(normalizeLevelName(getCurrentMapName()))
    ) {
        applicableParent = parent.data();
    }

    if (auto menu = DownloadMenu::create(
        applicableParent,
        m_maps[index].mapName
    )) {
        menu->show();
    }
}

void ServerMapListPopup::onRefresh(CCObject*) {
    loadMaps();
}

ServerMapListPopup* ServerMapListPopup::create(SectionListPopup* parent) {
    auto ret = new ServerMapListPopup();
    if (ret && ret->init(parent)) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

$on_mod(Loaded) {
    ensureProgressDataUpgradeBackup();
    ModPopupUIEvent().listen([](
        FLAlertLayer* popup,
        std::string_view modID,
        std::optional<Mod*>
    ) {
        if (
            modID != "tipp7.tutorial" ||
            !popup ||
            !popup->m_buttonMenu
        ) {
            return ListenerResult::Propagate;
        }

        auto serverButtonID = Mod::get()->expandSpriteName(
            "server-map-list-about-button"
        );
        if (!popup->getChildByIDRecursive(serverButtonID)) {
            auto sprite = ButtonSprite::create("Server Map List");
            sprite->setScale(0.44f);
            auto button = CCMenuItemExt::createSpriteExtra(
                sprite,
                [](CCMenuItemSpriteExtra*) {
                    if (auto list = ServerMapListPopup::create()) {
                        list->show();
                    }
                }
            );
            button->setID(serverButtonID);
            popup->m_buttonMenu->addChildAtPosition(
                button,
                Anchor::BottomRight,
                {-78.f, 23.f}
            );
        }

        auto myDataButtonID = Mod::get()->expandSpriteName(
            "my-data-list-about-button"
        );
        if (!popup->getChildByIDRecursive(myDataButtonID)) {
            auto sprite = ButtonSprite::create("My Data List");
            sprite->setScale(0.44f);
            auto button = CCMenuItemExt::createSpriteExtra(
                sprite,
                [](CCMenuItemSpriteExtra*) {
                    if (auto list = MyDataListPopup::create()) {
                        list->show();
                    }
                }
            );
            button->setID(myDataButtonID);
            popup->m_buttonMenu->addChildAtPosition(
                button,
                Anchor::BottomRight,
                {-185.f, 23.f}
            );
        }
        return ListenerResult::Propagate;
    }).leak();
}

void DownloadMenu::onOpenMapList(CCObject*) {
    auto parent = m_parent.lock();
    if (auto list = ServerMapListPopup::create(
        parent ? parent.data() : nullptr
    )) {
        list->show();
    }
}

void SectionListPopup::onServerDownload(CCObject*) {
    auto mapName = getCurrentMapName();

    if (mapName.empty()) {
        FLAlertLayer::create(
            "Server Download",
            "The current map has no name.",
            "OK"
        )->show();
        return;
    }

    if (auto menu = DownloadMenu::create(this)) {
        menu->show();
    }
}

#include <Geode/modify/PlayLayer.hpp>
#include <Geode/binding/PlayLayer.hpp>

using namespace geode::prelude;

class SectionProgressBar : public CCNode {
protected:
    std::vector<SectionData> m_sections;
    std::vector<FlagData> m_flags;

    struct FlagVisual {
        CCNode* root = nullptr;
        CCSprite* marker = nullptr;
        CCSprite* icon = nullptr;
        CCLabelBMFont* label = nullptr;
    };
    std::vector<FlagVisual> m_flagVisuals;

    CCSprite* m_barFrame = nullptr;
    CCProgressTimer* m_barFill = nullptr;

    CCLabelBMFont* m_partLabel = nullptr;
    CCLabelBMFont* m_percentLabel = nullptr;
    CCLabelBMFont* m_progressLabel = nullptr;
    CCLabelBMFont* m_difficultyLabel = nullptr;
    CCLabelBMFont* m_partIndexLabel = nullptr;


    CCSprite* m_faceSprite = nullptr;
    int m_currentFaceID = -1;
    std::string m_currentCustomImage;

    float m_barScale = 1.f;
    float m_height = 0.0f;
    float m_barWidth = 0.f;

    bool m_hudEnabled = true;
    bool m_showFlags = true;
    bool m_showBestHUD = true;
    bool m_showDifficultyHUD = true;
    bool m_showLabelHUD = true;
    bool m_showTotalPercentHUD = true;
    bool m_showPartPercentHUD = true;
    bool m_showPartIndexHUD = true;
    int m_percentDecimalPlaces = DEFAULT_PERCENT_DECIMAL_PLACES;

    float m_hudOpacity = 1.f;
    float m_difficultyFaceScale = 1.f;
    float m_difficultyFaceOffsetX = 0.f;
    float m_difficultyFaceOffsetY = 0.f;
    float m_difficultyFontScale = 1.f;
    float m_difficultyFontOffsetX = 0.f;
    float m_difficultyFontOffsetY = 0.f;
    float m_partNameScale = 1.f;
    float m_partNameOffsetX = 0.f;
    float m_partNameOffsetY = 0.f;
    std::string m_difficultyFont = DEFAULT_DIFFICULTY_FONT;
    std::string m_partNameFont = DEFAULT_PART_NAME_FONT;
    ccColor3B m_normalColor = ccc3(255, 255, 255);
    ccColor3B m_goColor = ccc3(102, 255, 56);
    ccColor3B m_bestColor = ccc3(245, 255, 100);

    float resolveFlagPercent(FlagData const& flag) const {
        if (flag.source != FlagPercentSource::PersonalBest) {
            return clampLevelPercent(flag.percent);
        }

        auto playLayer = PlayLayer::get();
        if (!playLayer || !playLayer->m_level) return 0.f;
        return clampLevelPercent(static_cast<float>(
            playLayer->m_level->m_normalPercent.value()
        ));
    }

    void hideFlagVisuals() {
        for (auto const& visual : m_flagVisuals) {
            if (visual.root) visual.root->setVisible(false);
        }
    }

    void rebuildFlagVisuals() {
        for (auto const& visual : m_flagVisuals) {
            if (visual.root) visual.root->removeFromParent();
        }
        m_flagVisuals.clear();

        for (auto const& flag : m_flags) {
            FlagVisual visual;
            visual.root = CCNode::create();
            visual.root->setScale(sanitizeFlagDetailValue(
                flag.scale,
                FLAG_DETAIL_SCALE_MIN,
                FLAG_DETAIL_SCALE_MAX,
                1.f
            ));
            this->addChild(visual.root, 10);

            visual.marker = CCSprite::create("Mark.png"_spr);
            if (visual.marker) {
                visual.marker->setAnchorPoint({0.5f, 0.5f});
                visual.marker->setScaleX(0.4f);
                visual.marker->setScaleY(0.7f);
                visual.root->addChild(visual.marker, 0);
            }

            visual.icon = createFlagIconSprite(flag.iconFrame, 11.f);
            if (visual.icon) {
                visual.icon->setAnchorPoint({0.5f, 0.5f});
                visual.icon->setPosition({0.f, 6.7f});
                visual.root->addChild(visual.icon, 1);
            }

            visual.label = CCLabelBMFont::create(
                flag.label.c_str(),
                "bigFont.fnt"
            );
            if (visual.label) {
                visual.label->setScale(0.24f);
                visual.label->setAnchorPoint({0.f, 0.5f});
                visual.label->limitLabelWidth(85.f, 0.24f, 0.12f);
                visual.label->setPosition({5.5f, 8.7f});
                visual.root->addChild(visual.label, 2);
            }

            m_flagVisuals.push_back(visual);
        }
        hideFlagVisuals();
    }

    void updateFlagVisuals(float start, float end, bool includeEnd) {
        if (!m_showFlags || end <= start) {
            hideFlagVisuals();
            return;
        }

        for (std::size_t index = 0; index < m_flagVisuals.size(); index++) {
            auto const percent = resolveFlagPercent(m_flags[index]);
            auto const inside = includeEnd
                ? percent >= start && percent <= end
                : percent >= start && percent < end;
            auto const visible = inside && end > start;
            auto const& visual = m_flagVisuals[index];

            if (visual.root) visual.root->setVisible(visible);
            if (!visible) continue;

            auto const ratio = std::clamp(
                (percent - start) / (end - start),
                0.f,
                1.f
            );
            auto const x = -m_barWidth / 2.f + m_barWidth * ratio;
            if (visual.root) {
                visual.root->setPosition({
                    x + sanitizeFlagDetailValue(
                        m_flags[index].offsetX,
                        FLAG_DETAIL_OFFSET_MIN,
                        FLAG_DETAIL_OFFSET_MAX,
                        0.f
                    ),
                    sanitizeFlagDetailValue(
                        m_flags[index].offsetY,
                        FLAG_DETAIL_OFFSET_MIN,
                        FLAG_DETAIL_OFFSET_MAX,
                        0.f
                    )
                });
            }
        }
    }

    ccColor3B progressColorForPercent(float totalPercent) const {
        auto color = m_normalColor;
        if (!m_showFlags) return color;

        auto latestPercent = -1.f;
        for (auto const& flag : m_flags) {
            auto const flagPercent = resolveFlagPercent(flag);
            if (
                totalPercent > flagPercent &&
                flagPercent >= latestPercent
            ) {
                latestPercent = flagPercent;
                color = unpackProgressColor(flag.passedColor);
            }
        }
        return color;
    }

    void updatePercentLabelPositions() {
        if (!m_percentLabel || !m_progressLabel) {
            return;
        }

        auto const totalPercentX = m_barWidth / 2.f + 4.f;
        auto const currentText = std::string(m_percentLabel->getString());
        auto const maximumText = formatHUDPercent(
            100.f,
            m_percentDecimalPlaces
        );

        // Reserve enough horizontal space for 100% at the selected precision,
        // so shorter live values never move or overlap the part percentage.
        m_percentLabel->setString(maximumText.c_str());
        auto const maximumWidth =
            m_percentLabel->getContentSize().width *
            m_percentLabel->getScaleX();
        m_percentLabel->setString(currentText.c_str());

        m_percentLabel->setPositionX(totalPercentX);
        m_progressLabel->setPositionX(
            totalPercentX + maximumWidth + 5.f
        );
    }

    void applyHUDOpacity() {
        auto opacity = static_cast<GLubyte>(
            std::round(255.f * m_hudOpacity)
        );
        auto markerOpacity = static_cast<GLubyte>(
            std::round(150.f * m_hudOpacity)
        );

        m_barFrame->setOpacity(opacity);
        m_barFill->setOpacity(opacity);
        for (std::size_t index = 0; index < m_flagVisuals.size(); index++) {
            auto const detailOpacity = index < m_flags.size()
                ? sanitizeFlagDetailValue(
                    m_flags[index].opacity,
                    FLAG_DETAIL_OPACITY_MIN,
                    FLAG_DETAIL_OPACITY_MAX,
                    1.f
                )
                : 1.f;
            auto const flagOpacity = static_cast<GLubyte>(
                std::round(static_cast<float>(opacity) * detailOpacity)
            );
            auto const flagMarkerOpacity = static_cast<GLubyte>(
                std::round(static_cast<float>(markerOpacity) * detailOpacity)
            );
            auto const& visual = m_flagVisuals[index];
            if (visual.marker) visual.marker->setOpacity(flagMarkerOpacity);
            if (visual.icon) visual.icon->setOpacity(flagOpacity);
            if (visual.label) visual.label->setOpacity(flagOpacity);
        }
        m_partLabel->setOpacity(opacity);
        m_percentLabel->setOpacity(opacity);
        m_progressLabel->setOpacity(opacity);
        m_difficultyLabel->setOpacity(opacity);
        m_partIndexLabel->setOpacity(opacity);
        m_faceSprite->setOpacity(opacity);
    }

public:
    static SectionProgressBar* create(std::vector<SectionData> sections) {
        auto ret = new SectionProgressBar();

        if (ret && ret->initWithSections(sections)) {
            ret->autorelease();
            return ret;
        }


        delete ret;
        return nullptr;
    }

    void setSections(std::vector<SectionData> sections) {
        m_sections = std::move(sections);
        sortSections(m_sections);

        m_currentFaceID = -1;
        m_currentCustomImage.clear();
        this->setVisible(m_hudEnabled);
        this->update(0.f);
    }

    void setFlags(std::vector<FlagData> flags) {
        sortFlags(flags);
        m_flags = std::move(flags);
        rebuildFlagVisuals();
        reloadCustomizationSettings();
    }

    void reloadCustomizationSettings() {
        m_hudEnabled = isProgressHUDEnabled();
        m_showFlags = isFlagHUDEnabled();
        m_showBestHUD = isBestHUDEnabled();
        m_showDifficultyHUD = isDifficultyHUDEnabled();
        m_showLabelHUD = isLabelHUDEnabled();
        m_showTotalPercentHUD = isTotalPercentHUDEnabled();
        m_showPartPercentHUD = isPartPercentHUDEnabled();
        m_showPartIndexHUD = isPartIndexHUDEnabled();
        m_percentDecimalPlaces = getPercentDecimalPlaces();
        m_hudOpacity = getProgressHUDOpacity();
        m_difficultyFaceScale = getDifficultyFaceScale();
        m_difficultyFaceOffsetX = getDifficultyFaceOffsetX();
        m_difficultyFaceOffsetY = getDifficultyFaceOffsetY();
        m_difficultyFontScale = getDifficultyFontScale();
        m_difficultyFontOffsetX = getDifficultyFontOffsetX();
        m_difficultyFontOffsetY = getDifficultyFontOffsetY();
        m_partNameScale = getPartNameScale();
        m_partNameOffsetX = getPartNameOffsetX();
        m_partNameOffsetY = getPartNameOffsetY();
        m_normalColor = getNormalProgressColor();
        m_goColor = getGoProgressColor();
        m_bestColor = getBestProgressColor();
        auto const difficultyFont = getDifficultyHUDFont();
        if (
            m_difficultyLabel &&
            difficultyFont != m_difficultyFont
        ) {
            m_difficultyLabel->setFntFile(difficultyFont.c_str());
        }
        m_difficultyFont = difficultyFont;
        auto const partNameFont = getPartNameHUDFont();
        if (
            m_partLabel &&
            partNameFont != m_partNameFont
        ) {
            m_partLabel->setFntFile(partNameFont.c_str());
        }
        m_partNameFont = partNameFont;
        if (m_faceSprite) {
            scaleFaceToReference(
                m_faceSprite,
                0.17f * m_difficultyFaceScale
            );
            m_faceSprite->setPosition({
                -m_barWidth / 2.f - 16.f + m_difficultyFaceOffsetX,
                -3.f + m_difficultyFaceOffsetY,
            });
        }
        if (m_difficultyLabel) {
            m_difficultyLabel->setScale(
                0.35f * m_difficultyFontScale
            );
            m_difficultyLabel->setPosition({
                -m_barWidth / 2.f - 16.f + m_difficultyFontOffsetX,
                -22.f + m_difficultyFontOffsetY,
            });
        }
        if (m_partLabel) {
            m_partLabel->setScale(0.36f * m_partNameScale);
            m_partLabel->setPosition({
                -m_barWidth / 2.f + m_partNameOffsetX,
                -10.f - m_height + m_partNameOffsetY,
            });
        }

        updatePercentLabelPositions();

        this->setScale(getProgressHUDScale());

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        this->setPosition({
            winSize.width / 2.f + getProgressHUDOffsetX(),
            winSize.height - 12.f + getProgressHUDOffsetY()
        });

        applyHUDOpacity();
        this->setVisible(m_hudEnabled);
        this->update(0.f);
    }

    bool initWithSections(std::vector<SectionData> sections) {
        if (!CCNode::init()) {
            return false;
        }

        m_sections = sections;
        sortSections(m_sections);
        m_flags = loadFlags();
        sortFlags(m_flags);

        auto fillSprite = CCSprite::create("fill.png"_spr);
        if (!fillSprite) return false;

        fillSprite->setScale(m_barScale);

        m_barFill = CCProgressTimer::create(fillSprite);
        m_barFill->setType(kCCProgressTimerTypeBar);
        m_barFill->setMidpoint({0.f, 0.5f});
        m_barFill->setBarChangeRate({1.f, 0.f});
        m_barFill->setPercentage(0.f);
        m_barFill->setPosition({0.f, 0.f - m_height});
        this->addChild(m_barFill, 0);

        m_barFrame = CCSprite::create("bar.png"_spr);
        if (!m_barFrame) return false;

        m_barFrame->setScale(m_barScale);
        m_barFrame->setPosition({0.f, 0.f - m_height});
        this->addChild(m_barFrame, 1);

        float barWidth = m_barFrame->getContentSize().width * m_barScale;

        m_percentLabel = CCLabelBMFont::create("0.0%", "bigFont.fnt");
        m_percentLabel->setScale(0.4f);
        m_percentLabel->setAnchorPoint({0.f, 0.5f});
        m_percentLabel->setPosition({barWidth / 2.f + 4.f, 0.5f});
        this->addChild(m_percentLabel, 5);

        m_progressLabel = CCLabelBMFont::create("0.0%", "bigFont.fnt");
        m_progressLabel->setScale(0.3f);
        m_progressLabel->setAnchorPoint({0.f, 0.5f});
        m_progressLabel->setPosition({barWidth / 2.f + 49.f, 0.5f});
        this->addChild(m_progressLabel, 5);

        m_partIndexLabel = CCLabelBMFont::create("Part 0/0", "goldFont.fnt");
        m_partIndexLabel->setScale(0.36f);
        m_partIndexLabel->setAnchorPoint({1.0f, 0.5f});
        m_partIndexLabel->setPosition({barWidth / 2.f, -10.f - m_height});
        this->addChild(m_partIndexLabel, 5);

        m_partNameFont = getPartNameHUDFont();
        m_partLabel = CCLabelBMFont::create(
            "",
            m_partNameFont.c_str()
        );
        m_partLabel->setScale(0.36f);
        m_partLabel->setAnchorPoint({0.0f, 0.5f});
        m_partLabel->setPosition({-barWidth / 2.f, -10.f - m_height});
        this->addChild(m_partLabel, 5);

        m_faceSprite = createFaceSprite(0);
        if (!m_faceSprite) return false;

        scaleFaceToReference(
            m_faceSprite,
            0.17f * m_difficultyFaceScale
        );
        m_faceSprite->setAnchorPoint({0.5f, 0.5f});
        m_faceSprite->setPosition({-barWidth / 2.f - 16.f, -3.f});
        this->addChild(m_faceSprite, 5);

        m_difficultyFont = getDifficultyHUDFont();
        m_difficultyLabel = CCLabelBMFont::create(
            "0.0/10",
            m_difficultyFont.c_str()
        );
        m_difficultyLabel->setScale(
            0.35f * m_difficultyFontScale
        );
        m_difficultyLabel->setPosition({-barWidth / 2.f -16.f, -22.f});
        this->addChild(m_difficultyLabel, 6);

        m_currentFaceID = 0;
        m_barWidth = m_barFrame->getContentSize().width * m_barScale;

        rebuildFlagVisuals();

        reloadCustomizationSettings();

        this->scheduleUpdate();
        return true;
    }

    void updateWithoutSections(float totalPercent) {
        m_barFill->setPercentage(totalPercent);
        m_percentLabel->setString(
            formatHUDPercent(totalPercent, m_percentDecimalPlaces).c_str()
        );
        m_percentLabel->setVisible(m_showTotalPercentHUD);

        m_progressLabel->setVisible(false);
        m_partIndexLabel->setVisible(false);

        auto mapName = getCurrentMapName();
        m_partLabel->setString(mapName.c_str());
        m_partLabel->setVisible(m_showLabelHUD);

        m_faceSprite->setVisible(false);
        m_difficultyLabel->setVisible(false);

        updateFlagVisuals(0.f, 100.f, true);
        m_barFill->setColor(progressColorForPercent(totalPercent));
    }

    float lerp(float a, float b, float t) {
        return a + (b - a) * t;
    }

    void updatePlatformer(PlayLayer* playLayer) {
        auto const elapsedMilliseconds = std::max<int64_t>(
            0,
            static_cast<int64_t>(playLayer->getPlayTimerMilli())
        );
        auto const bestMilliseconds = std::max<int64_t>(
            0,
            static_cast<int64_t>(playLayer->m_level->m_bestTime)
        );

        // Platformer levels have no meaningful linear completion percentage.
        // Use their native completion metric instead: elapsed time and PB.
        auto const pbPace = bestMilliseconds > 0
            ? std::clamp(
                static_cast<float>(elapsedMilliseconds) /
                    static_cast<float>(bestMilliseconds) * 100.f,
                0.f,
                100.f
            )
            : 0.f;
        m_barFill->setPercentage(pbPace);
        if (bestMilliseconds <= 0) {
            m_barFill->setColor(m_normalColor);
        }
        else if (elapsedMilliseconds > bestMilliseconds) {
            m_barFill->setColor(ccc3(255, 100, 80));
        }
        else if (pbPace >= 90.f) {
            m_barFill->setColor(m_bestColor);
        }
        else {
            m_barFill->setColor(m_goColor);
        }

        m_percentLabel->setString(
            formatPlatformerTime(elapsedMilliseconds).c_str()
        );
        m_percentLabel->setVisible(m_showTotalPercentHUD);
        m_progressLabel->setVisible(false);

        m_partIndexLabel->setString(
            bestMilliseconds > 0
                ? fmt::format(
                    "PB {}",
                    formatPlatformerTime(bestMilliseconds)
                ).c_str()
                : "PB --:--.---"
        );
        m_partIndexLabel->setVisible(m_showBestHUD);

        auto const mapName = getCurrentMapName();
        m_partLabel->setString(mapName.c_str());
        m_partLabel->setVisible(m_showLabelHUD);

        m_faceSprite->setVisible(false);
        m_difficultyLabel->setVisible(false);
        hideFlagVisuals();
    }

    void update(float) override {
        auto pl = PlayLayer::get();

        if (!pl || !pl->m_level || !pl->m_player1) {
            return;
        }

        this->setVisible(m_hudEnabled);
        if (!m_hudEnabled) {
            return;
        }

        if (pl->m_isPlatformer || pl->m_level->isPlatformer()) {
            updatePlatformer(pl);
            return;
        }

        float totalPercent = getCurrentLevelPercent(pl);

        if (m_sections.empty()) {
            updateWithoutSections(totalPercent);
            return;
        }

        m_percentLabel->setVisible(m_showTotalPercentHUD);
        m_progressLabel->setVisible(m_showPartPercentHUD);
        m_partIndexLabel->setVisible(m_showPartIndexHUD);

        int index = findCurrentSection(totalPercent);

        if (index < 0) {
            auto const end = std::max(0.f, m_sections.front().startPercent);
            auto const localPercent = end > 0.f
                ? std::clamp(totalPercent / end * 100.f, 0.f, 100.f)
                : 0.f;
            m_barFill->setPercentage(localPercent);
            m_barFill->setColor(progressColorForPercent(totalPercent));
            m_percentLabel->setString(
                formatHUDPercent(
                    totalPercent,
                    m_percentDecimalPlaces
                ).c_str()
            );
            m_progressLabel->setString(
                formatHUDPercent(
                    localPercent,
                    m_percentDecimalPlaces
                ).c_str()
            );
            m_partIndexLabel->setString(
                fmt::format("Part 0/{}", m_sections.size()).c_str()
            );
            m_partLabel->setString("");
            m_faceSprite->setVisible(false);
            m_difficultyLabel->setVisible(false);
            updateFlagVisuals(0.f, end, false);
            return;
        }

        auto const& section = m_sections[index];

        float start = section.startPercent;
        float end = 100.f;

        if (index + 1 < static_cast<int>(m_sections.size())) {
            end = m_sections[index + 1].startPercent;
        }

        float localPercent = 0.f;

        if (end > start) {
            localPercent = (totalPercent - start) / (end - start) * 100.f;
        }

        m_difficultyLabel->setString(
            fmt::format("{:.1f}", section.difficulty / 10.f).c_str()
        );
        float t = std::clamp(section.difficulty / 100.f, 0.f, 1.f);
        GLubyte r = 255;
        GLubyte g = static_cast<GLubyte>(255.f * (1.f - t));
        GLubyte b = 80;
        m_difficultyLabel->setColor(ccc3(r, g, b));

        m_progressLabel->setColor(ccc3(
            255.f * (1.f - localPercent / 100.f),
            255.f,
            255.f * (1.f - localPercent / 100.f)
        ));

        localPercent = std::clamp(localPercent, 0.f, 100.f);

        m_progressLabel->setString(
            formatHUDPercent(localPercent, m_percentDecimalPlaces).c_str()
        );

        m_partIndexLabel->setString(
            fmt::format("Part {}/{}", index + 1, m_sections.size()).c_str()
        );

        m_barFill->setPercentage(localPercent);

        m_percentLabel->setString(
            formatHUDPercent(totalPercent, m_percentDecimalPlaces).c_str()
        );

        m_partLabel->setString(
            section.partName.empty()
                ? ""
                : section.partName.c_str()
        );
        m_partLabel->setVisible(m_showLabelHUD);

        m_barFill->setColor(progressColorForPercent(totalPercent));

        updateFace(section.faces, section.customImage);
        updateFlagVisuals(
            start,
            end,
            index == static_cast<int>(m_sections.size()) - 1
        );
    }

    int findCurrentSection(float totalPercent) {
        int result = -1;

        for (int i = 0; i < static_cast<int>(m_sections.size()); i++) {
            if (totalPercent >= m_sections[i].startPercent) {
                result = i;
            }
            else {
                break;
            }
        }

        return result;
    }

    void updateFace(int faceID, std::string const& customImage) {
        if(!m_showDifficultyHUD) {
            m_faceSprite->setVisible(false);
            m_difficultyLabel->setVisible(false);
            return;
        }
        else {
            m_faceSprite->setVisible(true);
            m_difficultyLabel->setVisible(true);
        }

        faceID = std::clamp(faceID, 0, 32);

        if (faceID == m_currentFaceID && customImage == m_currentCustomImage) {
            return;
        }

        auto newFace = createFaceSprite(faceID, customImage);

        if (!newFace || !m_faceSprite) {
            return;
        }

        m_faceSprite->setDisplayFrame(newFace->displayFrame());
        scaleFaceToReference(
            m_faceSprite,
            0.17f * m_difficultyFaceScale
        );
        m_currentFaceID = faceID;
        m_currentCustomImage = customImage;
    }
};

static void syncActiveSectionProgressBar(
    std::vector<SectionData> const& sections
) {
    auto playLayer = PlayLayer::get();
    if (!playLayer) {
        return;
    }

    auto node = playLayer->getChildByID("section-progress-bar"_spr);

    if (node) {
        auto progressBar = typeinfo_cast<SectionProgressBar*>(node);
        if (!progressBar) {
            log::warn("Failed to cast section-progress-bar during data sync");
            return;
        }

        progressBar->setSections(sections);
        return;
    }

    auto bar = SectionProgressBar::create(sections);
    if (!bar) {
        return;
    }

    bar->setZOrder(99999);
    bar->setID("section-progress-bar"_spr);
    playLayer->addChild(bar);
    bar->update(0.f);
}

static void syncActiveFlagProgressBar(
    std::vector<FlagData> const& flags
) {
    auto playLayer = PlayLayer::get();
    if (!playLayer) return;

    auto node = playLayer->getChildByID("section-progress-bar"_spr);
    if (!node) {
        syncActiveSectionProgressBar(loadSections());
        node = playLayer->getChildByID("section-progress-bar"_spr);
    }
    if (!node) return;

    auto progressBar = typeinfo_cast<SectionProgressBar*>(node);
    if (!progressBar) {
        log::warn("Failed to cast section-progress-bar during flag sync");
        return;
    }
    progressBar->setFlags(flags);
}

class FreeProgressSettingsPopup : public geode::Popup {
protected:
    WeakRef<SectionListPopup> m_parent;

    bool init(SectionListPopup* parent) {
        if (!Popup::init(300.f, 150.f)) return false;
        m_parent = parent;
        this->setID("free-progress-settings-popup"_spr);
        this->setTitle("Progress Settings");

        auto label = CCLabelBMFont::create("Show Flags", "bigFont.fnt");
        label->setScale(0.5f);
        label->setAnchorPoint({0.f, 0.5f});
        label->setPosition({35.f, 75.f});
        m_mainLayer->addChild(label);

        auto toggle = CCMenuItemToggler::createWithStandardSprites(
            this,
            menu_selector(FreeProgressSettingsPopup::onToggleFlags),
            0.75f
        );
        toggle->toggle(isFlagHUDEnabled());
        toggle->setPosition({250.f, 75.f});
        toggle->setID("show-flags-toggle"_spr);
        m_buttonMenu->addChild(toggle);
        return true;
    }

    void onToggleFlags(CCObject* sender) {
        auto toggle = static_cast<CCMenuItemToggler*>(sender);
        Mod::get()->setSavedValue(SETTING_SHOW_FLAGS, !toggle->isToggled());

        if (auto playLayer = PlayLayer::get()) {
            auto node = playLayer->getChildByID("section-progress-bar"_spr);
            if (!node) return;
            if (auto progressBar = typeinfo_cast<SectionProgressBar*>(node)) {
                progressBar->reloadCustomizationSettings();
            }
        }
    }

public:
    void onClose(CCObject* sender) override {
        if (auto parent = m_parent.lock()) {
            parent->setSectionControlsEnabled(true);
        }
        Popup::onClose(sender);
    }

    static FreeProgressSettingsPopup* create(SectionListPopup* parent) {
        auto ret = new FreeProgressSettingsPopup();
        if (ret && ret->init(parent)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

void SectionListPopup::onOpenCustomizationSettings(CCObject*) {
    if (auto popup = FreeProgressSettingsPopup::create(this)) {
        setSectionControlsEnabled(false);
        popup->show();
    }
}
class $modify(ProcessDifficultyPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto spr = ButtonSprite::create("Part Setting");
        spr->setScale(0.6f);
        spr->setAnchorPoint({0.0f, 0.5f});

        auto btn = CCMenuItemSpriteExtra::create(
            spr,
            this,
            menu_selector(ProcessDifficultyPauseLayer::onOpenSections)
        );
        btn->setAnchorPoint({0.0f, 0.5f});
        btn->setPosition({0.f, 0.f});
        btn->setID("part-setting-pause-button"_spr);

        auto myDataSprite = ButtonSprite::create("List");
        myDataSprite->setScale(0.6f);
        myDataSprite->setAnchorPoint({0.f, 0.5f});
        auto myDataButton = CCMenuItemSpriteExtra::create(
            myDataSprite,
            this,
            menu_selector(ProcessDifficultyPauseLayer::onOpenMyData)
        );
        myDataButton->setAnchorPoint({0.f, 0.5f});
        myDataButton->setPosition({0.f, 24.f});
        myDataButton->setID("my-data-list-pause-button"_spr);

        auto menu = CCMenu::create();
        menu->setPosition({30.f, 30.f});
        menu->addChild(btn);
        menu->addChild(myDataButton);
        menu->setID("section-data-pause-menu"_spr);

        this->addChild(menu);
    }

    void onOpenSections(CCObject*) {
        SectionListPopup::create()->show();
    }

    void onOpenMyData(CCObject*) {
        if (auto list = MyDataListPopup::create()) {
            list->show();
        }
    }

};

class $modify(ProcessDifficultyAudioEngine, FMODAudioEngine) {
    int playEffect(gd::string path) {
        if (
            s_activeDeathSoundOverride.enabled &&
            isDefaultDeathSound(path)
        ) {
            if (s_activeDeathSoundOverride.volume <= 0.f) return -1;
            return FMODAudioEngine::playEffect(
                gd::string(s_activeDeathSoundOverride.path),
                1.f,
                0.f,
                s_activeDeathSoundOverride.volume
            );
        }
        return FMODAudioEngine::playEffect(path);
    }

    int playEffect(
        gd::string path,
        float speed,
        float unknown,
        float volume
    ) {
        if (
            s_activeDeathSoundOverride.enabled &&
            isDefaultDeathSound(path)
        ) {
            if (s_activeDeathSoundOverride.volume <= 0.f) return -1;
            return FMODAudioEngine::playEffect(
                gd::string(s_activeDeathSoundOverride.path),
                speed,
                unknown,
                std::clamp(
                    volume * s_activeDeathSoundOverride.volume,
                    0.f,
                    1.f
                )
            );
        }
        return FMODAudioEngine::playEffect(path, speed, unknown, volume);
    }

    int playEffectAdvanced(
        gd::string path,
        float speed,
        float unknown,
        float volume,
        float pitch,
        bool fastFourierTransform,
        bool reverb,
        int startMillis,
        int endMillis,
        int fadeIn,
        int fadeOut,
        bool loopEnabled,
        int effectID,
        bool override,
        bool noPreload,
        int channelID,
        int uniqueID,
        float minInterval,
        int sfxGroup
    ) {
        if (
            s_activeDeathSoundOverride.enabled &&
            isDefaultDeathSound(path)
        ) {
            if (s_activeDeathSoundOverride.volume <= 0.f) return -1;
            path = gd::string(s_activeDeathSoundOverride.path);
            volume = std::clamp(
                volume * s_activeDeathSoundOverride.volume,
                0.f,
                1.f
            );
        }
        return FMODAudioEngine::playEffectAdvanced(
            path,
            speed,
            unknown,
            volume,
            pitch,
            fastFourierTransform,
            reverb,
            startMillis,
            endMillis,
            fadeIn,
            fadeOut,
            loopEnabled,
            effectID,
            override,
            noPreload,
            channelID,
            uniqueID,
            minInterval,
            sfxGroup
        );
    }
};

class $modify(ProcessDifficultyPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        auto sections = loadSections();

        auto bar = SectionProgressBar::create(sections);

        if (bar) {
            bar->setZOrder(99999);
            bar->setID("section-progress-bar"_spr);

            this->addChild(bar);
        }

        return true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto const previousOverride = s_activeDeathSoundOverride;
        s_activeDeathSoundOverride = deathSoundForCurrentSection(this);
        PlayLayer::destroyPlayer(player, object);
        s_activeDeathSoundOverride = previousOverride;
    }
};
