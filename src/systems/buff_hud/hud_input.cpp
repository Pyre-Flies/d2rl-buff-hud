#include "hud_input.hpp"

#include "core/services.hpp"
#include "systems/blood_aura/blood_aura.hpp"
#include "systems/blood_aura/blood_aura_policy.hpp"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace BuffPanel::Systems::BuffHud::HudInput {
namespace {

using BloodAura::Policy::ScreenBox;

const D2RL::PluginContext* Context{};
HoverCallback OnHover{};
HHOOK MessageHook{};

std::mutex AreasMutex;
SlotAreas Areas{};
int ToggleSlot{-1};

std::atomic<int> HoveredSlot{-1};
std::atomic<bool> HoverQueued{};
std::atomic<bool> ConsumeNextUp{};
std::atomic<bool> ToggleQueued{};
std::atomic<std::uint64_t> Clicks{};
std::atomic<std::uint64_t> Hovers{};
std::atomic<int> LastClientWidth{};
std::atomic<int> LastClientHeight{};

// The slot under a screen point of the game window, or -1. `toggle` is set when
// it is the Blood Golem aura's slot.
[[nodiscard]] int SlotAt(HWND window, POINT screenPoint, bool& toggle) noexcept {
    toggle = false;
    RECT client{};
    POINT point = screenPoint;
    if (window == nullptr || !GetClientRect(window, &client) || !ScreenToClient(window, &point)) return -1;
    const auto width = static_cast<float>(client.right - client.left);
    const auto height = static_cast<float>(client.bottom - client.top);
    LastClientWidth.store(client.right - client.left, std::memory_order_relaxed);
    LastClientHeight.store(client.bottom - client.top, std::memory_order_relaxed);
    std::lock_guard lock(AreasMutex);
    for (std::size_t i = 0; i < Areas.size(); ++i) {
        const auto& area = Areas[i];
        ScreenBox box{};
        if (!area.visible || !BloodAura::Policy::LayoutToScreen(area.x, area.y, area.size, width, height, box)) continue;
        if (!BloodAura::Policy::Contains(box, static_cast<float>(point.x), static_cast<float>(point.y))) continue;
        toggle = static_cast<int>(i) == ToggleSlot;
        return static_cast<int>(i);
    }
    return -1;
}

void __cdecl HoverOnUiThread(const D2RL::PluginContext*, void*) noexcept {
    HoverQueued.store(false, std::memory_order_release);
    if (OnHover != nullptr) OnHover(HoveredSlot.load(std::memory_order_acquire));
}

void QueueHover() noexcept {
    const auto* threads = Core::Services().threads;
    if (threads == nullptr || HoverQueued.exchange(true, std::memory_order_acq_rel)) return;
    if (threads->runOnUiThread(Context, &HoverOnUiThread, nullptr) != D2RL::Threads::Result::Success) {
        HoverQueued.store(false, std::memory_order_release);
    }
}

void __cdecl ToggleOnUiThread(const D2RL::PluginContext*, void*) noexcept {
    ToggleQueued.store(false, std::memory_order_release);
    (void)BloodAura::Toggle();
}

LRESULT CALLBACK OnMessage(int code, WPARAM removed, LPARAM data) {
    if (code >= 0 && removed == PM_REMOVE && data != 0) {
        auto& message = *reinterpret_cast<MSG*>(data);
        bool toggle = false;
        if (message.message == WM_MOUSEMOVE) {
            const int slot = SlotAt(message.hwnd, message.pt, toggle);
            if (HoveredSlot.exchange(slot, std::memory_order_acq_rel) != slot) {
                if (slot >= 0) Hovers.fetch_add(1, std::memory_order_relaxed);
                QueueHover();
            }
        } else if (message.message == WM_LBUTTONDOWN || message.message == WM_LBUTTONDBLCLK) {
            if (SlotAt(message.hwnd, message.pt, toggle) >= 0 && toggle) {
                // Consume the click: it must not also walk or cast there.
                message.message = WM_NULL;
                message.wParam = 0;
                message.lParam = 0;
                ConsumeNextUp.store(true, std::memory_order_relaxed);
                Clicks.fetch_add(1, std::memory_order_relaxed);
                const auto* threads = Core::Services().threads;
                if (threads != nullptr && !ToggleQueued.exchange(true, std::memory_order_acq_rel)
                    && threads->runOnUiThread(Context, &ToggleOnUiThread, nullptr) != D2RL::Threads::Result::Success) {
                    ToggleQueued.store(false, std::memory_order_release);
                }
            }
        } else if (message.message == WM_LBUTTONUP && ConsumeNextUp.exchange(false, std::memory_order_relaxed)) {
            message.message = WM_NULL;
            message.wParam = 0;
            message.lParam = 0;
        }
    }
    return CallNextHookEx(MessageHook, code, removed, data);
}

[[nodiscard]] HWND FindGameWindow() noexcept {
    HWND window = nullptr;
    EnumWindows(
        [](HWND candidate, LPARAM out) -> BOOL {
            DWORD process = 0;
            GetWindowThreadProcessId(candidate, &process);
            if (process == GetCurrentProcessId() && IsWindowVisible(candidate) && GetWindow(candidate, GW_OWNER) == nullptr) {
                *reinterpret_cast<HWND*>(out) = candidate;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&window));
    return window;
}

} // namespace

void Initialize(const D2RL::PluginContext* context, HoverCallback onHover) noexcept {
    Shutdown();
    Context = context;
    OnHover = onHover;
}

void InstallInput() noexcept {
    if (MessageHook != nullptr || Context == nullptr) return;
    const HWND window = FindGameWindow();
    if (window == nullptr) {
        Context->LogWarn("Buff HUD: game window not ready; buff names on hover and the Blood Golem aura click are off until the next game.");
        return;
    }
    MessageHook = SetWindowsHookExW(WH_GETMESSAGE, &OnMessage, nullptr, GetWindowThreadProcessId(window, nullptr));
    if (MessageHook == nullptr) {
        Context->LogWarn("Buff HUD: could not watch the game window's mouse input; no buff names on hover and no Blood Golem aura click (use buff-blood-aura).");
    } else {
        Context->LogInfo("Buff HUD: watching the mouse over the HUD for buff names and the Blood Golem aura click.");
    }
}

void Publish(const SlotAreas& slots, int toggleSlot) noexcept {
    std::lock_guard lock(AreasMutex);
    Areas = slots;
    ToggleSlot = toggleSlot;
}

void Describe(char* output, std::size_t size) noexcept {
    int toggleSlot{};
    std::size_t shown{};
    {
        std::lock_guard lock(AreasMutex);
        toggleSlot = ToggleSlot;
        for (const auto& area : Areas) shown += area.visible ? 1u : 0u;
    }
    std::snprintf(output, size, "input=%s slotsShown=%zu toggleSlot=%d hovered=%d hovers=%llu clicks=%llu client=%dx%d",
        MessageHook != nullptr ? "on" : "off", shown, toggleSlot, HoveredSlot.load(),
        static_cast<unsigned long long>(Hovers.load()), static_cast<unsigned long long>(Clicks.load()),
        LastClientWidth.load(), LastClientHeight.load());
}

void Shutdown() noexcept {
    if (MessageHook != nullptr) {
        UnhookWindowsHookEx(MessageHook);
        MessageHook = nullptr;
    }
    {
        std::lock_guard lock(AreasMutex);
        Areas = {};
        ToggleSlot = -1;
    }
    HoveredSlot.store(-1);
    HoverQueued.store(false);
    ConsumeNextUp.store(false);
    ToggleQueued.store(false);
    OnHover = nullptr;
    Context = nullptr;
}

} // namespace BuffPanel::Systems::BuffHud::HudInput
