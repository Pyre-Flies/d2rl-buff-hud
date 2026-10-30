from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)(?:\+rev\.(\d+))?", version)
assert match is not None, f"Invalid VERSION: {version!r}"
major, minor, patch, rev = match.groups()
rev = rev or "0"

plugin = (ROOT / "src/plugin.cpp").read_text(encoding="utf-8")
assert f'.version = "{version}"' in plugin
assert '.id = "buffhud"' in plugin
assert '.name = "Buff HUD"' in plugin
assert f"Buff HUD {version} requires" in plugin
assert f"Buff HUD {version} loaded" in plugin

rc = (ROOT / "src/plugin.rc").read_text(encoding="utf-8")
numeric = f"{major},{minor},{patch},{rev}"
assert f"FILEVERSION {numeric}" in rc
assert f"PRODUCTVERSION {numeric}" in rc
assert f'VALUE "FileVersion", "{version}"' in rc
assert f'VALUE "ProductVersion", "{version}"' in rc
assert 'VALUE "InternalName", "buffhud"' in rc
assert 'VALUE "OriginalFilename", "d2rl-buffhud.dll"' in rc

for relative in (
    "src/systems/buff_hud/buff_hud.cpp",
    "src/systems/buff_tracker/buff_tracker.cpp",
):
    text = (ROOT / relative).read_text(encoding="utf-8")
    versions = set(re.findall(r"Buff HUD (\d+\.\d+\.\d+(?:\+rev\.\d+)?)", text))
    assert versions == {version}, f"{relative}: runtime versions {sorted(versions)} != {version}"

print(f"PASS: VERSION {version} matches DLL metadata, resources and runtime logs")
