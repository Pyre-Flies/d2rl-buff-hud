#include "loose_buff_hud.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace BuffPanel::Systems::BuffTracker::LooseBuffHud {
namespace {

inline constexpr std::string_view Header =
    "name\tstate_id\tdisplay_type\tvalue_stat\tmax_stat\tskill_id\tvalue_shift\tenabled";

inline constexpr std::size_t MaximumExcelBytes = 16u * 1024u * 1024u;
inline constexpr std::uint32_t SharedStashProxyStateId = 186u;

[[nodiscard]] bool ReadFileBounded(
    const std::filesystem::path& file,
    std::size_t maxBytes,
    const char* label,
    std::string& bytes,
    std::string& error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) {
        error = std::string("cannot get ") + label + " size";
        return false;
    }
    if (size > maxBytes) {
        error = std::string(label) + " exceeds size limit";
        return false;
    }
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        error = std::string("cannot open ") + label;
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    if (!bytes.empty() && !stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        error = std::string("cannot read complete ") + label;
        return false;
    }
    if (stream.peek() != std::char_traits<char>::eof()) {
        error = std::string(label) + " changed while being read";
        return false;
    }
    return true;
}

[[nodiscard]] bool ReadFile(
    const std::filesystem::path& file,
    std::string& bytes,
    std::string& error) {
    return ReadFileBounded(file, MaximumBytes, "external buff-hud.txt", bytes, error);
}

[[nodiscard]] bool ReadUnsigned(std::string_view value, std::uint32_t& output) noexcept {
    if (value.empty()) {
        output = 0;
        return true;
    }
    const char* const begin = value.data();
    const char* const end = begin + value.size();
    std::uint32_t parsed{};
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) return false;
    output = parsed;
    return true;
}

[[nodiscard]] bool ValidModName(const char* value) noexcept {
    if (value == nullptr || value[0] == '\0') return false;
    std::size_t count{};
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
        if (++count > 128 || *p < 0x20 || *p > 0x7e) return false;
        switch (*p) {
        case '/': case '\\': case ':': case '<': case '>': case '|':
        case '?': case '*': case '"':
            return false;
        default: break;
        }
    }
    if ((count == 1 && value[0] == '.')
        || (count == 2 && value[0] == '.' && value[1] == '.')) return false;
    return value[count - 1] != '.' && value[count - 1] != ' ';
}

[[nodiscard]] std::filesystem::path ModFolder(const char* activeMod) {
    std::wstring name;
    for (const char* it = activeMod; *it; ++it) name.push_back(static_cast<wchar_t>(*it));
    return std::filesystem::path(name + L".mpq");
}

[[nodiscard]] char LowerAscii(char value) noexcept {
    return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
}

[[nodiscard]] std::string LowerString(std::string_view value) {
    std::string out;
    out.resize(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        out[i] = LowerAscii(value[i]);
    }
    return out;
}

[[nodiscard]] bool EqualsNoCase(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (LowerAscii(lhs[i]) != LowerAscii(rhs[i])) return false;
    }
    return true;
}

[[nodiscard]] std::string_view TrimCell(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '"')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '"')) {
        value.remove_suffix(1);
    }
    return value;
}

class ExcelTable final {
public:
    [[nodiscard]] bool Load(const std::filesystem::path& path, const char* label, std::string& error) {
        rows_.clear();
        header_.clear();
        text_.clear();

        if (!ReadFileBounded(path, MaximumExcelBytes, label, text_, error)) {
            return false;
        }
        if (text_.empty()) {
            error = std::string(label) + " is empty";
            return false;
        }

        std::vector<std::string_view> line;
        std::size_t start = 0;
        bool first = true;
        for (std::size_t index = 0; index <= text_.size(); ++index) {
            if (index != text_.size() && text_[index] != '\n') {
                continue;
            }
            std::size_t end = index;
            if (end > start && text_[end - 1] == '\r') {
                --end;
            }
            const std::string_view raw(text_.data() + start, end - start);
            start = index + 1;
            if (raw.empty()) {
                continue;
            }

            line.clear();
            std::size_t cell = 0;
            for (std::size_t scan = 0; scan <= raw.size(); ++scan) {
                if (scan == raw.size() || raw[scan] == '\t') {
                    line.emplace_back(raw.data() + cell, scan - cell);
                    cell = scan + 1;
                }
            }
            if (first) {
                header_ = line;
                first = false;
            } else {
                rows_.push_back(line);
            }
        }
        if (header_.empty()) {
            error = std::string(label) + " has no header row";
            return false;
        }
        return true;
    }

