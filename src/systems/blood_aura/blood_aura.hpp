#pragma once

#include <D2RLPlugin/api.h>

#include <cstdint>

// Blood Golem aura opt-out.
//
// Reimagined's Blood Golem carries Blood Aura, which puts every player near the
// golem in states.txt blood_mana: their skills cost life instead of mana (an
// engine rule keyed on the state) and they get the aura's bonus stats. Some
// builds would rather not have it, so each character can switch it off from the
// Buff HUD. The choice is saved per character and sent to whoever runs the game
// (this process for a local game, a player's TCP/IP host, or d2rds), which then
// leaves that player out of both halves:
//
//   - the three engine reads of the state (server skill cast, server SpendMana,
//     client mana prediction) are pointed at BloodManaCheck, which answers "not
//     in Blood Mana" for an opted-out player;
//   - a scan on the game thread (or d2rds's server thread) suppresses the
//     state's stat list for those players with the engine's own
//     STATLIST_SuppressStateStats, the routine inactive set bonuses use.
//
// The state stays on the player, so the Buff HUD keeps showing the aura and its
// checkbox, and switching it back on restores both halves.
namespace BuffPanel::Systems::BloodAura {

inline constexpr std::uint32_t StateId = 0x72;   // states.txt blood_mana
inline constexpr std::int32_t IconSkillId = 85;  // skills.txt BloodGolem

// Never fails the plugin. Without its natives, the network or the character
// lookup, the aura simply cannot be switched off on this machine (logged).
void Initialize(const D2RL::PluginContext* context) noexcept;
void Shutdown() noexcept;

// Client side, any thread. True when this character has the aura switched off.
[[nodiscard]] bool LocalAuraOff() noexcept;
// True once this character's choice is loaded and the game's host has accepted
// the channel, so a change would take effect. Any thread.
[[nodiscard]] bool CanToggle() noexcept;
// UI thread. Flips the choice, saves it and sends it to the host. False when
// CanToggle is false.
bool Toggle() noexcept;

// UI thread. True while this machine's own player (the client copy) is in
// blood_mana, switched off or not - the state stays on either way.
[[nodiscard]] bool LocalPlayerInAura() noexcept;

} // namespace BuffPanel::Systems::BloodAura
