#include "buff_hud.hpp"
#include "tooltip_text.hpp"

#include "core/buff_display_bus.hpp"
#include "hud_input.hpp"
#include "core/services.hpp"
#include "icon_frame_backend.hpp"
#include "native/native_contract.hpp"
#include "skill_icon_resolver.hpp"
#include "systems/blood_aura/blood_aura.hpp"
#include "systems/buff_tracker/buff_tracker.hpp"

#include <Windows.h>

#include <D2RLPlugin/localization.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>


namespace BuffPanel::Systems::BuffHud {
namespace {

using Internal::IconFrameBackendState;
using Internal::SkillIconDescriptor;

constexpr std::size_t SlotCount = Core::MaximumDisplayedBuffs;
constexpr std::size_t AtlasCount = 9;
constexpr std::uint32_t FramesPerSecond = 25;
constexpr char TimerReserve[] = "__BUFF_PANEL_TIMER_RESERVE_00__";
constexpr std::size_t TimerReserveBytes = sizeof(TimerReserve);
using Internal::TooltipReserveBytes;
using Internal::TooltipReserveLength;
constexpr std::size_t TooltipReserveCodepoints = TooltipReserveLength / 3;
static_assert(TooltipReserveCodepoints * 3 == TooltipReserveLength);
constexpr std::size_t TooltipNativeScanBytes = 0x1000;
constexpr std::uint32_t NoRenderedSeconds = std::numeric_limits<std::uint32_t>::max();
constexpr std::int32_t NoRenderedValue = std::numeric_limits<std::int32_t>::min();

// The Blood Golem aura is not in the buff catalog (no duration, no class). Its
// entry comes from BloodAura, always takes slot 0 (top priority) and shows
// BuffHudhd.json's AuraCheck there: D2R's own checkbox sprite, frame 4 ticked
// and 0 empty, set through the qualified ButtonWidget frame path. Clicking the
// slot flips it (HudInput). Grid and slot fallbacks are the JSON's own rects,
// used if a rect cannot be read.
constexpr std::uint64_t BloodAuraKey = 0xB100'DA0B'A000'0072ULL;
constexpr std::int16_t BloodAuraPriority = 1000;
constexpr std::uint16_t CheckedFrame = 4;
constexpr std::uint16_t UncheckedFrame = 0;
constexpr D2RL::Widgets::Rect GridFallbackRect{-150, -665, 780, 300};
constexpr std::int32_t SlotFallbackPitchX = 112;
constexpr std::int32_t SlotFallbackPitchY = 105;
constexpr std::int32_t SlotFallbackBottomY = 210;
constexpr std::int32_t SlotFallbackEdge = 96;
constexpr std::size_t SlotsPerRow = 7;

// BuffHudhd.json HoverBox<row><size>: D2R's tooltip frame (TooltipBorder, 80%
// black) naming the hovered buff. A layout box cannot size itself to its text or
// move, so there are three widths (S/M/L) at three heights, each just above one
// row of slots; the one used fits the text and sits over the top occupied row.
// Each box's Text starts as this reserve and is overwritten in place, the same
// way the countdowns are.
constexpr char HoverReserve[] = "__BUFF_PANEL_HOVER_LABEL_RESERVE_______________________________";
constexpr std::size_t HoverReserveBytes = sizeof(HoverReserve);
static_assert(HoverReserveBytes == 64);
constexpr std::size_t HoverRows = 3;
constexpr std::array<char, 3> HoverSizes{'S', 'M', 'L'};
constexpr std::array<std::size_t, 2> HoverSizeLimits{12, 22}; // characters that fit S, M
constexpr std::size_t HoverBoxCount = HoverRows * HoverSizes.size();

enum class TooltipStorageEncoding : std::uint8_t {
    Unknown = 0,
    BlizzardUtf8 = 1,
    BlizzardUtf16 = 2,
};
static_assert(TimerReserveBytes == 32);

constexpr std::array<Core::BuffIconAtlas, AtlasCount> AtlasOrder{
    Core::BuffIconAtlas::Amazon,
    Core::BuffIconAtlas::Sorceress,
    Core::BuffIconAtlas::Necromancer,
    Core::BuffIconAtlas::Paladin,
    Core::BuffIconAtlas::Barbarian,
    Core::BuffIconAtlas::Druid,
    Core::BuffIconAtlas::Assassin,
    Core::BuffIconAtlas::Warlock,
    Core::BuffIconAtlas::Global,
};

constexpr std::array<const char*, AtlasCount> AtlasWidgetNames{
    "IconAmazon",
    "IconSorceress",
    "IconNecromancer",
    "IconPaladin",
    "IconBarbarian",
    "IconDruid",
    "IconAssassin",
    "IconWarlock",
    "IconGlobal",
};

constexpr std::array<const char*, SlotCount> SlotWidgetNames{
    "BuffSlot00",
    "BuffSlot01",
    "BuffSlot02",
    "BuffSlot03",
    "BuffSlot04",
    "BuffSlot05",
    "BuffSlot06",
    "BuffSlot07",
    "BuffSlot08",
    "BuffSlot09",
    "BuffSlot10",
    "BuffSlot11",
    "BuffSlot12",
    "BuffSlot13",
    "BuffSlot14",
    "BuffSlot15",
    "BuffSlot16",
    "BuffSlot17",
    "BuffSlot18",
    "BuffSlot19",
    "BuffSlot20",
};

using FindTopLevelPanelFn = void*(__fastcall*)(const char* name) noexcept;
using FindChildWidgetByNameFn = void*(__fastcall*)(void* parent, const char* name) noexcept;

const D2RL::PluginContext* Context{};
const D2RL::PanelService* Panels{};
const D2RL::WidgetService* Widgets{};
const D2RL::ThreadService* Threads{};
const D2RL::LifecycleService* Lifecycle{};
D2RL::Panels::RegistrationHandle RegisteredPanel{D2RL::Panels::InvalidHandle};
D2RL::Lifecycle::ListenerHandle DataTablesListener{D2RL::Lifecycle::InvalidHandle};
std::array<D2RL::Lifecycle::ListenerHandle, 3> GameplayListeners{};

FindTopLevelPanelFn FindTopLevelPanel{};
FindChildWidgetByNameFn FindChildWidgetByName{};

std::atomic<std::uint64_t> CurrentSessionGeneration{};
std::atomic<bool> PollScheduled{};
struct SlotHandles final {
    D2RL::Widgets::WidgetHandle slot{D2RL::Widgets::InvalidHandle};
    std::array<D2RL::Widgets::WidgetHandle, AtlasCount> icons{};
    D2RL::Widgets::WidgetHandle countdown{D2RL::Widgets::InvalidHandle};
    // BuffHudhd.json no longer has per-slot Tooltip widgets (hover goes through
    // HudInput and HoverLabel); a layout override that still has them gets their
    // native hover text as before.
    D2RL::Widgets::WidgetHandle tooltip{D2RL::Widgets::InvalidHandle};
};

struct SlotRenderState final {
    bool visible{};
    std::uint64_t key{};
    Core::BuffIconAtlas atlas{Core::BuffIconAtlas::Global};
    std::uint16_t requestedFrame{};
    std::uint16_t renderedFrame{};
    Core::BuffDisplayMode displayMode{Core::BuffDisplayMode::Timer};
    std::uint32_t seconds{NoRenderedSeconds};
    std::int32_t currentValue{NoRenderedValue};
    std::int32_t maximumValue{NoRenderedValue};
    std::int32_t sourceSkillId{Core::NoSourceSkillId};
    Core::BuffToggle toggle{Core::BuffToggle::None};
    bool timerVisible{};
    bool tooltipVisible{};
    std::uintptr_t qualifiedTimerBuffer{};
    std::uintptr_t qualifiedTooltipWidget{};
    std::uintptr_t qualifiedTooltipBuffer{};
    std::size_t qualifiedTooltipFieldOffset{std::numeric_limits<std::size_t>::max()};
    TooltipStorageEncoding qualifiedTooltipEncoding{TooltipStorageEncoding::Unknown};
};

D2RL::Widgets::WidgetHandle HudPanel{D2RL::Widgets::InvalidHandle};
D2RL::Widgets::WidgetHandle GridWidget{D2RL::Widgets::InvalidHandle};
// Optional (absent from older layout overrides): slot 0's checkbox and the
// hover boxes. Every SDK lookup holds a widget handle; per-slot additions broke
// handle resolution in game (2026-10-28), so these stay few.
D2RL::Widgets::WidgetHandle AuraCheck{D2RL::Widgets::InvalidHandle};
std::array<D2RL::Widgets::WidgetHandle, HoverBoxCount> HoverBoxes{};
std::array<std::uintptr_t, HoverBoxCount> HoverBuffers{};
bool AuraCheckShown{};
std::uint16_t AuraCheckFrame{std::numeric_limits<std::uint16_t>::max()};
int HoverBoxShown{-1};
int HoverSlotShown{-1};
std::array<Core::BuffDisplayEntry, SlotCount> SlotEntries{};
std::size_t SlotEntryCount{};
std::array<SlotHandles, SlotCount> Handles{};
std::array<SlotRenderState, SlotCount> RenderStates{};
bool HandlesResolved{};
void* NativeHudPanel{};
bool EmptySlotsHidden{};
std::array<void*, SlotCount> NativeCountdownWidgets{};
std::array<std::uintptr_t, SlotCount> NativeCountdownBuffers{};
std::uint64_t LastRenderedRevision{std::numeric_limits<std::uint64_t>::max()};
std::uint32_t LastRenderedGameFrame{NoRenderedSeconds};
bool LastRenderedHasClock{};

[[nodiscard]] std::size_t AtlasIndex(Core::BuffIconAtlas atlas) noexcept {
    for (std::size_t i = 0; i < AtlasOrder.size(); ++i) {
        if (AtlasOrder[i] == atlas) return i;
    }
    return AtlasOrder.size() - 1;
}

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

[[nodiscard]] bool IsWritableRange(void* address, std::size_t size) noexcept {
    if (address == nullptr || size == 0) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto end = start + size;
    if (start < 0x10000u || end < start) return false;
    __try {
        auto* bytes = static_cast<volatile std::uint8_t*>(address);
        for (std::size_t offset = 0; offset < size; offset += 4096) {
            const std::uint8_t value = bytes[offset];
            bytes[offset] = value;
        }
        const std::uint8_t last = bytes[size - 1];
        bytes[size - 1] = last;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
[[nodiscard]] bool ReadNativeField(const void* base, std::size_t offset, T& value) noexcept {
    if (base == nullptr) return false;
    const auto baseValue = reinterpret_cast<std::uintptr_t>(base);
    if (baseValue < 0x10000u || offset > static_cast<std::size_t>(UINTPTR_MAX - baseValue)) return false;
    const auto* address = reinterpret_cast<const void*>(baseValue + offset);
    __try {
        std::memcpy(&value, address, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

[[nodiscard]] bool IsTimerReserve(const char* buffer) noexcept {
    return buffer != nullptr
        && IsReadableRange(buffer, TimerReserveBytes)
        && std::memcmp(buffer, TimerReserve, TimerReserveBytes) == 0;
}

void MakeTooltipReserve(std::size_t slotIndex, char (&output)[TooltipReserveBytes]) noexcept {
    // Use a per-slot *invisible* UTF-8 qualification token instead of a long
    // run of ASCII spaces. The old reserve was visually blank but D2R still
    // measured all 127 spaces when the native tooltip field could not be
    // uniquely qualified, producing a huge empty tooltip. U+200B ZERO WIDTH
    // SPACE and U+200C ZERO WIDTH NON-JOINER give us 42 zero-width codepoints
    // (126 UTF-8 bytes) while the low five codepoints encode the slot id so the
    // backing std::string is unique for every one of the 21 reusable slots.
    constexpr std::array<unsigned char, 3> ZeroWidthSpace{0xE2, 0x80, 0x8B};
    constexpr std::array<unsigned char, 3> ZeroWidthNonJoiner{0xE2, 0x80, 0x8C};
    std::memset(output, 0, TooltipReserveBytes);
    for (std::size_t i = 0; i < TooltipReserveCodepoints; ++i) {
        const bool idBit = i < 5 && ((slotIndex >> i) & 1U) != 0;
        const auto& codepoint = idBit ? ZeroWidthNonJoiner : ZeroWidthSpace;
        std::memcpy(output + i * 3, codepoint.data(), codepoint.size());
    }
    output[TooltipReserveLength] = '\0';
}

void MakeTooltipReserveUtf16(std::size_t slotIndex, std::array<std::uint16_t, TooltipReserveCodepoints + 1>& output) noexcept {
    output.fill(0);
    for (std::size_t i = 0; i < TooltipReserveCodepoints; ++i) {
        const bool idBit = i < 5 && ((slotIndex >> i) & 1U) != 0;
        output[i] = static_cast<std::uint16_t>(idBit ? 0x200CU : 0x200BU);
    }
}

[[nodiscard]] bool Utf8ToUtf16(
    std::string_view input,
    std::array<std::uint16_t, TooltipReserveCodepoints + 1>& output,
    std::size_t& outputLength) noexcept {
    output.fill(0);
    outputLength = 0;
    std::size_t i{};
    while (i < input.size()) {
        const auto b0 = static_cast<unsigned char>(input[i]);
        std::uint32_t cp{};
        std::size_t count{};
        if (b0 < 0x80U) {
            cp = b0; count = 1;
        } else if ((b0 & 0xE0U) == 0xC0U && i + 1 < input.size()) {
            cp = static_cast<std::uint32_t>(b0 & 0x1FU) << 6;
            cp |= static_cast<unsigned char>(input[i + 1]) & 0x3FU;
            count = 2;
            if (cp < 0x80U) return false;
        } else if ((b0 & 0xF0U) == 0xE0U && i + 2 < input.size()) {
            cp = static_cast<std::uint32_t>(b0 & 0x0FU) << 12;
            cp |= static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 1]) & 0x3FU) << 6;
            cp |= static_cast<unsigned char>(input[i + 2]) & 0x3FU;
            count = 3;
            if (cp < 0x800U || (cp >= 0xD800U && cp <= 0xDFFFU)) return false;
        } else if ((b0 & 0xF8U) == 0xF0U && i + 3 < input.size()) {
            cp = static_cast<std::uint32_t>(b0 & 0x07U) << 18;
            cp |= static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 1]) & 0x3FU) << 12;
            cp |= static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 2]) & 0x3FU) << 6;
            cp |= static_cast<unsigned char>(input[i + 3]) & 0x3FU;
            count = 4;
            if (cp < 0x10000U || cp > 0x10FFFFU) return false;
        } else {
            return false;
        }
        for (std::size_t j = 1; j < count; ++j) {
            if ((static_cast<unsigned char>(input[i + j]) & 0xC0U) != 0x80U) return false;
        }
        if (cp <= 0xFFFFU) {
            if (outputLength >= TooltipReserveCodepoints) return false;
            output[outputLength++] = static_cast<std::uint16_t>(cp);
        } else {
            if (outputLength + 2 > TooltipReserveCodepoints) return false;
            cp -= 0x10000U;
            output[outputLength++] = static_cast<std::uint16_t>(0xD800U + (cp >> 10));
            output[outputLength++] = static_cast<std::uint16_t>(0xDC00U + (cp & 0x3FFU));
        }
        i += count;
    }
    return true;
}

