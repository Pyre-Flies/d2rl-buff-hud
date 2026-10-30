#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Buffs for a player who joined someone else's game (a dedicated server or a
// player's TCP/IP host). That player's client copy of their own unit has no game
// clock and no expiry metadata, so the copy running the game reads the server
// unit and sends what it sees; the client counts timers down on its own clock.
namespace BuffPanel::Core {

// One buff. Timers carry the frames left in `value`; resource pools (Bone
// Armor) their current and maximum after the catalog's value shift.
struct RemoteBuff final {
    std::uint16_t state{};
    std::int16_t skill{};
    std::int32_t value{};
    std::int32_t maximum{};
};
static_assert(sizeof(RemoteBuff) == 12);

inline constexpr std::uint16_t RemoteBuffsMessage = 2;
inline constexpr std::uint8_t RemoteBuffsVersion = 1;
inline constexpr std::size_t RemoteBuffsHeaderBytes = 4; // version, count, flags, reserved
// The player is in the Blood Golem aura on the server, switched off or not. The
// client cannot tell by itself: once its aura has been switched off and back on,
// its copy of the player no longer carries the state (seen in game 2026-10-28).
inline constexpr std::uint8_t RemoteBuffsInBloodAura = 0x01;
inline constexpr std::size_t RemoteBuffsMaximumBytes = 224; // NetworkService MaxPayloadSize
inline constexpr std::size_t MaximumRemoteBuffs =
    (RemoteBuffsMaximumBytes - RemoteBuffsHeaderBytes) / sizeof(RemoteBuff);

struct RemoteBuffList final {
    std::array<RemoteBuff, MaximumRemoteBuffs> buffs{};
    std::size_t count{};
    std::uint8_t flags{};
};

// Returns the payload size. Buffs past MaximumRemoteBuffs are left out.
[[nodiscard]] inline std::size_t EncodeRemoteBuffs(const RemoteBuffList& list,
    std::array<std::uint8_t, RemoteBuffsMaximumBytes>& output) noexcept {
    const auto count = list.count < MaximumRemoteBuffs ? list.count : MaximumRemoteBuffs;
    output = {};
    output[0] = RemoteBuffsVersion;
    output[1] = static_cast<std::uint8_t>(count);
    output[2] = list.flags;
    std::memcpy(output.data() + RemoteBuffsHeaderBytes, list.buffs.data(), count * sizeof(RemoteBuff));
    return RemoteBuffsHeaderBytes + count * sizeof(RemoteBuff);
}

[[nodiscard]] inline bool DecodeRemoteBuffs(const void* data, std::size_t size, RemoteBuffList& list) noexcept {
    list = {};
    if (data == nullptr || size < RemoteBuffsHeaderBytes || size > RemoteBuffsMaximumBytes) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const std::size_t count = bytes[1];
    if (bytes[0] != RemoteBuffsVersion || count > MaximumRemoteBuffs
        || size != RemoteBuffsHeaderBytes + count * sizeof(RemoteBuff)) {
        return false;
    }
    std::memcpy(list.buffs.data(), bytes + RemoteBuffsHeaderBytes, count * sizeof(RemoteBuff));
    list.count = count;
    list.flags = bytes[2];
    return true;
}

} // namespace BuffPanel::Core
