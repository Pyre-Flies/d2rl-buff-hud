#pragma once

#include "core/buff_display_bus.hpp"

#include <D2RLPlugin/api.h>

#include <array>
#include <cstddef>

// Mouse input over the HUD. BuffHud's widgets stay disabled so world clicks pass
// through them, which also means D2R gives them no hover or click. Instead the
// game window's message queue is watched (WH_GETMESSAGE, as identify-on-click
// does for its right clicks): a move over a shown slot reports which one is
// hovered, and a click on the Blood Golem aura's slot is consumed (the character
// does not walk there) and flips the aura. Nothing is drawn here.
namespace BuffPanel::Systems::BuffHud::HudInput {

// A shown slot, in HD layout units from the HUD panel's anchor (bottom centre).
struct SlotArea final {
    bool visible{};
    float x{};
    float y{};
    float size{};
};
using SlotAreas = std::array<SlotArea, Core::MaximumDisplayedBuffs>;

// UI thread: the slot under the cursor, or -1 when there is none.
using HoverCallback = void (*)(int slot) noexcept;

void Initialize(const D2RL::PluginContext* context, HoverCallback onHover) noexcept;
void Shutdown() noexcept;

// UI thread, once the game window exists (LocalPlayerReady).
void InstallInput() noexcept;

// UI thread. Where the slots are, and which one holds the switchable Blood Golem
// aura (-1 for none). Hidden slots take no hover and no clicks.
void Publish(const SlotAreas& slots, int toggleSlot) noexcept;

// One status line for buff-panel.
void Describe(char* output, std::size_t size) noexcept;

} // namespace BuffPanel::Systems::BuffHud::HudInput