[[nodiscard]] bool SetVisible(D2RL::Widgets::WidgetHandle handle, bool visible) noexcept;
[[nodiscard]] bool SetEnabled(D2RL::Widgets::WidgetHandle handle, bool enabled) noexcept;
[[nodiscard]] bool ApplyInputIsolation() noexcept;

// A failed lookup leaves every slot showing its layout placeholders, so say
// which one failed - once, until a lookup succeeds again.
bool ResolveFailureLogged{};

[[nodiscard]] bool FindHandle(D2RL::Widgets::WidgetHandle parent, const char* name, std::size_t slot,
    D2RL::Widgets::WidgetHandle& handle) noexcept {
    const auto result = Widgets->findWidget(Context, parent, name, &handle);
    if (result == D2RL::Widgets::Result::Success) return true;
    if (!ResolveFailureLogged) {
        ResolveFailureLogged = true;
        char line[192]{};
        std::snprintf(line, sizeof(line), "Buff HUD: widget lookup '%s' (slot %zu) failed with result %u; the HUD cannot draw.",
            name, slot, static_cast<unsigned>(result));
        Context->LogWarn(line);
    }
    return false;
}

[[nodiscard]] bool ResolveWidgetHandles() noexcept {
    if (HandlesResolved) return true;
    if (Context == nullptr || Widgets == nullptr) return false;

    HudPanel = D2RL::Widgets::InvalidHandle;
    GridWidget = D2RL::Widgets::InvalidHandle;
    Handles = {};

    if (Widgets->findPanel(Context, "buffhud/BuffHud", &HudPanel) != D2RL::Widgets::Result::Success
        || !FindHandle(HudPanel, "BuffGrid", 0, GridWidget)) {
        return false;
    }

    for (std::size_t i = 0; i < Handles.size(); ++i) {
        char slotName[24]{};
        std::snprintf(slotName, sizeof(slotName), "BuffSlot%02zu", i);
        auto& slot = Handles[i];
        if (!FindHandle(GridWidget, slotName, i, slot.slot)) {
            Handles = {};
            return false;
        }
        for (std::size_t atlas = 0; atlas < AtlasCount; ++atlas) {
            if (!FindHandle(slot.slot, AtlasWidgetNames[atlas], i, slot.icons[atlas])) {
                Handles = {};
                return false;
            }
        }
        if (!FindHandle(slot.slot, "Countdown", i, slot.countdown)) {
            Handles = {};
            return false;
        }
        if (Widgets->findWidget(Context, slot.slot, "Tooltip", &slot.tooltip) != D2RL::Widgets::Result::Success) {
            slot.tooltip = D2RL::Widgets::InvalidHandle;
        }

        // ButtonWidget visibility alone does not guarantee that D2R removes its
        // hit target. Empty BuffHud slots are display placeholders, so disable
        // every atlas button as soon as handles are resolved. Occupied slots
        // keep the selected atlas visible but disabled. A separate slot-local
        // FocusableWidget owns optional hover-only tooltip presentation.
        // In optional gameplay mode ApplyInputIsolation also disables the tooltip,
        // all slots, the grid and the panel to prioritize world input.
        for (const auto icon : slot.icons) {
            if (!SetEnabled(icon, false)) {
            }
        }
    }
    if (Widgets->findWidget(Context, Handles[0].slot, "AuraCheck", &AuraCheck) != D2RL::Widgets::Result::Success) {
        AuraCheck = D2RL::Widgets::InvalidHandle;
    }
    for (std::size_t i = 0; i < HoverBoxCount; ++i) {
        char name[16]{};
        std::snprintf(name, sizeof(name), "HoverBox%zu%c", i / HoverSizes.size(), HoverSizes[i % HoverSizes.size()]);
        if (Widgets->findWidget(Context, GridWidget, name, &HoverBoxes[i]) != D2RL::Widgets::Result::Success) {
            HoverBoxes[i] = D2RL::Widgets::InvalidHandle;
        }
    }

    HandlesResolved = true;
    ResolveFailureLogged = false;
    (void)ApplyInputIsolation();
    return true;
}

void RestoreQualifiedCountdownBuffers() noexcept {
    for (auto& state : RenderStates) {
        if (state.qualifiedTimerBuffer == 0) continue;
        auto* buffer = reinterpret_cast<char*>(state.qualifiedTimerBuffer);
        if (IsWritableRange(buffer, TimerReserveBytes)) {
            std::memcpy(buffer, TimerReserve, TimerReserveBytes);
        }
    }
}