    [[nodiscard]] std::int32_t Column(std::string_view name) const noexcept {
        for (std::size_t index = 0; index < header_.size(); ++index) {
            if (EqualsNoCase(TrimCell(header_[index]), name)) {
                return static_cast<std::int32_t>(index);
            }
        }
        return -1;
    }

    [[nodiscard]] std::size_t RowCount() const noexcept { return rows_.size(); }

    [[nodiscard]] std::string_view Cell(std::size_t row, std::int32_t column) const noexcept {
        if (column < 0 || row >= rows_.size()) return {};
        const auto& cells = rows_[row];
        const auto index = static_cast<std::size_t>(column);
        return index < cells.size() ? TrimCell(cells[index]) : std::string_view{};
    }

    [[nodiscard]] std::int32_t Number(std::size_t row, std::int32_t column) const noexcept {
        const std::string_view cell = Cell(row, column);
        if (cell.empty()) return 0;
        std::int32_t value = 0;
        const auto parsed = std::from_chars(cell.data(), cell.data() + cell.size(), value);
        return (parsed.ec == std::errc{} && parsed.ptr == cell.data() + cell.size()) ? value : 0;
    }

private:
    std::string text_;
    std::vector<std::string_view> header_;
    std::vector<std::vector<std::string_view>> rows_;
};

[[nodiscard]] bool IsExcludedTransientState(std::string_view stateName) noexcept {
    constexpr std::array<std::string_view, 4> Excluded{
        "berserk",
        "whirlwind",
        "sigil",
        "eldritchblastperiodic",
    };
    for (const auto item : Excluded) {
        if (stateName == item) return true;
    }
    return false;
}

[[nodiscard]] bool IsAllowedUncategorizedBuffState(std::string_view stateName) noexcept {
    constexpr std::array<std::string_view, 4> Allowed{
        "delerium",
        "markbear",
        "markwolf",
        "kings_charisma",
    };
    for (const auto item : Allowed) {
        if (stateName == item) return true;
    }
    return false;
}

[[nodiscard]] bool IsStandaloneTimerStateName(std::string_view stateName) noexcept {
    constexpr std::array<std::string_view, 17> Standalone{
        "wolf",
        "bear",
        "cloak_of_shadows",
        "delerium",
        "antidote",
        "thawing",
        "staminapot",
        "shrine_armor",
        "shrine_combat",
        "shrine_resist_lightning",
        "shrine_resist_fire",
        "shrine_resist_cold",
        "shrine_resist_poison",
        "shrine_skill",
        "shrine_mana_regen",
        "shrine_stamina",
        "shrine_experience",
    };
    for (const auto item : Standalone) {
        if (stateName == item) return true;
    }
    return false;
}

[[nodiscard]] bool HasExcelFiles(const std::filesystem::path& excelDir) noexcept {
    std::error_code ec;
    return std::filesystem::is_regular_file(excelDir / "states.txt", ec) && !ec
        && std::filesystem::is_regular_file(excelDir / "skills.txt", ec) && !ec
        && std::filesystem::is_regular_file(excelDir / "itemstatcost.txt", ec) && !ec;
}

