#include "core/remote_buffs.hpp"
#include "systems/blood_aura/blood_aura_policy.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace BuffPanel::Systems::BloodAura::Policy;

namespace {

Preference MakePreference(const char* name, std::uint8_t off, std::uint8_t version = PreferenceVersion) {
    Preference preference{};
    preference.version = version;
    preference.auraOff = off;
    preference.playerId = 7;
    std::memcpy(preference.name, name, std::strlen(name) < NameBytes ? std::strlen(name) : NameBytes);
    return preference;
}

void Names() {
    Name name{};
    assert(CopyName("Bonemancer", name) && std::strcmp(name, "Bonemancer") == 0);
    assert(!CopyName("", name));
    assert(!CopyName(nullptr, name));
    // 15 characters plus the terminator fit; 16 without one do not.
    assert(CopyName("ABCDEFGHIJKLMNO", name));
    const char unterminated[NameBytes] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P'};
    assert(!CopyName(unterminated, name));
    assert(!CopyName("Bad\x01Name", name));
    assert(!CopyName("Na\xC3\xAFve", name));

    assert(SameName("Bonemancer", "bONEMANCER"));
    assert(SameName("Blood_Golem-1", "blood_golem-1"));
    assert(!SameName("Bonemancer", "Bonemancers"));
    assert(!SameName("Bonemancer", "Bonemance"));
}

void Wire() {
    Name name{};
    bool off = false;
    bool wants = true;
    auto preference = MakePreference("Spelly", 1);
    assert(DecodePreference(&preference, sizeof(preference), name, off, wants) && off && !wants
        && std::strcmp(name, "Spelly") == 0);
    preference = MakePreference("Spelly", 0);
    preference.flags = WantsBuffsFlag;
    assert(DecodePreference(&preference, sizeof(preference), name, off, wants) && !off && wants);

    preference.flags = 0x0002; // unknown flag
    assert(!DecodePreference(&preference, sizeof(preference), name, off, wants));
    preference = MakePreference("Spelly", 2);
    assert(!DecodePreference(&preference, sizeof(preference), name, off, wants));
    preference = MakePreference("Spelly", 1, 2);
    assert(!DecodePreference(&preference, sizeof(preference), name, off, wants));
    preference = MakePreference("", 1);
    assert(!DecodePreference(&preference, sizeof(preference), name, off, wants));
    preference = MakePreference("Spelly", 1);
    assert(!DecodePreference(&preference, sizeof(preference) - 1, name, off, wants));
    assert(!DecodePreference(nullptr, sizeof(preference), name, off, wants));
    // A full 16-byte name has no terminator inside the field.
    preference = MakePreference("ABCDEFGHIJKLMNOP", 1);
    assert(!DecodePreference(&preference, sizeof(preference), name, off, wants));
}

void RemoteBuffs() {
    using namespace BuffPanel::Core;
    RemoteBuffList list{};
    list.buffs[0] = RemoteBuff{.state = 32, .skill = 149, .value = 7500};
    list.buffs[1] = RemoteBuff{.state = 14, .skill = 68, .value = 780, .maximum = 780};
    list.count = 2;
    list.flags = RemoteBuffsInBloodAura;
    std::array<std::uint8_t, RemoteBuffsMaximumBytes> payload{};
    const auto size = EncodeRemoteBuffs(list, payload);
    assert(size == RemoteBuffsHeaderBytes + 2 * sizeof(RemoteBuff));
    RemoteBuffList decoded{};
    assert(DecodeRemoteBuffs(payload.data(), size, decoded) && decoded.count == 2);
    assert(decoded.flags == RemoteBuffsInBloodAura);
    // An empty list still carries the aura flag: Blood Golem with no other buffs.
    RemoteBuffList auraOnly{};
    auraOnly.flags = RemoteBuffsInBloodAura;
    std::array<std::uint8_t, RemoteBuffsMaximumBytes> auraPayload{};
    RemoteBuffList auraDecoded{};
    assert(DecodeRemoteBuffs(auraPayload.data(), EncodeRemoteBuffs(auraOnly, auraPayload), auraDecoded)
        && auraDecoded.count == 0 && auraDecoded.flags == RemoteBuffsInBloodAura);
    assert(decoded.buffs[0].state == 32 && decoded.buffs[0].skill == 149 && decoded.buffs[0].value == 7500);
    assert(decoded.buffs[1].value == 780 && decoded.buffs[1].maximum == 780);

    assert(!DecodeRemoteBuffs(payload.data(), size - 1, decoded));
    assert(!DecodeRemoteBuffs(payload.data(), 2, decoded));
    assert(!DecodeRemoteBuffs(nullptr, size, decoded));
    payload[0] = 2; // unknown version
    assert(!DecodeRemoteBuffs(payload.data(), size, decoded));

    // A full list fits one network message; extra buffs are left out.
    RemoteBuffList full{};
    full.count = MaximumRemoteBuffs + 5;
    assert(EncodeRemoteBuffs(full, payload) == RemoteBuffsMaximumBytes - (RemoteBuffsMaximumBytes - RemoteBuffsHeaderBytes) % sizeof(RemoteBuff));
    assert(DecodeRemoteBuffs(payload.data(), RemoteBuffsHeaderBytes + MaximumRemoteBuffs * sizeof(RemoteBuff), decoded)
        && decoded.count == MaximumRemoteBuffs);
    static_assert(MaximumRemoteBuffs >= 18);
}

void Sidecar() {
    assert(!ParseSidecarAuraOff(""));
    assert(ParseSidecarAuraOff(FormatSidecar(true)));
    assert(!ParseSidecarAuraOff(FormatSidecar(false)));
    assert(ParseSidecarAuraOff("blood_golem_aura\toff\r\n"));
    assert(!ParseSidecarAuraOff("# blood_golem_aura\toff\n"));
    assert(!ParseSidecarAuraOff("blood_golem_aura off\n"));
    assert(!ParseSidecarAuraOff("blood_golem_aura\tOFF\n"));
    // The last line for the key wins; other keys are ignored.
    assert(!ParseSidecarAuraOff("blood_golem_aura\toff\nblood_golem_aura\ton\n"));
    assert(ParseSidecarAuraOff("other\ton\nblood_golem_aura\toff"));
}

void Screen() {
    ScreenBox box{};
    // 1920x1080: half scale. A box at (-84, -303) from the bottom centre.
    assert(LayoutToScreen(-84.0f, -303.0f, 30.0f, 1920.0f, 1080.0f, box));
    assert(box.left == 960.0f - 42.0f && box.top == 1080.0f - 151.5f);
    assert(box.right == box.left + 15.0f && box.bottom == box.top + 15.0f);
    assert(Contains(box, box.left, box.top));
    assert(!Contains(box, box.right, box.top));
    assert(!Contains(box, box.left - 1.0f, box.top));
    // 3840x2160 is the layout's own scale.
    assert(LayoutToScreen(-84.0f, -303.0f, 30.0f, 3840.0f, 2160.0f, box));
    assert(box.left == 1920.0f - 84.0f && box.bottom == 2160.0f - 303.0f + 30.0f);
    assert(!LayoutToScreen(0.0f, 0.0f, 30.0f, 0.0f, 1080.0f, box));
    assert(!LayoutToScreen(0.0f, 0.0f, 0.0f, 1920.0f, 1080.0f, box));
}

} // namespace

int main() {
    Names();
    Wire();
    RemoteBuffs();
    Sidecar();
    Screen();
    std::puts("PASS: Blood Golem aura names, settings and buff-list wire, sidecar and click geometry");
    return 0;
}
