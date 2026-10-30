#include <D2RLSecrets/session_secrets.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cctype>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <system_error>

namespace D2RLSecrets {
namespace {

// Anything bigger is not a file the launcher or provisioning wrote.
constexpr std::uintmax_t MaxFileBytes = 64 * 1024;

auto TrimLeft(std::string_view text) -> std::string_view {
	while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
		text.remove_prefix(1);
	}
	return text;
}

auto TrimRight(std::string_view text) -> std::string_view {
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
		text.remove_suffix(1);
	}
	return text;
}

// After a value: nothing, or a comment.
auto OnlyCommentLeft(std::string_view rest) -> bool {
	rest = TrimLeft(TrimRight(rest));
	return rest.empty() || rest.front() == '#';
}

// The raw text after `key =` on the root-level line that sets `key`.
auto FindValue(std::string_view toml, std::string_view key) -> std::optional<std::string_view> {
	if (toml.size() >= 3 && static_cast<unsigned char>(toml[0]) == 0xEF && static_cast<unsigned char>(toml[1]) == 0xBB
	    && static_cast<unsigned char>(toml[2]) == 0xBF) {
		toml.remove_prefix(3);
	}

	std::optional<std::string_view> found;
	while (!toml.empty()) {
		const size_t     end  = toml.find('\n');
		std::string_view line = TrimRight(TrimLeft(toml.substr(0, end)));
		toml                  = end == std::string_view::npos ? std::string_view {} : toml.substr(end + 1);

		if (line.empty() || line.front() == '#') {
			continue;
		}
		if (line.front() == '[') {
			break;
		}
		if (line.size() <= key.size() || line.substr(0, key.size()) != key) {
			continue;
		}

		std::string_view rest = TrimLeft(line.substr(key.size()));
		if (rest.empty() || rest.front() != '=') {
			continue;
		}
		// TOML forbids a key set twice; a file that does it is not one we wrote,
		// so neither value is trusted.
		if (found.has_value()) {
			return std::nullopt;
		}
		found = TrimLeft(rest.substr(1));
	}
	return found;
}

}

auto ParseString(std::string_view toml, std::string_view key) -> std::optional<std::string> {
	const auto raw = FindValue(toml, key);
	if (!raw.has_value() || raw->empty()) {
		return std::nullopt;
	}

	const std::string_view value = *raw;
	if (value.front() == '\'') {
		const size_t closing = value.find('\'', 1);
		if (closing == std::string_view::npos || !OnlyCommentLeft(value.substr(closing + 1))) {
			return std::nullopt;
		}
		return std::string(value.substr(1, closing - 1));
	}
	if (value.front() != '"') {
		return std::nullopt;
	}

	std::string out;
	for (size_t i = 1; i < value.size(); ++i) {
		const char c = value[i];
		if (c == '"') {
			if (!OnlyCommentLeft(value.substr(i + 1))) {
				return std::nullopt;
			}
			return out;
		}
		if (c != '\\') {
			out.push_back(c);
			continue;
		}
		if (++i == value.size()) {
			return std::nullopt;
		}
		switch (value[i]) {
		case '\\': out.push_back('\\'); break;
		case '"': out.push_back('"'); break;
		case 'n': out.push_back('\n'); break;
		case 't': out.push_back('\t'); break;
		case 'r': out.push_back('\r'); break;
		default: return std::nullopt;
		}
	}
	return std::nullopt; // unterminated
}

auto ParseInteger(std::string_view toml, std::string_view key) -> std::optional<int64_t> {
	const auto raw = FindValue(toml, key);
	if (!raw.has_value() || raw->empty()) {
		return std::nullopt;
	}

	std::string_view value = *raw;
	int64_t          sign  = 1;
	if (value.front() == '-' || value.front() == '+') {
		sign = value.front() == '-' ? -1 : 1;
		value.remove_prefix(1);
	}

	int64_t parsed = 0;
	size_t  digits = 0;
	while (digits < value.size() && value[digits] >= '0' && value[digits] <= '9') {
		if (parsed > (INT64_MAX - 9) / 10) {
			return std::nullopt;
		}
		parsed = parsed * 10 + (value[digits] - '0');
		++digits;
	}
	if (digits == 0 || !OnlyCommentLeft(value.substr(digits))) {
		return std::nullopt;
	}
	return sign * parsed;
}