[[nodiscard]] std::filesystem::path FindExcelDirInModRoot(
    const std::filesystem::path& root,
    const char* activeMod) {
    if (root.empty()) return {};
    const auto direct = root / "data" / "global" / "excel";
    if (HasExcelFiles(direct)) return direct;

    if (ValidModName(activeMod)) {
        const auto mpqFolder = ModFolder(activeMod);
        const auto candidate1 = root / mpqFolder / "data" / "global" / "excel";
        if (HasExcelFiles(candidate1)) return candidate1;
        const auto candidate2 = root / std::filesystem::path(activeMod) / mpqFolder / "data" / "global" / "excel";
        if (HasExcelFiles(candidate2)) return candidate2;
    }

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec) || ec) continue;
        const auto ext = entry.path().extension().wstring();
        if (ext == L".mpq" || ext == L".MPQ") {
            const auto candidate = entry.path() / "data" / "global" / "excel";
            if (HasExcelFiles(candidate)) return candidate;
        }
    }
    return {};
}

[[nodiscard]] std::filesystem::path FindExcelDirRelativeToModule(const char* activeMod) noexcept {
    try {
        HMODULE self = nullptr;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&FindExcelDirRelativeToModule),
                &self) == 0) {
            return {};
        }
        wchar_t modulePath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(self, modulePath, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) return {};

        std::filesystem::path dir( std::wstring_view(modulePath, length) );
        // .../mods/<Mod>/d2rloader/plugins/<dll> -> .../mods/<Mod>
        for (int step = 0; step < 3; ++step) {
            if (!dir.has_parent_path()) return {};
            dir = dir.parent_path();
        }
        return FindExcelDirInModRoot(dir, activeMod);
    } catch (...) {
        return {};
    }
}

} // namespace

bool ValidateAndNormalize(std::string& bytes, std::string& error) noexcept {
    std::vector<CatalogEntry> ignored;
    try {
        if (bytes.empty() || bytes.size() > MaximumBytes) {
            error = "table empty or exceeds 1 MiB";
            return false;
        }
        std::string normalized;
        normalized.reserve(bytes.size());
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            const unsigned char character = static_cast<unsigned char>(bytes[index]);
            if (character == '\r') {
                if (index + 1 >= bytes.size() || bytes[index + 1] != '\n') {
                    error = "bare CR detected; use LF or CRLF line endings";
                    return false;
                }
                continue;
            }
            if (character != '\n' && character != '\t'
                && (character < 0x20 || character > 0x7e)) {
                error = "non-printable/non-ASCII byte detected; save as plain ASCII without BOM";
                return false;
            }
            normalized.push_back(static_cast<char>(character));
        }
        if (!ParseCatalogFromTsv(normalized, ignored, error)) {
            return false;
        }
        bytes.swap(normalized);
        return true;
    } catch (...) {
        error = "exception while validating buff-hud.txt";
        return false;
    }
}

