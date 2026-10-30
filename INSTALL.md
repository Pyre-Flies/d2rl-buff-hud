# Buff HUD 1.3.1+rev.3

Requires D2RLoader with PluginSDK 0.3.0 services / plugin ABI 4. Native hooks remain qualified only for D2R build 93847.

With the game closed, copy **both** files from `d2rloader/plugins/` into your selected scope:

- Ladder: `<game>/mods/ReimaginedLadder/d2rloader/plugins/`
- Another mod: `<game>/mods/<mod-name>/d2rloader/plugins/`
- Global: `<game>/d2rloader/plugins/`

Keep `d2rl-buffhud.dll` and `d2rl-buffhud.mpq` together (and remove any legacy `d2rl-buff-panel.dll` / `d2rl-buff-panel.mpq`). Install in one scope only. Include this pair in the ladder bundle; no files need to be added to the mod's MPQ. Keep the included license notices with redistributions.

The companion contains the panel layout (`data/global/ui/layouts/buffhud/BuffHudhd.json`), while buff definitions are derived at runtime from the active mod's Excel files (`states.txt`, `skills.txt`, `itemstatcost.txt`, with optional loose `buff-hud.txt` override). Existing active-mod overrides at the same virtual paths take priority.

Start the game and use `buff-panel status` (or `buff-blood-aura status`) for the compact production status line. Verify real Fade, Shout/Battle Orders/Battle Command, and resource buffs such as Bone Armor in game.

This fork preserves the upstream author's attribution. Packaging/build verification is not live-game qualification.
