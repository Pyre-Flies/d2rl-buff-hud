#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace D2RLImage {
// Read-only, UI-thread access to the same local player used by D2RCore's
// reload-character capture. The lifecycle ID is checked before reading its name.
class CharacterIdentity {
public:
    bool Resolve();
    std::string Read(uint32_t expectedPlayerId) const;
    // The local player unit itself, when it is a player with that ID.
    const void* Unit(uint32_t expectedPlayerId) const;
    const std::string& Reason() const { return reason; }
private:
    using IndexFn = uint32_t(__fastcall*)();
    using PlayerFn = const void*(__fastcall*)(uint32_t);
    IndexFn index{};
    PlayerFn player{};
    std::string reason = "not resolved";
};

// Encoding is reversible and cannot introduce separators, dots, reserved device
// names or case-only collisions on Windows. Scope separates mods from vanilla.
inline std::string CharacterPathComponent(const std::string& value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "id-";
    for (unsigned char c : value) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') out += static_cast<char>(c);
        else { out += '~'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
inline std::filesystem::path CharacterStateFile(const std::filesystem::path& directory,
    const std::string& mod, const std::string& name) {
    if (directory.empty() || name.empty()) return {};
    return directory / "characters" / (mod.empty() ? "vanilla" : "mod-" + CharacterPathComponent(mod))
        / (CharacterPathComponent(name) + ".tsv");
}
}
