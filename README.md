# MLTweaks 1.1.0

A configurable tweak mod for **Manor Lords**. One settings file lets you scale production,
storage, carrying, crops, wildlife and more. Out of the box it changes **nothing** - every
value ships at its vanilla setting, so you turn on only what you want.

Tested on Manor Lords **0.8.104** (Steam) with UE4SS.

Prefer no DLL at all? **MLTweaks Lite** is the same mod without the native part - see the
optional files. It covers everything except crops, carrying, rich resources, free ox / animal
orders, the wildlife population cap and families per house.

---

## Requirements

- **UE4SS** installed for Manor Lords, in the layout where the `ue4ss` folder sits next to
  the game executable and the proxy DLL (`dwmapi.dll`) is in the game's `Win64` directory.
  Get it from <https://github.com/UE4SS-RE/RE-UE4SS>.
- Manor Lords 0.8.104. Other builds may work: any code patch that no longer fits is skipped
  and logged rather than applied (see *How it works*).
- Visual C++ runtime is **not** required - the native DLL is statically linked.

## Installation

1. Copy the `MLTweaks` folder into:
   `Manor Lords\ManorLords\Binaries\Win64\ue4ss\Mods\`
2. Start the game. `enabled.txt` is included, so there is nothing to add to `mods.txt`.
3. Check `ue4ss\Mods\MLTweaks\report.txt`. If it says `MLTweaks loaded`, you are set.

## Uninstalling

Delete the `MLTweaks` folder. See *What stays in your save* below for the few things that
do not revert.

## Configuring

Edit `Scripts/config.lua` and **restart the game**.

- Multipliers: `1.0` = no change.
- Values documented as `0 = leave unchanged` do exactly that.
- Do **not** use the UE4SS hot reload (Ctrl+R). Multipliers would be applied a second time
  on top of the already modified values.

---

## What it can do

Each entry lists the setting name in `config.lua`.

### Production and crops
- `FoodProductionMultiplier`, `ProcessedGoodsMultiplier` - output per production cycle.
- `CropYieldMultiplier`, `CropGrowthMultiplier` - per crop group (grains / vegetables / fruits).
- `HarvestAnytime` - harvest outside the autumn window.
- `HarvestGrowthThreshold` - how grown a crop must be before it can be harvested (game default 0.3).
- `NoFertilityLoss` - planting stops draining field fertility; fallow regeneration is untouched.

### Logistics
- `StorageMultiplier` - storage limits of storehouses, granaries, market stalls and so on.
- `HandCarryAmount`, `CartCarryAmount` - how much is moved in one trip, capped by the stock
  at the source so nothing is created out of thin air.
- `BulkFetchCapacity` - use those amounts when fetching building materials, but only when the
  destination is a building under construction.
- `CarryBulkDestinations` - which destinations receive bulk hauls. Workshops are deliberately
  excluded: they only request what they need, so extra goods just get hauled back.

### Speeds
- `VillagerWalkSpeedMultiplier`, `AnimalWalkSpeedMultiplier`, `WorkSpeedMultiplier`.

### Resources and wildlife
- `AllResourcesRich` - every resource node and mineral deposit counts as rich.
- `MiningMultiplier` - extra output per ore / salt / clay / stone mined.
- `WildResources` - berries, mushrooms, fish and eel. Sets the cap of each bush or fishing
  spot (`Capacity`) and can top the amount back up (`Refill`) instead of waiting for regrowth.
  `StopRefillInWinter` keeps winter working like vanilla: the spots drain, and the refill
  resumes in spring.
- `Wildlife` - deer and small game. `BreedingSpeedMultiplier` makes herds grow back faster,
  `MaxMultiplier` raises the population cap of a spot, and `SoloBreeding` lets a herd of one
  recover (vanilla needs two animals).

### Housing
- `BurgageFamilies` - how many families a house of each level holds (vanilla: level 1 and 2
  one family, level 3 two, level 4 three). The house expansion that adds a second dwelling to
  the plot still adds one more on top.

### Economy and other
- `FreeOxen`, `FreeCows`, `FreeAnimalOrders` - make animal orders cost nothing. The regular
  price of an ox order is the import price plus a flat trade fee; this mod zeroes the cost of
  the selected orders only, so ordinary trade prices are untouched.
- `SuperPerk` - gives one perk (matched by name) the effects of every other perk and removes
  the ones that are downsides.
- `MaxMilitiaSquads`, `MilitiaSquadMaxSize`, `ArcherDamageMultiplier`, `ArcherRangeMultiplier`,
  `TreeGrowthRate`, `MaxBanditCamps`, `RaidIntervalMultiplier`.

### Debug
- `DebugHotkeys` - `Ctrl+Shift+U` dumps villagers and their inventory to `units_snapshot.txt`,
  `Ctrl+Shift+R` spawns a bandit at the mouse position.
- `FreeOxHotkey` - `Ctrl+Shift+O` adds one ox for free (uses the game's own cheat function).
- `DebugDump` - writes the item, building and perk tables to text files on startup.

---

## Not fully tested

These are implemented but their in-game effect was never confirmed. They are off by default.

- Archer damage and range
- Militia squad cap and squad size
- Tree growth rate
- Mining multiplier (the log shows the extra amount being added; the effect on stock was not measured)
- Bandit camp cap and raid interval
- Free cows, and the free-ox hotkey

Everything else in the list above was checked in game.

---

## How it works

Two parts:

- **`Scripts/main.lua`** edits data tables and live objects through UE4SS.
- **`native/MLTweaksNative.dll`** patches game code in memory for the things Lua cannot reach.
  Each patch is located by a **byte pattern** and is only written when the original bytes match,
  so after a game update a patch that no longer fits is skipped and logged instead of corrupting
  anything. **The game executable on disk is never modified.**

The DLL is only loaded when at least one setting needs it - with the default settings it is
not loaded at all. When it is, Lua calls its single exported function, `MLTweaks_Init`; the DLL
does nothing on its own when Windows loads it. Code the DLL generates for its hooks is written
to read/write memory that is switched to execute/read before use, so the DLL never keeps memory
that is writable and executable at the same time.

The DLL's source is included in `native/src/` and it builds with `native/build.bat`
(Visual Studio 2022 Build Tools, `cl /O2 /MT /LD`).

### Logs

- `report.txt` - what the Lua side did.
- `native_log.txt` - one line per code patch: `patched at exe+0x...` or `SKIPPED`.

## What stays in your save

Most of what the mod changes lives in memory and is gone when you remove the mod: data table
values, game settings, production and storage multipliers.

These are written into the save and stay after uninstalling:

- Wild gathering spots keep the `capacity` you set them to.
- Animal populations keep the size they grew to.
- Resource nodes already flagged as rich stay rich.
- Houses keep the extra families they took in through `BurgageFamilies`. Nobody is evicted,
  but the free housing counter counts those families as missing housing, so it can show a
  negative number until you build more houses. No family is actually homeless and approval
  is not affected.

None of these break a vanilla game - they just stay at the value they reached.

## Troubleshooting

- **`report.txt` says the DLL was "not loaded"** - that is expected when no setting needs it.
- **Nothing happens** - check `report.txt` exists. If it does not, UE4SS is not loading the mod
  (is `enabled.txt` still in the folder?).
- **A feature does nothing after a game update** - open `native_log.txt`. A line ending in
  `SKIPPED` means that patch's byte pattern no longer matches the new game build. The rest of
  the mod keeps working.
- **A crash right after loading a save** - set `SuperPerk.Enabled = false` first; adding perk
  effects is the most invasive thing this mod does.
- **Free housing shows a negative number after lowering `BurgageFamilies`** - see *What stays
  in your save*: houses keep families they already took in, and the counter treats the extra
  ones as missing housing. Building more houses brings it back up.
- **Changed a value and nothing happened** - the game has to be restarted; hot reload is not
  supported.

## Changelog

**1.1.0**
- The DLL is no longer loaded unless a setting needs it. With the default settings the mod now
  runs on Lua alone.
- The DLL's work moved out of `DllMain` into an explicit `MLTweaks_Init` call from Lua.
- Code generated for hooks no longer sits in writable-and-executable memory.
- The DLL carries version information (Properties > Details) and writes its version on the first
  line of `native_log.txt`.
- `build.bat` creates its `build` folder itself, so a fresh clone builds without errors.
- New optional file: MLTweaks Lite, without the DLL.

**1.0.0**
- First release.

## Permissions

Do whatever you like with this. You may modify it, reuse any part of it, and redistribute it
- including a modified version, on any site - without asking first. A credit and a link back
are appreciated but not required.

The included source is offered on the same terms.

## Credits

Built with [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS).
