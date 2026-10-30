#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <D2RLImage/character_identity.h>
#include <D2RLImage/image_scan.h>
#include <cstring>
#include <vector>

namespace D2RLImage {
bool CharacterIdentity::Resolve() {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"D2RCore.dll"));
    std::vector<D2RLImage::Section> sections;
    if (!base || !D2RLImage::ReadSections(base, sections)) { reason="D2RCore image unavailable"; return false; }
    const auto* code = D2RLImage::FindSection(sections,".text");
    if (!code) { reason="D2RCore code unavailable"; return false; }
    // Core reload capture: active client index -> local player unit -> type 0.
    // Its following name comparison reads unit+0x10, and its reload caller
    // copies the same player-data string with a 64-byte terminator bound.
    constexpr uint8_t pattern[] = {0xff,0x15,0,0,0,0,0x90,0x89,0xc1,0xff,0x15,0,0,0,0,0x90,0x48,0x85,0xc0,0x74,0x05,0x83,0x38,0,0x74,0x15,
        0x31,0xc9,0x89,0xc8,0x48,0x83,0xc4,0x28,0x5b,0x5f,0x5e,0x41,0x5c,0x41,0x5d,0x41,0x5e,0x41,0x5f,0x5d,0xc3,0x48,0x83,0x78,0x10,0};
    uint8_t mask[sizeof(pattern)]; std::memset(mask,0xff,sizeof(mask));
    std::memset(mask+2,0,4); std::memset(mask+11,0,4);
    std::vector<uint32_t> hits; D2RLImage::FindPattern(*code,pattern,mask,sizeof(pattern),2,hits);
    if (hits.size()!=1) { reason="local-character signature did not match uniquely"; return false; }
    const auto start=base+hits[0];
    auto target=[&](size_t offset) -> uintptr_t {
        int32_t displacement{}; std::memcpy(&displacement,reinterpret_cast<void*>(start+offset+2),4);
        const auto slot=start+offset+6+displacement;
        if (!D2RLImage::Readable(reinterpret_cast<void*>(slot),sizeof(uintptr_t))) return 0;
        uintptr_t pointer{}; std::memcpy(&pointer,reinterpret_cast<void*>(slot),sizeof(pointer));
        MEMORY_BASIC_INFORMATION info{};
        if (!pointer || !VirtualQuery(reinterpret_cast<void*>(pointer),&info,sizeof(info))
            || !(info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return 0;
        return pointer;
    };
    index=reinterpret_cast<IndexFn>(target(0)); player=reinterpret_cast<PlayerFn>(target(9));
    if (!index || !player) { reason="local-character function pointers unavailable"; return false; }
    reason.clear(); return true;
}
const void* CharacterIdentity::Unit(uint32_t expectedPlayerId) const {
    if (!index || !player) return nullptr;
    auto check=[&](uint32_t slot, bool requireId) -> const uint8_t* {
        if (slot>7) return nullptr;
        const auto* unit=static_cast<const uint8_t*>(player(slot));
        if (!D2RLImage::Readable(unit,0x18)) return nullptr;
        uint32_t type{},id{};
        std::memcpy(&type,unit,4); std::memcpy(&id,unit+8,4);
        if (type!=0) return nullptr;
        if (requireId && id!=expectedPlayerId) return nullptr;
        return unit;
    };
    const auto active=index();
    if (const auto* unit=check(active,true)) return unit;
    if (expectedPlayerId!=0) {
        for (uint32_t slot=0;slot<8;++slot) {
            if (slot==active) continue;
            if (const auto* unit=check(slot,true)) return unit;
        }
    }
    return check(active,false);
}
std::string CharacterIdentity::Read(uint32_t expectedPlayerId) const {
    const auto* unit=static_cast<const uint8_t*>(Unit(expectedPlayerId));
    if (!unit) return {};
    const char* name{};
    std::memcpy(&name,unit+0x10,sizeof(name));
    if (!name) return {};
    std::string result;
    for (size_t i=0;i<64;++i) {
        if (!D2RLImage::Readable(name+i,1)) return {};
        const auto c=static_cast<unsigned char>(name[i]);
        if (!c) return result;
        if (c<0x20 || c==0x7f) return {};
        result+=static_cast<char>(c);
    }
    return {}; // Never guess at an unterminated name.
}
}