bool ParseCatalogFromTsv(
    std::string_view tsv,
    std::vector<CatalogEntry>& entries,
    std::string& error) noexcept {
    try {
        entries.clear();
        if (tsv.empty() || tsv.size() > MaximumBytes) {
            error = "table empty or exceeds 1 MiB";
            return false;
        }
        if (tsv.back() != '\n') {
            error = "table requires final line feed";
            return false;
        }
        const auto firstLineEnd = tsv.find('\n');
        if (firstLineEnd == std::string_view::npos
            || tsv.substr(0, firstLineEnd) != Header) {
            error = "header must exactly match the eight buff-hud.txt columns";
            return false;
        }

        std::unordered_set<std::uint32_t> enabledStates;
        std::size_t lineStart = firstLineEnd + 1;
        std::size_t rowNumber = 1;
        while (lineStart < tsv.size()) {
            const auto lineEnd = tsv.find('\n', lineStart);
            if (lineEnd == std::string_view::npos) {
                error = "missing final line feed";
                return false;
            }
            ++rowNumber;
            const std::string_view line = tsv.substr(lineStart, lineEnd - lineStart);
            lineStart = lineEnd + 1;
            if (line.empty()) continue;

            std::array<std::string_view, 8> cells{};
            std::size_t cellStart{};
            for (std::size_t col = 0; col < 7; ++col) {
                const auto separator = line.find('\t', cellStart);
                if (separator == std::string_view::npos) {
                    error = "row " + std::to_string(rowNumber) + ": fewer than eight columns";
                    return false;
                }
                cells[col] = line.substr(cellStart, separator - cellStart);
                cellStart = separator + 1;
            }
            cells[7] = line.substr(cellStart);
            if (cells[7].find('\t') != std::string_view::npos) {
                error = "row " + std::to_string(rowNumber) + ": more than eight columns";
                return false;
            }
            if (cells[0].empty() || cells[0].size() >= 48
                || cells[2].empty() || cells[2].size() >= 16) {
                error = "row " + std::to_string(rowNumber) + ": invalid name or display_type length";
                return false;
            }
            std::array<std::uint32_t, 6> numbers{};
            constexpr std::array<std::size_t, 6> NumericColumns{1, 3, 4, 5, 6, 7};
            for (std::size_t i = 0; i < numbers.size(); ++i) {
                if (!ReadUnsigned(cells[NumericColumns[i]], numbers[i])) {
                    error = "row " + std::to_string(rowNumber) + ": invalid unsigned decimal field";
                    return false;
                }
            }
            const auto stateId = numbers[0];
            const auto valueStat = numbers[1];
            const auto maxStat = numbers[2];
            const auto skillId = numbers[3];
            const auto shift = numbers[4];
            const auto enabled = numbers[5];
            if (stateId == 0 || stateId > 4095 || shift > 255 || enabled > 1) {
                error = "row " + std::to_string(rowNumber) + ": state/shift/enabled out of range";
                return false;
            }
            if (cells[2] != "timer" && cells[2] != "resource") {
                error = "row " + std::to_string(rowNumber) + ": display_type must be timer or resource";
                return false;
            }
            if (enabled != 0) {
                if (cells[2] == "timer"
                    && (valueStat || maxStat || skillId > 4095 || shift)) {
                    error = "row " + std::to_string(rowNumber)
                        + ": timer value_stat/max_stat/value_shift must be zero and skill_id <= 4095";
                    return false;
                }
                if (cells[2] == "resource"
                    && (!valueStat || !maxStat || !skillId
                        || valueStat > 4095 || maxStat > 4095 || skillId > 4095
                        || shift > 30)) {
                    error = "row " + std::to_string(rowNumber) + ": invalid resource stat/skill/shift";
                    return false;
                }
                if (!enabledStates.insert(stateId).second) {
                    error = "row " + std::to_string(rowNumber) + ": duplicate enabled state_id";
                    return false;
                }
                entries.push_back(CatalogEntry{
                    .stateId = stateId,
                    .isResource = (cells[2] == "resource"),
                    .valueStatId = static_cast<std::int32_t>(valueStat),
                    .maxStatId = static_cast<std::int32_t>(maxStat),
                    .sourceSkillId = static_cast<std::int32_t>(skillId),
                    .valueShift = static_cast<std::uint8_t>(shift),
                });
            }
        }

        std::sort(
            entries.begin(),
            entries.end(),
            [](const CatalogEntry& lhs, const CatalogEntry& rhs) noexcept {
                return lhs.stateId < rhs.stateId;
            });
        return true;
    } catch (...) {
        error = "exception while parsing buff-hud.txt";
        return false;
    }
}

