#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

// The Blood Golem aura opt-out's pure rules, kept free of game and SDK calls so
// tests/blood_aura_policy_test.cpp can check them offline.
namespace BuffPanel::Systems::BloodAura::Policy {

inline constexpr std::size_t NameBytes = 16; // PlayerData's name field
using Name = char[NameBytes + 1];

// A character name as D2R stores it: printable ASCII, NUL-terminated within 16
// bytes, not empty. Anything else matches nobody.
[[nodiscard]] inline bool CopyName(const char* source, Name& output) noexcept {
    std::memset(output, 0, sizeof(output));
    if (source == nullptr) return false;
    for (std::size_t i = 0; i < NameBytes; ++i) {
        const auto c = static_cast<unsigned char>(source[i]);
        if (c == 0) return i != 0;
        if (c < 0x20 || c >= 0x7F) return false;
        output[i] = static_cast<char>(c);
    }
    return false;
}

// Case-insensitive: D2R refuses two characters whose names differ only in case.
[[nodiscard]] inline bool SameName(const char* left, const char* right) noexcept {
    for (std::size_t i = 0; i < NameBytes; ++i) {
        auto l = static_cast<unsigned char>(left[i]);
        auto r = static_cast<unsigned char>(right[i]);
        if (l >= 'A' && l <= 'Z') l = static_cast<unsigned char>(l - 'A' + 'a');
        if (r >= 'A' && r <= 'Z') r = static_cast<unsigned char>(r - 'A' + 'a');
        if (l != r) return false;
        if (l == 0) return true;
    }
    return true;
}

// The wire between a player's Buff HUD and the copy running the game (channel 1):
//   client -> host, message 1: Preference, this character's settings;
//   host -> client, message 2: the player's buffs (core/remote_buffs.hpp), only
//   to a client that set WantsBuffsFlag because its own copy has no game clock.
// Change the last byte of the token whenever either message changes, so
// mismatched copies refuse to connect instead of misreading.
inline constexpr std::uint16_t ChannelId = 1;
inline constexpr std::uint64_t CompatibilityToken = 0x4255464648554402ULL; // "BUFFHUD" + 2
inline constexpr std::uint16_t PreferenceMessage = 1;
inline constexpr std::uint8_t PreferenceVersion = 1;
inline constexpr std::uint16_t WantsBuffsFlag = 0x0001;

struct Preference final {
    std::uint8_t version;
    std::uint8_t auraOff;
    std::uint16_t flags;
    std::uint32_t playerId;
    char name[NameBytes];
};
static_assert(sizeof(Preference) == 24);

[[nodiscard]] inline bool DecodePreference(const void* data, std::uint32_t size, Name& name, bool& auraOff,
    bool& wantsBuffs) noexcept {
    if (data == nullptr || size != sizeof(Preference)) return false;
    Preference preference{};
    std::memcpy(&preference, data, sizeof(preference));
    if (preference.version != PreferenceVersion || preference.auraOff > 1
        || (preference.flags & ~WantsBuffsFlag) != 0) {
        return false;
    }
    if (!CopyName(preference.name, name)) return false;
    auraOff = preference.auraOff != 0;
    wantsBuffs = (preference.flags & WantsBuffsFlag) != 0;
    return true;
}

// The per-character settings file: "key<TAB>value" lines, '#' comments. A
// missing file or key is the default, on.
inline constexpr std::string_view SidecarKey = "blood_golem_aura";

[[nodiscard]] inline bool ParseSidecarAuraOff(std::string_view text) noexcept {
    bool off = false;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto tab = line.find('\t');
        if (tab == std::string_view::npos || line.substr(0, tab) != SidecarKey) continue;
        off = line.substr(tab + 1) == "off";
    }
    return off;
}

[[nodiscard]] inline std::string FormatSidecar(bool auraOff) {
    std::string text = "# Buff HUD settings for this character.\n";
    text += SidecarKey;
    text += '\t';
    text += auraOff ? "off" : "on";
    text += '\n';
    return text;
}

// The checkbox: HD layouts are authored for a 2160-unit-tall screen; a panel's
// anchor is a fraction of the screen and its children are offset from it in
// those units. BuffHudhd.json anchors at the bottom centre.
inline constexpr float LayoutHeight = 2160.0f;
inline constexpr float AnchorX = 0.5f;
inline constexpr float AnchorY = 1.0f;

struct ScreenBox final {
    float left{};
    float top{};
    float right{};
    float bottom{};
};

[[nodiscard]] inline bool LayoutToScreen(float x, float y, float size, float width, float height, ScreenBox& box) noexcept {
    if (width <= 0.0f || height <= 0.0f || size <= 0.0f) return false;
    const float scale = height / LayoutHeight;
    box.left = width * AnchorX + x * scale;
    box.top = height * AnchorY + y * scale;
    box.right = box.left + size * scale;
    box.bottom = box.top + size * scale;
    return true;
}

[[nodiscard]] inline bool Contains(const ScreenBox& box, float x, float y) noexcept {
    return x >= box.left && x < box.right && y >= box.top && y < box.bottom;
}

} // namespace BuffPanel::Systems::BloodAura::Policy