void RestoreQualifiedTooltipBuffer(std::size_t slotIndex, SlotRenderState& state) noexcept {
    if (slotIndex >= SlotCount
        || state.qualifiedTooltipWidget == 0
        || state.qualifiedTooltipBuffer == 0
        || state.qualifiedTooltipFieldOffset == std::numeric_limits<std::size_t>::max()
        || state.qualifiedTooltipEncoding == TooltipStorageEncoding::Unknown) {
        return;
    }

    const auto stringObjectAddress = state.qualifiedTooltipWidget + state.qualifiedTooltipFieldOffset;
    if (!IsWritableRange(
            reinterpret_cast<void*>(stringObjectAddress + Native::Contract::BlizzardStringSizeOffset),
            sizeof(std::uint64_t))) {
        return;
    }

    std::uint64_t length{};
    if (state.qualifiedTooltipEncoding == TooltipStorageEncoding::BlizzardUtf8) {
        if (!IsWritableRange(reinterpret_cast<void*>(state.qualifiedTooltipBuffer), TooltipReserveBytes)) return;
        char reserve[TooltipReserveBytes]{};
        MakeTooltipReserve(slotIndex, reserve);
        std::memcpy(reinterpret_cast<void*>(state.qualifiedTooltipBuffer), reserve, TooltipReserveBytes);
        length = TooltipReserveLength;
    } else if (state.qualifiedTooltipEncoding == TooltipStorageEncoding::BlizzardUtf16) {
        constexpr std::size_t Utf16Bytes = (TooltipReserveCodepoints + 1) * sizeof(std::uint16_t);
        if (!IsWritableRange(reinterpret_cast<void*>(state.qualifiedTooltipBuffer), Utf16Bytes)) return;
        std::array<std::uint16_t, TooltipReserveCodepoints + 1> reserve{};
        MakeTooltipReserveUtf16(slotIndex, reserve);
        std::memcpy(reinterpret_cast<void*>(state.qualifiedTooltipBuffer), reserve.data(), Utf16Bytes);
        length = TooltipReserveCodepoints;
    } else {
        return;
    }

    std::memcpy(
        reinterpret_cast<void*>(stringObjectAddress + Native::Contract::BlizzardStringSizeOffset),
        &length,
        sizeof(length));
}

void RestoreQualifiedTooltipBuffers() noexcept {
    for (std::size_t slotIndex = 0; slotIndex < RenderStates.size(); ++slotIndex) {
        RestoreQualifiedTooltipBuffer(slotIndex, RenderStates[slotIndex]);
    }
}

void InvalidateWidgetHandles() noexcept {
    HudPanel = D2RL::Widgets::InvalidHandle;
    GridWidget = D2RL::Widgets::InvalidHandle;
    AuraCheck = D2RL::Widgets::InvalidHandle;
    HoverBoxes = {};
    HoverBuffers = {};
    AuraCheckShown = false;
    AuraCheckFrame = std::numeric_limits<std::uint16_t>::max();
    HoverBoxShown = -1;
    HoverSlotShown = -1;
    SlotEntries = {};
    SlotEntryCount = 0;
    Handles = {};
    RenderStates = {};
    HandlesResolved = false;
    NativeHudPanel = nullptr;
    EmptySlotsHidden = false;
    NativeCountdownWidgets = {};
    NativeCountdownBuffers = {};
    LastRenderedRevision = std::numeric_limits<std::uint64_t>::max();
    LastRenderedGameFrame = NoRenderedSeconds;
    LastRenderedHasClock = false;
}

[[nodiscard]] bool SetVisible(D2RL::Widgets::WidgetHandle handle, bool visible) noexcept {
    if (Context == nullptr || Widgets == nullptr || handle == D2RL::Widgets::InvalidHandle) return false;
    return Widgets->setWidgetVisible(Context, handle, visible) == D2RL::Widgets::Result::Success;
}

[[nodiscard]] bool SetEnabled(D2RL::Widgets::WidgetHandle handle, bool enabled) noexcept {
    if (Context == nullptr || Widgets == nullptr || handle == D2RL::Widgets::InvalidHandle) return false;
    return Widgets->setWidgetEnabled(Context, handle, enabled) == D2RL::Widgets::Result::Success;
}

// Call only on the UI thread, after ResolveWidgetHandles. A disabled widget
// can still be drawn in D2R (this is already how the atlas ButtonWidgets are
// rendered), but it cannot be relied on for native hover. Do not pretend that
// this proves end-to-end click-through: the test must also cover Panel hit-test
// behavior in the running game.
[[nodiscard]] bool ApplyInputIsolation() noexcept {
    if (!HandlesResolved || Context == nullptr || Widgets == nullptr) return false;
    bool allSucceeded = true;
    auto disable = [&](D2RL::Widgets::WidgetHandle handle) noexcept {
        if (handle == D2RL::Widgets::InvalidHandle) return; // optional widget absent
        if (!SetEnabled(handle, false)) allSucceeded = false;
    };
    for (auto& slot : Handles) {
        disable(slot.slot);
        disable(slot.tooltip);
        for (const auto icon : slot.icons) disable(icon);
    }
    disable(AuraCheck);
    for (const auto box : HoverBoxes) disable(box);
    disable(GridWidget);
    disable(HudPanel);
    if (!allSucceeded) Context->LogWarn(
        "Buff HUD: one or more widget enabled-state updates failed; gameplay input isolation may be incomplete.");
    return allSucceeded;
}

[[nodiscard]] void* ResolveNativeSlotFromGrid(void* grid, std::size_t slotIndex) noexcept {
    if (grid == nullptr || FindChildWidgetByName == nullptr || slotIndex >= SlotCount) {
        return nullptr;
    }
    return FindChildWidgetByName(grid, SlotWidgetNames[slotIndex]);
}

[[nodiscard]] void* ResolveNativeSlotChild(std::size_t slotIndex, const char* childName) noexcept {
    if (FindTopLevelPanel == nullptr || FindChildWidgetByName == nullptr
        || slotIndex >= SlotCount || childName == nullptr) {
        return nullptr;
    }
    void* panel = FindTopLevelPanel("buffhud/BuffHud");
    void* grid = panel != nullptr ? FindChildWidgetByName(panel, "BuffGrid") : nullptr;
    void* slot = ResolveNativeSlotFromGrid(grid, slotIndex);
    return slot != nullptr ? FindChildWidgetByName(slot, childName) : nullptr;
}

// SDK handles are paths and survive UI reconstruction; rendered state and
// qualified native buffers do not. Inspect only the current tree on the UI thread.
// Never restore/write a buffer retained from a tree that has been replaced.
bool ReconcileNativeWidgets() noexcept {
    void* panel = FindTopLevelPanel != nullptr ? FindTopLevelPanel("buffhud/BuffHud") : nullptr;
    void* grid = (panel != nullptr && FindChildWidgetByName != nullptr)
        ? FindChildWidgetByName(panel, "BuffGrid")
        : nullptr;
    void* slot0 = ResolveNativeSlotFromGrid(grid, 0);
    void* widget0 = (slot0 != nullptr && FindChildWidgetByName != nullptr)
        ? FindChildWidgetByName(slot0, "Countdown")
        : nullptr;
    std::uintptr_t buffer0{};
    if (widget0 != nullptr) {
        (void)ReadNativeField(widget0, Native::Contract::HudTextPointerOffset, buffer0);
    }

    const bool canaryReplaced = panel != NativeHudPanel
        || widget0 != NativeCountdownWidgets[0]
        || buffer0 != NativeCountdownBuffers[0];
    bool canaryReset = RenderStates[0].timerVisible && buffer0 != 0
        && IsTimerReserve(reinterpret_cast<const char*>(buffer0));
    if (!canaryReplaced && !canaryReset && !RenderStates[0].timerVisible) {
        for (std::size_t i = 1; i < SlotCount; ++i) {
            if (!RenderStates[i].timerVisible) continue;
            const auto buffer = NativeCountdownBuffers[i];
            if (buffer != 0 && IsTimerReserve(reinterpret_cast<const char*>(buffer))) {
                canaryReset = true;
            }
            break;
        }
    }

    if (!canaryReplaced && !canaryReset && EmptySlotsHidden) {
        return false;
    }

    bool changed = false;
    for (std::size_t i = 0; i < SlotCount; ++i) {
        void* slot = i == 0 ? slot0 : ResolveNativeSlotFromGrid(grid, i);
        void* widget = i == 0
            ? widget0
            : ((slot != nullptr && FindChildWidgetByName != nullptr)
                ? FindChildWidgetByName(slot, "Countdown")
                : nullptr);
        std::uintptr_t buffer = i == 0 ? buffer0 : 0;
        if (i != 0 && widget != nullptr) {
            (void)ReadNativeField(widget, Native::Contract::HudTextPointerOffset, buffer);
        }
        auto& state = RenderStates[i];
        const bool replaced = widget != NativeCountdownWidgets[i]
            || buffer != NativeCountdownBuffers[i];
        const bool reset = state.timerVisible && buffer != 0
            && IsTimerReserve(reinterpret_cast<const char*>(buffer));
        if (replaced || reset) {
            const auto old = state;
            state = {};
            void* tooltip = (slot != nullptr && FindChildWidgetByName != nullptr)
                ? FindChildWidgetByName(slot, "Tooltip")
                : nullptr;
            std::uintptr_t tooltipBuffer{};
            if (old.qualifiedTooltipWidget != 0
                && reinterpret_cast<std::uintptr_t>(tooltip) == old.qualifiedTooltipWidget
                && old.qualifiedTooltipFieldOffset != std::numeric_limits<std::size_t>::max()
                && ReadNativeField(tooltip, old.qualifiedTooltipFieldOffset, tooltipBuffer)
                && tooltipBuffer == old.qualifiedTooltipBuffer) {
                state.qualifiedTooltipWidget = old.qualifiedTooltipWidget;
                state.qualifiedTooltipBuffer = old.qualifiedTooltipBuffer;
                state.qualifiedTooltipFieldOffset = old.qualifiedTooltipFieldOffset;
                state.qualifiedTooltipEncoding = old.qualifiedTooltipEncoding;
            }
            NativeCountdownWidgets[i] = widget;
            NativeCountdownBuffers[i] = buffer;
            changed = true;
        }
        // Native input-mode switching can also reset visibility in place.
        // Empty slots must be hidden when reconciling a replaced/reset tree.
        if (!state.visible) {
            (void)SetVisible(Handles[i].slot, false);
            (void)SetVisible(Handles[i].countdown, false);
            (void)SetVisible(Handles[i].tooltip, false);
        }
    }
    NativeHudPanel = panel;
    EmptySlotsHidden = (panel != nullptr);
    if (changed) {
        // A rebuilt or reset tree shows its placeholders again; the render
        // re-applies the checkbox and hover box from scratch.
        (void)SetVisible(AuraCheck, false);
        for (const auto box : HoverBoxes) (void)SetVisible(box, false);
        AuraCheckShown = false;
        AuraCheckFrame = std::numeric_limits<std::uint16_t>::max();
        HoverBoxShown = -1;
        HoverBuffers = {};
        (void)ApplyInputIsolation();
        LastRenderedRevision = std::numeric_limits<std::uint64_t>::max();
        LastRenderedGameFrame = NoRenderedSeconds;
        LastRenderedHasClock = false;
    }
    return changed;
}

