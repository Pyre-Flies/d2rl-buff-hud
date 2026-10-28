# Buff HUD — D2RLoader plugin

A standalone 3×7 temporary-buff HUD and countdown tracker.

## Acknowledgements

Many thanks to [RuffnecKk](https://github.com/RuffDood) for being a great teacher and awesome dude.
Make sure to check out his amazing plugin [suite](https://github.com/RuffDood/RuffnecKk-D2RLoader-Suite)!

Huge thank you also goes to [Collin](https://github.com/CollinHerber) for contributing to the codebase. Make sure to check his mod [D2R:Reimagined](https://www.d2r-reimagined.com/)!

## What it provides

- 21 reusable display-only buff slots in a 3×7, lower-left-fill panel.
- Timed entries with countdowns based on the game's authoritative 25-frame-per-second clock.
- Resource entries with an optional current/max stat counter (e.g. Bone Armor, Cyclone Armor, Psychic Ward), independent of time remaining.
- Automatic icon and localized name discovery using game Skills/SkillDesc and original skill icon atlases (Amazon through Warlock and Global).
- Automatic runtime buff catalog derived directly from the active mod's Excel files (`states.txt`, `skills.txt`, and `itemstatcost.txt`), with optional loose `buff-hud.txt` override support and built-in fallback.
- Excludes curses and shared-stash proxy states; built-in semantic checks on native buff metadata.

### Runtime catalog derivation

Instead of requiring a compiled custom table in the companion MPQ, the plugin reads the active mod's `data/global/excel/{states.txt, skills.txt, itemstatcost.txt}` once during table load:

1. **Resource buffs:** Any skill in `skills.txt` with a non-curse `aurastate` where `aurastat1` and `aurastat2 == aurastat1 + "max"` resolve to valid stat IDs in `itemstatcost.txt` (e.g. `Bone Armor`, `Cyclone Armor`, `Psychic Ward`). Because stat IDs are resolved by name from the active mod's `itemstatcost.txt`, mod-remapped stats (such as Reimagined's `psychicward` / `psychicwardmax` at `427`/`428`) work automatically without editing a static table.
2. **Skill-driven timer buffs:** Any player-class or item-proc buff skill in `skills.txt` with a non-curse, non-red-debuff `aurastate` and non-empty `auralencalc`. Party warcries (`Shout`, `Battle Orders`, `Battle Command`) automatically record their skill ID as a fallback when `aurastate == auratargetstate`.
3. **Standalone state timer buffs:** Non-curse, non-red states in `states.txt` for transformations (`wolf`, `bear`, `delerium`), `cloak_of_shadows`, temporary resistance/stamina potions (`antidote`, `thawing`, `staminapot`), and shrines (`shrine_*`).

If a mod author wants to provide an explicit override table, placing a loose `data/global/excel/d2rloader/buff-panel/buff-hud.txt` file in the mod folder takes priority over automatic derivation:

```text
name\tstate_id\tdisplay_type\tvalue_stat\tmax_stat\tskill_id\tvalue_shift\tenabled
fade\t159\ttimer\t0\t0\t0\t0\t1
bone_armor\t14\tresource\t132\t133\t68\t8\t1
```

## Installation

Copy **both** `d2rl-buff-panel.dll` and `d2rl-buff-panel.mpq` into the plugin directory:

```text
<game>/mods/<mod-name>/d2rloader/plugins/
<game>/d2rloader/plugins/
```

The companion MPQ contains only the layout JSON:

```text
data/global/ui/layouts/buff-panel/BuffHudhd.json
```

## Customizing the panel position

Edit `companion/data/global/ui/layouts/buff-panel/BuffHudhd.json` (or override `data/global/ui/layouts/buff-panel/BuffHudhd.json` in your mod) to adjust `BuffGrid.fields.rect.x` and `.y`. The default is `x: -150, y: -515`; increasing `x` moves right, and making `y` more negative moves up. Keep the outer anchor and the 21 child slots unchanged for a simple reposition.

## Known limitations / verification

- Qualified low-level offsets and bridge signatures target D2R build **93847** and fail closed on mismatch.
- `buff-panel` owns native patching; do not run it alongside another uncoordinated plugin that changes the same bridge without safe chaining.
