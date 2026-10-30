#pragma once

// A dedicated server's game thread, offered to other plugins.
//
// D2RLoader's ThreadService::runOnGameThread queues work for the local
// player's game, so on a dedicated server - d2rds, which hosts many games and
// has no local player - it answers Unavailable, and every SDK item and
// inventory call built on it fails the same way. d2rds publishes this service
// instead (plugin id "d2rds", name "server-thread") so a shared plugin can still
// act on a joined player's game: queue a callback onto the server loop, and
// from there visit every connected client's game and player unit.
//
// Acquire it through PluginCommunicationService only when runOnGameThread is
// Unavailable; a local game or a player's TCP/IP host never publishes it.

#include <cstdint>

namespace D2RLServerThread {

inline constexpr const char* ProviderPluginId = "d2rds";
inline constexpr const char* ServiceName      = "server-thread";
inline constexpr uint32_t    ServiceVersion   = 1;

using Callback = void(__cdecl*)(void* userData) noexcept;

// game and player are the server's native D2Game* and player D2UnitStrc*, valid
// only for the duration of the call. Return false to stop.
using ClientVisitor = bool(__cdecl*)(uint32_t clientId, void* game, void* player, const char* characterName, void* userData) noexcept;

struct ServiceV1 {
	uint32_t structSize;
	uint32_t reserved;

	// Runs callback once on the server loop thread, before the next game frame.
	// Callable from any thread. False when the queue is full or the server is
	// not running.
	bool(__cdecl* runOnServerThread)(Callback callback, void* userData) noexcept;

	// Server loop thread only (inside a runOnServerThread callback). Visits each
	// connected client that has a player in a game; returns how many it visited.
	uint32_t(__cdecl* forEachClient)(ClientVisitor visitor, void* userData) noexcept;
};

inline constexpr uint32_t ServiceV1Size = static_cast<uint32_t>(sizeof(ServiceV1));

}