[[nodiscard]] bool WriteCountdownText(std::size_t slotIndex, const char* text) noexcept {
    if (slotIndex >= SlotCount || text == nullptr) return false;
    const auto length = std::strlen(text);
    if (length + 1 > TimerReserveBytes) return false;

    void* widget = NativeCountdownWidgets[slotIndex];
    if (widget == nullptr) {
        widget = ResolveNativeSlotChild(slotIndex, "Countdown");
    }
    std::uintptr_t pointer{};
    if (widget == nullptr
        || !ReadNativeField(widget, Native::Contract::HudTextPointerOffset, pointer)
        || pointer == 0) {
        return false;
    }

    auto* buffer = reinterpret_cast<char*>(pointer);
    if (!IsWritableRange(buffer, TimerReserveBytes)) {
        return false;
    }

    auto& state = RenderStates[slotIndex];
    if (state.qualifiedTimerBuffer != pointer) {
        if (!IsTimerReserve(buffer)) {
            return false;
        }
        state.qualifiedTimerBuffer = pointer;
    }

    std::memcpy(buffer, TimerReserve, TimerReserveBytes);
    std::memcpy(buffer, text, length + 1);
    if (std::memcmp(buffer, text, length + 1) != 0) {
        return false;
    }
    return true;
}

[[nodiscard]] bool QualifyTooltipString(
    std::size_t slotIndex,
    void* widget,
    SlotRenderState& state) noexcept {
    if (widget == nullptr || slotIndex >= SlotCount) return false;

    char expectedUtf8[TooltipReserveBytes]{};
    MakeTooltipReserve(slotIndex, expectedUtf8);
    std::array<std::uint16_t, TooltipReserveCodepoints + 1> expectedUtf16{};
    MakeTooltipReserveUtf16(slotIndex, expectedUtf16);
    constexpr std::size_t Utf16Bytes = (TooltipReserveCodepoints + 1) * sizeof(std::uint16_t);

    std::size_t candidateCount{};
    std::size_t candidateOffset{};
    std::uintptr_t candidateBuffer{};
    TooltipStorageEncoding candidateEncoding{TooltipStorageEncoding::Unknown};

    // Build 93847 already has one independently runtime-qualified Blizzard
    // string layout: TooltipsPanel+0x180. It is {data@+0x00, size@+0x08,
    // capacity/flags@+0x10}, not an MSVC std::string. The previous BuffHud
    // implementation searched FocusableWidget for an MSVC basic_string tuple,
    // which explains the runtime witness tooltipQualFailures>0 with
    // skillNames=ready/name=Fade. Qualify the same blz::basic_string layout
    // here. Accept both char and 16-bit character storage because UI layout
    // fields are not guaranteed to use the same character width.
    for (std::size_t offset = 0;
         offset + Native::Contract::BlizzardStringCapacityFlagsOffset + sizeof(std::uint64_t) <= TooltipNativeScanBytes;
         offset += alignof(std::uintptr_t)) {
        std::uintptr_t dataPointer{};
        std::uint64_t size{};
        std::uint64_t capacityFlags{};
        if (!ReadNativeField(widget, offset + Native::Contract::BlizzardStringDataOffset, dataPointer)
            || !ReadNativeField(widget, offset + Native::Contract::BlizzardStringSizeOffset, size)
            || !ReadNativeField(widget, offset + Native::Contract::BlizzardStringCapacityFlagsOffset, capacityFlags)) {
            continue;
        }

        const auto capacity = capacityFlags & Native::Contract::BlizzardStringCapacityMask;
        const bool embedded = (capacityFlags & Native::Contract::BlizzardStringEmbeddedFlag) != 0;
        if (embedded
            || dataPointer == 0
            || capacity > Native::Contract::MaximumTooltipCapacityBytes) {
            continue;
        }

        TooltipStorageEncoding encoding{TooltipStorageEncoding::Unknown};
        if (size == TooltipReserveLength
            && capacity >= TooltipReserveLength
            && IsWritableRange(reinterpret_cast<void*>(dataPointer), TooltipReserveBytes)
            && Internal::MatchesTooltipReserve(reinterpret_cast<const void*>(dataPointer), expectedUtf8)) {
            encoding = TooltipStorageEncoding::BlizzardUtf8;
        } else if (size == TooltipReserveCodepoints
            && capacity >= TooltipReserveCodepoints
            && IsWritableRange(reinterpret_cast<void*>(dataPointer), Utf16Bytes)
            && std::memcmp(reinterpret_cast<const void*>(dataPointer), expectedUtf16.data(), Utf16Bytes) == 0) {
            encoding = TooltipStorageEncoding::BlizzardUtf16;
        }
        if (encoding == TooltipStorageEncoding::Unknown) continue;

        ++candidateCount;
        candidateOffset = offset;
        candidateBuffer = dataPointer;
        candidateEncoding = encoding;
    }

    if (candidateCount != 1) {
        return false;
    }

    state.qualifiedTooltipWidget = reinterpret_cast<std::uintptr_t>(widget);
    state.qualifiedTooltipBuffer = candidateBuffer;
    state.qualifiedTooltipFieldOffset = candidateOffset;
    state.qualifiedTooltipEncoding = candidateEncoding;
    return true;
}

[[nodiscard]] bool EnsureTooltipQualified(std::size_t slotIndex, SlotRenderState& state) noexcept {
    if (slotIndex >= SlotCount) return false;
    void* widget = ResolveNativeSlotChild(slotIndex, "Tooltip");
    if (widget == nullptr) {
        return false;
    }

    const auto widgetAddress = reinterpret_cast<std::uintptr_t>(widget);
    if (state.qualifiedTooltipWidget == widgetAddress
        && state.qualifiedTooltipBuffer != 0
        && state.qualifiedTooltipFieldOffset != std::numeric_limits<std::size_t>::max()
        && state.qualifiedTooltipEncoding != TooltipStorageEncoding::Unknown) {
        return true;
    }

    RestoreQualifiedTooltipBuffer(slotIndex, state);
    state.qualifiedTooltipWidget = 0;
    state.qualifiedTooltipBuffer = 0;
    state.qualifiedTooltipFieldOffset = std::numeric_limits<std::size_t>::max();
    state.qualifiedTooltipEncoding = TooltipStorageEncoding::Unknown;
    return QualifyTooltipString(slotIndex, widget, state);
}

[[nodiscard]] bool WriteTooltipText(std::size_t slotIndex, const char* text) noexcept {
    if (slotIndex >= SlotCount || text == nullptr) return false;
    const auto utf8Length = std::strlen(text);
    if (utf8Length > TooltipReserveLength) {
        return false;
    }

    auto& state = RenderStates[slotIndex];
    if (!EnsureTooltipQualified(slotIndex, state)) {
        return false;
    }

    const auto stringObject = state.qualifiedTooltipWidget + state.qualifiedTooltipFieldOffset;
    std::uintptr_t dataPointer{};
    std::uint64_t capacityFlags{};
    if (!ReadNativeField(
            reinterpret_cast<void*>(state.qualifiedTooltipWidget),
            state.qualifiedTooltipFieldOffset + Native::Contract::BlizzardStringDataOffset,
            dataPointer)
        || !ReadNativeField(
            reinterpret_cast<void*>(state.qualifiedTooltipWidget),
            state.qualifiedTooltipFieldOffset + Native::Contract::BlizzardStringCapacityFlagsOffset,
            capacityFlags)
        || dataPointer == 0
        || dataPointer != state.qualifiedTooltipBuffer) {
        return false;
    }

    const auto capacity = capacityFlags & Native::Contract::BlizzardStringCapacityMask;
    const bool embedded = (capacityFlags & Native::Contract::BlizzardStringEmbeddedFlag) != 0;
    if (embedded
        || !IsWritableRange(
            reinterpret_cast<void*>(stringObject + Native::Contract::BlizzardStringSizeOffset),
            sizeof(std::uint64_t))) {
        return false;
    }

    std::uint64_t nativeLength{};
    if (state.qualifiedTooltipEncoding == TooltipStorageEncoding::BlizzardUtf8) {
        if (capacity < TooltipReserveLength
            || !IsWritableRange(reinterpret_cast<void*>(dataPointer), TooltipReserveBytes)) {
            return false;
        }
        auto* buffer = reinterpret_cast<char*>(dataPointer);
        if (!Internal::StoreTooltipText(buffer, std::string_view(text, utf8Length))) {
            return false;
        }
        nativeLength = utf8Length;
        if (std::memcmp(buffer, text, utf8Length + 1) != 0) {
            return false;
        }
    } else if (state.qualifiedTooltipEncoding == TooltipStorageEncoding::BlizzardUtf16) {
        if (capacity < TooltipReserveCodepoints) {
            return false;
        }
        std::array<std::uint16_t, TooltipReserveCodepoints + 1> converted{};
        std::size_t convertedLength{};
        if (!Utf8ToUtf16(text, converted, convertedLength)) {
            return false;
        }
        constexpr std::size_t Utf16Bytes = (TooltipReserveCodepoints + 1) * sizeof(std::uint16_t);
        if (!IsWritableRange(reinterpret_cast<void*>(dataPointer), Utf16Bytes)) {
            return false;
        }
        std::memset(reinterpret_cast<void*>(dataPointer), 0, Utf16Bytes);
        if (convertedLength != 0) {
            std::memcpy(reinterpret_cast<void*>(dataPointer), converted.data(), convertedLength * sizeof(std::uint16_t));
        }
        nativeLength = convertedLength;
    } else {
        return false;
    }

    std::memcpy(
        reinterpret_cast<void*>(stringObject + Native::Contract::BlizzardStringSizeOffset),
        &nativeLength,
        sizeof(nativeLength));

    std::uint64_t verifyLength{};
    if (!ReadNativeField(
            reinterpret_cast<void*>(state.qualifiedTooltipWidget),
            state.qualifiedTooltipFieldOffset + Native::Contract::BlizzardStringSizeOffset,
            verifyLength)
        || verifyLength != nativeLength) {
        return false;
    }

    return true;
}