bool LoadCatalogFromExcelDirectory(
    const std::filesystem::path& excelDir,
    std::vector<CatalogEntry>& entries,
    std::string& error) noexcept {
    try {
        entries.clear();

        ExcelTable statsTable;
        if (!statsTable.Load(excelDir / "itemstatcost.txt", "itemstatcost.txt", error)) {
            return false;
        }
        const auto statNameCol = statsTable.Column("Stat");
        const auto statIdCol = statsTable.Column("*ID");
        if (statNameCol < 0 || statIdCol < 0) {
            error = "itemstatcost.txt is missing Stat or *ID column";
            return false;
        }
        std::unordered_map<std::string, std::int32_t> statIdByName;
        for (std::size_t row = 0; row < statsTable.RowCount(); ++row) {
            const auto name = statsTable.Cell(row, statNameCol);
            const auto id = statsTable.Number(row, statIdCol);
            if (!name.empty() && id > 0 && id <= 4095) {
                statIdByName.emplace(LowerString(name), id);
            }
        }

        ExcelTable statesTable;
        if (!statesTable.Load(excelDir / "states.txt", "states.txt", error)) {
            return false;
        }
        const auto stateNameCol = statesTable.Column("state");
        const auto stateIdCol = statesTable.Column("*ID");
        if (stateNameCol < 0 || stateIdCol < 0) {
            error = "states.txt is missing state or *ID column";
            return false;
        }
        const auto curseCol = statesTable.Column("curse");
        const auto attRedCol = statesTable.Column("attred");
        const auto damRedCol = statesTable.Column("damred");
        const auto armRedCol = statesTable.Column("armred");
        const auto rfRedCol = statesTable.Column("rfred");
        const auto rlRedCol = statesTable.Column("rlred");
        const auto rcRedCol = statesTable.Column("rcred");
        const auto rpRedCol = statesTable.Column("rpred");

        struct StateInfo final {
            std::uint32_t id{};
            bool redDebuff{};
        };
        std::unordered_map<std::string, StateInfo> statesByName;
        for (std::size_t row = 0; row < statesTable.RowCount(); ++row) {
            const auto rawName = statesTable.Cell(row, stateNameCol);
            const auto id = statesTable.Number(row, stateIdCol);
            if (rawName.empty() || id <= 0 || id > 4095
                || static_cast<std::uint32_t>(id) == SharedStashProxyStateId) {
                continue;
            }
            if (statesTable.Number(row, curseCol) != 0) {
                continue;
            }
            const bool redDebuff = statesTable.Number(row, attRedCol) != 0
                || statesTable.Number(row, damRedCol) != 0
                || statesTable.Number(row, armRedCol) != 0
                || statesTable.Number(row, rfRedCol) != 0
                || statesTable.Number(row, rlRedCol) != 0
                || statesTable.Number(row, rcRedCol) != 0
                || statesTable.Number(row, rpRedCol) != 0;
            statesByName.emplace(
                LowerString(rawName),
                StateInfo{.id = static_cast<std::uint32_t>(id), .redDebuff = redDebuff});
        }

        ExcelTable skillsTable;
        if (!skillsTable.Load(excelDir / "skills.txt", "skills.txt", error)) {
            return false;
        }
        auto skillIdCol = skillsTable.Column("*Id");
        if (skillIdCol < 0) skillIdCol = skillsTable.Column("Id");
        const auto auraStateCol = skillsTable.Column("aurastate");
        const auto auraTargetStateCol = skillsTable.Column("auratargetstate");
        const auto auraLenCalcCol = skillsTable.Column("auralencalc");
        const auto auraStat1Col = skillsTable.Column("aurastat1");
        const auto auraStat2Col = skillsTable.Column("aurastat2");
        const auto charClassCol = skillsTable.Column("charclass");
        if (skillIdCol < 0 || auraStateCol < 0 || auraLenCalcCol < 0
            || auraStat1Col < 0 || auraStat2Col < 0) {
            error = "skills.txt is missing required *Id/aurastate/auralencalc/aurastat columns";
            return false;
        }

        struct ResourceCandidate final {
            std::int32_t valueStatId{};
            std::int32_t maxStatId{};
            std::int32_t skillId{};
            bool hasClass{};
        };
        std::unordered_map<std::uint32_t, ResourceCandidate> resourceByState;
        std::unordered_map<std::uint32_t, std::int32_t> timerSkillByState;
        std::unordered_set<std::uint32_t> timerStates;

        for (std::size_t row = 0; row < skillsTable.RowCount(); ++row) {
            const auto skillId = skillsTable.Number(row, skillIdCol);
            if (skillId <= 0 || skillId > 4095) continue;

            const auto rawAuraState = skillsTable.Cell(row, auraStateCol);
            if (rawAuraState.empty()) continue;
            const auto auraState = LowerString(rawAuraState);
            const auto stateIt = statesByName.find(auraState);
            if (stateIt == statesByName.end()) continue;
            const auto stateInfo = stateIt->second;

            const auto charClass = skillsTable.Cell(row, charClassCol);
            const bool hasClass = !charClass.empty();
            const auto stat1 = LowerString(skillsTable.Cell(row, auraStat1Col));
            const auto stat2 = LowerString(skillsTable.Cell(row, auraStat2Col));
            if (!stat1.empty() && stat2 == stat1 + "max") {
                const auto vIt = statIdByName.find(stat1);
                const auto mIt = statIdByName.find(stat2);
                if (vIt != statIdByName.end() && mIt != statIdByName.end()) {
                    const auto existing = resourceByState.find(stateInfo.id);
                    if (existing == resourceByState.end()
                        || (hasClass && !existing->second.hasClass)) {
                        resourceByState[stateInfo.id] = ResourceCandidate{
                            .valueStatId = vIt->second,
                            .maxStatId = mIt->second,
                            .skillId = skillId,
                            .hasClass = hasClass,
                        };
                    }
                    continue;
                }
            }

            const auto lenCalc = skillsTable.Cell(row, auraLenCalcCol);
            if (!lenCalc.empty()
                && !stateInfo.redDebuff
                && !IsExcludedTransientState(auraState)
                && (hasClass || IsAllowedUncategorizedBuffState(auraState))) {
                timerStates.insert(stateInfo.id);
                const auto targetState = skillsTable.Cell(row, auraTargetStateCol);
                if (hasClass && EqualsNoCase(auraState, targetState)) {
                    timerSkillByState[stateInfo.id] = skillId;
                }
            }
        }

        for (const auto& [stateName, stateInfo] : statesByName) {
            if (!stateInfo.redDebuff && IsStandaloneTimerStateName(stateName)) {
                timerStates.insert(stateInfo.id);
            }
        }

        entries.reserve(resourceByState.size() + timerStates.size());
        for (const auto& [stateId, res] : resourceByState) {
            entries.push_back(CatalogEntry{
                .stateId = stateId,
                .isResource = true,
                .valueStatId = res.valueStatId,
                .maxStatId = res.maxStatId,
                .sourceSkillId = res.skillId,
                .valueShift = 8u,
            });
        }
        for (const auto stateId : timerStates) {
            if (resourceByState.find(stateId) != resourceByState.end()) continue;
            const auto skIt = timerSkillByState.find(stateId);
            entries.push_back(CatalogEntry{
                .stateId = stateId,
                .isResource = false,
                .valueStatId = 0,
                .maxStatId = 0,
                .sourceSkillId = skIt != timerSkillByState.end() ? skIt->second : 0,
                .valueShift = 0u,
            });
        }

        if (entries.empty()) {
            error = "no buff states derived from states.txt/skills.txt/itemstatcost.txt";
            return false;
        }
        std::sort(
            entries.begin(),
            entries.end(),
            [](const CatalogEntry& lhs, const CatalogEntry& rhs) noexcept {
                return lhs.stateId < rhs.stateId;
            });
        return true;
    } catch (...) {
        error = "exception while reading Excel tables";
        return false;
    }
}

