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

If a mod author wants to provide an explicit override table, placing a loose `data/global/excel/d2rloader/buffhud/buff-hud.txt` file in the mod folder takes priority over automatic derivation:

```text
name\tstate_id\tdisplay_type\tvalue_stat\tmax_stat\tskill_id\tvalue_shift\tenabled
fade\t159\ttimer\t0\t0\t0\t0\t1
bone_armor\t14\tresource\t132\t133\t68\t8\t1
```

## Blood Golem aura

Reimagined's Blood Golem carries Blood Aura, which puts every player near the
golem in `blood_mana`: their skills cost **life instead of mana**, and they get
the aura's bonus magic damage and life after each kill. Some builds would
rather not have it, so the HUD shows the aura in its first slot with D2R's own
checkbox on it. Clicking the aura's icon flips it:

- **Ticked** (default): the aura works as designed.
- **Empty**: the aura does nothing to this character - skills cost mana
  again and the bonuses are gone. The icon stays, labelled `OFF`, so it can be
  ticked again.

The choice is saved per character in
`%USERPROFILE%\Saved Games\Diablo II Resurrected\.buffhud\characters\mod-id-<mod>\id-<character>.tsv`
(`blood_golem_aura<TAB>off`). `buff-blood-aura [status|on|off]` in the console
does the same as the checkbox and prints what the host is doing.

How it works: the client sends its choice to whoever runs the game over
D2RLoader's plugin network (channel 1, `src/systems/blood_aura`). That copy - this
process in a local game, the player's TCP/IP host, or d2rds - then:

1. answers "not in Blood Mana" for that player at the engine's three reads of the
   state (server skill cast, server SpendMana, client mana prediction), which are
   pointed at the plugin through a 12-byte relay in an int3 pad; and
2. suppresses the state's stat list for that player with the engine's own
   `STATLIST_SuppressStateStats` (the routine inactive set bonuses use), from a
   scan on the game thread (d2rds: its server thread) every half second. The
   state itself stays on, which is what keeps the icon.

The checkbox is `AuraCheck` in slot 0 of `BuffHudhd.json`; the aura's entry has
the top priority so it always lands there. Its clicks are taken from the game
window's messages (`src/systems/buff_hud/hud_input.cpp`), because the HUD's own
widgets are click-through. It appears only when the game's host runs this
version and accepted the channel; otherwise the icon shows without a box. Every
native address is registered in `docs/loader-updates/native-deps/buffhud.toml`
(`buffhud.blood-aura.*`); a witness mismatch turns only this feature off.

## Buff names on hover

Hovering a buff names it in D2R's tooltip frame just above the top occupied
row (`HoverBox<row><S|M|L>` in `BuffHudhd.json`: a layout box can neither size
itself to its text nor move, so there are three widths at three heights and the
HUD shows the one that fits). The slots themselves stay disabled so world clicks
pass through; the hover comes from the same window-message watch as the
checkbox. While the Blood Golem aura is switched off it keeps its slot even out
of range - the client loses the state once the server suppresses it, and the
slot is the only way to turn it back on.
The old per-slot `Tooltip` widgets are gone: input isolation always disabled
them, and every SDK widget lookup holds a handle - adding widgets to all 21
slots broke handle resolution in game, so new widgets here are one of each.

## Joined games (dedicated servers, someone else's TCP/IP host)

A player who joined someone else's game has no game clock of their own: their
client's copy of their unit has no game, so the local tracker cannot read it
(the log says `ignoring a buff post ... it has no game`). Their Buff HUD tells
the host so (a flag in the settings message), and the host's scan reads that
player's server unit with the same catalog rules and sends the list about once a
second (message 2 on channel 1, `src/core/remote_buffs.hpp`). The client counts
timers down on a 25 Hz clock started when it joined. Both sides need this
version.

## Installation

Copy **both** `d2rl-buffhud.dll` and `d2rl-buffhud.mpq` into the plugin directory:

```text
<game>/mods/<mod-name>/d2rloader/plugins/
<game>/d2rloader/plugins/
```

The companion MPQ contains only the layout JSON:

```text
data/global/ui/layouts/buffhud/BuffHudhd.json
```

## Customizing the panel position

Edit `companion/data/global/ui/layouts/buffhud/BuffHudhd.json` (or override `data/global/ui/layouts/buffhud/BuffHudhd.json` in your mod) to adjust `BuffGrid.fields.rect.x` and `.y`. The default is `x: -150, y: -665`, which keeps the bottom row (ending at y −359) clear of D2R's quick cast skill row (`hudpanelhd.json` `SkillBindTemplate`, starting at y −332); increasing `x` moves right, and making `y` more negative moves up. Keep the outer anchor and the 21 child slots unchanged for a simple reposition.

## Known limitations / verification

- Qualified low-level offsets and bridge signatures target D2R build **93847** and fail closed on mismatch.
- `buffhud` owns native patching; do not run it alongside another uncoordinated plugin that changes the same bridge without safe chaining.