void ClearTooltip(std::size_t slotIndex) noexcept {
    if (slotIndex >= SlotCount || Handles[slotIndex].tooltip == D2RL::Widgets::InvalidHandle) return;
    (void)SetVisible(Handles[slotIndex].tooltip, false);
    if (WriteTooltipText(slotIndex, "")) {
        RenderStates[slotIndex].tooltipVisible = false;
    }
}

[[nodiscard]] const char* ToggleTooltip(Core::BuffToggle toggle) noexcept {
    switch (toggle) {
    // Hover box text: fits the widest box (about 36 characters).
    case Core::BuffToggle::On:
        return "Blood Golem Aura: click to turn off";
    case Core::BuffToggle::Off:
        return "Blood Golem Aura: click to turn on";
    case Core::BuffToggle::Unavailable:
        return "Blood Golem Aura";
    default:
        return nullptr;
    }
}

void ApplyTooltip(
    std::size_t slotIndex,
    std::int32_t sourceSkillId,
    const char* text = nullptr) noexcept {
    if (slotIndex >= SlotCount || !ResolveWidgetHandles()) return;
    // Names on hover come from HoverLabel; only a layout override that still
    // has per-slot Tooltip widgets gets native hover text.
    if (Handles[slotIndex].tooltip == D2RL::Widgets::InvalidHandle) return;

    (void)SetVisible(Handles[slotIndex].tooltip, false);
    char localizedName[TooltipReserveBytes]{};
    if (text != nullptr) {
        std::snprintf(localizedName, sizeof(localizedName), "%s", text);
    } else if (sourceSkillId == Core::NoSourceSkillId
        || !Internal::TryResolveSkillName(sourceSkillId, localizedName, sizeof(localizedName))) {
        ClearTooltip(slotIndex);
        return;
    }

    if (!WriteTooltipText(slotIndex, localizedName)) {
        RenderStates[slotIndex].tooltipVisible = false;
        return;
    }

    // Native hover is optional: a FocusableWidget may intercept world clicks.
    // Preserve the valid-text visibility guard independently of the mouse mode.
    RenderStates[slotIndex].tooltipVisible = SetVisible(Handles[slotIndex].tooltip, true);
}

[[nodiscard]] bool ResolveEntryIcon(
    const Core::BuffDisplayEntry& entry,
    SkillIconDescriptor& descriptor) noexcept {
    descriptor = {};
    if (entry.iconFrame != Core::AutoIconFrame) {
        descriptor.frame = entry.iconFrame;
        descriptor.atlas = entry.iconAtlas == Core::BuffIconAtlas::Auto
            ? Core::BuffIconAtlas::Global
            : entry.iconAtlas;
        return true;
    }
    if (entry.sourceSkillId != Core::NoSourceSkillId
        && Internal::TryResolveSkillIcon(entry.sourceSkillId, descriptor)) {
        return true;
    }
    descriptor = {.atlas = Core::BuffIconAtlas::Global, .frame = 0};
    return false;
}

[[nodiscard]] bool IsFrameInFuture(std::uint32_t expire, std::uint32_t now) noexcept {
    return static_cast<std::int32_t>(expire - now) > 0;
}

[[nodiscard]] std::uint32_t RemainingSeconds(std::uint32_t expire, std::uint32_t now) noexcept {
    const auto delta = static_cast<std::uint32_t>(expire - now);
    return (delta + FramesPerSecond - 1) / FramesPerSecond;
}

void FormatSeconds(std::uint32_t seconds, char (&output)[16]) noexcept {
    if (seconds >= 3600) {
        const auto hours = seconds / 3600;
        const auto minutes = (seconds % 3600) / 60;
        std::snprintf(output, sizeof(output), "%u:%02u", hours, minutes);
    } else if (seconds >= 60) {
        std::snprintf(output, sizeof(output), "%u:%02u", seconds / 60, seconds % 60);
    } else {
        std::snprintf(output, sizeof(output), "%u", seconds);
    }
}

[[nodiscard]] bool ApplyIcon(std::size_t slotIndex, const SkillIconDescriptor& icon) noexcept {
    if (slotIndex >= SlotCount || !ResolveWidgetHandles()) return false;
    auto& handles = Handles[slotIndex];
    const auto selected = AtlasIndex(icon.atlas);

    for (std::size_t i = 0; i < handles.icons.size(); ++i) {
        const bool selectedIcon = i == selected;
        // BuffHud is display-only. Even when a buff is active, its atlas button
        // must stay click-through, so keep every ButtonWidget disabled and only
        // use visibility to choose the currently rendered atlas.
        if (!SetEnabled(handles.icons[i], false)) {
        }
        (void)SetVisible(handles.icons[i], selectedIcon);
    }

    void* nativeIcon = ResolveNativeSlotChild(slotIndex, AtlasWidgetNames[selected]);
    const auto declaredFrame = static_cast<std::uint16_t>(slotIndex * 2);
    if (Internal::TryApplyIconFrame(nativeIcon, icon.frame, declaredFrame)) return true;

    // Fail closed if the qualified live-frame path cannot be applied. The atlas
    // selection remains valid, but the slot keeps its JSON witness frame and the
    // fallback counter makes the failure visible in `buff-panel status`.
    return false;
}

void HideSlot(std::size_t slotIndex) noexcept {
    if (slotIndex >= SlotCount || !ResolveWidgetHandles()) return;
    auto& state = RenderStates[slotIndex];
    if (!state.visible) return;

    // Visibility alone does not remove ButtonWidget hit targets. Once an
    // occupied slot becomes empty, disable every atlas child before hiding it.
    // Initial empty slots were already disabled by ResolveWidgetHandles().
    for (const auto icon : Handles[slotIndex].icons) {
        if (!SetEnabled(icon, false)) {
        }
    }
    ClearTooltip(slotIndex);
    state.tooltipVisible = false;

    (void)SetVisible(Handles[slotIndex].slot, false);
    const auto qualifiedTimerBuffer = state.qualifiedTimerBuffer;
    const auto qualifiedTooltipWidget = state.qualifiedTooltipWidget;
    const auto qualifiedTooltipBuffer = state.qualifiedTooltipBuffer;
    const auto qualifiedTooltipFieldOffset = state.qualifiedTooltipFieldOffset;
    const auto qualifiedTooltipEncoding = state.qualifiedTooltipEncoding;
    state = {};
    state.qualifiedTimerBuffer = qualifiedTimerBuffer;
    state.qualifiedTooltipWidget = qualifiedTooltipWidget;
    state.qualifiedTooltipBuffer = qualifiedTooltipBuffer;
    state.qualifiedTooltipFieldOffset = qualifiedTooltipFieldOffset;
    state.qualifiedTooltipEncoding = qualifiedTooltipEncoding;
}

void RenderSlot(
    std::size_t slotIndex,
    const Core::BuffDisplayEntry& entry,
    std::uint32_t nowFrame,
    bool hasClock) noexcept {
    if (slotIndex >= SlotCount || !ResolveWidgetHandles()) return;

    SkillIconDescriptor icon{};
    (void)ResolveEntryIcon(entry, icon);
    auto& state = RenderStates[slotIndex];
    const bool assignmentChanged = !state.visible
        || state.key != entry.key
        || state.atlas != icon.atlas
        || state.requestedFrame != icon.frame
        || state.displayMode != entry.displayMode
        || state.sourceSkillId != entry.sourceSkillId
        || state.toggle != entry.toggle;

    if (assignmentChanged) {
        (void)SetVisible(Handles[slotIndex].slot, false);
        (void)SetVisible(Handles[slotIndex].countdown, false);
        const bool iconApplied = ApplyIcon(slotIndex, icon);
        state.key = entry.key;
        state.atlas = icon.atlas;
        state.requestedFrame = icon.frame;
        state.renderedFrame = iconApplied
            ? icon.frame
            : static_cast<std::uint16_t>(slotIndex * 2);
        state.displayMode = entry.displayMode;
        state.sourceSkillId = entry.sourceSkillId;
        state.seconds = NoRenderedSeconds;
        state.currentValue = NoRenderedValue;
        state.maximumValue = NoRenderedValue;
        state.timerVisible = false;
        state.tooltipVisible = false;
        state.toggle = entry.toggle;
        ApplyTooltip(slotIndex, entry.sourceSkillId, ToggleTooltip(entry.toggle));
        state.visible = true;
        (void)SetVisible(Handles[slotIndex].slot, true);
    }

    // A switchable entry never counts down; its label says only when it is off.
    if (entry.toggle != Core::BuffToggle::None) {
        const bool off = entry.toggle == Core::BuffToggle::Off;
        if (off && !state.timerVisible && WriteCountdownText(slotIndex, "OFF")) {
            (void)SetVisible(Handles[slotIndex].countdown, true);
            state.timerVisible = true;
        } else if (!off && state.timerVisible) {
            (void)SetVisible(Handles[slotIndex].countdown, false);
            state.timerVisible = false;
        }
        return;
    }

    if (entry.displayMode == Core::BuffDisplayMode::Resource) {
        if (state.timerVisible && state.currentValue == entry.currentValue) {
            return;
        }

        char text[32]{};
        std::snprintf(text, sizeof(text), "%d", entry.currentValue);
        if (WriteCountdownText(slotIndex, text)) {
            if (!state.timerVisible) {
                (void)SetVisible(Handles[slotIndex].countdown, true);
                state.timerVisible = true;
            }
            state.seconds = NoRenderedSeconds;
            state.currentValue = entry.currentValue;
            state.maximumValue = entry.maximumValue;
        }
        return;
    }

    const bool wantsTimer = entry.expireGameFrame != 0;
    if (!wantsTimer) {
        if (state.timerVisible) {
            (void)SetVisible(Handles[slotIndex].countdown, false);
            state.timerVisible = false;
            state.seconds = NoRenderedSeconds;
            state.currentValue = NoRenderedValue;
            state.maximumValue = NoRenderedValue;
        }
        return;
    }

    if (!hasClock) {
        if (state.seconds != NoRenderedSeconds || !state.timerVisible) {
            if (WriteCountdownText(slotIndex, "--")) {
                if (!state.timerVisible) {
                    (void)SetVisible(Handles[slotIndex].countdown, true);
                    state.timerVisible = true;
                }
                state.seconds = NoRenderedSeconds;
            }
        }
        return;
    }

    const auto seconds = RemainingSeconds(entry.expireGameFrame, nowFrame);
    if (state.timerVisible && state.seconds == seconds) return;

    char text[16]{};
    FormatSeconds(seconds, text);
    if (WriteCountdownText(slotIndex, text)) {
        if (!state.timerVisible) {
            (void)SetVisible(Handles[slotIndex].countdown, true);
            state.timerVisible = true;
        }
        state.seconds = seconds;
    }
}

