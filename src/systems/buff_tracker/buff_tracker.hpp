#pragma once

#include "core/remote_buffs.hpp"

#include <D2RLPlugin/api.h>

namespace BuffPanel::Systems::BuffTracker {

[[nodiscard]] bool Initialize(const D2RL::PluginContext* context) noexcept;
void Shutdown() noexcept;

// Host side, on the game or d2rds server thread: the catalog buffs on one server
// player unit, for that player's client (see core/remote_buffs.hpp).
void CollectRemoteBuffs(void* serverUnit, Core::RemoteBuffList& list) noexcept;

// Joined client, any thread: shows the host's list of this player's buffs. A
// game clock of this process's own (a local game or a TCP/IP host) wins and the
// list is ignored.
void ApplyRemoteBuffs(const Core::RemoteBuffList& list) noexcept;

// Joined client UI poll: keeps the local 25 Hz remote clock advancing smoothly
// between 1-second host buff packets.
void TickRemoteClock() noexcept;

} // namespace BuffPanel::Systems::BuffTracker
