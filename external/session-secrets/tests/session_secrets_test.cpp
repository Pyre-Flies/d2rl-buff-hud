#include <D2RLSecrets/session_secrets.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace D2RLSecrets;

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
	if (!condition) {
		std::printf("FAIL: %s\n", what);
		++g_failures;
	}
}

void Write(const std::filesystem::path& path, const std::string& text) {
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	stream << text;
}

void Parsing() {
	Check(ParseString("access_token = \"abc.def\"\n", "access_token") == "abc.def", "basic string");
	Check(ParseString("access_token='lit\\eral' # note\n", "access_token") == "lit\\eral", "literal string keeps backslashes");
	Check(ParseString("\xEF\xBB\xBF" "access_token = \"x\"\r\n", "access_token") == "x", "BOM and CRLF");
	Check(ParseString("access_token = \"a\\\"b\\\\c\"", "access_token") == "a\"b\\c", "escapes");
	Check(ParseString("access_token = \"\"\n", "access_token") == "", "empty value is present");
	Check(!ParseString("access_token = \"bad\\q\"", "access_token").has_value(), "unknown escape rejected");
	Check(!ParseString("access_token = \"open", "access_token").has_value(), "unterminated rejected");
	Check(!ParseString("access_token = bare", "access_token").has_value(), "bare value rejected");
	Check(!ParseString("access_token = \"a\" junk", "access_token").has_value(), "trailing junk rejected");
	Check(!ParseString("access_token = \"a\"\naccess_token = \"b\"", "access_token").has_value(), "duplicate key rejected");
	Check(!ParseString("access_token_x = \"a\"", "access_token").has_value(), "longer key does not match");
	Check(!ParseString("# access_token = \"a\"", "access_token").has_value(), "comment ignored");
	Check(!ParseString("[table]\naccess_token = \"a\"", "access_token").has_value(), "table ends the root");
	Check(ParseInteger("expires_at_unix = 1790000000 # soon", "expires_at_unix") == 1790000000, "integer");
	Check(!ParseInteger("expires_at_unix = \"1\"", "expires_at_unix").has_value(), "quoted integer rejected");
	Check(!ParseInteger("expires_at_unix = 99999999999999999999", "expires_at_unix").has_value(), "overflow rejected");
}

void Files() {
	const auto dir = std::filesystem::temp_directory_path() / "d2rl-session-secrets-test";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);

	Check(ReadSessionFile(dir, 100).state == SecretsFile::State::Missing, "missing session file");
	Check(ReadSessionFile({}, 100).state == SecretsFile::State::Missing, "no directory");

	Write(dir / SessionFileName, "access_token = \"live\"\nexpires_at_unix = 200\nladder_launch_ticket = \"\"\n");
	auto session = ReadSessionFile(dir, 100);
	Check(session.state == SecretsFile::State::Valid, "valid session file");
	Check(ReadSessionFile(dir, 200).state == SecretsFile::State::Expired, "expired at the boundary");

	Write(dir / SessionFileName, "access_token = \"live\"\n");
	Check(ReadSessionFile(dir, 100).state == SecretsFile::State::Expired, "no expiry means ignored");

	Write(dir / ServerFileName, "lobby_server_key = \"k\"\n");
	auto server = ReadServerFile(dir);
	Check(server.state == SecretsFile::State::Valid, "server file never expires");
	Check(Resolve(server, "lobby_server_key", "old").value == "k", "server file wins");
	Check(Resolve(server, "api_token", "cfg").source == Source::PluginConfig, "key absent from file falls back");

	std::filesystem::remove_all(dir);
}

void Resolution() {
	const SecretsFile live {SecretsFile::State::Valid, "access_token = \"live\"\nladder_launch_ticket = \"\"\n"};
	auto secret = Resolve(live, "access_token", "stale");
	Check(secret.value == "live" && secret.source == Source::SecretsFile, "file beats a stale config token");

	secret = Resolve(live, "ladder_launch_ticket", "stale-ticket");
	Check(secret.value.empty() && secret.source == Source::SecretsFile, "an empty value in the file still wins");

	const SecretsFile expired {SecretsFile::State::Expired, {}};
	secret = Resolve(expired, "access_token", "dev");
	Check(secret.value == "dev" && secret.source == Source::PluginConfig, "developer override without a live file");

	secret = Resolve(expired, "access_token", "");
	Check(secret.value.empty() && secret.source == Source::None, "nothing anywhere");

	Check(DescribeSource(Source::SecretsFile, SessionFileName, "x.toml") == "reimagined-secrets/session.toml", "describe file");
}

}

void DedicatedServer() {
	Check(IsDedicatedServerCommandLine(L"\"C:\\Program Files (x86)\\Diablo II Resurrected\\D2RLoader.exe\" -mod Reimagined -txt -d2rds"),
		"the server launch");
	Check(IsDedicatedServerCommandLine(L"D2RLoader.exe -D2RDS -mod Reimagined"), "any case, any position");
	Check(!IsDedicatedServerCommandLine(L"\"C:\\Program Files (x86)\\Diablo II Resurrected\\D2RLoader.exe\" -mod ReimaginedLadder -txt"),
		"a player's launch");
	Check(!IsDedicatedServerCommandLine(L"D2RLoader.exe -d2rds-dryrun"), "the dry run is not a server");
	Check(!IsDedicatedServerCommandLine(L"\"C:\\games\\-d2rds folder\\D2RLoader.exe\" -mod Reimagined"),
		"inside a quoted path does not count");
	Check(!IsDedicatedServerCommandLine(L""), "an empty command line");
}

void LadderMod() {
	Check(IsLadderMod("ReimaginedLadder") && IsLadderMod("reimaginedladder"), "the ladder mod, any case");
	Check(!IsLadderMod("Reimagined") && !IsLadderMod("ReimaginedLadder2") && !IsLadderMod(""), "other mods are not the ladder");
}

int main() {
	Parsing();
	Files();
	Resolution();
	DedicatedServer();
	LadderMod();
	if (g_failures != 0) {
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("session-secrets: all tests passed\n");
	return 0;
}
