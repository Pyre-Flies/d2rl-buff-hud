#include "blood_aura.hpp"
#include "blood_aura_policy.hpp"

#include "core/remote_buffs.hpp"
#include "core/rip_indirect_bridge.hpp"
#include "native/blood_aura_contract.hpp"
#include "native/native_contract.hpp"
#include "systems/buff_tracker/buff_tracker.hpp"

#include <D2RLImage/character_identity.h>
#include <D2RLSecrets/session_secrets.h>
#include <D2RLServerThread/server_thread.h>

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace BuffPanel::Systems::BloodAura {
namespace {

namespace Contract = Native::BloodAuraContract;
namespace fs = std::filesystem;

using CheckStateFn = std::int32_t(__fastcall*)(void* unit, std::int32_t state) noexcept;
using SuppressStateStatsFn = std::int32_t(__fastcall*)(void* unit, std::int32_t state, std::int32_t suppress) noexcept;
using UnlinkStatListFn = void(__fastcall*)(std::uint8_t dataContext, void* list) noexcept;
using UnitDataContextFn = std::uint8_t(__fastcall*)(void* unit) noexcept;
using GetStatListFromUnitAndStateFn = void*(__fastcall*)(void* unit, std::int32_t state) noexcept;
using GetGameByHandleFn = void*(__fastcall*)(std::uint64_t handle) noexcept;

using Policy::CopyName;
using Policy::SameName;
static_assert(Policy::NameBytes == Contract::PlayerNameBytes);

constexpr std::uint32_t TickMilliseconds = 500;
constexpr std::uint64_t ConnectRetryMilliseconds = 2000;
constexpr std::uint64_t ServerThreadRetryMilliseconds = 1000;
constexpr std::size_t MaximumHostEntries = 512;

enum class HostLink : std::uint8_t { None, Connecting, Connected, Refused };

const D2RL::PluginContext* Context{};
const D2RL::ThreadService* Threads{};
const D2RL::LifecycleService* Lifecycle{};
const D2RL::NetworkService* Network{};
const D2RL::PluginCommunicationService* Communication{};
D2RL::Network::ChannelHandle Channel{D2RL::Network::InvalidChannelHandle};
std::array<D2RL::Lifecycle::ListenerHandle, 2> GameplayListeners{};
bool DedicatedServer{};

// Natives, set once by ResolveNatives and never changed while the worker runs.
CheckStateFn CheckState{};
SuppressStateStatsFn SuppressStateStats{};
UnlinkStatListFn UnlinkStatList{};
UnitDataContextFn UnitDataContext{};
GetStatListFromUnitAndStateFn GetStatListFromUnitAndState{};
GetGameByHandleFn GetGameByHandle{};
const std::uint64_t* GameHandles{};
std::atomic<bool> RelayInstalled{};

// Host side: every player of this process's games who sent their settings
// (including this machine's own in a local game, which connects in-process), one
// entry per name. The network reports no host-side disconnects, so a dedicated
// server's list is bounded by MaximumHostEntries instead (oldest goes first).
struct HostEntry final {
    D2RL::Network::PeerHandle peer{};
    char name[Contract::PlayerNameBytes + 1]{};
    bool auraOff{};
    bool wantsBuffs{};   // a joined client: its own copy of the player has no game clock
    std::uint64_t updated{};
};
std::shared_mutex HostMutex;
std::vector<HostEntry> HostEntries;
std::atomic<bool> HostAnyAuraOff{};
std::atomic<bool> HostAnyWantsBuffs{};
std::atomic<bool> SuppressionMaybeActive{};
std::atomic<std::uint64_t> LastBuffSend{};
constexpr std::uint64_t BuffSendMilliseconds = 1000;

// Host -> client buff lists built on the game thread of a player's TCP/IP host,
// sent from the UI thread (network calls must run on the app thread).
struct OutgoingBuffs final {
    D2RL::Network::PeerHandle peer{};
    std::array<std::uint8_t, Core::RemoteBuffsMaximumBytes> payload{};
    std::size_t size{};
};
std::mutex OutboxMutex;
std::vector<OutgoingBuffs> Outbox;
std::atomic<bool> OutboxQueued{};
std::atomic<std::uint64_t> BuffListsSent{};
std::atomic<std::uint64_t> BuffListsReceived{};

// Client: whether this process runs its own game (decided per game by the
// authority probe); a joined client asks the host for its buffs.
std::atomic<bool> RemoteClient{};
// When the server last saw this machine's player in the Blood Golem aura (ms, 0
// for never): reported by the host's buff list (joined client) or read by this
// process's own scan (local game). The client's copy of the player stops
// carrying the state once the aura has been switched off and on again.
constexpr std::uint64_t AuraReportMilliseconds = 3000;
std::atomic<std::uint64_t> HostInAuraAt{};
std::atomic<std::uint64_t> LocalServerInAuraAt{};

// Client side: this machine's character.
std::mutex LocalMutex;
std::string LocalName;                  // empty until resolved for this game
std::atomic<bool> LocalOff{};
std::atomic<bool> LocalLoaded{};
std::atomic<std::uint32_t> LocalPlayerId{};
std::atomic<bool> InGame{};
std::atomic<bool> PreferenceSent{};
std::atomic<std::uint64_t> LastPreferenceSentAt{};
std::atomic<std::uint64_t> LastBuffReceivedAt{};
std::atomic<HostLink> Link{HostLink::None};
std::atomic<std::uint64_t> NextConnect{};
// Diagnostics: each connect result is logged once per game, and a game that
// still has no character or link after StallMilliseconds says so once.
constexpr std::uint64_t StallMilliseconds = 10000;
std::atomic<std::uint32_t> LastConnectResult{~0u};
std::atomic<bool> StallReported{};
std::atomic<std::uint64_t> InGameSince{};
D2RLImage::CharacterIdentity Identity;
fs::path StateDirectory;

// Worker: one tick every TickMilliseconds queues the host scan and the client
// tick. A global std::thread still joinable at exit is std::terminate, so it is a
// pointer that Shutdown joins.
std::thread* Worker{};
std::mutex WorkerMutex;
std::condition_variable WorkerWake;
bool WorkerStopping{true};
std::atomic<bool> ScanQueued{};
std::atomic<std::uint64_t> ScanQueuedAt{};
std::atomic<bool> ClientTickQueued{};

// d2rds's server-thread service, acquired on a dedicated server only.
std::mutex ServerThreadMutex;
const D2RLServerThread::ServiceV1* ServerThreadService{};
D2RL::PluginCommunication::ServiceHandle ServerThreadHandle{D2RL::PluginCommunication::InvalidServiceHandle};
std::uint64_t ServerThreadLastTry{};

std::atomic<std::uint64_t> Scans{};
std::atomic<std::uint64_t> Suppressed{};
std::atomic<std::uint64_t> Restored{};
std::atomic<std::uint64_t> ManaReadsAnswered{};
std::atomic<std::uint64_t> PreferencesReceived{};

[[nodiscard]] std::uint64_t NowMs() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void LogLine(bool warn, const char* format, ...) noexcept {
    if (Context == nullptr) return;
    char line[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    warn ? Context->LogWarn(line) : Context->LogInfo(line);
}

// Game thread, server thread or the client's own thread; the unit belongs to
// whichever called. Guarded: a unit that is not a player reads as no name.
[[nodiscard]] bool ReadPlayerName(void* unit, char (&output)[Contract::PlayerNameBytes + 1]) noexcept {
    if (reinterpret_cast<std::uintptr_t>(unit) < 0x10000u) return false;
    __try {
        const auto* base = static_cast<const std::uint8_t*>(unit);
        std::uint32_t type{};
        std::memcpy(&type, base + Contract::UnitTypeOffset, sizeof(type));
        if (type != Contract::PlayerUnitType) return false;
        const char* data{};
        std::memcpy(&data, base + Contract::UnitDataOffset, sizeof(data));
        if (reinterpret_cast<std::uintptr_t>(data) < 0x10000u) return false;
        char raw[Contract::PlayerNameBytes]{};
        std::memcpy(raw, data, sizeof(raw));
        return CopyName(raw, output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

[[nodiscard]] bool HostAuraOff(const char* name) noexcept {
    if (!HostAnyAuraOff.load(std::memory_order_acquire)) return false;
    std::shared_lock lock(HostMutex);
    return std::any_of(HostEntries.begin(), HostEntries.end(),
        [name](const HostEntry& entry) noexcept { return entry.auraOff && SameName(entry.name, name); });
}

// The peer to send this player's buffs to, if their client asked for them.
[[nodiscard]] D2RL::Network::PeerHandle HostBuffPeer(const char* name) noexcept {
    if (!HostAnyWantsBuffs.load(std::memory_order_acquire)) return D2RL::Network::InvalidPeerHandle;
    std::shared_lock lock(HostMutex);
    for (const auto& entry : HostEntries) {
        if (entry.wantsBuffs && SameName(entry.name, name)) return entry.peer;
    }
    return D2RL::Network::InvalidPeerHandle;
}

// HostMutex held exclusively.
void RefreshHostFlags() noexcept {
    HostAnyAuraOff.store(std::any_of(HostEntries.begin(), HostEntries.end(),
        [](const HostEntry& entry) noexcept { return entry.auraOff; }), std::memory_order_release);
    HostAnyWantsBuffs.store(std::any_of(HostEntries.begin(), HostEntries.end(),
        [](const HostEntry& entry) noexcept { return entry.wantsBuffs; }), std::memory_order_release);
}

// This machine's character, but only once its host has taken the channel: a
// host that cannot honour the choice still drains life, and the client's
// prediction must keep agreeing with it.
[[nodiscard]] bool LocalCharacterAuraOff(const char* name) noexcept {
    if (!LocalOff.load(std::memory_order_acquire)
        || Link.load(std::memory_order_acquire) != HostLink::Connected) {
        return false;
    }
    std::lock_guard lock(LocalMutex);
    return !LocalName.empty() && SameName(LocalName.c_str(), name);
}

[[nodiscard]] bool UnitAuraOff(void* unit) noexcept {
    char name[Contract::PlayerNameBytes + 1]{};
    if (!ReadPlayerName(unit, name)) return false;
    return HostAuraOff(name) || LocalCharacterAuraOff(name);
}

// The three Blood Mana reads call this instead of STATES_CheckState, through the
// relay in the cave. Every other state and player goes straight through.
std::int32_t __fastcall BloodManaCheck(void* unit, std::int32_t state) noexcept {
    if (state == Contract::BloodManaState && UnitAuraOff(unit)) {
        ManaReadsAnswered.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    return CheckState(unit, state);
}

// ---------------------------------------------------------------------------
// Natives

[[nodiscard]] bool CheckBytes(std::uintptr_t rva, const std::uint8_t* bytes, std::size_t size, const char* name) noexcept {
    if (Context->CheckExpectedBytes(rva, bytes, static_cast<std::uint32_t>(size))) return true;
    LogLine(true, "Buff HUD: Blood Golem aura opt-out witness '%s' (0x%llX) does not match this game; the aura cannot be switched off.",
        name, static_cast<unsigned long long>(rva));
    return false;
}

[[nodiscard]] bool ResolveNatives() noexcept {
    const char* version = Context->buildVersion;
    if (version == nullptr || std::strncmp(version, "3.3", 3) != 0 || (version[3] != '\0' && version[3] != '.')) {
        LogLine(true, "Buff HUD: Blood Golem aura opt-out needs a 3.3 game build (this is %s); the aura cannot be switched off.",
            version != nullptr ? version : "unknown");
        return false;
    }
    using namespace Contract;
    if (!CheckBytes(CheckStateRva, CheckStateExpected.data(), CheckStateExpected.size(), "CheckState")
        || !CheckBytes(ServerSkillManaSiteRva, ServerSkillManaSiteExpected.data(), ServerSkillManaSiteExpected.size(), "ServerSkillManaSite")
        || !CheckBytes(ServerSpendManaSiteRva, ServerSpendManaSiteExpected.data(), ServerSpendManaSiteExpected.size(), "ServerSpendManaSite")
        || !CheckBytes(ClientPredictManaSiteRva, ClientPredictManaSiteExpected.data(), ClientPredictManaSiteExpected.size(), "ClientPredictManaSite")
        || !CheckBytes(RelayCaveRva, RelayCaveExpected.data(), RelayCaveExpected.size(), "RelayCave")
        || !CheckBytes(InitClientTableRva, InitClientTableExpected.data(), InitClientTableExpected.size(), "InitClientTable")
        || !CheckBytes(GetGameByHandleRva, GetGameByHandleExpected.data(), GetGameByHandleExpected.size(), "GetGameByHandle")
        || !CheckBytes(GetServerUnitRva, GetServerUnitExpected.data(), GetServerUnitExpected.size(), "GetServerUnit")
        || !CheckBytes(UnitListBucketRva, UnitListBucketExpected.data(), UnitListBucketExpected.size(), "UnitListBucket")
        || !CheckBytes(GetPlayerDataRva, GetPlayerDataExpected.data(), GetPlayerDataExpected.size(), "GetPlayerData")
        || !CheckBytes(Native::Contract::GenericCurseStateLookupCallRva,
            Native::Contract::GenericCurseStateLookupCallExpected.data(),
            Native::Contract::GenericCurseStateLookupCallExpected.size(), "GetStatListFromUnitAndState")) {
        return false;
    }
    Core::RipIndirectBridgeResolution suppress{};
    if (!Core::ResolveRipIndirectBridge(Context, SuppressStateStatsRva, SuppressStateStatsExpected,
            SuppressStateStatsBridgeSlotRva, &suppress)) {
        LogLine(true, "Buff HUD: Blood Golem aura opt-out cannot resolve D2RLoader's SuppressStateStats bridge (0x%llX); the aura cannot be switched off.",
            static_cast<unsigned long long>(SuppressStateStatsRva));
        return false;
    }
    Core::RipIndirectBridgeResolution unlink{};
    if (!Core::ResolveRipIndirectBridge(Context, UnlinkStatListRva, UnlinkStatListExpected,
            UnlinkStatListBridgeSlotRva, &unlink)
        || !CheckBytes(UnitDataContextRva, UnitDataContextExpected.data(), UnitDataContextExpected.size(), "UnitDataContext")) {
        LogLine(true, "Buff HUD: Blood Golem aura opt-out cannot resolve the stat-list unlink (0x%llX); the aura cannot be switched off.",
            static_cast<unsigned long long>(UnlinkStatListRva));
        return false;
    }
    const auto base = Context->exeBase;
    CheckState = reinterpret_cast<CheckStateFn>(base + CheckStateRva);
    SuppressStateStats = reinterpret_cast<SuppressStateStatsFn>(suppress.targetAddress);
    UnlinkStatList = reinterpret_cast<UnlinkStatListFn>(unlink.targetAddress);
    UnitDataContext = reinterpret_cast<UnitDataContextFn>(base + UnitDataContextRva);
    GetStatListFromUnitAndState = reinterpret_cast<GetStatListFromUnitAndStateFn>(
        base + Native::Contract::GetStatListFromUnitAndStateRva);
    GetGameByHandle = reinterpret_cast<GetGameByHandleFn>(base + GetGameByHandleRva);
    GameHandles = reinterpret_cast<const std::uint64_t*>(base + GameHandleArrayRva);
    return true;
}

// Writes the relay into the cave and points the three Blood Mana calls at it,
// all in one transaction: either every read is redirected or none is.
[[nodiscard]] bool InstallRelay() noexcept {
    const D2RL::MutationService* mutations{};
    if (Context->QueryService(&mutations) != D2RL::ServiceQueryResult::Success
        || !D2RL::HasMutationServiceField(mutations, D2RL::MutationServiceRequiredSize)) {
        LogLine(true, "Buff HUD: MutationService is unavailable; the Blood Golem aura cannot be switched off.");
        return false;
    }
    using namespace Contract;
    std::array<std::uint8_t, RelaySize> relay{0x48, 0xB8};
    const auto target = reinterpret_cast<std::uint64_t>(&BloodManaCheck);
    std::memcpy(relay.data() + 2, &target, sizeof(target));
    relay[10] = 0xFF;
    relay[11] = 0xE0;

    D2RL::Mutations::TransactionHandle transaction{};
    if (mutations->beginTransaction(Context, &transaction) != D2RL::Mutations::Result::Success) {
        LogLine(true, "Buff HUD: could not start the Blood Golem aura patch transaction.");
        return false;
    }
    bool staged = true;
    D2RL::Mutations::OperationHandle operation{};
    const D2RL::Mutations::BytePatchRequest cave{
        .structSize = D2RL::Mutations::BytePatchRequestSize,
        .rva = RelayCaveRva,
        .expected = RelayCaveExpected.data(),
        .expectedSize = static_cast<std::uint32_t>(RelaySize),
        .bytes = relay.data(),
        .size = static_cast<std::uint32_t>(RelaySize),
    };
    staged = mutations->stageBytePatch(Context, transaction, &cave, &operation) == D2RL::Mutations::Result::Success;

    const std::array<std::pair<std::uintptr_t, const std::uint8_t*>, 3> calls{{
        {ServerSkillManaCallRva, ServerSkillManaSiteExpected.data() + (ServerSkillManaCallRva - ServerSkillManaSiteRva)},
        {ServerSpendManaCallRva, ServerSpendManaSiteExpected.data() + (ServerSpendManaCallRva - ServerSpendManaSiteRva)},
        {ClientPredictManaCallRva, ClientPredictManaSiteExpected.data() + (ClientPredictManaCallRva - ClientPredictManaSiteRva)},
    }};
    for (const auto& [rva, expected] : calls) {
        if (!staged) break;
        const D2RL::Mutations::Rel32PatchRequest call{
            .structSize = D2RL::Mutations::Rel32PatchRequestSize,
            .rva = rva,
            .expected = expected,
            .expectedSize = static_cast<std::uint32_t>(ManaCallSize),
            .kind = D2RL::Rel32PatchKind::Call,
            .targetRva = RelayCaveRva,
            .size = static_cast<std::uint32_t>(ManaCallSize),
        };
        staged = mutations->stageRel32Patch(Context, transaction, &call, &operation) == D2RL::Mutations::Result::Success;
    }

    D2RL::Mutations::CommitResult result{.structSize = D2RL::Mutations::CommitResultSize};
    if (!staged || mutations->commit(Context, transaction, &result) != D2RL::Mutations::Result::Success) {
        (void)mutations->cancelTransaction(Context, transaction);
        LogLine(true, "Buff HUD: the Blood Golem aura patch was refused (staged=%d result=%u); the aura cannot be switched off.",
            staged ? 1 : 0, static_cast<unsigned>(result.result));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Host: the scan that keeps each player's aura stats in step with their choice

template <typename Visit>
void ForEachServerPlayer(Visit&& visit) noexcept {
    if (GameHandles == nullptr || GetGameByHandle == nullptr) return;
    for (std::uint32_t slot = 0; slot < Contract::MaxGames; ++slot) {
        const std::uint64_t handle = GameHandles[slot];
        if (handle == 0 || handle == ~0ULL) continue;
        auto* game = static_cast<std::uint8_t*>(GetGameByHandle(handle));
        if (game == nullptr) continue;
        for (std::size_t bucket = 0; bucket < Contract::GameListBuckets; ++bucket) {
            void* unit{};
            std::memcpy(&unit, game + Contract::GamePlayerList + bucket * sizeof(void*), sizeof(unit));
            std::uint32_t steps = 0;
            while (unit != nullptr && steps++ < Contract::MaxChain) {
                visit(unit);
                std::memcpy(&unit, static_cast<std::uint8_t*>(unit) + Contract::UnitHashNext, sizeof(unit));
            }
        }
    }
}

// App thread: sends the buff lists the last scan built.
void FlushOutbox() noexcept {
    OutboxQueued.store(false, std::memory_order_release);
    std::vector<OutgoingBuffs> sending;
    {
        std::lock_guard lock(OutboxMutex);
        sending.swap(Outbox);
    }
    if (Network == nullptr || Channel == D2RL::Network::InvalidChannelHandle) return;
    for (const auto& outgoing : sending) {
        if (Network->sendToClient(Context, Channel, outgoing.peer, Core::RemoteBuffsMessage,
                outgoing.payload.data(), static_cast<std::uint32_t>(outgoing.size)) == D2RL::Network::Result::Success) {
            BuffListsSent.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void __cdecl FlushOutboxOnUiThread(const D2RL::PluginContext*, void*) noexcept {
    FlushOutbox();
}

// One scan's buff lists, one per peer. A dedicated server can still hold an
// older game with a unit of the same name (seen in game 2026-10-28: the client
// got its buffs and an empty list every second, so they flickered away); the
// fullest list wins.
struct PendingBuffs final {
    D2RL::Network::PeerHandle peer{};
    Core::RemoteBuffList buffs{};
};
using PendingList = std::vector<PendingBuffs>;
std::atomic<bool> DuplicateUnitReported{};

struct ScanState final {
    bool sendBuffs{};
    std::uint32_t suppressedNow{};
    PendingList pending;
};

// One server player: keeps their aura stats in step with their choice and, for a
// joined client that asked, collects their buffs.
void ScanPlayer(void* unit, ScanState& scan, const char* fallbackName = nullptr) noexcept {
    auto& suppressedNow = scan.suppressedNow;
    void* auraList = GetStatListFromUnitAndState != nullptr
        ? GetStatListFromUnitAndState(unit, Contract::BloodManaState)
        : nullptr;
    char name[Contract::PlayerNameBytes + 1]{};
    const bool named = ReadPlayerName(unit, name)
        || (fallbackName != nullptr && CopyName(fallbackName, name));
    // This machine's own player in a local game or on its TCP/IP host: the HUD
    // asks LocalPlayerInAura, which reads this (see LocalServerInAuraAt).
    if (named && auraList != nullptr && !RemoteClient.load(std::memory_order_acquire)) {
        std::lock_guard lock(LocalMutex);
        if (!LocalName.empty() && SameName(LocalName.c_str(), name)) {
            LocalServerInAuraAt.store(NowMs(), std::memory_order_relaxed);
        }
    }
    if (void* list = auraList; list != nullptr
        && SuppressStateStats != nullptr && UnlinkStatList != nullptr && UnitDataContext != nullptr) {
        std::uint32_t flags{};
        std::memcpy(&flags, static_cast<std::uint8_t*>(list) + Native::Contract::StatListBuffFlagsOffset, sizeof(flags));
        const bool suppressed = (flags & Contract::StatListSuppressedFlag) != 0;
        const bool off = UnitAuraOff(unit);
        if (off == suppressed) {
            if (suppressed) ++suppressedNow;
        } else if (off) {
            if (SuppressStateStats(unit, Contract::BloodManaState, 1) != 0) {
                ++suppressedNow;
                Suppressed.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            // Switched back on: un-suppressing would bring the stats back but
            // not the state. Drop the list instead; the aura's next pulse (every
            // two seconds) applies it from scratch, state, bonuses and the
            // client's copy included. A suppressed list carries no stats, so
            // nothing is subtracted.
            UnlinkStatList(UnitDataContext(unit), list);
            Restored.fetch_add(1, std::memory_order_relaxed);
            LogLine(false, "Buff HUD: Blood Golem aura switched back on; the aura re-applies with its next pulse.");
        }
    }
    if (!scan.sendBuffs || !named) return;
    const auto peer = HostBuffPeer(name);
    if (peer == D2RL::Network::InvalidPeerHandle) return;
    Core::RemoteBuffList buffs{};
    BuffTracker::CollectRemoteBuffs(unit, buffs);
    if (auraList != nullptr) buffs.flags |= Core::RemoteBuffsInBloodAura;
    for (auto& pending : scan.pending) {
        if (pending.peer != peer) continue;
        if (!DuplicateUnitReported.exchange(true, std::memory_order_relaxed)) {
            LogLine(true, "Buff HUD: two server units are named %s (%zu and %zu buffs); sending the fuller list.",
                name, pending.buffs.count, buffs.count);
        }
        const auto flags = static_cast<std::uint8_t>(pending.buffs.flags | buffs.flags);
        if (buffs.count > pending.buffs.count) pending.buffs = buffs;
        pending.buffs.flags = flags;
        return;
    }
    scan.pending.push_back(PendingBuffs{.peer = peer, .buffs = buffs});
}

bool __cdecl ScanClient(std::uint32_t, void*, void* player, const char* characterName, void* userData) noexcept {
    if (player != nullptr) ScanPlayer(player, *static_cast<ScanState*>(userData), characterName);
    return true;
}

// Server game state only: the authoritative game thread or d2rds's loop. A
// dedicated server visits only players with a connected client; a local game
// or a player's host walks its one game's player list.
void HostScan(bool appThread) noexcept {
    const auto now = NowMs();
    ScanState scan{};
    scan.sendBuffs = HostAnyWantsBuffs.load(std::memory_order_acquire)
        && now - LastBuffSend.load(std::memory_order_relaxed) >= BuffSendMilliseconds;
    if (scan.sendBuffs) LastBuffSend.store(now, std::memory_order_relaxed);
    try {
        if (DedicatedServer && ServerThreadService != nullptr && ServerThreadService->forEachClient != nullptr) {
            (void)ServerThreadService->forEachClient(&ScanClient, &scan);
        } else {
            ForEachServerPlayer([&](void* unit) noexcept { ScanPlayer(unit, scan); });
        }
    } catch (...) {
        scan.pending.clear();
    }
    // Keep scanning while any list is left suppressed, so one switched back on
    // is restored even after the last opted-out player has gone.
    SuppressionMaybeActive.store(scan.suppressedNow != 0, std::memory_order_release);
    Scans.fetch_add(1, std::memory_order_relaxed);
    if (!scan.sendBuffs) return;
    {
        std::lock_guard lock(OutboxMutex);
        for (const auto& pending : scan.pending) {
            OutgoingBuffs outgoing{.peer = pending.peer};
            outgoing.size = Core::EncodeRemoteBuffs(pending.buffs, outgoing.payload);
            Outbox.push_back(outgoing);
        }
    }
    // d2rds's server loop is the app thread; a player host's game thread is not.
    if (appThread) {
        FlushOutbox();
    } else if (!OutboxQueued.exchange(true, std::memory_order_acq_rel)
        && Threads->runOnUiThread(Context, &FlushOutboxOnUiThread, nullptr) != D2RL::Threads::Result::Success) {
        OutboxQueued.store(false, std::memory_order_release);
    }
}

void __cdecl HostScanOnGameThread(const D2RL::PluginContext*, void*) noexcept {
    HostScan(false);
    ScanQueued.store(false, std::memory_order_release);
}

void __cdecl HostScanOnServerThread(void*) noexcept {
    HostScan(true);
    ScanQueued.store(false, std::memory_order_release);
}

[[nodiscard]] const D2RLServerThread::ServiceV1* ServerThread() noexcept {
    std::lock_guard lock(ServerThreadMutex);
    if (ServerThreadService != nullptr || !DedicatedServer || Communication == nullptr) return ServerThreadService;
    const auto now = NowMs();
    if (now - ServerThreadLastTry < ServerThreadRetryMilliseconds) return nullptr;
    ServerThreadLastTry = now;
    const D2RL::PluginCommunication::AcquireServiceRequest request{
        .structSize = D2RL::PluginCommunication::AcquireServiceRequestSize,
        .providerPluginId = D2RLServerThread::ProviderPluginId,
        .name = D2RLServerThread::ServiceName,
        .minimumVersion = D2RLServerThread::ServiceVersion,
        .minimumTableSize = D2RLServerThread::ServiceV1Size,
    };
    D2RL::PluginCommunication::AcquiredService acquired{.structSize = D2RL::PluginCommunication::AcquiredServiceSize};
    if (Communication->acquireService(Context, &request, &acquired) == D2RL::PluginCommunication::Result::Success
        && acquired.table != nullptr) {
        ServerThreadService = static_cast<const D2RLServerThread::ServiceV1*>(acquired.table);
        ServerThreadHandle = acquired.handle;
        LogLine(false, "Buff HUD: dedicated server; Blood Golem aura choices are applied on d2rds's server thread.");
    }
    return ServerThreadService;
}

void QueueHostScan() noexcept {
    // A local game always scans: it is how this machine's own HUD learns whether
    // its player is in the aura (LocalServerInAuraAt).
    const bool localGame = InGame.load(std::memory_order_acquire) && !RemoteClient.load(std::memory_order_acquire);
    if (!HostAnyAuraOff.load(std::memory_order_acquire) && !HostAnyWantsBuffs.load(std::memory_order_acquire)
        && !SuppressionMaybeActive.load(std::memory_order_acquire) && !localGame) {
        return;
    }
    const auto now = NowMs();
    // A queued call dropped on a session change never reports back; stop
    // waiting for it after a few seconds.
    if (ScanQueued.load(std::memory_order_acquire) && now - ScanQueuedAt.load(std::memory_order_relaxed) < 3000) return;
    ScanQueued.store(true, std::memory_order_release);
    ScanQueuedAt.store(now, std::memory_order_relaxed);
    if (DedicatedServer) {
        if (const auto* server = ServerThread(); server != nullptr && server->runOnServerThread(&HostScanOnServerThread, nullptr)) return;
    } else if (Threads->runOnGameThread(Context, &HostScanOnGameThread, nullptr) == D2RL::Threads::Result::Success) {
        return;
    }
    // A joined client or the menu: no server game here.
    ScanQueued.store(false, std::memory_order_release);
}

void __cdecl OnHostMessage(
    const D2RL::PluginContext*, D2RL::Network::ChannelHandle, D2RL::Network::PeerHandle peer,
    std::uint16_t messageId, const void* data, std::uint32_t size, void*) noexcept {
    HostEntry entry{.peer = peer, .updated = NowMs()};
    if (messageId != Policy::PreferenceMessage || peer == D2RL::Network::InvalidPeerHandle
        || !Policy::DecodePreference(data, size, entry.name, entry.auraOff, entry.wantsBuffs)) {
        return;
    }
    PreferencesReceived.fetch_add(1, std::memory_order_relaxed);
    {
        std::unique_lock lock(HostMutex);
        // A player re-joining comes back on a new peer; one entry per name.
        std::erase_if(HostEntries, [&](const HostEntry& existing) noexcept {
            return existing.peer == peer || SameName(existing.name, entry.name);
        });
        if (entry.auraOff || entry.wantsBuffs) {
            if (HostEntries.size() >= MaximumHostEntries) {
                HostEntries.erase(std::min_element(HostEntries.begin(), HostEntries.end(),
                    [](const HostEntry& l, const HostEntry& r) noexcept { return l.updated < r.updated; }));
            }
            HostEntries.push_back(entry);
        }
        RefreshHostFlags();
    }
    // A switch back on must still be undone by a scan.
    if (!entry.auraOff) SuppressionMaybeActive.store(true, std::memory_order_release);
    LogLine(false, "Buff HUD: %s has the Blood Golem aura %s%s.", entry.name, entry.auraOff ? "off" : "on",
        entry.wantsBuffs ? "; sending them their buffs" : "");
}

// ---------------------------------------------------------------------------
// Client: this character's choice, its sidecar file and the host link

fs::path ResolveStateDirectory() noexcept {
    try {
        PWSTR savedGames = nullptr;
        fs::path directory;
        if (SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &savedGames) == S_OK) {
            directory = fs::path(savedGames) / L"Diablo II Resurrected" / L".buffhud";
        }
        if (savedGames != nullptr) CoTaskMemFree(savedGames);
        if (directory.empty() && Context->pluginConfigPath != nullptr) {
            directory = fs::path(Context->pluginConfigPath).parent_path() / L"buffhud-state";
        }
        return directory;
    } catch (...) {
        return {};
    }
}

fs::path SidecarPath(const std::string& name) noexcept {
    try {
        return D2RLImage::CharacterStateFile(StateDirectory, Context->activeMod != nullptr ? Context->activeMod : "", name);
    } catch (...) {
        return {};
    }
}

// A missing file is the default: on.
[[nodiscard]] bool LoadSidecar(const std::string& name) noexcept {
    try {
        const auto path = SidecarPath(name);
        if (path.empty()) return false;
        std::ifstream file(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        LocalOff.store(Policy::ParseSidecarAuraOff(text), std::memory_order_release);
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool SaveSidecar(const std::string& name, bool off) noexcept {
    try {
        const auto path = SidecarPath(name);
        if (path.empty()) return false;
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        if (error) return false;
        auto temporary = path;
        temporary += L".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file << Policy::FormatSidecar(off);
            if (!file) return false;
        }
        return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    } catch (...) {
        return false;
    }
}

// UI thread (network calls must run on the app thread).
void SendPreference() noexcept {
    if (Network == nullptr || Channel == D2RL::Network::InvalidChannelHandle
        || Link.load(std::memory_order_acquire) != HostLink::Connected) {
        return;
    }
    Policy::Preference preference{
        .version = Policy::PreferenceVersion,
        .auraOff = static_cast<std::uint8_t>(LocalOff.load(std::memory_order_acquire) ? 1 : 0),
        .flags = static_cast<std::uint16_t>(RemoteClient.load(std::memory_order_acquire) ? Policy::WantsBuffsFlag : 0),
        .playerId = LocalPlayerId.load(std::memory_order_relaxed),
    };
    {
        std::lock_guard lock(LocalMutex);
        if (LocalName.empty()) return;
        std::memcpy(preference.name, LocalName.data(), std::min(LocalName.size(), sizeof(preference.name)));
    }
    const auto sent = Network->sendToHost(Context, Channel, Policy::PreferenceMessage, &preference, sizeof(preference));
    if (sent == D2RL::Network::Result::Success) {
        PreferenceSent.store(true, std::memory_order_release);
        LastPreferenceSentAt.store(NowMs(), std::memory_order_relaxed);
        LogLine(false, "Buff HUD: told the host the Blood Golem aura is %s for %.16s.", preference.auraOff != 0 ? "off" : "on", preference.name);
        return;
    }
    LogLine(true, "Buff HUD: could not send the Blood Golem aura setting to the host (result=%u).", static_cast<unsigned>(sent));
    if (sent == D2RL::Network::Result::NotConnected) {
        Link.store(HostLink::None, std::memory_order_release);
    }
}

// UI thread.
void ConnectToHost() noexcept {
    if (Network == nullptr || Channel == D2RL::Network::InvalidChannelHandle) return;
    const auto link = Link.load(std::memory_order_acquire);
    const auto now = NowMs();
    if (link == HostLink::Connected || link == HostLink::Refused || now < NextConnect.load(std::memory_order_relaxed)) return;
    NextConnect.store(now + ConnectRetryMilliseconds, std::memory_order_relaxed);
    const auto result = Network->connectToHost(Context, Channel);
    const auto code = static_cast<std::uint32_t>(result);
    if (LastConnectResult.exchange(code, std::memory_order_relaxed) != code) {
        LogLine(false, "Buff HUD: settings channel connectToHost result=%u (link=%u).", code, static_cast<unsigned>(Link.load()));
    }
    if (!StallReported.load(std::memory_order_relaxed) && now - InGameSince.load(std::memory_order_relaxed) > StallMilliseconds) {
        StallReported.store(true, std::memory_order_relaxed);
        LogLine(true, "Buff HUD: settings channel still not connected after %llu s (link=%u, last result=%u); the Blood Golem aura checkbox stays hidden.",
            static_cast<unsigned long long>(StallMilliseconds / 1000), static_cast<unsigned>(Link.load()), code);
    }
    switch (result) {
    case D2RL::Network::Result::Success:
        // A local game connects in-process, possibly before this returns.
        if (Link.load(std::memory_order_acquire) != HostLink::Connected) {
            Link.store(HostLink::Connecting, std::memory_order_release);
        }
        break;
    case D2RL::Network::Result::Incompatible:
    case D2RL::Network::Result::Unsupported:
    case D2RL::Network::Result::WrongRole:
        Link.store(HostLink::Refused, std::memory_order_release);
        LogLine(true, "Buff HUD: this game's host cannot take Buff HUD settings; the Blood Golem aura cannot be switched off here.");
        break;
    default:
        break; // Busy, Unavailable, NotConnected: not ready yet.
    }
}

// UI thread: resolves the character, loads its choice, keeps the host link up
// and sends the choice once per connection.
void __cdecl ClientTickOnUiThread(const D2RL::PluginContext*, void*) noexcept {
    ClientTickQueued.store(false, std::memory_order_release);
    if (!InGame.load(std::memory_order_acquire)) return;
    if (!LocalLoaded.load(std::memory_order_acquire)) {
        std::string name;
        try {
            name = Identity.Read(LocalPlayerId.load(std::memory_order_relaxed));
        } catch (...) {
        }
        if (name.empty() || name.size() > Contract::PlayerNameBytes - 1) {
            if (!StallReported.load(std::memory_order_relaxed)
                && NowMs() - InGameSince.load(std::memory_order_relaxed) > StallMilliseconds) {
                StallReported.store(true, std::memory_order_relaxed);
                LogLine(true, "Buff HUD: this character's name is still unknown after %llu s (playerId=%u); the Blood Golem aura checkbox stays hidden.",
                    static_cast<unsigned long long>(StallMilliseconds / 1000), LocalPlayerId.load());
            }
            return;
        }
        if (!LoadSidecar(name)) {
            LogLine(true, "Buff HUD: could not read %s's settings; the Blood Golem aura stays on.", name.c_str());
            LocalOff.store(false, std::memory_order_release);
        }
        {
            std::lock_guard lock(LocalMutex);
            LocalName = name;
        }
        LocalLoaded.store(true, std::memory_order_release);
        PreferenceSent.store(false, std::memory_order_release);
        LogLine(false, "Buff HUD: %s has the Blood Golem aura %s (saved setting).", name.c_str(), LocalOff.load() ? "off" : "on");
    }
    ConnectToHost();
    if (Link.load(std::memory_order_acquire) == HostLink::Connected) {
        if (!PreferenceSent.load(std::memory_order_acquire)) {
            SendPreference();
        } else if (RemoteClient.load(std::memory_order_acquire)) {
            const auto now = NowMs();
            const auto lastRx = LastBuffReceivedAt.load(std::memory_order_relaxed);
            const auto lastTx = LastPreferenceSentAt.load(std::memory_order_relaxed);
            if (now - (lastRx != 0 ? lastRx : InGameSince.load(std::memory_order_relaxed)) > 3500
                && now - lastTx > 4000) {
                SendPreference();
            }
        }
    }
}

void QueueClientTick() noexcept {
    if (!InGame.load(std::memory_order_acquire) || Threads == nullptr) return;
    if (ClientTickQueued.exchange(true, std::memory_order_acq_rel)) return;
    if (Threads->runOnUiThread(Context, &ClientTickOnUiThread, nullptr) != D2RL::Threads::Result::Success) {
        ClientTickQueued.store(false, std::memory_order_release);
    }
}

void __cdecl OnConnectionState(const D2RL::PluginContext*, const D2RL::Network::ConnectionEvent* event, void*) noexcept {
    if (!D2RL::Network::HasConnectionEventField(event, D2RL::Network::ConnectionEventRequiredSize)) return;
    switch (event->state) {
    case D2RL::Network::ConnectionState::Connected:
        if (Link.exchange(HostLink::Connected, std::memory_order_acq_rel) != HostLink::Connected) {
            LogLine(false, "Buff HUD: connected to the host's settings channel; the Blood Golem aura can be switched here.");
        }
        PreferenceSent.store(false, std::memory_order_release);
        QueueClientTick();
        break;
    case D2RL::Network::ConnectionState::Connecting:
        Link.store(HostLink::Connecting, std::memory_order_release);
        break;
    case D2RL::Network::ConnectionState::Rejected:
        if (event->reason == D2RL::Network::RejectionReason::HostUnavailable) {
            Link.store(HostLink::None, std::memory_order_release);
        } else {
            Link.store(HostLink::Refused, std::memory_order_release);
            LogLine(true, "Buff HUD: the host refused the settings channel (reason=%u); the Blood Golem aura cannot be switched off in this game.",
                static_cast<unsigned>(event->reason));
        }
        break;
    default:
        Link.store(HostLink::None, std::memory_order_release);
        break;
    }
}

void __cdecl OnClientMessage(
    const D2RL::PluginContext*, D2RL::Network::ChannelHandle, std::uint16_t messageId, const void* data,
    std::uint32_t size, void*) noexcept {
    if (messageId != Core::RemoteBuffsMessage || !RemoteClient.load(std::memory_order_acquire)) return;
    Core::RemoteBuffList buffs{};
    if (!Core::DecodeRemoteBuffs(data, size, buffs)) return;
    BuffListsReceived.fetch_add(1, std::memory_order_relaxed);
    const auto now = NowMs();
    LastBuffReceivedAt.store(now, std::memory_order_relaxed);
    HostInAuraAt.store((buffs.flags & Core::RemoteBuffsInBloodAura) != 0 ? now : 0, std::memory_order_relaxed);
    BuffTracker::ApplyRemoteBuffs(buffs);
}

// Reaching the game thread is the whole answer: only a local game or a TCP/IP
// host has one, and runOnGameThread says so without queueing for a joined client.
void __cdecl AuthorityProbe(const D2RL::PluginContext*, void*) noexcept {}

void ResetClientSession() noexcept {
    {
        std::lock_guard lock(LocalMutex);
        LocalName.clear();
    }
    LocalLoaded.store(false, std::memory_order_release);
    LocalOff.store(false, std::memory_order_release);
    PreferenceSent.store(false, std::memory_order_release);
    LastPreferenceSentAt.store(0, std::memory_order_relaxed);
    LastBuffReceivedAt.store(0, std::memory_order_relaxed);
    // Each game may have a different host; a refusal is not carried over.
    Link.store(HostLink::None, std::memory_order_release);
    NextConnect.store(0, std::memory_order_relaxed);
    LastConnectResult.store(~0u, std::memory_order_relaxed);
    StallReported.store(false, std::memory_order_relaxed);
    InGameSince.store(NowMs(), std::memory_order_relaxed);
    HostInAuraAt.store(0, std::memory_order_relaxed);
    LocalServerInAuraAt.store(0, std::memory_order_relaxed);
}

void __cdecl OnGameplayEvent(const D2RL::PluginContext*, const D2RL::Lifecycle::GameplayEvent* event, void*) noexcept {
    if (!D2RL::Lifecycle::HasGameplayEventField(event, D2RL::Lifecycle::GameplayEventRequiredSize)) return;
    switch (event->kind) {
    case D2RL::Lifecycle::GameplayEventKind::LocalPlayerReady:
        ResetClientSession();
        LocalPlayerId.store(event->playerId, std::memory_order_relaxed);
        RemoteClient.store(Threads->runOnGameThread(Context, &AuthorityProbe, nullptr) != D2RL::Threads::Result::Success,
            std::memory_order_release);
        LogLine(false, "Buff HUD: %s.", RemoteClient.load() ? "joined someone else's game; buffs come from the host"
                                                             : "this process runs the game; buffs are read locally");
        InGame.store(true, std::memory_order_release);
        ClientTickOnUiThread(Context, nullptr);
        break;
    case D2RL::Lifecycle::GameplayEventKind::GameLeft:
        InGame.store(false, std::memory_order_release);
        ResetClientSession();
        // A local game or TCP/IP host ends with its game; its peers are gone.
        if (!DedicatedServer) {
            std::unique_lock lock(HostMutex);
            HostEntries.clear();
            RefreshHostFlags();
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Worker

void Tick() noexcept {
    QueueHostScan();
    QueueClientTick();
}

void StartWorker() {
    {
        std::lock_guard lock(WorkerMutex);
        WorkerStopping = false;
    }
    Worker = new std::thread([] {
        std::unique_lock lock(WorkerMutex);
        while (!WorkerWake.wait_for(lock, std::chrono::milliseconds(TickMilliseconds), [] { return WorkerStopping; })) {
            lock.unlock();
            Tick();
            lock.lock();
        }
    });
}

void StopWorker() noexcept {
    {
        std::lock_guard lock(WorkerMutex);
        WorkerStopping = true;
    }
    WorkerWake.notify_all();
    if (Worker != nullptr && Worker->joinable()) Worker->join();
    delete Worker;
    Worker = nullptr;
}

// ---------------------------------------------------------------------------
// Console

const char* LinkName(HostLink link) noexcept {
    switch (link) {
    case HostLink::Connecting: return "connecting";
    case HostLink::Connected: return "connected";
    case HostLink::Refused: return "refused";
    default: return "none";
    }
}

void PrintStatus(const D2RL::PluginContext* context) noexcept {
    std::string name;
    {
        std::lock_guard lock(LocalMutex);
        name = LocalName;
    }
    std::size_t hostEntries{};
    {
        std::shared_lock lock(HostMutex);
        hostEntries = HostEntries.size();
    }
    char line[512]{};
    std::snprintf(line, sizeof(line),
        "Blood Golem aura: %s for %s; natives=%s host=%s dedicated=%s joined=%s playersHere=%zu scans=%llu suppressed=%llu restored=%llu manaReads=%llu buffListsSent=%llu buffListsReceived=%llu received=%llu",
        LocalOff.load() ? "OFF" : "on", name.empty() ? "(no character)" : name.c_str(),
        RelayInstalled.load() ? "ok" : "off", LinkName(Link.load()), DedicatedServer ? "yes" : "no",
        RemoteClient.load() ? "yes" : "no", hostEntries,
        static_cast<unsigned long long>(Scans.load()), static_cast<unsigned long long>(Suppressed.load()),
        static_cast<unsigned long long>(Restored.load()), static_cast<unsigned long long>(ManaReadsAnswered.load()),
        static_cast<unsigned long long>(BuffListsSent.load()), static_cast<unsigned long long>(BuffListsReceived.load()),
        static_cast<unsigned long long>(PreferencesReceived.load()));
    context->WriteConsoleMessage(line);
}

D2RL::ConsoleCommandResult __cdecl AuraCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept {
    if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;
    std::string_view args = command->args != nullptr ? std::string_view(command->args, command->argsLength) : std::string_view{};
    while (!args.empty() && (args.front() == ' ' || args.front() == '\t')) args.remove_prefix(1);
    while (!args.empty() && (args.back() == ' ' || args.back() == '\t' || args.back() == '\r' || args.back() == '\n')) args.remove_suffix(1);
    if (args.empty() || args == "status") {
        PrintStatus(command->plugin);
        return D2RL::ConsoleCommandResult::Handled;
    }
    if (args == "on" || args == "off") {
        if (LocalAuraOff() != (args == "off") && !Toggle()) {
            command->plugin->WriteConsoleMessage("Blood Golem aura: cannot change it yet (no character, or this game's host cannot take the setting).");
            return D2RL::ConsoleCommandResult::Handled;
        }
        PrintStatus(command->plugin);
        return D2RL::ConsoleCommandResult::Handled;
    }
    command->plugin->WriteConsoleMessage("Usage: buff-blood-aura [status|on|off]");
    return D2RL::ConsoleCommandResult::InvalidArguments;
}

[[nodiscard]] bool RegisterLifecycle() noexcept {
    const std::array kinds{
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

[[nodiscard]] bool RegisterChannel() noexcept {
    if (Context->QueryService(&Network) != D2RL::ServiceQueryResult::Success
        || !D2RL::HasNetworkServiceField(Network, D2RL::NetworkServiceRequiredSize)) {
        Network = nullptr;
        LogLine(true, "Buff HUD: the network service is unavailable; the Blood Golem aura cannot be switched off.");
        return false;
    }
    const D2RL::Network::ChannelRegistration registration{
        .structSize = D2RL::Network::ChannelRegistrationSize,
        .localChannelId = Policy::ChannelId,
        .compatibilityToken = Policy::CompatibilityToken,
        .hostMessage = &OnHostMessage,
        .clientMessage = &OnClientMessage,
        .connectionState = &OnConnectionState,
    };
    const auto result = Network->registerChannel(Context, &registration, &Channel);
    if (result != D2RL::Network::Result::Success) {
        Channel = D2RL::Network::InvalidChannelHandle;
        LogLine(true, "Buff HUD: the settings channel was rejected (result=%u); the Blood Golem aura cannot be switched off.",
            static_cast<unsigned>(result));
        return false;
    }
    return true;
}

} // namespace

bool LocalAuraOff() noexcept {
    return LocalOff.load(std::memory_order_acquire);
}

bool CanToggle() noexcept {
    return RelayInstalled.load(std::memory_order_acquire) && LocalLoaded.load(std::memory_order_acquire)
        && Link.load(std::memory_order_acquire) == HostLink::Connected;
}

bool LocalPlayerInAura() noexcept {
    if (GetStatListFromUnitAndState == nullptr || !InGame.load(std::memory_order_acquire)) return false;
    // What the server sees wins; the client's own copy of the state is lost
    // once the aura has been switched off and on.
    const auto now = NowMs();
    for (const auto seen : {HostInAuraAt.load(std::memory_order_relaxed), LocalServerInAuraAt.load(std::memory_order_relaxed)}) {
        if (seen != 0 && now - seen < AuraReportMilliseconds) return true;
    }
    try {
        auto* unit = const_cast<void*>(Identity.Unit(LocalPlayerId.load(std::memory_order_relaxed)));
        return unit != nullptr && GetStatListFromUnitAndState(unit, Contract::BloodManaState) != nullptr;
    } catch (...) {
        return false;
    }
}

bool Toggle() noexcept {
    if (!CanToggle()) return false;
    std::string name;
    {
        std::lock_guard lock(LocalMutex);
        name = LocalName;
    }
    const bool off = !LocalOff.load(std::memory_order_acquire);
    if (!SaveSidecar(name, off)) {
        LogLine(true, "Buff HUD: could not save %s's Blood Golem aura setting; it applies to this game only.", name.c_str());
    }
    LocalOff.store(off, std::memory_order_release);
    PreferenceSent.store(false, std::memory_order_release);
    SendPreference();
    LogLine(false, "Buff HUD: Blood Golem aura switched %s for %s.", off ? "off" : "on", name.c_str());
    return true;
}

void Initialize(const D2RL::PluginContext* context) noexcept {
    Shutdown();
    if (context == nullptr) return;
    try {
        Context = context;
        DedicatedServer = D2RLSecrets::IsDedicatedServer();
        if (context->QueryService(&Threads) != D2RL::ServiceQueryResult::Success
            || !D2RL::HasThreadServiceField(Threads, D2RL::ThreadServiceRequiredSize)
            || context->QueryService(&Lifecycle) != D2RL::ServiceQueryResult::Success
            || !D2RL::HasLifecycleServiceField(Lifecycle, D2RL::LifecycleServiceRequiredSize)) {
            LogLine(true, "Buff HUD: thread/lifecycle services unavailable; the Blood Golem aura cannot be switched off.");
            Threads = nullptr;
            Lifecycle = nullptr;
            return;
        }
        if (context->QueryService(&Communication) != D2RL::ServiceQueryResult::Success
            || !D2RL::HasPluginCommunicationServiceField(Communication, D2RL::PluginCommunicationServiceRequiredSize)
            || Communication->acquireService == nullptr) {
            Communication = nullptr;
        }
        // Natives first: the Blood Golem aura toggle requires the local relay,
        // while a joined client can still receive remote buffs over the network
        // channel even if the local relay patch is unavailable.
        if (ResolveNatives() && InstallRelay()) {
            RelayInstalled.store(true, std::memory_order_release);
        }
        if (!RegisterChannel()) return;
        if (!RegisterLifecycle()) {
            LogLine(true, "Buff HUD: could not listen for game start; the Blood Golem aura cannot be switched off.");
            return;
        }
        StateDirectory = ResolveStateDirectory();
        if (!DedicatedServer && !Identity.Resolve()) {
            LogLine(true, "Buff HUD: character lookup unavailable (%s); the Blood Golem aura cannot be switched off on this machine.",
                Identity.Reason().c_str());
        }
        if (!context->RegisterConsoleCommand("buff-blood-aura", &AuraCommand,
                "Show or set this character's Blood Golem aura (status|on|off).")) {
            context->LogWarn("Buff HUD: console command 'buff-blood-aura' could not be registered.");
        }
        StartWorker();
        LogLine(false, "Buff HUD: Blood Golem aura opt-out ready (%s).", DedicatedServer ? "dedicated server" : "player");
    } catch (...) {
        LogLine(true, "Buff HUD: Blood Golem aura opt-out failed to start.");
    }
}

void Shutdown() noexcept {
    StopWorker();
    if (Context != nullptr && Network != nullptr && Channel != D2RL::Network::InvalidChannelHandle) {
        (void)Network->unregisterChannel(Context, Channel);
    }
    if (Context != nullptr && Communication != nullptr && Communication->releaseService != nullptr
        && ServerThreadHandle != D2RL::PluginCommunication::InvalidServiceHandle) {
        (void)Communication->releaseService(Context, ServerThreadHandle);
    }
    {
        std::unique_lock lock(HostMutex);
        HostEntries.clear();
        RefreshHostFlags();
    }
    {
        std::lock_guard lock(OutboxMutex);
        Outbox.clear();
    }
    OutboxQueued.store(false);
    RemoteClient.store(false);
    SuppressionMaybeActive.store(false);
    ResetClientSession();
    InGame.store(false);
    ScanQueued.store(false);
    ClientTickQueued.store(false);
    // The loader removes the relay patch at unload; the function pointers stay
    // valid until then, so only the flag is cleared here.
    RelayInstalled.store(false);
    Channel = D2RL::Network::InvalidChannelHandle;
    GameplayListeners = {};
    ServerThreadService = nullptr;
    ServerThreadHandle = D2RL::PluginCommunication::InvalidServiceHandle;
    ServerThreadLastTry = 0;
    Network = nullptr;
    Communication = nullptr;
    Threads = nullptr;
    Lifecycle = nullptr;
    Context = nullptr;
}

} // namespace BuffPanel::Systems::BloodAura
