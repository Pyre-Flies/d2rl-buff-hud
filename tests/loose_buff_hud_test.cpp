#include "systems/buff_tracker/loose_buff_hud.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace B = BuffPanel::Systems::BuffTracker::LooseBuffHud;
namespace fs = std::filesystem;

constexpr std::string_view Default =
    "name\tstate_id\tdisplay_type\tvalue_stat\tmax_stat\tskill_id\tvalue_shift\tenabled\n"
    "fade\t159\ttimer\t0\t0\t0\t0\t1\n";

void Write(const fs::path& target, std::string_view content) {
    fs::create_directories(target.parent_path());
    std::ofstream out(target, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    assert(out.good());
}

const B::CatalogEntry* FindByState(const std::vector<B::CatalogEntry>& entries, std::uint32_t stateId) {
    const auto it = std::find_if(entries.begin(), entries.end(), [stateId](const B::CatalogEntry& entry) {
        return entry.stateId == stateId;
    });
    return it != entries.end() ? &*it : nullptr;
}

int main() {
    const auto header = std::string(Default.substr(0, Default.find('\n') + 1));
    std::string error;
    auto timer = header + "custom_buff\t350\ttimer\t0\t0\t510\t0\t1\n";
    assert(B::ValidateAndNormalize(timer, error));
    timer = header + "battle_orders\t32\ttimer\t0\t0\t149\t0\t1\n";
    assert(B::ValidateAndNormalize(timer, error));
    for (const auto* invalid : {"1\t0\t149\t0", "0\t1\t149\t0", "0\t0\t4096\t0", "0\t0\t149\t8"}) {
        timer = header + "bad\t32\ttimer\t" + invalid + "\t1\n";
        assert(!B::ValidateAndNormalize(timer, error));
    }
    auto base = fs::temp_directory_path() / "buff-panel-loose-test";
    fs::remove_all(base);
    fs::create_directories(base / "test-mod");
    const auto mod = base / "test-mod";
    const std::wstring root = mod.wstring();
    const auto path = mod / "test-mod.mpq" / std::string(B::VirtualPath);

    auto result = B::Select(root.c_str(), "test-mod", Default);
    assert(result.source == B::Source::Embedded && result.bytes == Default);

    const std::string custom =
        "name\tstate_id\tdisplay_type\tvalue_stat\tmax_stat\tskill_id\tvalue_shift\tenabled\r\n"
        "venom\t31\ttimer\t0\t0\t0\t0\t1\r\n";
    Write(path, custom);
    result = B::Select(root.c_str(), "test-mod", Default);
    assert(result.source == B::Source::ActiveMod);
    assert(result.bytes.find("venom\t31") != std::string::npos);
    assert(result.bytes.find('\r') == std::string::npos);
    assert(result.path == path);
    assert(result.bytes.back() == '\n');

    const std::string timerSkillFallback =
        "name\tstate_id\tdisplay_type\tvalue_stat\tmax_stat\tskill_id\tvalue_shift\tenabled\n"
        "shout\t26\ttimer\t0\t0\t138\t0\t1\n"
        "battle_orders\t32\ttimer\t0\t0\t149\t0\t1\n"
        "battle_command\t51\ttimer\t0\t0\t155\t0\t1\n";
    Write(path, timerSkillFallback);
    result = B::Select(root.c_str(), "test-mod", Default);
    assert(result.source == B::Source::ActiveMod);
    assert(result.bytes.find("shout\t26\ttimer\t0\t0\t138") != std::string::npos);
    assert(result.bytes.find("battle_orders\t32\ttimer\t0\t0\t149") != std::string::npos);

    Write(path, "wrong\theader\n");
    result = B::Select(root.c_str(), "test-mod", Default);
    assert(result.source == B::Source::InvalidOverride);
    assert(!result.error.empty());

    Write(path, std::string(Default).substr(0, Default.size() - 1));
    result = B::Select(root.c_str(), "test-mod", Default);
    assert(result.source == B::Source::InvalidOverride);

    Write(path, std::string(Default) + "fade2\t159\ttimer\t0\t0\t0\t0\t1\n");
    result = B::Select(root.c_str(), "test-mod", Default);
    assert(result.source == B::Source::InvalidOverride);

    // Some SDK contexts point modDirectory directly at the .mpq data folder.
    fs::remove(path);
    const auto directPath = mod / std::string(B::VirtualPath);
    Write(directPath, custom);
    result = B::Select(root.c_str(), "test-mod", Default);
    assert(result.source == B::Source::ActiveMod && result.path == directPath);
    fs::remove(directPath);

    Write(path, std::string("\xef\xbb\xbf") + std::string(Default));
    result = B::Select(root.c_str(), "test-mod", Default);
    assert(result.source == B::Source::InvalidOverride);

    Write(path, Default);
    result = B::Select(root.c_str(), "../evil", Default);
    assert(result.source == B::Source::Embedded);
    fs::remove(path);

    // Verify runtime Excel derivation (states.txt + skills.txt + itemstatcost.txt).
    const auto excelDir = mod / "test-mod.mpq" / std::string(B::ExcelDirectoryRelativePath);
    Write(
        excelDir / "itemstatcost.txt",
        "Stat\t*ID\n"
        "armor_override_percent\t132\n"
        "armor_override_percentmax\t133\n"
        "item_corrupted\t361\n"
        "item_corruptedDummy\t362\n"
        "psychicward\t427\n"
        "psychicwardmax\t428\n");
    Write(
        excelDir / "states.txt",
        "state\t*ID\tcurse\tsetfunc\tremfunc\tcolorpri\n"
        "none\t0\t\t\t\t\n"
        "amplifydamage\t9\t1\t\t\t51\n"
        "bonearmor\t14\t\t\t\t\n"
        "shout\t26\t\t\t\t\n"
        "battleorders\t32\t\t5\t6\t\n"
        "shrine_armor\t128\t\t\t\t\n"
        "wolf\t139\t\t11\t2\t\n"
        "antidote\t176\t\t19\t12\t\n"
        "psychic_ward\t206\t\t25\t20\t\n"
        "slam_state\t238\t\t\t\t\n");
    Write(
        excelDir / "skills.txt",
        "skill\t*Id\tcharclass\taurastate\tauratargetstate\tauralencalc\taurastat1\taurastat2\n"
        "Bone Armor\t68\tnec\tbonearmor\t\t\tarmor_override_percent\tarmor_override_percentmax\n"
        "MonBoneArmor\t210\t\tbonearmor\t\t\tarmor_override_percent\tarmor_override_percentmax\n"
        "Shout\t138\tbar\tshout\tshout\tlvl*125\titem_armor_percent\t\n"
        "Battle Orders\t149\tbar\tbattleorders\tbattleorders\tlvl*150\tmaxhp\tmaxmana\n"
        "Amplify Damage\t66\tnec\t\tamplifydamage\tlvl*75\t\t\n"
        "Psychic Ward\t387\twar\tpsychic_ward\t\t\tpsychicward\tpsychicwardmax\n"
        "Slam\t405\tbar\tslam_state\t\t100\tdamagepercent\t\n");

    auto resolved = B::ResolveCatalog(root.c_str(), root.c_str(), "test-mod");
    assert(resolved.source == B::CatalogSource::ExcelFiles);
    assert(!resolved.entries.empty());
    assert(resolved.path == excelDir);
    assert(FindByState(resolved.entries, 9) == nullptr); // curse excluded

    const auto* boneArmor = FindByState(resolved.entries, 14);
    assert(boneArmor != nullptr);
    assert(boneArmor->isResource);
    assert(boneArmor->valueStatId == 132 && boneArmor->maxStatId == 133);
    assert(boneArmor->sourceSkillId == 68 && boneArmor->valueShift == 8); // prefers Necromancer skill 68 over MonBoneArmor 210

    const auto* psychicWard = FindByState(resolved.entries, 206);
    assert(psychicWard != nullptr);
    assert(psychicWard->isResource);
    assert(psychicWard->valueStatId == 427 && psychicWard->maxStatId == 428);
    assert(psychicWard->sourceSkillId == 387 && psychicWard->valueShift == 8);

    const auto* battleOrders = FindByState(resolved.entries, 32);
    assert(battleOrders != nullptr);
    assert(!battleOrders->isResource);
    assert(battleOrders->sourceSkillId == 149);

    const auto* slamState = FindByState(resolved.entries, 238);
    assert(slamState != nullptr);
    assert(!slamState->isResource);

    const auto* shrineArmor = FindByState(resolved.entries, 128);
    assert(shrineArmor != nullptr && !shrineArmor->isResource);

    const auto* wolf = FindByState(resolved.entries, 139);
    assert(wolf != nullptr && !wolf->isResource);

    const auto* antidote = FindByState(resolved.entries, 176);
    assert(antidote != nullptr && !antidote->isResource);

    // Explicit loose buff-hud.txt override takes priority over Excel derivation.
    Write(path, custom);
    resolved = B::ResolveCatalog(root.c_str(), root.c_str(), "test-mod");
    assert(resolved.source == B::CatalogSource::LooseOverride);
    assert(resolved.entries.size() == 1 && resolved.entries[0].stateId == 31);

    // Malformed loose override fails closed rather than silently falling back.
    Write(path, "invalid\theader\n");
    resolved = B::ResolveCatalog(root.c_str(), root.c_str(), "test-mod");
    assert(resolved.source == B::CatalogSource::InvalidOverride);
    assert(resolved.entries.empty());

    // Built-in fallback works when no loose override or Excel directory is present.
    fs::remove_all(mod / "test-mod.mpq");
    resolved = B::ResolveCatalog(root.c_str(), root.c_str(), "test-mod");
    assert(resolved.source == B::CatalogSource::BuiltinFallback);
    assert(resolved.entries.size() == 53);

    fs::remove_all(base);
    std::cout << "PASS: runtime Excel derivation, remapped stat ID resolution, loose override precedence, fail-closed override validation, and built-in fallback\n";
}