Selection Select(
    const wchar_t* modDirectory,
    const char* activeMod,
    std::string_view embedded) noexcept {
    Selection selected;
    try {
        if (modDirectory != nullptr && modDirectory[0] != L'\0'
            && ValidModName(activeMod)) {
            const std::filesystem::path modRoot(modDirectory);
            const std::filesystem::path modMpq = ModFolder(activeMod);
            const std::array candidates{
                modRoot / modMpq / std::filesystem::path(VirtualPath),
                modRoot / std::filesystem::path(VirtualPath),
                modRoot / std::filesystem::path(activeMod) / modMpq
                    / std::filesystem::path(VirtualPath),
            };
            for (const auto& path : candidates) {
                std::error_code ec;
                const bool exists = std::filesystem::exists(path, ec);
                if (ec) {
                    selected.source = Source::InvalidOverride;
                    selected.path = path;
                    selected.error = "failed to inspect external buff-hud.txt path";
                    return selected;
                }
                if (!exists) continue;
                selected.path = path;
                selected.source = Source::InvalidOverride;
                if (!std::filesystem::is_regular_file(path, ec) || ec) {
                    selected.error = "external buff-hud.txt exists but is not a regular file";
                    return selected;
                }
                if (!ReadFile(path, selected.bytes, selected.error)) return selected;
                if (!ValidateAndNormalize(selected.bytes, selected.error)) return selected;
                selected.source = Source::ActiveMod;
                return selected;
            }
        }
        selected.source = Source::Embedded;
        selected.bytes.assign(embedded.data(), embedded.size());
        if (!ValidateAndNormalize(selected.bytes, selected.error)) {
            selected.source = Source::InvalidOverride;
        }
        return selected;
    } catch (...) {
        selected.source = Source::InvalidOverride;
        selected.error = "exception while selecting buff-hud.txt";
        return selected;
    }
}