[[nodiscard]] bool EffectiveFrame(
    const Core::BuffDisplaySnapshot& snapshot,
    std::uint32_t& frame) noexcept {
    if (!snapshot.hasGameFrame) { frame = 0; return false; }
    frame = snapshot.currentGameFrame;
    return true;
}

// Diagnostics: each change to what the HUD shows, bounded per game.
constexpr std::uint32_t MaximumDisplayLogLines = 300;
std::uint32_t DisplayLogLines{};

void LogDisplay(const Core::BuffDisplaySnapshot& snapshot,
    const std::array<Core::BuffDisplayEntry, Core::MaximumPublishedBuffs>& ordered,
    std::size_t count, std::uint32_t nowFrame, bool hasClock) noexcept {
    if (Context == nullptr || DisplayLogLines >= MaximumDisplayLogLines) return;
    ++DisplayLogLines;
    char line[1024]{};
    int used = std::snprintf(line, sizeof(line), "Buff HUD: display revision %llu session %llu frame %u%s: %zu entr%s",
        static_cast<unsigned long long>(snapshot.revision), static_cast<unsigned long long>(snapshot.sessionGeneration),
        nowFrame, hasClock ? "" : " (no clock)", count, count == 1 ? "y" : "ies");
    for (std::size_t i = 0; i < count && used > 0 && used < static_cast<int>(sizeof(line)) - 64; ++i) {
        const auto& entry = ordered[i];
        used += std::snprintf(line + used, sizeof(line) - static_cast<std::size_t>(used),
            "%s skill=%d %s expire=%u value=%d toggle=%u", i == 0 ? ":" : ";", entry.sourceSkillId,
            entry.displayMode == Core::BuffDisplayMode::Resource ? "resource" : "timer",
            entry.expireGameFrame, entry.currentValue, static_cast<unsigned>(entry.toggle));
    }
    Context->LogInfo(line);
}

// UI thread. Adds, updates or removes the Blood Golem aura's entry; touches the
// display bus only when something changed, so an idle HUD stays idle.
void SyncBloodAuraEntry() noexcept {
    const auto snapshot = Core::BuffDisplays().Snapshot();
    const Core::BuffDisplayEntry* existing = nullptr;
    for (std::size_t i = 0; i < snapshot.count; ++i) {
        if (snapshot.entries[i].key == BloodAuraKey) {
            existing = &snapshot.entries[i];
            break;
        }
    }
    // Switched off, the aura's stats are suppressed on the server and the client
    // loses the state (seen in game 2026-10-28), so the slot stays while it is
    // off: otherwise there would be nothing left to click to turn it back on.
    const bool switchedOff = BloodAura::LocalAuraOff() && BloodAura::CanToggle();
    if (!BloodAura::LocalPlayerInAura() && !switchedOff) {
        if (existing != nullptr) (void)Core::BuffDisplays().Remove(BloodAuraKey);
        return;
    }
    const auto toggle = !BloodAura::CanToggle() ? Core::BuffToggle::Unavailable
        : BloodAura::LocalAuraOff() ? Core::BuffToggle::Off
        : Core::BuffToggle::On;
    if (existing != nullptr && existing->toggle == toggle) return;
    Core::BuffDisplayEntry entry{};
    entry.key = BloodAuraKey;
    entry.sourceSkillId = BloodAura::IconSkillId;
    entry.displayMode = Core::BuffDisplayMode::Timer;
    entry.expireGameFrame = 0;
    entry.stacks = 1;
    entry.priority = BloodAuraPriority;
    entry.toggle = toggle;
    (void)Core::BuffDisplays().Upsert(entry);
}

// UI thread. A slot's square in HD layout units from the panel anchor.
[[nodiscard]] HudInput::SlotArea SlotLayoutArea(std::size_t index) noexcept {
    D2RL::Widgets::Rect grid{};
    D2RL::Widgets::Rect slot{};
    if (Widgets->getWidgetRect == nullptr
        || Widgets->getWidgetRect(Context, GridWidget, &grid) != D2RL::Widgets::Result::Success
        || Widgets->getWidgetRect(Context, Handles[index].slot, &slot) != D2RL::Widgets::Result::Success
        || slot.width <= 0) {
        grid = GridFallbackRect;
        slot = D2RL::Widgets::Rect{
            static_cast<std::int32_t>(index % SlotsPerRow) * SlotFallbackPitchX,
            SlotFallbackBottomY - static_cast<std::int32_t>(index / SlotsPerRow) * SlotFallbackPitchY,
            SlotFallbackEdge,
            SlotFallbackEdge,
        };
    }
    return HudInput::SlotArea{
        .visible = true,
        .x = static_cast<float>(grid.x + slot.x),
        .y = static_cast<float>(grid.y + slot.y),
        .size = static_cast<float>(slot.width),
    };
}

// UI thread. Slot 0's checkbox: shown ticked (aura on) or empty (off) while the
// aura holds slot 0, hidden otherwise.
void ShowAuraCheck(bool shown, bool checked) noexcept {
    if (AuraCheck == D2RL::Widgets::InvalidHandle) return;
    if (shown) {
        const auto frame = checked ? CheckedFrame : UncheckedFrame;
        if (AuraCheckFrame != frame
            && Internal::TryApplyIconFrame(ResolveNativeSlotChild(0, "AuraCheck"), frame, CheckedFrame)) {
            AuraCheckFrame = frame;
        }
    }
    if (AuraCheckShown != shown && SetVisible(AuraCheck, shown)) AuraCheckShown = shown;
}

// UI thread. Writes a hover box's text in place over its JSON reserve, the way
// WriteCountdownText writes a countdown.
[[nodiscard]] bool WriteHoverText(std::size_t box, const char* text) noexcept {
    const auto length = std::strlen(text);
    if (box >= HoverBoxCount || length + 1 > HoverReserveBytes
        || FindTopLevelPanel == nullptr || FindChildWidgetByName == nullptr) {
        return false;
    }
    char name[16]{};
    std::snprintf(name, sizeof(name), "HoverBox%zu%c", box / HoverSizes.size(), HoverSizes[box % HoverSizes.size()]);
    void* panel = FindTopLevelPanel("buffhud/BuffHud");
    void* grid = panel != nullptr ? FindChildWidgetByName(panel, "BuffGrid") : nullptr;
    void* frame = grid != nullptr ? FindChildWidgetByName(grid, name) : nullptr;
    void* label = frame != nullptr ? FindChildWidgetByName(frame, "Text") : nullptr;
    std::uintptr_t pointer{};
    if (label == nullptr || !ReadNativeField(label, Native::Contract::HudTextPointerOffset, pointer) || pointer == 0) {
        return false;
    }
    auto* buffer = reinterpret_cast<char*>(pointer);
    if (!IsWritableRange(buffer, HoverReserveBytes)) return false;
    if (pointer != HoverBuffers[box]) {
        // A new widget tree: only a buffer still holding the reserve is ours.
        if (std::memcmp(buffer, HoverReserve, HoverReserveBytes) != 0) return false;
        HoverBuffers[box] = pointer;
    }
    std::memcpy(buffer, HoverReserve, HoverReserveBytes);
    std::memcpy(buffer, text, length + 1);
    return true;
}

// Which box: the width that fits `length` characters, over the top occupied
// row (slots fill the bottom row, 0-6, first; row 2 is the bottom one).
[[nodiscard]] std::size_t HoverBoxFor(std::size_t length, std::size_t occupiedSlots) noexcept {
    const auto topSlot = occupiedSlots == 0 ? 0 : occupiedSlots - 1;
    const auto row = HoverRows - 1 - std::min(topSlot / SlotsPerRow, HoverRows - 1);
    const std::size_t size = length <= HoverSizeLimits[0] ? 0 : length <= HoverSizeLimits[1] ? 1 : 2;
    return row * HoverSizes.size() + size;
}

// UI thread (HudInput's hover callback, and after a render): names the buff in
// the hovered slot in a tooltip box above the slots, or hides it.
void __cdecl ShowHover(int slot) noexcept {
    if (!HandlesResolved) return;
    HoverSlotShown = slot;
    char text[HoverReserveBytes]{};
    if (slot >= 0 && static_cast<std::size_t>(slot) < SlotEntryCount) {
        const auto& entry = SlotEntries[static_cast<std::size_t>(slot)];
        if (const char* toggleText = ToggleTooltip(entry.toggle); toggleText != nullptr) {
            std::snprintf(text, sizeof(text), "%s", toggleText);
        } else if (entry.sourceSkillId != Core::NoSourceSkillId) {
            char name[TooltipReserveBytes]{};
            if (Internal::TryResolveSkillName(entry.sourceSkillId, name, sizeof(name))) {
                std::size_t length = std::min(std::strlen(name), HoverReserveBytes - 1);
                // Never cut a UTF-8 character in half.
                while (length > 0 && length < std::strlen(name)
                    && (static_cast<unsigned char>(name[length]) & 0xC0u) == 0x80u) {
                    --length;
                }
                std::memcpy(text, name, length);
                text[length] = '\0';
            }
        }
    }
    int box = -1;
    if (text[0] != '\0') {
        const auto candidate = HoverBoxFor(std::strlen(text), SlotEntryCount);
        if (HoverBoxes[candidate] != D2RL::Widgets::InvalidHandle && WriteHoverText(candidate, text)) {
            box = static_cast<int>(candidate);
        }
    }
    if (box == HoverBoxShown) return;
    if (HoverBoxShown >= 0) (void)SetVisible(HoverBoxes[static_cast<std::size_t>(HoverBoxShown)], false);
    if (box >= 0 && !SetVisible(HoverBoxes[static_cast<std::size_t>(box)], true)) box = -1;
    HoverBoxShown = box;
}

