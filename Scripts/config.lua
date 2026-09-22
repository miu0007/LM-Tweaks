-- ============================================================
--  MLTweaks - settings
--  Everything below is set to vanilla values, so a fresh install changes nothing.
--  Turn on only what you want.
--
--  Multipliers: 1.0 = no change. Restart the game after editing this file.
--  (Do not use the UE4SS hot reload, Ctrl+R - multipliers would be applied twice.)
-- ============================================================
return {

    -- 1. Output per production cycle
    FoodProductionMultiplier      = 1.0,   -- food (bread, meat, cheese, ale, ...)
    ProcessedGoodsMultiplier      = 1.0,   -- everything else crafted (planks, cloth, tools, weapons, ...)

    -- 2. Crops: harvest yield and growth speed
    CropYieldMultiplier = {
        Grains     = 1.0,   -- wheat, barley, rye, oats
        Vegetables = 1.0,   -- vegetable plots (carrots, cabbages, beetroots, ...)
        Fruits     = 1.0,   -- orchards (apples, pears, quinces)
    },
    CropGrowthMultiplier = {
        Grains     = 1.0,
        Vegetables = 1.0,
        Fruits     = 1.0,
    },
    HarvestAnytime         = false,  -- allow harvesting outside the autumn window
    HarvestGrowthThreshold = 0,      -- growth a crop needs before it can be harvested
                                     -- (game default 0.3). 0 = leave unchanged
    NoFertilityLoss        = false,  -- planting no longer drains field fertility
                                     -- (fallow regeneration is untouched)

    -- 3. Storage limit of buildings (storehouse, granary, market stalls, ...)
    StorageMultiplier             = 1.0,

    -- 4. Movement and work speed
    VillagerWalkSpeedMultiplier   = 1,
    WorkSpeedMultiplier           = 1,   -- work animations (logging, farming, crafting, ...)
    AnimalWalkSpeedMultiplier     = 1,   -- oxen, horses, mules

    -- 5. How much is carried in one trip (never more than the stock at the source).
    --    0 = leave unchanged
    HandCarryAmount               = 0,     -- carried by hand (game default 1)
    CartCarryAmount               = 0,     -- with a handcart (game default about 10)
    BulkFetchCapacity             = false, -- also use the amounts above when fetching
                                           -- building materials for a construction site
    -- Destinations that receive bulk hauls (EBuildingType). Workshops are left out on
    -- purpose: they only request what they need, so extra goods just get hauled back.
    CarryBulkDestinations = {
        72, 99,       -- storehouse lv1 / lv2
        80, 68,       -- granary lv1 / lv2
        13,           -- marketplace
        91, 92, 93,   -- food / firewood / goods stalls
        102, 103,     -- firewood and food carts
        6, 85, 88,    -- trading post, trade point, supply point
        112,          -- hauling post
    },

    -- 6. Mining output (mines, quarries, ...)
    MiningMultiplier = {
        IronOre = 1.0,
        Salt    = 1.0,
        Clay    = 1.0,
        Stone   = 1.0,
    },

    -- 7. Archers (yours only)
    ArcherDamageMultiplier        = 1,
    ArcherRangeMultiplier         = 1,

    -- 8. Militia
    MaxMilitiaSquads              = 0,   -- number of militia squads (game default 6). 0 = leave unchanged
    MilitiaSquadMaxSize           = 0,   -- men per militia squad. 0 = leave unchanged

    -- 9. Tree growth rate (the game setting itself, default 1.0). 0 = leave unchanged
    TreeGrowthRate                = 0,

    -- 10. Treat every resource node and mineral deposit as a rich one
    AllResourcesRich              = false,

    -- 11. Free livestock
    FreeOxen                      = false, -- the monthly "order oxen" costs nothing
    FreeCows                      = false, -- also make cows free (this drops their sale value to 0 as well)
    -- Other animal orders to make free (oxen = 13 is covered by FreeOxen above)
    --   19 = horse, 21 = mule, 26 = hunting hound, 27 = pig, 28 = goat
    FreeAnimalOrders              = {},
    FreeOxHotkey                  = false, -- Ctrl+Shift+O adds one ox for free (uses the game's own cheat)

    -- 12. Wild gathering spots (berries, mushrooms, fish, eel)
    --    Every bush / fishing spot has an amount left (amt) and a cap (capacity).
    --    Capacity sets that cap; vanilla is about 36 per bush and 31 per fishing spot.
    --    0 = leave the cap unchanged (200 is a good value if you want plenty).
    WildResources = {
        Enabled  = false,
        Refill   = true,   -- put the amount back to the cap instead of waiting for regrowth
        -- Hold that refill while it is winter (when the tooltip reads "seasonal (in decline)").
        --   true  = winter drains the spots like vanilla, spring fills them up again
        --   false = always full, so they can be gathered all year round
        StopRefillInWinter = true,
        Capacity = {
            Berries   = 0,
            Mushrooms = 0,
            Fish      = 0,
            Eel       = 0,
        },
    },

    -- 13. Wild animals (deer and small game)
    Wildlife = {
        -- Breeding speed. Divides the breeding threshold (vanilla: deer 162, small game 30)
        BreedingSpeedMultiplier = 1.0,
        -- Multiplier for the population cap of one spot (vanilla is about 8 deer / 16 small game).
        -- 1 = leave unchanged. Nothing is written to the save, so setting this back to 1
        -- simply stops the herds from growing further.
        MaxMultiplier           = 1,
        -- Let a herd of one breed (vanilla needs two). This also doubles the daily progress.
        SoloBreeding            = false,
    },

    -- 14. Families per house (burgage plot)
    --    How many families a house of each level holds. The house expansion - the upgrade
    --    that adds a second dwelling to the plot, not a backyard extension - still adds one
    --    more on top, as in vanilla. 0 = leave unchanged.
    --    Vanilla: Lv1 = 1, Lv2 = 1, Lv3 = 2, Lv4 = 3.
    --    Lowering a value later, or removing the mod, does not evict anyone: houses that
    --    already hold more families keep them. The free housing counter then counts those
    --    families as missing housing and can go negative until you build more houses, but
    --    nobody is actually homeless and approval is not affected.
    BurgageFamilies = { Lv1 = 0, Lv2 = 0, Lv3 = 0, Lv4 = 0 },

    -- 15. Perks: give one perk the effects of every other perk and strip its downsides
    SuperPerk = {
        Enabled = false,
        NameMatch = { "Bamberg" },  -- perks whose name contains one of these strings
        -- Effects treated as downsides (EPerkEffect ids). They are removed from the perk
        -- and never copied over from other perks.
        Negative = {
            12, -- BackyardAgricultureNerf
            18, -- DecreasedLoggerSpeed
            46, -- TenPercentRegionWealthDecrease
            50, -- DecreasedApproval
            51, -- RawResourcesPriceDeducation
            52, -- TaxGenerationReduction
            56, -- TitheApprovalPenalty
            57, -- FarmingYieldPenalty
            60, -- MiningCraftingSpeedMalus
            65, -- TaxApprovalGeneralPenalty
            70, -- LivestockYieldPenalty
            75, -- TradeRouteCostPenalty
            78, -- HuntingAndFishingProductivityPenalty
            83, -- RetinueHireCostPenalty
            87, -- ArtisanEfficiencyPenalty
            91, -- FieldFertilityClamp
        },
        -- Effects that are never added (crash guard). These belong to the unfinished
        -- "StrengthOfTheSoil" perk and crash the trading screen.
        Exclude = {
            93, -- DroughtResistance
            94, -- ReducedCropSpoilage
            95, -- IncreasedCropExportValue (the one that crashes the trading screen)
        },
    },

    -- 16. Bandits and raids
    --    NOT VERIFIED: the in-game effect of these two was never confirmed, so they are off.
    --    Leave them as they are unless you want to experiment.
    MaxBanditCamps                = -1,  -- cap on bandit camps; fewer camps means fewer monthly
                                         -- thefts (0 = no new camps). -1 = leave unchanged
    RaidIntervalMultiplier        = 1.0, -- 2 = twice as long between raids, 0.5 = half as long

    -- Debug hotkeys
    --   Ctrl+Shift+U : write villagers / soldiers and their inventory to units_snapshot.txt
    --   Ctrl+Shift+R : spawn a bandit at the mouse position (for testing)
    DebugHotkeys                  = false,

    -- Debug: write the item, building and perk tables to text files on startup
    DebugDump                     = false,
}
