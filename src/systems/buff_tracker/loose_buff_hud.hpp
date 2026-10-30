#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace BuffPanel::Systems::BuffTracker::LooseBuffHud {

inline constexpr std::string_view VirtualPath =
    "data/global/excel/d2rloader/buffhud/buff-hud.txt";
inline constexpr std::string_view ExcelDirectoryRelativePath =
    "data/global/excel";
inline constexpr std::size_t MaximumBytes = 1024u * 1024u;

inline constexpr std::string_view DefaultCatalogTsv =
    "name\tstate_id\tdisplay_type\tvalue_stat\tmax_stat\tskill_id\tvalue_shift\tenabled\n"
    "frozen_armor\t10\ttimer\t0\t0\t0\t0\t1\n"
    "blaze\t13\ttimer\t0\t0\t0\t0\t1\n"
    "bone_armor\t14\tresource\t132\t133\t68\t8\t1\n"
    "enchant\t16\ttimer\t0\t0\t0\t0\t1\n"
    "chilling_armor\t20\ttimer\t0\t0\t0\t0\t1\n"
    "shout\t26\ttimer\t0\t0\t138\t0\t1\n"
    "energy_shield\t30\ttimer\t0\t0\t0\t0\t1\n"
    "venom\t31\ttimer\t0\t0\t0\t0\t1\n"
    "battle_orders\t32\ttimer\t0\t0\t149\t0\t1\n"
    "thunder_storm\t38\ttimer\t0\t0\t0\t0\t1\n"
    "battle_command\t51\ttimer\t0\t0\t155\t0\t1\n"
    "shiver_armor\t88\ttimer\t0\t0\t0\t0\t1\n"
    "frenzy\t94\ttimer\t0\t0\t0\t0\t1\n"
    "holy_shield\t101\ttimer\t0\t0\t0\t0\t1\n"
    "maul\t117\ttimer\t0\t0\t0\t0\t1\n"
    "feral_rage\t120\ttimer\t0\t0\t0\t0\t1\n"
    "tiger_strike_charges\t122\ttimer\t0\t0\t0\t0\t1\n"
    "cobra_strike_charges\t123\ttimer\t0\t0\t0\t0\t1\n"
    "phoenix_strike_charges\t124\ttimer\t0\t0\t0\t0\t1\n"
    "fists_of_fire_charges\t125\ttimer\t0\t0\t0\t0\t1\n"
    "blades_of_ice_charges\t126\ttimer\t0\t0\t0\t0\t1\n"
    "claws_of_thunder_charges\t127\ttimer\t0\t0\t0\t0\t1\n"
    "armor_shrine\t128\ttimer\t0\t0\t0\t0\t1\n"
    "combat_shrine\t129\ttimer\t0\t0\t0\t0\t1\n"
    "lightning_resist_shrine\t130\ttimer\t0\t0\t0\t0\t1\n"
    "fire_resist_shrine\t131\ttimer\t0\t0\t0\t0\t1\n"
    "cold_resist_shrine\t132\ttimer\t0\t0\t0\t0\t1\n"
    "poison_resist_shrine\t133\ttimer\t0\t0\t0\t0\t1\n"
    "skill_shrine\t134\ttimer\t0\t0\t0\t0\t1\n"
    "mana_regen_shrine\t135\ttimer\t0\t0\t0\t0\t1\n"
    "stamina_shrine\t136\ttimer\t0\t0\t0\t0\t1\n"
    "experience_shrine\t137\ttimer\t0\t0\t0\t0\t1\n"
    "werewolf\t139\ttimer\t0\t0\t0\t0\t1\n"
    "werebear\t140\ttimer\t0\t0\t0\t0\t1\n"
    "hurricane\t144\ttimer\t0\t0\t0\t0\t1\n"
    "armageddon\t145\ttimer\t0\t0\t0\t0\t1\n"
    "cyclone_armor\t151\tresource\t132\t133\t235\t8\t1\n"
    "cloak_of_shadows_caster\t153\ttimer\t0\t0\t0\t0\t1\n"
    "burst_of_speed\t157\ttimer\t0\t0\t0\t0\t1\n"
    "blade_shield\t158\ttimer\t0\t0\t0\t0\t1\n"
    "fade\t159\ttimer\t0\t0\t0\t0\t1\n"
    "delirium\t177\ttimer\t0\t0\t0\t0\t1\n"
    "antidote_potion\t178\ttimer\t0\t0\t0\t0\t1\n"
    "thawing_potion\t179\ttimer\t0\t0\t0\t0\t1\n"
    "stamina_potion\t180\ttimer\t0\t0\t0\t0\t1\n"
    "mark_of_the_bear\t190\ttimer\t0\t0\t0\t0\t1\n"
    "mark_of_the_wolf\t191\ttimer\t0\t0\t0\t0\t1\n"
    "hex_bane_caster\t198\ttimer\t0\t0\t0\t0\t1\n"
    "hex_siphon_caster\t200\ttimer\t0\t0\t0\t0\t1\n"
    "hex_purge_caster\t202\ttimer\t0\t0\t0\t0\t1\n"
    "psychic_ward\t206\tresource\t361\t362\t387\t8\t1\n"
    "consume\t208\ttimer\t0\t0\t0\t0\t1\n"
    "engorge\t211\ttimer\t0\t0\t0\t0\t1\n";

enum class Source { Embedded, ActiveMod, InvalidOverride };

struct Selection final {
    Source source{Source::Embedded};
    std::filesystem::path path{};
    std::string bytes{};
    std::string error{};
};

[[nodiscard]] Selection Select(
    const wchar_t* modDirectory,
    const char* activeMod,
    std::string_view embedded) noexcept;

[[nodiscard]] bool ValidateAndNormalize(std::string& bytes, std::string& error) noexcept;

enum class CatalogSource {
    ExcelFiles,
    LooseOverride,
    BuiltinFallback,
    InvalidOverride,
};

struct CatalogEntry final {
    std::uint32_t stateId{};
    bool isResource{};
    std::int32_t valueStatId{};
    std::int32_t maxStatId{};
    std::int32_t sourceSkillId{};
    std::uint8_t valueShift{};
};

struct CatalogResolution final {
    CatalogSource source{CatalogSource::BuiltinFallback};
    std::filesystem::path path{};
    std::vector<CatalogEntry> entries{};
    std::string error{};
};

[[nodiscard]] bool ParseCatalogFromTsv(
    std::string_view tsv,
    std::vector<CatalogEntry>& entries,
    std::string& error) noexcept;

[[nodiscard]] bool LoadCatalogFromExcelDirectory(
    const std::filesystem::path& excelDir,
    std::vector<CatalogEntry>& entries,
    std::string& error) noexcept;

[[nodiscard]] CatalogResolution ResolveCatalog(
    const wchar_t* modSupportDirectory,
    const wchar_t* modDirectory,
    const char* activeMod) noexcept;

} // namespace BuffPanel::Systems::BuffTracker::LooseBuffHud