// UI thread. Tells HudInput where the shown slots are and which one is the
// aura's, places the checkbox, and refreshes a hover label left over a slot
// whose buff changed.
void PublishSlotInput(const std::array<Core::BuffDisplayEntry, Core::MaximumPublishedBuffs>& ordered,
    std::size_t visibleCount) noexcept {
    HudInput::SlotAreas areas{};
    int toggleSlot = -1;
    bool checked = false;
    SlotEntryCount = visibleCount;
    for (std::size_t i = 0; i < visibleCount; ++i) {
        SlotEntries[i] = ordered[i];
        areas[i] = SlotLayoutArea(i);
        const auto toggle = ordered[i].toggle;
        if (toggleSlot < 0 && (toggle == Core::BuffToggle::On || toggle == Core::BuffToggle::Off)) {
            toggleSlot = static_cast<int>(i);
            checked = toggle == Core::BuffToggle::On;
        }
    }
    // The checkbox exists only in slot 0, which the aura's top priority keeps.
    ShowAuraCheck(toggleSlot == 0, checked);
    HudInput::Publish(areas, toggleSlot);
    if (HoverSlotShown >= 0) ShowHover(HoverSlotShown);
}

void RenderSnapshot() noexcept {
    if (!ResolveWidgetHandles()) return;
    const bool reconciled = ReconcileNativeWidgets();
    SyncBloodAuraEntry();

    auto snapshot = Core::BuffDisplays().Snapshot();
    std::uint32_t nowFrame{};
    const bool hasClock = EffectiveFrame(snapshot, nowFrame);

    if (!reconciled
        && snapshot.revision == LastRenderedRevision
        && hasClock == LastRenderedHasClock
        && (!hasClock || nowFrame == LastRenderedGameFrame)) {
        return;
    }

    // The display bus is not gameplay state. Once authoritative time says an
    // entry is over, removing it from the display registry is safe and keeps
    // stale producers from occupying HUD slots forever.
    if (hasClock) {
        std::array<std::uint64_t, Core::MaximumPublishedBuffs> expired{};
        std::size_t expiredCount{};
        for (std::size_t i = 0; i < snapshot.count; ++i) {
            const auto& entry = snapshot.entries[i];
            if (entry.expireGameFrame != 0
                && !IsFrameInFuture(entry.expireGameFrame, nowFrame)) {
                expired[expiredCount++] = entry.key;
            }
        }
        if (expiredCount != 0) {
            for (std::size_t i = 0; i < expiredCount; ++i) {
                if (Core::BuffDisplays().Remove(expired[i])) {
                }
            }
            if (DisplayLogLines < MaximumDisplayLogLines) {
                ++DisplayLogLines;
                char line[160]{};
                std::snprintf(line, sizeof(line), "Buff HUD: display dropped %zu expired entr%s at frame %u (first key 0x%llX).",
                    expiredCount, expiredCount == 1 ? "y" : "ies", nowFrame, static_cast<unsigned long long>(expired[0]));
                Context->LogInfo(line);
            }
            snapshot = Core::BuffDisplays().Snapshot();
            if (snapshot.count == 0 && !snapshot.hasGameFrame) {
            }
        }
    }

    std::array<Core::BuffDisplayEntry, Core::MaximumPublishedBuffs> ordered{};
    std::size_t orderedCount{};
    for (std::size_t i = 0; i < snapshot.count; ++i) ordered[orderedCount++] = snapshot.entries[i];
    std::sort(
        ordered.begin(),
        ordered.begin() + static_cast<std::ptrdiff_t>(orderedCount),
        [](const Core::BuffDisplayEntry& lhs, const Core::BuffDisplayEntry& rhs) noexcept {
            if (lhs.priority != rhs.priority) return lhs.priority > rhs.priority;
            return lhs.sequence < rhs.sequence;
        });

    if (snapshot.revision != LastRenderedRevision) LogDisplay(snapshot, ordered, orderedCount, nowFrame, hasClock);

    const auto visibleCount = std::min<std::size_t>(orderedCount, SlotCount);
    for (std::size_t i = 0; i < visibleCount; ++i) {
        RenderSlot(i, ordered[i], nowFrame, hasClock);
    }
    for (std::size_t i = visibleCount; i < SlotCount; ++i) HideSlot(i);
    PublishSlotInput(ordered, visibleCount);

    LastRenderedRevision = snapshot.revision;
    LastRenderedGameFrame = nowFrame;
    LastRenderedHasClock = hasClock;
}

void QueuePoll() noexcept;

void __cdecl PollOnUiThread(const D2RL::PluginContext* context, void*) noexcept {
    PollScheduled.store(false, std::memory_order_release);
    if (context == nullptr || context != Context
        || CurrentSessionGeneration.load(std::memory_order_acquire) == 0) {
        return;
    }

    BuffTracker::TickRemoteClock();
    RenderSnapshot();
    QueuePoll();
}

void QueuePoll() noexcept {
    if (Context == nullptr || Threads == nullptr
        || CurrentSessionGeneration.load(std::memory_order_acquire) == 0) {
        return;
    }
    bool expected = false;
    if (!PollScheduled.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return;
    }
    if (Threads->runOnUiThread(Context, &PollOnUiThread, nullptr)
        != D2RL::Threads::Result::Success) {
        PollScheduled.store(false, std::memory_order_release);
    }
}

void OpenPanel() noexcept {
    if (Context == nullptr || Panels == nullptr
        || RegisteredPanel == D2RL::Panels::InvalidHandle) return;
    const auto result = Panels->openPanel(Context, RegisteredPanel);
    if (result != D2RL::Panels::Result::Success) {
        return;
    }
    InvalidateWidgetHandles();
    if (ResolveWidgetHandles()) {
        // The layout's "visible": false does not survive a panel open; hide every
        // placeholder explicitly (seen in game 2026-10-28: all nine hover boxes
        // showed their reserve text until this was done).
        for (std::size_t i = 0; i < SlotCount; ++i) {
            (void)SetVisible(Handles[i].slot, false);
            ClearTooltip(i);
        }
        (void)SetVisible(AuraCheck, false);
        for (const auto box : HoverBoxes) (void)SetVisible(box, false);
    }
}

void ClosePanel() noexcept {
    HudInput::Publish({}, -1);
    for (const auto buffer : HoverBuffers) {
        if (buffer != 0 && IsWritableRange(reinterpret_cast<void*>(buffer), HoverReserveBytes)) {
            std::memcpy(reinterpret_cast<void*>(buffer), HoverReserve, HoverReserveBytes);
        }
    }
    RestoreQualifiedCountdownBuffers();
    RestoreQualifiedTooltipBuffers();
    if (Context != nullptr && Panels != nullptr
        && RegisteredPanel != D2RL::Panels::InvalidHandle) {
        (void)Panels->closePanel(Context, RegisteredPanel);
    }
    InvalidateWidgetHandles();
}

void __cdecl RebuildIconsOnGameThread(const D2RL::PluginContext*, void*) noexcept {
    const auto previous = Internal::SkillIconStatus();
    const auto revision = previous.tableRevision == 0 ? 1 : previous.tableRevision;
    const bool ready = Internal::RebuildSkillIconCache(revision);
    if (Context != nullptr) {
        const auto status = Internal::SkillIconStatus();
        char line[384]{};
        std::snprintf(
            line,
            sizeof(line),
            "Buff HUD: manual skill presentation cache rebuild %s bank=%u skills=%u desc=%u rowSize=%u/%u candidates(link=%u class=%u name=%u) offsets(link=0x%X class=0x%X icon=0x%X nameId=0x%X) names=%s.",
            ready ? "succeeded" : "FAILED",
            status.bank,
            status.skillCount,
            status.skillDescCount,
            status.skillsRowSize,
            status.skillDescRowSize,
            status.linkCandidateCount,
            status.classCandidateCount,
            status.nameCandidateCount,
            status.skillDescLinkOffset,
            status.skillClassOffset,
            status.iconCelOffset,
            status.skillNameStringIdOffset,
            status.namesReady ? "ready" : "not-qualified");
        ready ? Context->LogInfo(line) : Context->LogWarn(line);
    }
}

void __cdecl OnDataTablesLoaded(
    const D2RL::PluginContext*,
    const D2RL::Lifecycle::DataTablesLoadedEvent* event,
    void*) noexcept {
    if (event == nullptr) return;
    const bool ready = Internal::RebuildSkillIconCache(event->revision);
    if (Context == nullptr) return;
    const auto status = Internal::SkillIconStatus();
    char line[384]{};
    std::snprintf(
        line,
        sizeof(line),
        "Buff HUD: skill presentation table resolver %s revision=%llu bank=%u skills=%u desc=%u rowSize=%u/%u candidates(link=%u class=%u name=%u) offsets(link=0x%X class=0x%X icon=0x%X nameId=0x%X) names=%s.",
        ready ? "ready" : "not-qualified",
        static_cast<unsigned long long>(event->revision),
        status.bank,
        status.skillCount,
        status.skillDescCount,
        status.skillsRowSize,
        status.skillDescRowSize,
        status.linkCandidateCount,
        status.classCandidateCount,
        status.nameCandidateCount,
        status.skillDescLinkOffset,
        status.skillClassOffset,
        status.iconCelOffset,
        status.skillNameStringIdOffset,
        status.namesReady ? "ready" : "not-qualified");
    ready ? Context->LogInfo(line) : Context->LogWarn(line);
}