CatalogResolution ResolveCatalog(
    const wchar_t* modSupportDirectory,
    const wchar_t* modDirectory,
    const char* activeMod) noexcept {
    CatalogResolution result;
    try {
        // 1. Honor an explicit loose buff-hud.txt override if one was placed in the mod.
        const auto loose = Select(modDirectory, activeMod, DefaultCatalogTsv);
        if (loose.source == Source::InvalidOverride) {
            result.source = CatalogSource::InvalidOverride;
            result.path = loose.path;
            result.error = loose.error;
            return result;
        }
        if (loose.source == Source::ActiveMod) {
            if (!ParseCatalogFromTsv(loose.bytes, result.entries, result.error)) {
                result.source = CatalogSource::InvalidOverride;
                result.path = loose.path;
                return result;
            }
            result.source = CatalogSource::LooseOverride;
            result.path = loose.path;
            return result;
        }

        // 2. Read states.txt, skills.txt, and itemstatcost.txt directly from the active mod's files.
        std::filesystem::path excelDir;
        if (modSupportDirectory != nullptr && modSupportDirectory[0] != L'\0') {
            excelDir = FindExcelDirInModRoot(std::filesystem::path(modSupportDirectory), activeMod);
        }
        if (excelDir.empty() && modDirectory != nullptr && modDirectory[0] != L'\0') {
            excelDir = FindExcelDirInModRoot(std::filesystem::path(modDirectory), activeMod);
        }
        if (excelDir.empty()) {
            excelDir = FindExcelDirRelativeToModule(activeMod);
        }

        if (!excelDir.empty()) {
            std::string excelError;
            if (LoadCatalogFromExcelDirectory(excelDir, result.entries, excelError)) {
                result.source = CatalogSource::ExcelFiles;
                result.path = excelDir;
                return result;
            }
        }

        // 3. Built-in fallback when running against packed archives without loose Excel files.
        if (!ParseCatalogFromTsv(DefaultCatalogTsv, result.entries, result.error)) {
            result.source = CatalogSource::InvalidOverride;
            return result;
        }
        result.source = CatalogSource::BuiltinFallback;
        return result;
    } catch (...) {
        result.source = CatalogSource::InvalidOverride;
        result.error = "exception while resolving buff catalog";
        return result;
    }
}

} // namespace BuffPanel::Systems::BuffTracker::LooseBuffHud
