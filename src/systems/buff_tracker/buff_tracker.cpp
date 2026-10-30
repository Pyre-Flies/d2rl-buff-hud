#include "buff_tracker.hpp"
#include "loose_buff_hud.hpp"
#include "timer_metadata.hpp"

#include "core/buff_display_bus.hpp"
#include "core/services.hpp"
#include "core/stat_read_bus.hpp"
#include "core/stat_list_post_bus.hpp"
#include "native/blood_aura_contract.hpp"
#include "native/native_contract.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>


namespace BuffPanel::Systems::BuffTracker {
namespace {

using GetGameFromUnitFn = void*(__fastcall*)(void* unit) noexcept;
using GetStatListFromUnitAndStateFn = void*(__fastcall*)(void* unit, std::int32_t state) noexcept;
using GetGameByHandleFn = void*(__fastcall*)(std::uint64_t handle) noexcept;

// StatList semantic field layout is runtime-qualified on D2R build 93847. The
// runtime catalog is derived directly from the active mod's Excel files
// (states.txt, skills.txt, itemstatcost.txt), with optional loose buff-hud.txt
// override support, and declares how each state is presented: finite timer or
// resource pool. Native CURSE lists remain excluded as a safety boundary. Timer
// rows require finite future expiry metadata. Native skill ID is preferred;
// timer skill_id is an optional fallback when the StatList exposes skill=0.
// skillLevel is not used for qualification. Resource rows read their
// current/max values through the already-owned Core unit-stat reader and do not
// require an expiry.
constexpr std::uint32_t UnitTypeOffset = 0x00;
constexpr std::uint32_t UnitIdOffset = 0x08;
constexpr std::uint32_t UnitStatListExOffset = 0x88;
constexpr std::uint32_t PlayerUnitType = 0;
constexpr std::uint32_t MaxPlayerBucketWalk = 64u;
constexpr std::uint32_t StatListPrevLinkOffset = 0x68;
constexpr std::uint32_t StatListActiveHeadOffset = 0x90;
constexpr std::uint32_t MaxStatListWalk = 256u;
constexpr std::uint32_t StatListCurseFlag = 0x00000020u;
constexpr std::uint32_t SharedStashProxyState = 186u; // states.txt: sharedstash; D2RLoader 1.3 proxy marker

const D2RL::PluginContext* Context{};
const D2RL::ThreadService* Threads{};
const D2RL::LifecycleService* Lifecycle{};
GetGameFromUnitFn GetGameFromUnit{};
GetStatListFromUnitAndStateFn GetStatListFromUnitAndState{};
GetGameByHandleFn GetGameByHandle{};
const std::uint64_t* GameHandles{};
D2RL::Lifecycle::ListenerHandle DataTablesListener{D2RL::Lifecycle::InvalidHandle};
std::array<D2RL::Lifecycle::ListenerHandle, 3> GameplayListeners{};

struct BuffDefinition final {
    std::uint32_t stateId{};
    Core::BuffDisplayMode displayMode{Core::BuffDisplayMode::Timer};
    std::int32_t valueStatId{};
    std::int32_t maxStatId{};
    std::int32_t sourceSkillId{};
    std::uint8_t valueShift{};
};

struct WhitelistCache final {
    std::uint64_t revision{};
    std::vector<BuffDefinition> definitions;
};

std::mutex WhitelistUpdateMutex;
std::array<WhitelistCache, 4> WhitelistSlots{};
std::uint64_t WhitelistGeneration{};
std::atomic<const WhitelistCache*> Whitelist{nullptr};

struct TimerPresenceRecord final {
    std::uint64_t key{};
    std::uint32_t stateId{};
    std::uint32_t skillId{};
    std::uint32_t expireFrame{};
    std::uint32_t lastPostFrame{};
};

std::mutex TimerPresenceMutex;
std::array<TimerPresenceRecord, Core::MaximumPublishedBuffs> TimerPresenceRecords{};

std::atomic<std::uint64_t> CurrentSessionGeneration{};
std::atomic<void*> AuthoritativeGame{};
std::atomic<void*> AuthoritativePlayer{};
std::atomic<std::uint32_t> LocalPlayerUnitId{};
std::atomic<bool> HasLocalGameAuthority{};
std::atomic<bool> FramePumpScheduled{};

std::atomic<bool> NoClockReported{};
std::atomic<bool> ClockReported{};

// A joined client's buffs come from its host (ApplyRemoteBuffs). Its own copy of
// the player has no game, so the HUD runs on a 25 Hz clock started when the
// game was joined.
std::mutex RemoteMutex;
std::vector<std::uint64_t> RemoteKeys;
std::atomic<std::int64_t> RemoteEpochMs{};
std::atomic<bool> RemoteReported{};
std::atomic<std::uint32_t> LastPumpedGameFrame{}; // once per 25 Hz game frame
std::atomic<std::uint32_t> LastTimerDiscoveryFrame{}; // once per game frame

[[nodiscard]] bool IsReadableRange(const void* address, std::size_t size) noexcept {
    if (address == nullptr || size == 0) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto end = start + size;
    if (start < 0x10000u || end < start) return false;
    __try {
        const auto* bytes = static_cast<const volatile std::uint8_t*>(address);
        for (std::size_t offset = 0; offset < size; offset += 4096) {
            (void)bytes[offset];
        }
        (void)bytes[size - 1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
[[nodiscard]] bool ReadField(const void* base, std::size_t offset, T& value) noexcept {
    if (base == nullptr) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    if (start < 0x10000u || offset > static_cast<std::size_t>(UINTPTR_MAX - start)) return false;
    const auto* address = reinterpret_cast<const void*>(start + offset);
    __try {
        std::memcpy(&value, address, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __cdecl AuthorityProbe(const D2RL::PluginContext*, void*) noexcept {}

[[nodiscard]] bool RefreshLocalGameAuthority() noexcept {
    if (Context == nullptr || Threads == nullptr || Threads->runOnGameThread == nullptr) {
        HasLocalGameAuthority.store(false, std::memory_order_release);
        return false;
    }
    const bool authoritative =
        Threads->runOnGameThread(Context, &AuthorityProbe, nullptr) == D2RL::Threads::Result::Success;
    HasLocalGameAuthority.store(authoritative, std::memory_order_release);
    return authoritative;
}

[[nodiscard]] bool IsServerPlayerUnit(const void* unit, const void* game, std::uint32_t& unitId) noexcept {
    unitId = 0;
    std::uint32_t unitType{};
    if (unit == nullptr || reinterpret_cast<std::uintptr_t>(game) < 0x10000u
        || !ReadField(unit, UnitTypeOffset, unitType)
        || unitType != PlayerUnitType
        || !ReadField(unit, UnitIdOffset, unitId)) {
        return false;
    }
    void* node{};
    const auto bucketOffset =
        Native::BloodAuraContract::GamePlayerList + static_cast<std::size_t>(unitId & 0x7Fu) * sizeof(void*);
    if (!ReadField(game, bucketOffset, node)) {
        return false;
    }
    for (std::uint32_t step = 0; node != nullptr && step < MaxPlayerBucketWalk; ++step) {
        if (node == unit) return true;
        void* next{};
        if (!ReadField(node, Native::BloodAuraContract::UnitHashNext, next) || next == node) break;
        node = next;
    }
    return false;
}

[[nodiscard]] bool ReadGameFrame(void* game, std::uint32_t& frame) noexcept {
    frame = 0;
    if (reinterpret_cast<std::uintptr_t>(game) < 0x10000u) return false;
    return ReadField(game, Native::Contract::GameFrameOffset, frame) && frame != 0;
}

[[nodiscard]] const BuffDefinition* FindBuffDefinition(
    const WhitelistCache* cache,
    std::uint32_t state) noexcept {
    if (cache == nullptr) return nullptr;
    const auto it = std::lower_bound(
        cache->definitions.begin(),
        cache->definitions.end(),
        state,
        [](const BuffDefinition& definition, std::uint32_t value) noexcept {
            return definition.stateId < value;
        });
    return it != cache->definitions.end() && it->stateId == state ? &*it : nullptr;
}

[[nodiscard]] const char* CatalogSourceLabel(LooseBuffHud::CatalogSource source) noexcept {
    switch (source) {
    case LooseBuffHud::CatalogSource::LooseOverride:
        return "loose-override";
    case LooseBuffHud::CatalogSource::ExcelFiles:
        return "excel-files";
    case LooseBuffHud::CatalogSource::BuiltinFallback:
        return "builtin-fallback";
    default:
        return "none";
    }
}

[[nodiscard]] bool RefreshWhitelist(std::uint64_t revision) noexcept {
    if (Context == nullptr) return false;
    const auto resolution = LooseBuffHud::ResolveCatalog(
        Context->modSupportDirectory != nullptr ? Context->modSupportDirectory : L"",
        Context->modDirectory != nullptr ? Context->modDirectory : L"",
        Context->activeMod != nullptr ? Context->activeMod : "");
    if (resolution.source == LooseBuffHud::CatalogSource::InvalidOverride
        || resolution.entries.empty()) {
        Whitelist.store(nullptr, std::memory_order_release);
        char line[512]{};
        std::snprintf(
            line,
            sizeof(line),
            "Buff HUD: catalog resolution failed (%s); whitelist-driven BuffHud publishing is disabled.",
            resolution.error.c_str());
        Context->LogError(line);
        return false;
    }

    std::size_t count{};
    std::uint64_t publishedRevision{};
    {
        std::lock_guard lock(WhitelistUpdateMutex);
        ++WhitelistGeneration;
        auto& slot = WhitelistSlots[WhitelistGeneration % WhitelistSlots.size()];
        slot.revision = revision != 0 ? revision : WhitelistGeneration;
        slot.definitions.clear();
        slot.definitions.reserve(resolution.entries.size());
        for (const auto& entry : resolution.entries) {
            slot.definitions.push_back(BuffDefinition{
                .stateId = entry.stateId,
                .displayMode = entry.isResource
                    ? Core::BuffDisplayMode::Resource
                    : Core::BuffDisplayMode::Timer,
                .valueStatId = entry.valueStatId,
                .maxStatId = entry.maxStatId,
                .sourceSkillId = entry.sourceSkillId,
                .valueShift = entry.valueShift,
            });
        }
        count = slot.definitions.size();
        publishedRevision = slot.revision;
        Whitelist.store(&slot, std::memory_order_release);
    }

    const auto sourcePath = resolution.path.empty()
        ? std::string{"<builtin>"}
        : resolution.path.string();
    char line[512]{};
    std::snprintf(
        line,
        sizeof(line),
        "Buff HUD: catalog ready; source=%s path=%s enabledDefinitions=%zu revision=%llu.",
        CatalogSourceLabel(resolution.source),
        sourcePath.c_str(),
        count,
        static_cast<unsigned long long>(publishedRevision));
    Context->LogInfo(line);
    return true;
}

void __cdecl OnTablesLoaded(
    const D2RL::PluginContext* context,
    const D2RL::Lifecycle::DataTablesLoadedEvent* event,
    void*) noexcept {
    if (context == nullptr
        || !D2RL::Lifecycle::HasDataTablesLoadedEventField(
            event, D2RL::Lifecycle::DataTablesLoadedEventRequiredSize)) {
        return;
    }
    (void)RefreshWhitelist(event->revision);
}

[[nodiscard]] std::uint64_t AutomaticBuffKey(std::uint32_t state, std::uint32_t skill) noexcept {
    // Numeric FNV-1a namespace so automatic StatList entries cannot collide
    // with the readable keys used by explicit gameplay producers.
    std::uint64_t hash = 14695981039346656037ULL;
    constexpr char tag[] = "buff-panel:auto-statlist-buff";
    for (const unsigned char ch : tag) {
        if (ch == 0) break;
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= static_cast<std::uint8_t>(state >> shift);
        hash *= 1099511628211ULL;
    }
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= static_cast<std::uint8_t>(skill >> shift);
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1 : hash;
}

[[nodiscard]] bool IsFrameInFuture(std::uint32_t future, std::uint32_t now) noexcept {
    return static_cast<std::int32_t>(future - now) > 0;
}

void ClearTimerPresenceRecords() noexcept {
    std::lock_guard lock(TimerPresenceMutex);
    TimerPresenceRecords = {};
}

void RecordTimerPresence(
    std::uint64_t key,
    std::uint32_t stateId,
    std::uint32_t skillId,
    std::uint32_t expireFrame,
    std::uint32_t postFrame) noexcept {
    if (key == 0 || stateId == 0 || skillId == 0 || expireFrame == 0) return;
    std::lock_guard lock(TimerPresenceMutex);
    TimerPresenceRecord* empty = nullptr;
    for (auto& record : TimerPresenceRecords) {
        if (record.key == key) {
            record.stateId = stateId;
            record.skillId = skillId;
            record.expireFrame = expireFrame;
            record.lastPostFrame = postFrame;
            return;
        }
        if (record.key == 0 && empty == nullptr) empty = &record;
    }
    if (empty != nullptr) {
        *empty = TimerPresenceRecord{
            .key = key,
            .stateId = stateId,
            .skillId = skillId,
            .expireFrame = expireFrame,
            .lastPostFrame = postFrame,
        };
    }
}

// The native list can be re-used when a skill such as Venom is recast. A
// STATLIST_PostStatList notification is not guaranteed for an in-place update:
// read the *currently attached* state every frame, not only its presence.
// Keep the original 0x30 native readable-range qualification, while the logical
// timer model intentionally stops before the unused skillLevel field at +0x2C.
using NativeTimerMetadata = Internal::TimerMetadata;

[[nodiscard]] bool TryResolveStatListTimer(
    void* statList,
    std::uint32_t expectedState,
    std::uint32_t configuredSkill,
    std::uint32_t currentFrame,
    std::uint32_t& skill,
    std::uint32_t& expiry,
    std::uint32_t requiredSkill = 0) noexcept {
    NativeTimerMetadata metadata{};
    if (!IsReadableRange(statList, Native::Contract::StatListBuffMetadataBytes)
        || !ReadField(statList, Native::Contract::StatListBuffFlagsOffset, metadata)
        || !Internal::ResolveTimer(
            metadata, expectedState, configuredSkill, currentFrame, skill, expiry)) {
        return false;
    }
    return requiredSkill == 0 || skill == requiredSkill;
}

// STATLIST_GetStatListFromUnitAndState (0x2F5940) inspects both the active
// (+0x90) and secondary/suppressed (+0x98) chains and returns the first state
// match. If that first match is non-temporary or suppressed, walk the active
// +0x90 chain directly so an active timer is never hidden by a secondary list.
[[nodiscard]] bool FindActiveTimerOnUnit(
    void* unit,
    std::uint32_t expectedState,
    std::uint32_t configuredSkill,
    std::uint32_t currentFrame,
    std::uint32_t& skill,
    std::uint32_t& expiry,
    std::uint32_t requiredSkill = 0) noexcept {
    skill = 0;
    expiry = 0;
    if (unit == nullptr || expectedState == 0 || currentFrame == 0) return false;
    if (GetStatListFromUnitAndState != nullptr) {
        void* candidate = GetStatListFromUnitAndState(unit, static_cast<std::int32_t>(expectedState));
        if (candidate != nullptr
            && TryResolveStatListTimer(
                candidate, expectedState, configuredSkill, currentFrame, skill, expiry, requiredSkill)) {
            return true;
        }
    }
    void* statListEx{};
    std::uint32_t exFlags{};
    if (!ReadField(unit, UnitStatListExOffset, statListEx)
        || statListEx == nullptr
        || !ReadField(statListEx, Native::Contract::StatListBuffFlagsOffset, exFlags)
        || (exFlags & Internal::StatListExtendedFlag) == 0) {
        return false;
    }
    void* list{};
    if (!ReadField(statListEx, StatListActiveHeadOffset, list)) return false;
    for (std::uint32_t step = 0; list != nullptr && step < MaxStatListWalk; ++step) {
        if (TryResolveStatListTimer(
                list, expectedState, configuredSkill, currentFrame, skill, expiry, requiredSkill)) {
            return true;
        }
        void* prev{};
        if (!ReadField(list, StatListPrevLinkOffset, prev) || prev == list) break;
        list = prev;
    }
    return false;
}

void RefreshTimerPresence(std::uint32_t currentFrame) noexcept {
    auto* player = AuthoritativePlayer.load(std::memory_order_acquire);
    if (player == nullptr || GetStatListFromUnitAndState == nullptr || currentFrame == 0) return;

    struct Removal final {
        std::uint64_t key{};
        std::uint32_t stateId{};
        std::uint32_t skillId{};
        std::uint32_t expireFrame{};
    };
    struct Refresh final {
        std::uint64_t key{};
        std::uint32_t stateId{};
        std::uint32_t skillId{};
        std::uint32_t oldExpireFrame{};
        std::uint32_t newExpireFrame{};
    };
    std::array<Removal, Core::MaximumPublishedBuffs> removals{};
    std::size_t removalCount{};
    std::array<Refresh, Core::MaximumPublishedBuffs> refreshes{};
    std::size_t refreshCount{};

    {
        std::lock_guard lock(TimerPresenceMutex);
        for (auto& record : TimerPresenceRecords) {
            if (record.key == 0) continue;

            // Preserve the original countdown lifecycle: natural expiration
            // belongs to BuffHud; never reinterpret it as early removal.
            if (!IsFrameInFuture(record.expireFrame, currentFrame)) {
                record = {};
                continue;
            }
            if (record.lastPostFrame == currentFrame) continue;

            std::uint32_t liveSkill{};
            std::uint32_t liveExpire{};
            if (FindActiveTimerOnUnit(
                    player,
                    record.stateId,
                    record.skillId,
                    currentFrame,
                    liveSkill,
                    liveExpire,
                    record.skillId)) {
                const auto delta = liveExpire > record.expireFrame
                    ? (liveExpire - record.expireFrame)
                    : (record.expireFrame - liveExpire);
                if (delta >= 3u && refreshCount < refreshes.size()) {
                    refreshes[refreshCount++] = Refresh{
                        .key = record.key,
                        .stateId = record.stateId,
                        .skillId = record.skillId,
                        .oldExpireFrame = record.expireFrame,
                        .newExpireFrame = liveExpire,
                    };
                    record.expireFrame = liveExpire;
                    record.lastPostFrame = currentFrame;
                }
                continue;
            }

            // No active temporary timer for this state/skill remains on the
            // unit (either the state was removed or only a non-timer/suppressed
            // StatList remains). Remove the HUD entry immediately.
            if (removalCount < removals.size()) {
                removals[removalCount++] = Removal{
                    .key = record.key,
                    .stateId = record.stateId,
                    .skillId = record.skillId,
                    .expireFrame = record.expireFrame,
                };
            }
            record = {};
        }
    }

    for (std::size_t i = 0; i < refreshCount; ++i) {
        const auto& refresh = refreshes[i];
        const auto snapshot = Core::BuffDisplays().Snapshot();
        for (std::size_t j = 0; j < snapshot.count; ++j) {
            const auto& entry = snapshot.entries[j];
            if (entry.key != refresh.key || entry.displayMode != Core::BuffDisplayMode::Timer
                || entry.expireGameFrame != refresh.oldExpireFrame) continue;
            auto updated = entry;
            updated.expireGameFrame = refresh.newExpireFrame;
            if (!Core::BuffDisplays().Upsert(updated)) {
                break;
            }
            break;
        }
    }

    for (std::size_t i = 0; i < removalCount; ++i) {
        const auto& removal = removals[i];
        if (Core::BuffDisplays().Remove(removal.key)) {
        } else {
        }
    }
}

[[nodiscard]] std::int32_t NormalizeResourceValue(std::int32_t rawValue, std::uint8_t valueShift) noexcept {
    if (valueShift == 0) return rawValue;
    const auto divisor = static_cast<std::int64_t>(1) << valueShift;
    return static_cast<std::int32_t>(static_cast<std::int64_t>(rawValue) / divisor);
}

void RefreshResourceBuffs() noexcept {
    auto* player = AuthoritativePlayer.load(std::memory_order_acquire);
    const auto getter = Core::StatReads().RawGetter();
    const auto* cache = Whitelist.load(std::memory_order_acquire);
    if (player == nullptr || getter == nullptr || cache == nullptr) return;

    // D2RLoader 1.3 materializes the shared stash as a UNIT_PLAYER-shaped
    // proxy carrying state 186 (sharedstash). Never poll player stats from that
    // proxy: Loader deliberately asserts if its proxy is queried before/without
    // the marker state, and the proxy is not the gameplay player anyway.
    if (GetStatListFromUnitAndState != nullptr
        && GetStatListFromUnitAndState(player, SharedStashProxyState) != nullptr) {
        AuthoritativePlayer.store(nullptr, std::memory_order_release);
        return;
    }

    const auto snapshot = Core::BuffDisplays().Snapshot();
    for (const auto& definition : cache->definitions) {
        if (definition.displayMode != Core::BuffDisplayMode::Resource
            || definition.valueStatId <= 0
            || definition.maxStatId <= 0
            || definition.sourceSkillId <= 0) {
            continue;
        }

        const auto key = AutomaticBuffKey(
            definition.stateId,
            static_cast<std::uint32_t>(definition.sourceSkillId));
        const Core::BuffDisplayEntry* existing = nullptr;
        for (std::size_t index = 0; index < snapshot.count; ++index) {
            if (snapshot.entries[index].key == key) {
                existing = &snapshot.entries[index];
                break;
            }
        }

        // A positive stat pool alone does not prove that its buff is active.
        // Mods can reuse stat IDs (Reimagined uses stock Psychic Ward IDs for
        // corruption flags), and a depleted buff can leave its maximum behind.
        // Require the attached native state for EVERY resource definition.
        const bool isAttached = GetStatListFromUnitAndState != nullptr
            && GetStatListFromUnitAndState(
                player, static_cast<std::int32_t>(definition.stateId)) != nullptr;
        if (!isAttached) {
            if (existing != nullptr && Core::BuffDisplays().Remove(key)) {
            }
            continue;
        }

        const auto rawCurrent = getter(player, definition.valueStatId, 0);
        const auto rawMaximum = getter(player, definition.maxStatId, 0);
        const auto current = NormalizeResourceValue(rawCurrent, definition.valueShift);
        const auto maximum = NormalizeResourceValue(rawMaximum, definition.valueShift);

        if (rawCurrent <= 0 || rawMaximum <= 0) {
            if (existing != nullptr && Core::BuffDisplays().Remove(key)) {
            }
            if (rawCurrent > 0 && rawMaximum <= 0) {
            }
            continue;
        }

        if (existing != nullptr
            && existing->displayMode == Core::BuffDisplayMode::Resource
            && existing->sourceSkillId == definition.sourceSkillId
            && existing->valueStatId == definition.valueStatId
            && existing->maxStatId == definition.maxStatId
            && existing->currentValue == current
            && existing->maximumValue == maximum) {
            continue;
        }

        Core::BuffDisplayEntry entry{};
        entry.key = key;
        entry.sourceSkillId = definition.sourceSkillId;
        entry.displayMode = Core::BuffDisplayMode::Resource;
        entry.valueStatId = definition.valueStatId;
        entry.maxStatId = definition.maxStatId;
        entry.currentValue = current;
        entry.maximumValue = maximum;
        entry.stacks = 1;
        entry.priority = 100;
        if (!Core::BuffDisplays().Upsert(entry)) {
            continue;
        }

        if (existing == nullptr) {
        } else {
        }
    }
}

// Some duration effects renew an attached StatList in place, without a new
// STATLIST_PostStatList event. Discover only explicitly whitelisted timer
// states from the authoritative player's currently attached native lists.
// This is a read-only fallback, not a second native hook or a new buff source.
void DiscoverAttachedTimers(std::uint32_t currentFrame) noexcept {
    if (currentFrame == 0 || LastTimerDiscoveryFrame.load(std::memory_order_relaxed) == currentFrame) return;
    LastTimerDiscoveryFrame.store(currentFrame, std::memory_order_relaxed);
    auto* player = AuthoritativePlayer.load(std::memory_order_acquire);
    const auto* cache = Whitelist.load(std::memory_order_acquire);
    if (player == nullptr || GetStatListFromUnitAndState == nullptr || cache == nullptr) return;
    // The loader's UNIT_PLAYER-shaped shared stash proxy must never be treated
    // as a gameplay player. This matches RefreshResourceBuffs' safety guard.
    if (GetStatListFromUnitAndState(player, SharedStashProxyState) != nullptr) return;

    auto snapshot = Core::BuffDisplays().Snapshot();
    for (const auto& definition : cache->definitions) {
        if (definition.displayMode != Core::BuffDisplayMode::Timer) continue;
        const auto state = definition.stateId;
        std::uint32_t skill{}, expiry{};
        if (!FindActiveTimerOnUnit(
                player,
                state,
                definition.sourceSkillId > 0
                    ? static_cast<std::uint32_t>(definition.sourceSkillId) : 0u,
                currentFrame,
                skill,
                expiry)) {
            continue;
        }
        const TimerPresenceRecord witness{
            .key = AutomaticBuffKey(state, skill),
            .stateId = state,
            .skillId = skill,
        };
        bool alreadyPublished = false;
        for (std::size_t i = 0; i < snapshot.count; ++i) {
            if (snapshot.entries[i].key == witness.key
                && snapshot.entries[i].displayMode == Core::BuffDisplayMode::Timer) {
                alreadyPublished = true;
                break;
            }
        }
        if (alreadyPublished) {
            continue;
        }
        Core::BuffDisplayEntry entry{};
        entry.key = witness.key;
        entry.sourceSkillId = static_cast<std::int32_t>(skill);
        entry.displayMode = Core::BuffDisplayMode::Timer;
        entry.expireGameFrame = expiry;
        entry.stacks = 1;
        entry.priority = 100;
        if (!Core::BuffDisplays().Upsert(entry)) {
            continue;
        }
        RecordTimerPresence(witness.key, state, skill, expiry, currentFrame);
        // Keep the per-frame snapshot in step so a duplicate table row or key
        // cannot consume two HUD positions during this scan.
        snapshot = Core::BuffDisplays().Snapshot();
    }
}

void QueueFramePump() noexcept;

[[nodiscard]] bool EnsureAuthoritativePlayer(void*& outGame, std::uint32_t& outFrame) noexcept {
    outGame = AuthoritativeGame.load(std::memory_order_acquire);
    void* player = AuthoritativePlayer.load(std::memory_order_acquire);
    const auto expectedPlayerId = LocalPlayerUnitId.load(std::memory_order_acquire);
    std::uint32_t currentUnitId{};
    if (outGame != nullptr && player != nullptr
        && IsServerPlayerUnit(player, outGame, currentUnitId)
        && (expectedPlayerId == 0 || currentUnitId == expectedPlayerId)
        && (GetStatListFromUnitAndState == nullptr
            || GetStatListFromUnitAndState(player, SharedStashProxyState) == nullptr)
        && ReadGameFrame(outGame, outFrame)) {
        if (!ClockReported.exchange(true, std::memory_order_relaxed) && Context != nullptr) {
            char line[160]{};
            std::snprintf(line, sizeof(line),
                "Buff HUD: tracking buffs from a player unit with a game clock (frame %u).", outFrame);
            Context->LogInfo(line);
        }
        return true;
    }

    outGame = nullptr;
    outFrame = 0;
    if (GameHandles == nullptr || GetGameByHandle == nullptr) return false;

    for (std::uint32_t slot = 0; slot < Native::BloodAuraContract::MaxGames; ++slot) {
        std::uint64_t handle{};
        if (!ReadField(GameHandles, static_cast<std::size_t>(slot) * sizeof(std::uint64_t), handle)
            || handle == 0 || handle == ~0ULL) {
            continue;
        }
        void* game = GetGameByHandle(handle);
        std::uint32_t frame{};
        if (!ReadGameFrame(game, frame)) continue;

        void* matchedUnit = nullptr;
        void* singleUnit = nullptr;
        std::uint32_t singleUnitId = 0;
        std::uint32_t playerCount = 0;
        for (std::size_t bucket = 0; bucket < Native::BloodAuraContract::GameListBuckets; ++bucket) {
            void* unit{};
            if (!ReadField(game,
                    Native::BloodAuraContract::GamePlayerList + bucket * sizeof(void*),
                    unit)) {
                continue;
            }
            for (std::uint32_t step = 0; unit != nullptr && step < MaxPlayerBucketWalk; ++step) {
                std::uint32_t unitType{};
                std::uint32_t unitId{};
                if (ReadField(unit, UnitTypeOffset, unitType)
                    && unitType == PlayerUnitType
                    && ReadField(unit, UnitIdOffset, unitId)
                    && (GetStatListFromUnitAndState == nullptr
                        || GetStatListFromUnitAndState(unit, SharedStashProxyState) == nullptr)) {
                    ++playerCount;
                    singleUnit = unit;
                    singleUnitId = unitId;
                    if (expectedPlayerId != 0 && unitId == expectedPlayerId) {
                        matchedUnit = unit;
                        break;
                    }
                }
                void* next{};
                if (!ReadField(unit, Native::BloodAuraContract::UnitHashNext, next) || next == unit) break;
                unit = next;
            }
            if (matchedUnit != nullptr) break;
        }

        void* resolvedUnit = matchedUnit != nullptr
            ? matchedUnit
            : (playerCount == 1 ? singleUnit : nullptr);
        if (resolvedUnit != nullptr) {
            if (matchedUnit == nullptr && singleUnitId != 0) {
                LocalPlayerUnitId.store(singleUnitId, std::memory_order_release);
            }
            AuthoritativeGame.store(game, std::memory_order_release);
            AuthoritativePlayer.store(resolvedUnit, std::memory_order_release);
            outGame = game;
            outFrame = frame;
            if (!ClockReported.exchange(true, std::memory_order_relaxed) && Context != nullptr) {
                char line[160]{};
                std::snprintf(line, sizeof(line),
                    "Buff HUD: tracking buffs from a player unit with a game clock (frame %u).", frame);
                Context->LogInfo(line);
            }
            return true;
        }
    }
    return false;
}

void __cdecl FramePumpOnGameThread(const D2RL::PluginContext* context, void*) noexcept {
    FramePumpScheduled.store(false, std::memory_order_release);
    if (context == nullptr || context != Context) return;
    const auto session = CurrentSessionGeneration.load(std::memory_order_acquire);
    if (session == 0) return;

    void* game = nullptr;
    std::uint32_t frame{};
    if (EnsureAuthoritativePlayer(game, frame)) {
        if (LastPumpedGameFrame.exchange(frame, std::memory_order_acq_rel) != frame) {
            Core::BuffDisplays().PublishGameFrame(session, frame);
            RefreshResourceBuffs();
            RefreshTimerPresence(frame);
            DiscoverAttachedTimers(frame);
        }
        QueueFramePump();
    }
}

void QueueFramePump() noexcept {
    if (Context == nullptr || Threads == nullptr
        || CurrentSessionGeneration.load(std::memory_order_acquire) == 0
        || !HasLocalGameAuthority.load(std::memory_order_acquire)) {
        return;
    }
    bool expected = false;
    if (!FramePumpScheduled.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return;
    }
    if (Threads->runOnGameThread(Context, &FramePumpOnGameThread, nullptr)
        != D2RL::Threads::Result::Success) {
        FramePumpScheduled.store(false, std::memory_order_release);
    }
}

void OnStatListPost(
    const Core::StatListPostEvent& event,
    Core::StatListPostPhase phase,
    void*) noexcept {
    if (event.unit == nullptr || event.statList == nullptr || GetGameFromUnit == nullptr) return;
    if (!HasLocalGameAuthority.load(std::memory_order_acquire) && !RefreshLocalGameAuthority()) {
        return;
    }

    // Read StatList metadata before any native unit helper. D2RLoader 1.3
    // creates its shared-stash proxy by posting state 186 to a UNIT_PLAYER-
    // shaped proxy. During BeforeNative that marker is not attached yet.
    if (!IsReadableRange(event.statList, Native::Contract::StatListBuffMetadataBytes)) {
        return;
    }

    std::uint32_t flags{};
    std::uint32_t state{};
    float expireFrameFloat{};
    std::uint32_t skill{};
    if (!ReadField(event.statList, Native::Contract::StatListBuffFlagsOffset, flags)
        || !ReadField(event.statList, Native::Contract::StatListBuffStateOffset, state)
        || !ReadField(event.statList, Native::Contract::StatListBuffExpireFrameFloatOffset, expireFrameFloat)
        || !ReadField(event.statList, Native::Contract::StatListBuffSkillIdOffset, skill)) {
        return;
    }

    if (state == SharedStashProxyState) {
        return;
    }

    // Never inspect client-side player units or units in a joined-client session:
    // on a client UnitAny, offset +0xD8 is not a server D2GameStrc*, and reading
    // +0x170 from it can fabricate a bogus non-zero clock frame that locks out
    // ApplyRemoteBuffs and freezes HUD timers.
    void* game = GetGameFromUnit(event.unit);
    std::uint32_t unitId{};
    if (!IsServerPlayerUnit(event.unit, game, unitId)) return;

    if (GetStatListFromUnitAndState != nullptr
        && GetStatListFromUnitAndState(event.unit, SharedStashProxyState) != nullptr) {
        return;
    }

    // On a TCP/IP host, STATLIST_PostStatList fires for every party member's
    // server unit. Only the host's own player unit may drive AuthoritativePlayer.
    const auto expectedPlayerId = LocalPlayerUnitId.load(std::memory_order_acquire);
    if (expectedPlayerId != 0 && unitId != expectedPlayerId) {
        return;
    }
    std::uint32_t currentFrame{};
    if (!ReadGameFrame(game, currentFrame)) {
        // Diagnostics: say once per game why buffs are not being tracked.
        if (!NoClockReported.exchange(true, std::memory_order_relaxed) && Context != nullptr) {
            char line[256]{};
            std::snprintf(line, sizeof(line),
                "Buff HUD: ignoring a buff post for player unit %u (state %u): %s; buffs are not tracked from this unit.",
                unitId, state, game == nullptr ? "it has no game" : "its game has no readable frame");
            Context->LogWarn(line);
        }
        return;
    }
    if (!ClockReported.exchange(true, std::memory_order_relaxed) && Context != nullptr) {
        char line[160]{};
        std::snprintf(line, sizeof(line), "Buff HUD: tracking buffs from a player unit with a game clock (frame %u).", currentFrame);
        Context->LogInfo(line);
    }

    if (phase == Core::StatListPostPhase::BeforeNative) {
        AuthoritativeGame.store(game, std::memory_order_release);
        AuthoritativePlayer.store(event.unit, std::memory_order_release);
        const auto session = CurrentSessionGeneration.load(std::memory_order_acquire);
        if (session != 0) {
            Core::BuffDisplays().PublishGameFrame(session, currentFrame);
            QueueFramePump();
        }
    }

    if (phase == Core::StatListPostPhase::BeforeNative) {
    }

    const auto* cache = Whitelist.load(std::memory_order_acquire);
    const auto* definition = FindBuffDefinition(cache, state);
    if (definition == nullptr) {
        if (phase == Core::StatListPostPhase::BeforeNative) {
        }
        return;
    }
    if ((flags & StatListCurseFlag) != 0) {
        if (phase == Core::StatListPostPhase::BeforeNative) {
        }
        return;
    }
    const auto session = CurrentSessionGeneration.load(std::memory_order_acquire);
    if (session == 0) return;

    if (definition->displayMode == Core::BuffDisplayMode::Timer) {
        if (phase != Core::StatListPostPhase::BeforeNative) return;
        const NativeTimerMetadata metadata{flags, state, expireFrameFloat, skill};
        std::uint32_t resolvedSkill{};
        std::uint32_t expireFrame{};
        if (!Internal::ResolveTimer(
                metadata,
                definition->stateId,
                definition->sourceSkillId > 0
                    ? static_cast<std::uint32_t>(definition->sourceSkillId) : 0u,
                currentFrame,
                resolvedSkill,
                expireFrame)) {
            return;
        }

        Core::BuffDisplayEntry entry{};
        entry.key = AutomaticBuffKey(state, resolvedSkill);
        entry.sourceSkillId = static_cast<std::int32_t>(resolvedSkill);
        entry.displayMode = Core::BuffDisplayMode::Timer;
        entry.expireGameFrame = expireFrame;
        entry.stacks = 1;
        entry.priority = 100;
        if (!Core::BuffDisplays().Upsert(entry)) {
            return;
        }
        RecordTimerPresence(entry.key, state, resolvedSkill, expireFrame, currentFrame);
        return;
    }

    // Resource-mode rows are not classified from StatList metadata. Some
    // absorb-pool skills (notably Bone Armor) do not expose a reliable
    // state/skill/level tuple on the posted list. Refresh resource buffs once
    // the native post completes and again on 25 Hz frame advances.
    if (phase == Core::StatListPostPhase::AfterNative) {
        RefreshResourceBuffs();
    }
    return;

}

[[nodiscard]] std::int64_t SteadyMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void ResetRemoteBuffs() noexcept {
    std::lock_guard lock(RemoteMutex);
    RemoteKeys.clear();
    RemoteEpochMs.store(SteadyMs(), std::memory_order_relaxed);
    RemoteReported.store(false, std::memory_order_relaxed);
}

// 25 Hz frames since the game was joined; never 0, which the HUD reads as no clock.
[[nodiscard]] std::uint32_t RemoteClockFrame() noexcept {
    const auto elapsed = SteadyMs() - RemoteEpochMs.load(std::memory_order_relaxed);
    return static_cast<std::uint32_t>((elapsed > 0 ? elapsed : 0) * 25 / 1000) + 1u;
}

void __cdecl OnGameplayEvent(
    const D2RL::PluginContext*,
    const D2RL::Lifecycle::GameplayEvent* event,
    void*) noexcept {
    if (event == nullptr) return;
    switch (event->kind) {
    case D2RL::Lifecycle::GameplayEventKind::GameJoined:
        NoClockReported.store(false, std::memory_order_relaxed);
        ClockReported.store(false, std::memory_order_relaxed);
        AuthoritativeGame.store(nullptr, std::memory_order_release);
        AuthoritativePlayer.store(nullptr, std::memory_order_release);
        LocalPlayerUnitId.store(event->playerId, std::memory_order_release);
        (void)RefreshLocalGameAuthority();
        ResetRemoteBuffs();
        ClearTimerPresenceRecords();
        LastPumpedGameFrame.store(0, std::memory_order_relaxed);
        LastTimerDiscoveryFrame.store(0, std::memory_order_relaxed);
        CurrentSessionGeneration.store(event->sessionGeneration, std::memory_order_release);
        QueueFramePump();
        break;
    case D2RL::Lifecycle::GameplayEventKind::LocalPlayerReady:
        if (event->playerId != 0) {
            LocalPlayerUnitId.store(event->playerId, std::memory_order_release);
        }
        (void)RefreshLocalGameAuthority();
        LastPumpedGameFrame.store(0, std::memory_order_relaxed);
        CurrentSessionGeneration.store(event->sessionGeneration, std::memory_order_release);
        QueueFramePump();
        break;
    case D2RL::Lifecycle::GameplayEventKind::GameLeft:
        CurrentSessionGeneration.store(0, std::memory_order_release);
        AuthoritativeGame.store(nullptr, std::memory_order_release);
        AuthoritativePlayer.store(nullptr, std::memory_order_release);
        LocalPlayerUnitId.store(0, std::memory_order_release);
        HasLocalGameAuthority.store(false, std::memory_order_release);
        FramePumpScheduled.store(false, std::memory_order_release);
        LastPumpedGameFrame.store(0, std::memory_order_relaxed);
        LastTimerDiscoveryFrame.store(0, std::memory_order_relaxed);
        ClearTimerPresenceRecords();
        ResetRemoteBuffs();
        break;
    default:
        break;
    }
}

[[nodiscard]] bool RegisterLifecycle() noexcept {
    if (Context == nullptr || Lifecycle == nullptr) return false;
    const D2RL::Lifecycle::DataTablesLoadedListener tableListener{
        .structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize,
        .flags = 0,
        .callback = &OnTablesLoaded,
        .userData = nullptr,
    };
    if (Lifecycle->registerDataTablesLoadedListener(Context, &tableListener, &DataTablesListener)
            != D2RL::Lifecycle::Result::Success
        || DataTablesListener == D2RL::Lifecycle::InvalidHandle) {
        return false;
    }
    constexpr std::array kinds{
        D2RL::Lifecycle::GameplayEventKind::GameJoined,
        D2RL::Lifecycle::GameplayEventKind::LocalPlayerReady,
        D2RL::Lifecycle::GameplayEventKind::GameLeft,
    };
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        const D2RL::Lifecycle::GameplayEventListener listener{
            .structSize = D2RL::Lifecycle::GameplayEventListenerSize,
            .flags = 0,
            .kind = kinds[i],
            .reserved = 0,
            .callback = &OnGameplayEvent,
            .userData = nullptr,
        };
        if (Lifecycle->registerGameplayEventListener(Context, &listener, &GameplayListeners[i])
            != D2RL::Lifecycle::Result::Success) {
            return false;
        }
    }
    return true;
}

} // namespace

bool Initialize(const D2RL::PluginContext* context) noexcept {
    Shutdown();
    if (context == nullptr || context->exeBase == 0) return false;
    Context = context;
    const auto& services = Core::Services();
    Threads = services.threads;
    Lifecycle = services.lifecycle;
    if (Threads == nullptr || Lifecycle == nullptr
        || !D2RL::HasThreadServiceField(Threads, D2RL::ThreadServiceRequiredSize)
        || !D2RL::HasLifecycleServiceField(Lifecycle, D2RL::LifecycleServiceRequiredSize)) {
        Context->LogError("Buff HUD: required Thread/Lifecycle service unavailable.");
        Shutdown();
        return false;
    }

    if (!Context->CheckExpectedBytes(
            Native::Contract::GetGameFromUnitRva,
            Native::Contract::GetGameFromUnitExpected.data(),
            static_cast<std::uint32_t>(Native::Contract::GetGameFromUnitExpected.size()))) {
        Context->LogError("Buff HUD: UNITS_GetGame native fingerprint mismatch; refusing whitelist-driven buff tracking.");
        Shutdown();
        return false;
    }
    GetGameFromUnit = reinterpret_cast<GetGameFromUnitFn>(
        context->exeBase + Native::Contract::GetGameFromUnitRva);

    if (!Context->CheckExpectedBytes(
            Native::Contract::GenericCurseStateLookupCallRva,
            Native::Contract::GenericCurseStateLookupCallExpected.data(),
            static_cast<std::uint32_t>(Native::Contract::GenericCurseStateLookupCallExpected.size()))) {
        Context->LogError("Buff HUD: exact-state StatList lookup native fingerprint mismatch; refusing premature timer-removal tracking.");
        Shutdown();
        return false;
    }
    GetStatListFromUnitAndState = reinterpret_cast<GetStatListFromUnitAndStateFn>(
        context->exeBase + Native::Contract::GetStatListFromUnitAndStateRva);

    if (Context->CheckExpectedBytes(
            Native::BloodAuraContract::InitClientTableRva,
            Native::BloodAuraContract::InitClientTableExpected.data(),
            static_cast<std::uint32_t>(Native::BloodAuraContract::InitClientTableExpected.size()))
        && Context->CheckExpectedBytes(
            Native::BloodAuraContract::GetGameByHandleRva,
            Native::BloodAuraContract::GetGameByHandleExpected.data(),
            static_cast<std::uint32_t>(Native::BloodAuraContract::GetGameByHandleExpected.size()))
        && Context->CheckExpectedBytes(
            Native::BloodAuraContract::GetServerUnitRva,
            Native::BloodAuraContract::GetServerUnitExpected.data(),
            static_cast<std::uint32_t>(Native::BloodAuraContract::GetServerUnitExpected.size()))
        && Context->CheckExpectedBytes(
            Native::BloodAuraContract::UnitListBucketRva,
            Native::BloodAuraContract::UnitListBucketExpected.data(),
            static_cast<std::uint32_t>(Native::BloodAuraContract::UnitListBucketExpected.size()))) {
        GetGameByHandle = reinterpret_cast<GetGameByHandleFn>(
            context->exeBase + Native::BloodAuraContract::GetGameByHandleRva);
        GameHandles = reinterpret_cast<const std::uint64_t*>(
            context->exeBase + Native::BloodAuraContract::GameHandleArrayRva);
    }

    if (Core::StatReads().RawGetter() == nullptr) {
        Context->LogError("Buff HUD: shared Core unit-stat reader is unavailable; resource-mode BuffHud entries cannot be supported.");
        Shutdown();
        return false;
    }

    if (!Core::StatListPosts().EnsureInstalled(
            Native::Contract::PostStatListRva,
            Native::Contract::PostStatListExpected,
            Native::Contract::PostStatListBridgeSlotRva)
        || !Core::StatListPosts().Register({
            .owner = "buff-tracker.player-buffs",
            .priority = 50,
            .callback = &OnStatListPost,
            .userData = nullptr,
        })) {
        Context->LogError("Buff HUD: failed to register with the shared Core STATLIST_PostStatList bus.");
        Shutdown();
        return false;
    }

    if (!RegisterLifecycle() || !RefreshWhitelist(0)) {
        Context->LogError("Buff HUD: failed to initialize runtime buff catalog or lifecycle listeners.");
        Shutdown();
        return false;
    }
    Context->LogInfo(
        "Buff HUD 1.3.1+rev.3 BuffTracker initialized: runtime Excel buff catalog tracking with native skill attribution, configured skill fallback, exact-state lifetime checks, and finite native expiry validation.");
    return true;
}

void Shutdown() noexcept {
    CurrentSessionGeneration.store(0, std::memory_order_release);
    AuthoritativeGame.store(nullptr, std::memory_order_release);
    AuthoritativePlayer.store(nullptr, std::memory_order_release);
    LocalPlayerUnitId.store(0, std::memory_order_release);
    HasLocalGameAuthority.store(false, std::memory_order_release);
    FramePumpScheduled.store(false, std::memory_order_release);
    LastPumpedGameFrame.store(0, std::memory_order_relaxed);
    LastTimerDiscoveryFrame.store(0, std::memory_order_relaxed);
    Whitelist.store(nullptr, std::memory_order_release);
    DataTablesListener = D2RL::Lifecycle::InvalidHandle;
    GameplayListeners = {};
    ClearTimerPresenceRecords();
    GetGameFromUnit = nullptr;
    GetStatListFromUnitAndState = nullptr;
    GetGameByHandle = nullptr;
    GameHandles = nullptr;
    Threads = nullptr;
    Lifecycle = nullptr;
    Context = nullptr;
    ResetRemoteBuffs();
}

void CollectRemoteBuffs(void* serverUnit, Core::RemoteBuffList& list) noexcept {
    list = {};
    const auto* cache = Whitelist.load(std::memory_order_acquire);
    const auto getter = Core::StatReads().RawGetter();
    if (serverUnit == nullptr || cache == nullptr || GetGameFromUnit == nullptr
        || GetStatListFromUnitAndState == nullptr) {
        return;
    }
    std::uint32_t frame{};
    if (!ReadGameFrame(GetGameFromUnit(serverUnit), frame)) return;

    for (const auto& definition : cache->definitions) {
        if (list.count >= list.buffs.size()) break;
        const auto state = definition.stateId;
        void* nativeState = GetStatListFromUnitAndState(serverUnit, static_cast<std::int32_t>(state));
        if (nativeState == nullptr) continue;

        // The same rules the local tracker applies (RefreshResourceBuffs,
        // DiscoverAttachedTimers), read from the server's copy of the player.
        if (definition.displayMode == Core::BuffDisplayMode::Resource) {
            if (getter == nullptr || definition.valueStatId <= 0 || definition.maxStatId <= 0
                || definition.sourceSkillId <= 0) {
                continue;
            }
            const auto rawCurrent = getter(serverUnit, definition.valueStatId, 0);
            const auto rawMaximum = getter(serverUnit, definition.maxStatId, 0);
            if (rawCurrent <= 0 || rawMaximum <= 0) continue;
            list.buffs[list.count++] = Core::RemoteBuff{
                .state = static_cast<std::uint16_t>(state),
                .skill = static_cast<std::int16_t>(definition.sourceSkillId),
                .value = NormalizeResourceValue(rawCurrent, definition.valueShift),
                .maximum = NormalizeResourceValue(rawMaximum, definition.valueShift),
            };
            continue;
        }

        std::uint32_t skill{};
        std::uint32_t expiry{};
        if (!FindActiveTimerOnUnit(
                serverUnit,
                state,
                definition.sourceSkillId > 0 ? static_cast<std::uint32_t>(definition.sourceSkillId) : 0u,
                frame,
                skill,
                expiry)) {
            continue;
        }
        list.buffs[list.count++] = Core::RemoteBuff{
            .state = static_cast<std::uint16_t>(state),
            .skill = static_cast<std::int16_t>(skill),
            .value = static_cast<std::int32_t>(expiry - frame),
        };
    }
}

void ApplyRemoteBuffs(const Core::RemoteBuffList& list) noexcept {
    const auto session = CurrentSessionGeneration.load(std::memory_order_acquire);
    const auto* cache = Whitelist.load(std::memory_order_acquire);
    if (session == 0 || cache == nullptr || ClockReported.load(std::memory_order_relaxed)) return;

    std::lock_guard lock(RemoteMutex);
    const auto now = RemoteClockFrame();
    Core::BuffDisplays().PublishGameFrame(session, now);
    const auto snapshot = Core::BuffDisplays().Snapshot();
    std::vector<std::uint64_t> keys;
    keys.reserve(list.count);
    for (std::size_t i = 0; i < list.count; ++i) {
        const auto& buff = list.buffs[i];
        const auto* definition = FindBuffDefinition(cache, buff.state);
        if (definition == nullptr) continue;
        const bool resource = definition->displayMode == Core::BuffDisplayMode::Resource;
        const auto skill = resource ? definition->sourceSkillId : static_cast<std::int32_t>(buff.skill);
        if (skill <= 0) continue;

        Core::BuffDisplayEntry entry{};
        entry.key = AutomaticBuffKey(buff.state, static_cast<std::uint32_t>(skill));
        entry.sourceSkillId = skill;
        entry.displayMode = definition->displayMode;
        entry.stacks = 1;
        entry.priority = 100;
        if (resource) {
            entry.valueStatId = definition->valueStatId;
            entry.maxStatId = definition->maxStatId;
            entry.currentValue = buff.value;
            entry.maximumValue = buff.maximum;
        } else {
            entry.expireGameFrame = now + static_cast<std::uint32_t>(buff.value > 0 ? buff.value : 1);
        }
        keys.push_back(entry.key);

        // Touch the bus only for a real change, so an idle HUD stays idle: a
        // timer whose deadline moved less than half a second is the same timer.
        const Core::BuffDisplayEntry* existing = nullptr;
        for (std::size_t j = 0; j < snapshot.count; ++j) {
            if (snapshot.entries[j].key == entry.key) {
                existing = &snapshot.entries[j];
                break;
            }
        }
        if (existing != nullptr && existing->displayMode == entry.displayMode) {
            if (resource && existing->currentValue == entry.currentValue && existing->maximumValue == entry.maximumValue) continue;
            if (!resource) {
                const auto drift = static_cast<std::int32_t>(existing->expireGameFrame - entry.expireGameFrame);
                if (drift > -12 && drift < 12) continue;
            }
        }
        (void)Core::BuffDisplays().Upsert(entry);
    }
    for (const auto key : RemoteKeys) {
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) (void)Core::BuffDisplays().Remove(key);
    }
    RemoteKeys = std::move(keys);

    if (!RemoteReported.exchange(true, std::memory_order_relaxed) && Context != nullptr) {
        char line[160]{};
        std::snprintf(line, sizeof(line), "Buff HUD: showing %zu buff%s from the host (this client has no game clock of its own).",
            list.count, list.count == 1 ? "" : "s");
        Context->LogInfo(line);
    }
}

void TickRemoteClock() noexcept {
    const auto session = CurrentSessionGeneration.load(std::memory_order_acquire);
    if (session == 0) return;
    if (HasLocalGameAuthority.load(std::memory_order_acquire)) {
        if (!FramePumpScheduled.load(std::memory_order_relaxed)) {
            QueueFramePump();
        }
        return;
    }
    if (ClockReported.load(std::memory_order_relaxed)
        || !RemoteReported.load(std::memory_order_relaxed)) {
        return;
    }
    Core::BuffDisplays().PublishGameFrame(session, RemoteClockFrame());
}

} // namespace BuffPanel::Systems::BuffTracker
