// Secrets that must never live in the mod folder.
//
// D2RLoader hands a host's mods/<mod> tree to players who join it, configs
// included, so a bearer token written into d2rloader/config/<plugin>.toml ends
// up on other people's machines. Secrets live here instead:
//
//   <game folder>/reimagined-secrets/session.toml   written by the launcher
//       before every launch and deleted when the game exits: access_token,
//       ladder_launch_ticket, status_session_id and expires_at_unix. A file
//       whose expires_at_unix is missing or past is ignored as a whole.
//   <game folder>/reimagined-secrets/server.toml    written once by dedicated
//       server provisioning: lobby_server_key, api_token. Never expires.
//
// The game folder is the one holding the running executable, D2R.exe, which is
// the install root on players and servers alike - outside mods/, so nothing
// D2RLoader shares ever reaches it.
//
// Every plugin follows one rule (Resolve): a key the secrets file carries wins,
// even when its value is empty, so a stale token left in an old config can never
// stand in for the live session. Only when the file is absent, expired or lacks
// the key does the plugin's own config value count - a developer override for
// running the game without the launcher, logged as such.

#pragma once

#include <D2RLPlugin/api.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace D2RLSecrets {

inline constexpr const char* DirectoryName   = "reimagined-secrets";
inline constexpr const char* SessionFileName = "session.toml";
inline constexpr const char* ServerFileName  = "server.toml";

struct SecretsFile {
	enum class State : uint8_t {
		Missing, // no file, or no game folder to look in
		Expired, // session file past (or without) expires_at_unix
		Invalid, // present but unreadable
		Valid,
	};

	State       state = State::Missing;
	std::string text; // the file's contents, only when Valid
};

enum class Source : uint8_t {
	None,
	SecretsFile,
	PluginConfig,
};

struct Secret {
	std::string value;
	Source      source = Source::None;
};

// <folder of the running executable>/reimagined-secrets; empty when the
// executable's path cannot be read.
auto SecretsDirectory() -> std::filesystem::path;

// session.toml from SecretsDirectory(), checked against the current time.
// Always Missing in a dedicated server (-d2rds): the file is a player's, and a
// local test server shares its install - and so its secrets folder - with the
// player's client (2026-10-25: the launcher's local d2rds rig).
auto ReadSessionFile() -> SecretsFile;
// server.toml from SecretsDirectory().
auto ReadServerFile() -> SecretsFile;

// The same reads against an explicit folder and clock, for tests.
auto ReadSessionFile(const std::filesystem::path& directory, int64_t nowUnix) -> SecretsFile;
auto ReadServerFile(const std::filesystem::path& directory) -> SecretsFile;

// The rule every plugin follows - see the top of this file.
auto Resolve(const SecretsFile& file, std::string_view key, std::string_view pluginConfigValue) -> Secret;

// "reimagined-secrets/session.toml"-style words for a log line.
auto DescribeSource(Source source, const char* secretsFileName, const char* pluginConfigName) -> std::string;

// What a plugin calls once at load: Resolve() against session.toml (or
// server.toml), plus one log line naming where `key` came from - never its
// value. `pluginConfigValue` is what the plugin read from its own config, and
// `pluginConfigName` that file's name, for the log.
auto LoadSessionSecret(const D2RL::PluginContext* context, const char* logPrefix, const char* key,
                       std::string_view pluginConfigValue, const char* pluginConfigName) -> std::string;
auto LoadServerSecret(const D2RL::PluginContext* context, const char* logPrefix, const char* key,
                      std::string_view pluginConfigValue, const char* pluginConfigName) -> std::string;

// True when this process is a dedicated server: D2RLoader launched with -d2rds
// (the switch that wakes d2rds up; see plugins/d2rds). A server holds no
// player session - no session.toml, no access token - so a plugin that talks
// to the API as a player (chat, notices, feeds) skips its connection there
// rather than being refused with HTTP 401 forever (2026-10-05: global-chat
// logged "refused the upgrade with HTTP 401; retrying in 60s" every minute).
auto IsDedicatedServer() -> bool;

// The rule IsDedicatedServer applies to the process command line, for tests:
// a whole argument -d2rds, any case. -d2rds-dryrun is not a server.
auto IsDedicatedServerCommandLine(std::wstring_view commandLine) -> bool;

// The ladder's mod folder name; the launcher installs the ladder bundle there.
inline constexpr const char* LadderModName = "ReimaginedLadder";

// True when `activeMod` is the ladder's mod, any case.
auto IsLadderMod(std::string_view activeMod) -> bool;

// True on a ladder launch: the ladder's mod is active, or the launcher's live
// session.toml carries a ladder_launch_ticket. Debug and test commands are off
// there, and settings that shape a ladder's game come from the DLL, not from
// the player's config. Reads session.toml on every call; call it at load.
auto IsLadderLaunch(const D2RL::PluginContext* context) -> bool;

// Pure parsing, exposed for tests. Root-level `key = "value"` or `key = 'value'`
// lines only; a table header ends the root table. nullopt when the key is absent
// or its value is not a well-formed string.
auto ParseString(std::string_view toml, std::string_view key) -> std::optional<std::string>;
auto ParseInteger(std::string_view toml, std::string_view key) -> std::optional<int64_t>;

}