void __cdecl OnGameplayEvent(
    const D2RL::PluginContext*,
    const D2RL::Lifecycle::GameplayEvent* event,
    void*) noexcept {
    if (event == nullptr) return;

    switch (event->kind) {
    case D2RL::Lifecycle::GameplayEventKind::GameJoined:
        CurrentSessionGeneration.store(event->sessionGeneration, std::memory_order_release);
        Core::BuffDisplays().BeginSession(event->sessionGeneration);
        break;
    case D2RL::Lifecycle::GameplayEventKind::LocalPlayerReady:
        DisplayLogLines = 0;
        if (CurrentSessionGeneration.load(std::memory_order_acquire) != event->sessionGeneration) {
            CurrentSessionGeneration.store(event->sessionGeneration, std::memory_order_release);
            Core::BuffDisplays().BeginSession(event->sessionGeneration);
        }
        OpenPanel();
        HudInput::InstallInput();
        QueuePoll();
        if (!Internal::SkillIconStatus().ready && Threads != nullptr) {
            (void)Threads->runOnGameThread(Context, &RebuildIconsOnGameThread, nullptr);
        }
        break;
    case D2RL::Lifecycle::GameplayEventKind::GameLeft:
        Core::BuffDisplays().EndSession(event->sessionGeneration);
        CurrentSessionGeneration.store(0, std::memory_order_release);
        PollScheduled.store(false, std::memory_order_release);
        ClosePanel();
        break;
    default:
        break;
    }
}

void PrintStatus(const D2RL::PluginContext* context) noexcept {
    if (context == nullptr) return;
    const auto snapshot = Core::BuffDisplays().Snapshot();
    const auto icons = Internal::SkillIconStatus();
    char line[512]{};
    std::snprintf(line, sizeof(line),
        "Buff HUD 1.3.1+rev.3: displayed=%zu/%zu session=%llu frame=%u panel=%s companion=enabled skillIcons=%s skillNames=%s tableRevision=%llu inputIsolation=enabled.",
        snapshot.count, SlotCount,
        static_cast<unsigned long long>(snapshot.sessionGeneration),
        snapshot.currentGameFrame,
        RegisteredPanel != D2RL::Panels::InvalidHandle ? "registered" : "missing",
        icons.ready ? "ready" : "not-qualified",
        icons.namesReady ? "ready" : "not-qualified",
        static_cast<unsigned long long>(icons.tableRevision));
    context->WriteConsoleMessage(line);
    context->LogInfo(line);
    char input[256]{};
    HudInput::Describe(input, sizeof(input));
    std::snprintf(line, sizeof(line), "Blood Golem aura: inAura=%s canToggle=%s off=%s checkbox=%s/%s hoverBoxes=%s; %s",
        BloodAura::LocalPlayerInAura() ? "yes" : "no", BloodAura::CanToggle() ? "yes" : "no",
        BloodAura::LocalAuraOff() ? "yes" : "no", AuraCheck != D2RL::Widgets::InvalidHandle ? "found" : "missing",
        AuraCheckShown ? (AuraCheckFrame == CheckedFrame ? "ticked" : "empty") : "hidden",
        std::all_of(HoverBoxes.begin(), HoverBoxes.end(),
            [](D2RL::Widgets::WidgetHandle handle) { return handle != D2RL::Widgets::InvalidHandle; })
            ? "found" : "missing", input);
    context->WriteConsoleMessage(line);
    context->LogInfo(line);
}

D2RL::ConsoleCommandResult __cdecl BuffCommand(
    D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept {
    if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;
    const std::string_view args = command->args != nullptr
        ? std::string_view(command->args, command->argsLength) : std::string_view{};
    const auto first = args.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        PrintStatus(command->plugin); return D2RL::ConsoleCommandResult::Handled;
    }
    const auto last = args.find_last_not_of(" \t\r\n");
    if (args.substr(first, last - first + 1) == "status") {
        PrintStatus(command->plugin); return D2RL::ConsoleCommandResult::Handled;
    }
    command->plugin->WriteConsoleMessage("Usage: buff-panel [status]");
    return D2RL::ConsoleCommandResult::InvalidArguments;
}

[[nodiscard]] bool RegisterLayoutAndPanel() noexcept {
    if (Context == nullptr || Panels == nullptr) return false;
    // D2RLoader resolves the layout from our companion MPQ, including packed
    // active-mod overrides. Do not shadow it with a stale in-memory copy.
    const D2RL::Panels::PanelRegistration panel{
        .structSize = D2RL::Panels::PanelRegistrationSize,
        .flags = D2RL::Panels::PanelFlags::None,
        .localId = "BuffHud",
    };
    if (Panels->registerPanel(Context, &panel, &RegisteredPanel)
        != D2RL::Panels::Result::Success) {
        Context->LogError("Buff HUD: failed to register plugin-owned BuffHud panel.");
        return false;
    }
    return true;
}

[[nodiscard]] bool RegisterLifecycle() noexcept {
    if (Context == nullptr || Lifecycle == nullptr) return false;
    const D2RL::Lifecycle::DataTablesLoadedListener tableListener{
        .structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize,
        .flags = 0,
        .callback = &OnDataTablesLoaded,
        .userData = nullptr,
    };
    if (Lifecycle->registerDataTablesLoadedListener(Context, &tableListener, &DataTablesListener)
        != D2RL::Lifecycle::Result::Success) {
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

[[nodiscard]] bool ValidateNativeUiContract() noexcept {
    if (Context == nullptr || Context->exeBase == 0) return false;
    if (!Context->CheckExpectedBytes(
            Native::Contract::FindTopLevelPanelRva,
            Native::Contract::FindTopLevelPanelExpected.data(),
            static_cast<std::uint32_t>(Native::Contract::FindTopLevelPanelExpected.size()))
        || !Context->CheckExpectedBytes(
            Native::Contract::FindChildWidgetByNameRva,
            Native::Contract::FindChildWidgetByNameExpected.data(),
            static_cast<std::uint32_t>(Native::Contract::FindChildWidgetByNameExpected.size()))) {
        Context->LogError("Buff HUD: native UI resolver fingerprint mismatch; refusing BuffHud initialization.");
        return false;
    }
    FindTopLevelPanel = reinterpret_cast<FindTopLevelPanelFn>(
        Context->exeBase + Native::Contract::FindTopLevelPanelRva);
    FindChildWidgetByName = reinterpret_cast<FindChildWidgetByNameFn>(
        Context->exeBase + Native::Contract::FindChildWidgetByNameRva);
    return true;
}

} // namespace

bool Initialize(const D2RL::PluginContext* context) noexcept {
    Shutdown();
    if (context == nullptr) return false;
    Context = context;

    const auto& services = Core::Services();
    Panels = services.panels;
    Widgets = services.widgets;
    Threads = services.threads;
    Lifecycle = services.lifecycle;

    const bool panelsReady = Panels != nullptr
        && D2RL::HasPanelServiceField(Panels, D2RL::PanelServiceRequiredSize);
    const bool widgetsReady = Widgets != nullptr
        && D2RL::HasWidgetServiceField(Widgets, D2RL::WidgetServiceRequiredSize);
    const bool threadsReady = Threads != nullptr
        && D2RL::HasThreadServiceField(Threads, D2RL::ThreadServiceRequiredSize);
    const bool lifecycleReady = Lifecycle != nullptr
        && D2RL::HasLifecycleServiceField(Lifecycle, D2RL::LifecycleServiceRequiredSize);

    if (!panelsReady || !widgetsReady || !threadsReady || !lifecycleReady) {
        char serviceLine[256]{};
        std::snprintf(
            serviceLine,
            sizeof(serviceLine),
            "Buff HUD: required PluginSDK service unavailable/undersized: Panel=%d Widget=%d Thread=%d Lifecycle=%d.",
            panelsReady ? 1 : 0,
            widgetsReady ? 1 : 0,
            threadsReady ? 1 : 0,
            lifecycleReady ? 1 : 0);
        Context->LogError(serviceLine);
        Shutdown();
        return false;
    }

    // D2RLoader ABI 4 does not guarantee that every named service is active for
    // every loader build/runtime phase. Localization is presentation-only here:
    // icons/timers/resources remain fully functional without it. The skill cache
    // retries LocalizationService when data tables are ready, so a service that
    // becomes available later still restores localized hover names.
    const bool localizationReady = Core::LocalizationService() != nullptr;
    if (!localizationReady) {
        Context->LogWarn(
            "Buff HUD: LocalizationService unavailable at plugin initialization; BuffHud remains enabled and localized hover names will be retried when the skill cache is rebuilt.");
    }

    if (!ValidateNativeUiContract()
        || !Internal::InitializeIconFrameBackend(Context)
        || !RegisterLayoutAndPanel()
        || !RegisterLifecycle()) {
        Shutdown();
        return false;
    }

    HudInput::Initialize(Context, &ShowHover);

    if (!Context->RegisterConsoleCommand(
            "buff-panel",
            &BuffCommand,
            "Show Buff HUD status.")) {
        Context->LogWarn("Buff HUD: console command 'buff-panel' could not be registered.");
    }
    Context->LogInfo(
        "Buff HUD 1.3.1+rev.3 BuffHud initialized: 21 display-only slots, companion-resource layout, fixed gameplay input isolation, timer/resource rendering, and runtime skill icon/name resolution.");
    return true;
}

void Shutdown() noexcept {
    HudInput::Shutdown();
    Core::BuffDisplays().EndSession();
    Internal::ResetSkillIconCache();
    Internal::ShutdownIconFrameBackend();
    CurrentSessionGeneration.store(0, std::memory_order_release);
    PollScheduled.store(false, std::memory_order_release);
    // Restore any native countdown backing buffers we qualified before dropping
    // their pointers. This is bounded/readability-checked and does not call UI
    // services from the loader shutdown thread.
    RestoreQualifiedCountdownBuffers();
    RestoreQualifiedTooltipBuffers();
    InvalidateWidgetHandles();

    RegisteredPanel = D2RL::Panels::InvalidHandle;
    DataTablesListener = D2RL::Lifecycle::InvalidHandle;
    GameplayListeners = {};
    FindTopLevelPanel = nullptr;
    FindChildWidgetByName = nullptr;
    Panels = nullptr;
    Widgets = nullptr;
    Threads = nullptr;
    Lifecycle = nullptr;
    Context = nullptr;
}

} // namespace BuffPanel::Systems::BuffHud