namespace {

auto ReadFile(const std::filesystem::path& path) -> SecretsFile {
	std::error_code error;
	if (path.empty() || !std::filesystem::is_regular_file(path, error)) {
		return {};
	}
	const std::uintmax_t size = std::filesystem::file_size(path, error);
	if (error || size > MaxFileBytes) {
		return {SecretsFile::State::Invalid, {}};
	}

	std::ifstream stream(path, std::ios::binary);
	if (!stream) {
		return {SecretsFile::State::Invalid, {}};
	}
	std::string text {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
	if (stream.bad()) {
		return {SecretsFile::State::Invalid, {}};
	}
	return {SecretsFile::State::Valid, std::move(text)};
}

auto NowUnix() -> int64_t {
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}

auto SecretsDirectory() -> std::filesystem::path {
	std::wstring buffer(MAX_PATH, L'\0');
	for (;;) {
		const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (length == 0) {
			return {};
		}
		if (length < buffer.size()) {
			buffer.resize(length);
			break;
		}
		if (buffer.size() >= 32768) {
			return {};
		}
		buffer.resize(buffer.size() * 2);
	}
	return std::filesystem::path(buffer).parent_path() / DirectoryName;
}

auto ReadSessionFile(const std::filesystem::path& directory, int64_t nowUnix) -> SecretsFile {
	if (directory.empty()) {
		return {};
	}
	SecretsFile file = ReadFile(directory / SessionFileName);
	if (file.state != SecretsFile::State::Valid) {
		return file;
	}
	// A crashed or closed launcher never deleted it: the expiry is what keeps a
	// file from an old session from signing a later, unrelated launch in.
	const auto expires = ParseInteger(file.text, "expires_at_unix");
	if (!expires.has_value() || *expires <= nowUnix) {
		return {SecretsFile::State::Expired, {}};
	}
	return file;
}

auto ReadServerFile(const std::filesystem::path& directory) -> SecretsFile {
	if (directory.empty()) {
		return {};
	}
	return ReadFile(directory / ServerFileName);
}

auto ReadSessionFile() -> SecretsFile {
	if (IsDedicatedServer()) return {};
	return ReadSessionFile(SecretsDirectory(), NowUnix());
}

auto ReadServerFile() -> SecretsFile {
	return ReadServerFile(SecretsDirectory());
}

auto Resolve(const SecretsFile& file, std::string_view key, std::string_view pluginConfigValue) -> Secret {
	if (file.state == SecretsFile::State::Valid) {
		if (auto value = ParseString(file.text, key); value.has_value()) {
			return {std::move(*value), Source::SecretsFile};
		}
	}
	if (!pluginConfigValue.empty()) {
		return {std::string(pluginConfigValue), Source::PluginConfig};
	}
	return {};
}

auto DescribeSource(Source source, const char* secretsFileName, const char* pluginConfigName) -> std::string {
	switch (source) {
	case Source::SecretsFile: return std::string(DirectoryName) + "/" + secretsFileName;
	case Source::PluginConfig:
		return std::string(pluginConfigName) + " (developer override - that file is in the mod folder and can reach other players)";
	case Source::None: break;
	}
	return "nowhere";
}

namespace {

auto DescribeState(SecretsFile::State state) -> const char* {
	switch (state) {
	case SecretsFile::State::Missing: return "missing";
	case SecretsFile::State::Expired: return "expired";
	case SecretsFile::State::Invalid: return "unreadable";
	case SecretsFile::State::Valid: return "present";
	}
	return "unknown";
}

auto LoadSecret(const D2RL::PluginContext* context, const char* logPrefix, const char* key, std::string_view pluginConfigValue,
                const char* pluginConfigName, const SecretsFile& file, const char* fileName) -> std::string {
	Secret secret = Resolve(file, key, pluginConfigValue);
	if (context != nullptr) {
		std::string line = std::string(logPrefix) + ": " + key;
		if (secret.source == Source::None || secret.value.empty()) {
			line += " not set (" + std::string(DirectoryName) + "/" + fileName + " " + DescribeState(file.state) + ")";
			context->LogInfo(line.c_str());
		} else if (secret.source == Source::PluginConfig) {
			line += " from " + DescribeSource(secret.source, fileName, pluginConfigName);
			context->LogWarn(line.c_str());
		} else {
			line += " from " + DescribeSource(secret.source, fileName, pluginConfigName);
			context->LogInfo(line.c_str());
		}
	}
	return std::move(secret.value);
}

}

auto LoadSessionSecret(const D2RL::PluginContext* context, const char* logPrefix, const char* key,
                       std::string_view pluginConfigValue, const char* pluginConfigName) -> std::string {
	return LoadSecret(context, logPrefix, key, pluginConfigValue, pluginConfigName, ReadSessionFile(), SessionFileName);
}

auto LoadServerSecret(const D2RL::PluginContext* context, const char* logPrefix, const char* key,
                      std::string_view pluginConfigValue, const char* pluginConfigName) -> std::string {
	return LoadSecret(context, logPrefix, key, pluginConfigValue, pluginConfigName, ReadServerFile(), ServerFileName);
}

auto IsDedicatedServerCommandLine(std::wstring_view commandLine) -> bool {
	// Arguments as Windows splits them: whitespace outside double quotes, the
	// quotes themselves dropped.
	std::wstring argument;
	bool         quoted = false;
	auto isServerSwitch = [](const std::wstring& word) {
		static constexpr std::wstring_view Switch = L"-d2rds";
		if (word.size() != Switch.size()) return false;
		for (size_t i = 0; i < word.size(); ++i) {
			if (::towlower(word[i]) != Switch[i]) return false;
		}
		return true;
	};
	for (const wchar_t c : commandLine) {
		if (c == L'"') {
			quoted = !quoted;
		} else if (!quoted && (c == L' ' || c == L'\t')) {
			if (isServerSwitch(argument)) return true;
			argument.clear();
		} else {
			argument += c;
		}
	}
	return isServerSwitch(argument);
}

auto IsDedicatedServer() -> bool {
	static const bool server = [] {
		const wchar_t* commandLine = ::GetCommandLineW();
		return commandLine != nullptr && IsDedicatedServerCommandLine(commandLine);
	}();
	return server;
}

auto IsLadderMod(std::string_view activeMod) -> bool {
	constexpr std::string_view Ladder = LadderModName;
	if (activeMod.size() != Ladder.size()) return false;
	for (size_t i = 0; i < Ladder.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(activeMod[i])) != std::tolower(static_cast<unsigned char>(Ladder[i]))) {
			return false;
		}
	}
	return true;
}

auto IsLadderLaunch(const D2RL::PluginContext* context) -> bool {
	if (context != nullptr && context->activeMod != nullptr && IsLadderMod(context->activeMod)) return true;
	const SecretsFile session = ReadSessionFile();
	if (session.state != SecretsFile::State::Valid) return false;
	const auto ticket = ParseString(session.text, "ladder_launch_ticket");
	return ticket.has_value() && !ticket->empty();
}

}
