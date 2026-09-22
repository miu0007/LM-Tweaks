-- MLTweaks : Manor Lords gameplay multipliers (UE4SS Lua)
-- Game version: 0.8.065 / UE4SS v4.0.0-rc1

local cfg = require("config")

local MOD_DIR = "ue4ss/Mods/MLTweaks/"
local reportLines = {}

local function log(msg)
    print("[MLTweaks] " .. tostring(msg) .. "\n")
    table.insert(reportLines, os.date("%H:%M:%S ") .. tostring(msg))
    if #reportLines > 400 then table.remove(reportLines, 1) end
end

local function flushReport()
    local f = io.open(MOD_DIR .. "report.txt", "w")
    if f then f:write(table.concat(reportLines, "\n")); f:close() end
end

-- ============================================================
-- Native code patches (MLTweaksNative.dll). The DLL reads native.cfg and patches
-- game code in memory when it is loaded; results go to native_log.txt.
-- ============================================================
-- Crop types by group (ECropType). Growth / yield / harvest season are applied by the DLL,
-- which multiplies the untouched values it reads at startup.
local CROP_GROUP = {
    [1] = "Grains", [3] = "Grains", [7] = "Grains", [15] = "Grains",   -- Wheat, Barley, Rye, Oats
    [5] = "Vegetables", [10] = "Vegetables", [11] = "Vegetables", [12] = "Vegetables",
    [9] = "Fruits", [13] = "Fruits", [14] = "Fruits",                  -- Apples, Pears, Quinces
}

-- Wild gathering spots (one Resource actor per bush / fishing spot). "amt" is what is left,
-- "capacity" the maximum it regrows to; both are plain properties, so they can be set live.
local WILD_RESOURCE_BP = {
    { class = "Resource_Berries_C",   key = "Berries" },
    { class = "Resource_Mushrooms_C", key = "Mushrooms" },
    { class = "Resource_Fish_C",      key = "Fish" },
    { class = "Resource_Eel_C",       key = "Eel" },
}

-- Settings that only the native DLL can apply. When none of them is switched on, the DLL is
-- not loaded at all (it would have nothing to do), which also keeps the Lite package - the
-- same mod without the DLL - free of load errors.
local function familiesSet()
    local bf = cfg.BurgageFamilies or {}
    for lv = 1, 4 do
        if (bf["Lv" .. lv] or 0) > 0 then return true end
    end
    return false
end

local function nativeNeeded()
    local ym, gm = cfg.CropYieldMultiplier or {}, cfg.CropGrowthMultiplier or {}
    for _, g in ipairs({ "Grains", "Vegetables", "Fruits" }) do
        if (ym[g] or 1) ~= 1 or (gm[g] or 1) ~= 1 then return true end
    end
    local wl = cfg.Wildlife or {}
    return (cfg.AllResourcesRich or cfg.HarvestAnytime or cfg.NoFertilityLoss
        or (cfg.HarvestGrowthThreshold or 0) > 0
        or (cfg.HandCarryAmount or 0) > 1 or (cfg.CartCarryAmount or 0) > 1
        or cfg.FreeOxen or #(cfg.FreeAnimalOrders or {}) > 0
        or wl.SoloBreeding or (wl.MaxMultiplier or 1) > 1
        or familiesSet()) and true or false
end

local function loadNativePatches()
    local f = io.open(MOD_DIR .. "native.cfg", "w")
    if not f then log("native: cannot write native.cfg"); return end
    f:write("RichResources=" .. (cfg.AllResourcesRich and "1" or "0") .. "\n")
    local ym, gm = cfg.CropYieldMultiplier or {}, cfg.CropGrowthMultiplier or {}
    for ct, group in pairs(CROP_GROUP) do
        f:write(string.format("CropYieldMul%d=%.3f\n", ct, ym[group] or 1))
        f:write(string.format("CropGrowthMul%d=%.3f\n", ct, gm[group] or 1))
    end
    f:write("HarvestAllYear=" .. (cfg.HarvestAnytime and "1" or "0") .. "\n")
    f:write(string.format("HarvestThreshold=%.3f\n", cfg.HarvestGrowthThreshold or 0))
    f:write("NoFertilityLoss=" .. (cfg.NoFertilityLoss and "1" or "0") .. "\n")
    -- per-plant harvest multiplier for backyard gardens / orchards (ECropType index)
    for _, ct in ipairs({ 5, 10, 11, 12 }) do f:write(string.format("CropYield%d=%d\n", ct, math.floor((ym.Vegetables or 1) + 0.5))) end
    for _, ct in ipairs({ 9, 13, 14 }) do f:write(string.format("CropYield%d=%d\n", ct, math.floor((ym.Fruits or 1) + 0.5))) end
    f:write("HandCarry=" .. tostring(math.floor(cfg.HandCarryAmount or 0)) .. "\n")
    f:write("CartCarry=" .. tostring(math.floor(cfg.CartCarryAmount or 0)) .. "\n")
    f:write("CarryDest=" .. table.concat(cfg.CarryBulkDestinations or {}, ",") .. "\n")
    f:write("BulkFetch=" .. (cfg.BulkFetchCapacity and "1" or "0") .. "\n")
    -- Animal order upgrades whose regional wealth cost the DLL forces to 0
    -- (13 = OrderOxen; its price is import price + TradeSettings.importFee, not the table cost)
    local freeUpgrades = {}
    if cfg.FreeOxen then table.insert(freeUpgrades, 13) end
    for _, id in ipairs(cfg.FreeAnimalOrders or {}) do table.insert(freeUpgrades, id) end
    f:write("FreeUpgrade=" .. table.concat(freeUpgrades, ",") .. "\n")
    local wl = cfg.Wildlife or {}
    f:write("WildlifeBreed=" .. (wl.SoloBreeding and "1" or "0") .. "\n")
    f:write("WildlifeMax=" .. tostring(math.floor(wl.MaxMultiplier or 1)) .. "\n")
    local bf = cfg.BurgageFamilies or {}
    for lv = 1, 4 do f:write(string.format("FamiliesLv%d=%d\n", lv, math.floor(bf["Lv" .. lv] or 0))) end
    f:close()
    if not nativeNeeded() then
        log("native: no setting needs MLTweaksNative.dll - not loaded")
        return
    end
    local dll = MOD_DIR .. "native/MLTweaksNative.dll"
    local probe = io.open(dll, "rb")
    if not probe then
        log("native: MLTweaksNative.dll not found (Lite package?) - crop, carrying, rich resource,")
        log("        free animal order, herd cap and families per house settings are ignored")
        return
    end
    probe:close()
    if not package or not package.loadlib then log("native: package.loadlib not available"); return end
    local init, err = package.loadlib(dll, "MLTweaks_Init")
    if not init then log("native: failed to load DLL: " .. tostring(err)); return end
    local ok, e = pcall(init)
    if ok then
        log("native: MLTweaksNative.dll loaded (see native_log.txt)")
    else
        log("native: MLTweaks_Init failed: " .. tostring(e))
    end
end

local function S(fn, default)
    local ok, r = pcall(fn)
    if ok and r ~= nil then return r end
    return default
end

local function valid(o) return o ~= nil and S(function() return o:IsValid() end, false) end

local function round(x) return math.floor(x + 0.5) end

local function mulInt(v, m)
    if v <= 0 or m == 1 then return v end
    return math.max(1, round(v * m))
end

local function fstr(v) return S(function() return v:ToString() end, tostring(v)) end

-- TArray helpers (UE4SS TArrays are 1-indexed)
local function arrLen(a) return S(function() return #a end, 0) end

-- "(x,y,z,w)" for a Vector4 struct, "?" when it cannot be read
local function vec4Str(v)
    if not v then return "?" end
    local c = {}
    for _, k in ipairs({ "X", "Y", "Z", "W" }) do
        table.insert(c, string.format("%g", S(function() return v[k] end, 0/0)))
    end
    return "(" .. table.concat(c, ",") .. ")"
end

-- "a,b,c" for a TArray of ints
local function intsStr(arr)
    local out = {}
    for i = 1, arrLen(arr) do table.insert(out, tostring(S(function() return arr[i] end, "?"))) end
    return table.concat(out, ",")
end

-- ============================================================
-- Live value tracker: re-applies a multiplier whenever the game resets a value,
-- without compounding (remembers the value we wrote).
-- ============================================================
local tracked = {}

local function applyTracked(obj, field, mult, key, isInt, square)
    if mult == 1 then return false end
    local cur = S(function() return obj[field] end)
    if type(cur) ~= "number" then return false end
    local t = tracked[key]
    if t and math.abs(cur - t.set) <= 1e-3 * math.max(1, math.abs(t.set)) then return false end
    local newv = cur * mult
    if isInt then newv = mulInt(cur, mult) end
    if newv == cur then tracked[key] = { base = cur, set = cur }; return false end
    local ok = pcall(function() obj[field] = newv end)
    if ok then
        tracked[key] = { base = cur, set = S(function() return obj[field] end, newv) }
        if square then pcall(function() obj[square] = newv * newv end) end
        return true
    end
    return false
end

local function restoreTracked(obj, field, key, square)
    local t = tracked[key]
    if not t then return end
    local cur = S(function() return obj[field] end)
    if type(cur) == "number" and math.abs(cur - t.set) <= 1e-3 * math.max(1, math.abs(t.set)) then
        pcall(function() obj[field] = t.base end)
        if square then pcall(function() obj[square] = t.base * t.base end) end
    end
    tracked[key] = nil
end

-- ============================================================
-- Game object access
-- ============================================================
local function getEngine()
    local e = FindFirstOf("RTSMultiEngineCPP")
    if valid(e) then return e end
end

local function getPlayerPawn(engine)
    local p = S(function() return engine:getPawnBySetupIndex(0) end)
    if valid(p) and S(function() return p.isMainPlayer end, false) then return p end
    local all = FindAllOf("PawnCPP") or {}
    for _, pw in ipairs(all) do
        if valid(pw) and S(function() return pw.isMainPlayer end, false) then return pw end
    end
end

local function findDT(path)
    local dt = StaticFindObject(path)
    if valid(dt) then return dt end
end

-- ============================================================
-- Item table helpers
-- ============================================================
local EItemCategory_Food = 2

local function itemCategoryMap(dtItems)
    local map = {}
    dtItems:ForEachRow(function(rowName, row)
        map[tostring(rowName)] = S(function() return row.ItemCategory end, 0)
    end)
    return map
end

local function goodsStr(arr)
    local parts = {}
    for i = 1, arrLen(arr) do
        local g = arr[i]
        table.insert(parts, tostring(S(function() return g.Type end, "?")) .. "x" .. tostring(S(function() return g.amt end, "?")))
    end
    return table.concat(parts, ",")
end

-- ============================================================
-- One-time (per session) data changes. Each task returns true when done.
-- Original values are remembered so that re-application never compounds.
-- ============================================================
local orig = {}
local function origOf(key, cur)
    if orig[key] == nil then orig[key] = cur end
    return orig[key]
end

local tasks = {}

-- DEBUG: dump item / building tables so balancing can be verified
tasks.debugDump = function()
    if not cfg.DebugDump then return true end
    local dt = findDT("/Game/NotStronghold/Data/DT_Items.DT_Items")
    if not dt then return false end
    local L = {}
    dt:ForEachRow(function(rowName, row)
        table.insert(L, string.format("%s | %s | cat=%s sub=%s | prog=%s | in=[%s] out=[%s] | rAtt=%s sRange=%s range=%s | value=%s | weight=%s combat=%s",
            tostring(rowName), fstr(S(function() return row.Name end, "")),
            tostring(S(function() return row.ItemCategory end, "?")), tostring(S(function() return row.Subcategory end, "?")),
            tostring(S(function() return row.creationProg end, "?")),
            goodsStr(S(function() return row.Resources end)), goodsStr(S(function() return row.craftingOutput end)),
            tostring(S(function() return row.rangedAtt end, "?")), tostring(S(function() return row.shootingRange end, "?")),
            tostring(S(function() return row.range end, "?")), tostring(S(function() return row.Value end, "?")),
            tostring(S(function() return row.Weight end, "?")), tostring(S(function() return row.bCombatType end, "?"))))
    end)
    local bs = findDT("/Game/NotStronghold/Data/buildingStats.buildingStats")
    if bs then
        table.insert(L, "")
        bs:ForEachRow(function(rowName, row)
            table.insert(L, string.format("BUILDING %s | %s | storage G=%s L=%s P=%s | occ=%s work=%s lvl=%s from=%s upg=[%s] | prod=[%s]",
                tostring(rowName), fstr(S(function() return row.DisplayName end, "")),
                tostring(S(function() return row.storageLimitGeneric end, "?")), tostring(S(function() return row.storageLimitLarge end, "?")),
                tostring(S(function() return row.storageLimitPantry end, "?")),
                vec4Str(S(function() return row.occupantTypes end)), vec4Str(S(function() return row.workerTypes end)),
                tostring(S(function() return row.settlementLevel end, "?")), tostring(S(function() return row.upgradedFrom end, "?")),
                intsStr(S(function() return row.upgrades end)),
                goodsStr(S(function() return row.averageProduction end))))
        end)
    end
    local an = findDT("/Game/NotStronghold/Data/DT_AnimsetWork.DT_AnimsetWork")
    if an then
        table.insert(L, "")
        an:ForEachRow(function(rowName, row)
            local v = S(function() return row.variations end)
            local sp = {}
            for i = 1, arrLen(v) do
                local e = v[i]
                table.insert(sp, fstr(S(function() return e.clipName end, "?")) .. "@" .. tostring(S(function() return e.spd end, "?")))
            end
            table.insert(L, string.format("ANIM %s | tag=%s rep=%s+%s | %s", tostring(rowName),
                fstr(S(function() return row.Tag end, "?")), tostring(S(function() return row.repetitions end, "?")),
                tostring(S(function() return row.additionalRepetitions end, "?")), table.concat(sp, " ")))
        end)
    end
    local f = io.open(MOD_DIR .. "debug_tables.txt", "w")
    if f then f:write(table.concat(L, "\n")); f:close() end
    log("debug_tables.txt written (" .. #L .. " lines)")
    return true
end

-- 1. Food / processed goods output
tasks.production = function()
    local fm, pm = cfg.FoodProductionMultiplier or 1, cfg.ProcessedGoodsMultiplier or 1
    if fm == 1 and pm == 1 then return true end
    local dt = findDT("/Game/NotStronghold/Data/DT_Items.DT_Items")
    if not dt then return false end
    local cats = itemCategoryMap(dt)
    local nFood, nProc = 0, 0
    dt:ForEachRow(function(rowName, row)
        local outs = S(function() return row.craftingOutput end)
        local n = arrLen(outs)
        for i = 1, n do
            local g = outs[i]
            local t = S(function() return g.Type end, 0)
            local a = S(function() return g.amt end, 0)
            local cat = cats[tostring(t)] or S(function() return row.ItemCategory end, 0)
            local m = (cat == EItemCategory_Food) and fm or pm
            local base = origOf("out|" .. tostring(rowName) .. "|" .. i, a)
            if base > 0 and m ~= 1 then
                pcall(function() g.amt = mulInt(base, m) end)
                if cat == EItemCategory_Food then nFood = nFood + 1 else nProc = nProc + 1 end
            end
        end
    end)
    log(string.format("production: food outputs x%.2f = %d, processed outputs x%.2f = %d", fm, nFood, pm, nProc))
    return true
end

-- 2. Crop yield / growth / harvest season are applied by MLTweaksNative.dll
-- (it multiplies the untouched values it reads at startup; see native.cfg).

-- 3. Storage (table part; live buildings are handled in the loop)
tasks.storageTable = function()
    local m = cfg.StorageMultiplier or 1
    if m == 1 then return true end
    local dt = findDT("/Game/NotStronghold/Data/buildingStats.buildingStats")
    if not dt then return false end
    local n = 0
    dt:ForEachRow(function(rowName, row)
        for _, f in ipairs({ "storageLimitGeneric", "storageLimitLarge", "storageLimitPantry" }) do
            local cur = S(function() return row[f] end, 0)
            local base = origOf("st|" .. tostring(rowName) .. "|" .. f, cur)
            if base > 0 then
                pcall(function() row[f] = mulInt(base, m) end); n = n + 1
            end
        end
    end)
    log(string.format("storage table: %d limits x%.2f", n, m))
    return true
end

-- 4b. Work speed (animation speed of work animations)
tasks.workSpeed = function()
    local m = cfg.WorkSpeedMultiplier or 1
    if m == 1 then return true end
    local dt = findDT("/Game/NotStronghold/Data/DT_AnimsetWork.DT_AnimsetWork")
    if not dt then return false end
    local n = 0
    dt:ForEachRow(function(rowName, row)
        local v = S(function() return row.variations end)
        for i = 1, arrLen(v) do
            local e = v[i]
            local cur = S(function() return e.spd end)
            if type(cur) == "number" and cur > 0 then
                local base = origOf("anim|" .. tostring(rowName) .. "|" .. i, cur)
                pcall(function() e.spd = base * m end); n = n + 1
            end
        end
    end)
    log(string.format("work speed: %d work animations x%.2f", n, m))
    return true
end

-- 13. Super perk: add every non-negative perk effect to the target perk, strip negatives
local PERK_TABLES = {
    "/Game/NotStronghold/Data/techList.techList",
    "/Game/UI/Development/DT_RegionDevelopmentTech.DT_RegionDevelopmentTech",
}
local PERK_TEXT_TABLES = {
    "/Game/Translation/HoodedHorse/DT_Translation_Perks.DT_Translation_Perks",
    "/Game/Translation/HoodedHorse/DT_Translation_DevelopmentBranches.DT_Translation_DevelopmentBranches",
}

local function setToList(set)
    local out = {}
    S(function() set:ForEach(function(e) table.insert(out, S(function() return e:get() end, -1)) end) end)
    table.sort(out)
    return out
end

tasks.superPerk = function()
    local sp = cfg.SuperPerk
    if not sp or not sp.Enabled then return true end
    local tables = {}
    for _, p in ipairs(PERK_TABLES) do
        local dt = findDT(p)
        if dt then table.insert(tables, dt) end
    end
    if #tables == 0 then return false end

    -- translations: row key -> "en / ja"
    local texts = {}
    for _, p in ipairs(PERK_TEXT_TABLES) do
        local dt = findDT(p)
        if dt then
            dt:ForEachRow(function(rowName, row)
                texts[tostring(rowName)] = {
                    en = fstr(S(function() return row.en_US end, "")),
                    ja = fstr(S(function() return row.ja_JA end, "")),
                }
            end)
        end
    end

    if next(texts) == nil then return false end -- translations not loaded yet, retry

    local negative = {}
    for _, v in ipairs(sp.Negative or {}) do negative[v] = true end
    local exclude = {}
    for _, v in ipairs(sp.Exclude or {}) do exclude[v] = true end

    -- effects that have a configured bonus value (for diagnosing trade-UI lookups)
    local ps = StaticFindObject("/Script/ManorLords.Default__PerkSettings")
    if valid(ps) then
        local keys = {}
        S(function() ps.PerkEffectBonuses:ForEach(function(k, v)
            table.insert(keys, tostring(S(function() return k:get() end, "?")))
        end) end)
        log("super perk: PerkEffectBonuses keys = [" .. table.concat(keys, ",") .. "]")
    end

    local allGood, targets, L = {}, {}, {}
    for _, dt in ipairs(tables) do
        dt:ForEachRow(function(rowName, row)
            local perkName = fstr(S(function() return row.PerkName end, ""))
            local t = texts[perkName] or texts[tostring(rowName)] or { en = "", ja = "" }
            local effects = setToList(S(function() return row.Effects end))
            -- only borrow effects from real (translated) perks; unfinished perks crash the trade UI
            if t.en ~= "" then
                for _, e in ipairs(effects) do
                    if not negative[e] and not exclude[e] then allGood[e] = true end
                end
            end
            local isTarget = false
            for _, m in ipairs(sp.NameMatch or {}) do
                if (t.ja ~= "" and t.ja:find(m, 1, true)) or (t.en ~= "" and t.en:lower():find(m:lower(), 1, true))
                    or perkName:lower():find(m:lower(), 1, true) then
                    isTarget = true
                end
            end
            if isTarget then table.insert(targets, { row = row, name = perkName }) end
            table.insert(L, string.format("%s%s | %s | en=%s | ja=%s | effects=[%s]", isTarget and "* " or "  ",
                tostring(rowName), perkName, t.en, t.ja, table.concat(effects, ",")))
        end)
    end

    local f = io.open(MOD_DIR .. "debug_perks.txt", "w")
    if f then f:write(table.concat(L, "\n")); f:close() end

    if #targets == 0 then
        log("super perk: no perk matched " .. table.concat(sp.NameMatch or {}, "/") .. " (see debug_perks.txt)")
        return true
    end
    for _, tg in ipairs(targets) do
        local set = S(function() return tg.row.Effects end)
        if set then
            for e in pairs(negative) do pcall(function() set:Remove(e) end) end
            for e in pairs(exclude) do pcall(function() set:Remove(e) end) end
            for e in pairs(allGood) do pcall(function() set:Add(e) end) end
            log(string.format("super perk: %s now has %d effects [%s]", tg.name, S(function() return #set end, -1),
                table.concat(setToList(set), ",")))
        end
    end
    return true
end

-- 6b. Militia squad size
tasks.militiaSize = function()
    local size = cfg.MilitiaSquadMaxSize or 0
    if size <= 0 then return true end
    local dt = findDT("/Game/NotStronghold/Data/DT_UnitTemplates.DT_UnitTemplates")
    if not dt then return false end
    local n = 0
    dt:ForEachRow(function(rowName, row)
        if S(function() return row.isMilitia end, false) then
            pcall(function() row.maxSize = size end); n = n + 1
        end
    end)
    log(string.format("militia squad size: %d templates -> %d", n, size))
    return true
end

-- 8a. Rich resources for newly generated nodes (amounts)
tasks.resourceSettings = function()
    if not cfg.AllResourcesRich then return true end
    local cdo = StaticFindObject("/Script/ManorLords.Default__ResourceSettings")
    if not valid(cdo) then return false end
    local arr = S(function() return cdo.ResourceNodeData end)
    local n = 0
    for i = 1, arrLen(arr) do
        local d = arr[i]
        local p = S(function() return d.Properties end)
        if p then
            local minR = S(function() return p.MinRichResourceAmount end, 0)
            local maxR = S(function() return p.MaxRichResourceAmount end, 0)
            local minN = S(function() return p.MinResourceAmount end, 0)
            if minR > minN then
                pcall(function() p.MinResourceAmount = minR; p.MaxResourceAmount = maxR end); n = n + 1
            end
            -- rich-like behaviour: surface deposits never run out
            if S(function() return p.bIsLimitedResource end, false) then
                pcall(function() p.bIsLimitedResource = false end)
                log("resource settings: node type " .. tostring(S(function() return d.Type end, "?")) .. " made unlimited")
            end
        end
    end
    log(string.format("resource settings: %d node types use rich amounts", n))
    return true
end

-- 8b. Wildlife breeding speed. The daily breeding progress of a deer / small game node is
-- compared against ResourceNodeProperties.BreedingThreshold (vanilla: deer 162, small game
-- 30), so dividing the threshold makes herds grow back that much faster. The value is read
-- fresh every day, so this works on an existing save as well.
tasks.wildlifeBreeding = function()
    local mult = (cfg.Wildlife or {}).BreedingSpeedMultiplier or 1
    if mult <= 1 then return true end
    local cdo = StaticFindObject("/Script/ManorLords.Default__ResourceSettings")
    if not valid(cdo) then return false end
    local arr = S(function() return cdo.ResourceNodeData end)
    local parts = {}
    for i = 1, arrLen(arr) do
        local d = arr[i]
        local p = S(function() return d.Properties end)
        local bt = p and S(function() return p.BreedingThreshold end, 0) or 0
        if bt > 0 then
            local nv = math.max(1, math.floor(bt / mult + 0.5))
            if nv ~= bt and pcall(function() p.BreedingThreshold = nv end) then
                table.insert(parts, string.format("type %s: %d -> %d", tostring(S(function() return d.Type end, "?")), bt, nv))
            end
        end
    end
    log("wildlife breeding threshold: " .. (#parts > 0 and table.concat(parts, ", ") or "nothing to change"))
    return true
end

-- 9. Free oxen (DT_Upgrades row 13 = EUpgradeType::OrderOxen)
tasks.freeOxen = function()
    if not cfg.FreeOxen then return true end
    local dt = findDT("/Game/NotStronghold/Data/DT_Upgrades.DT_Upgrades")
    if not dt then return false end
    local row = S(function() return dt:FindRow("13") end)
    if not row then log("free oxen: row 13 not found"); return true end
    local before = string.format("treasury=%s wealth=%s cost=[%s]", tostring(S(function() return row.treasury end)),
        tostring(S(function() return row.regionalWealth end)), goodsStr(S(function() return row.cost end)))
    pcall(function() row.treasury = 0 end)
    pcall(function() row.regionalWealth = 0 end)
    pcall(function() row.cost:Empty() end)
    log("free oxen: OrderOxen cost cleared (was " .. before .. ")")
    return true
end

-- 9b. Livestock price: set the item value of oxen (and optionally cows) to 0.
-- Region:getLivestockPrice is logged before/after so the effect can be verified.
local LIVESTOCK_ITEMS = { Ox = 234, Cow = 347, Horse = 98, Mule = 306 }

local function logLivestockPrices(tag)
    local engine = getEngine()
    local pawn = engine and getPlayerPawn(engine)
    local region = pawn and S(function() return pawn:getCurrentRegion() end)
    if not valid(region) then return false end
    local parts = {}
    for name, id in pairs(LIVESTOCK_ITEMS) do
        table.insert(parts, name .. "=" .. tostring(S(function() return region:getLivestockPrice(id) end, "?")))
    end
    log("livestock prices (" .. tag .. "): " .. table.concat(parts, " "))
    return true
end

tasks.freeLivestock = function()
    if not cfg.FreeOxen and not cfg.FreeCows then return true end
    local dt = findDT("/Game/NotStronghold/Data/DT_Items.DT_Items")
    if not dt then return false end
    if not logLivestockPrices("before") then return false end -- wait until a game is loaded
    local ids = {}
    if cfg.FreeOxen then table.insert(ids, LIVESTOCK_ITEMS.Ox) end
    if cfg.FreeCows then table.insert(ids, LIVESTOCK_ITEMS.Cow) end
    for _, id in ipairs(ids) do
        local row = S(function() return dt:FindRow(tostring(id)) end)
        if row then pcall(function() row.Value = 0 end) end
    end
    logLivestockPrices("after")
    return true
end

-- ============================================================
-- 8b. Rich flags in save data (applied when a save is loaded / written)
-- ============================================================
local function patchSaveGame(sg, why)
    if not cfg.AllResourcesRich or not valid(sg) then return end
    local nodes, deps = 0, 0
    local a = S(function() return sg.savedResourceNodes end)
    for i = 1, arrLen(a) do
        local e = a[i]
        if not S(function() return e.bRichNode end, true) then pcall(function() e.bRichNode = true end); nodes = nodes + 1 end
    end
    local d = S(function() return sg.savedDeposits end)
    for i = 1, arrLen(d) do
        local e = d[i]
        if not S(function() return e.bRichDeposit end, true) then pcall(function() e.bRichDeposit = true end); deps = deps + 1 end
    end
    if nodes + deps > 0 then
        log(string.format("rich resources (%s): %d nodes, %d deposits set rich", why, nodes, deps))
    end
end

-- NOTE: hooking IoHandler:LoadGameFromSlot / SaveGameToSlot crashes the game at startup
-- (called natively from BP_MLGameInstance:ReadSettingsFromDisk), so save data is patched
-- from the in-memory MLSaveGame objects instead (see liveTick).

-- ============================================================
-- Live (per tick) changes
-- ============================================================
local VILLAGER_ROLES = { [0] = true, [1] = true, [2] = true, [3] = true } -- Husband, Wife, Son, Daughter
local WORK_ANIMAL_ROLES = { [12] = true, [15] = true, [16] = true }        -- Ox, Horse, Mule
local raidState = { key = nil, last = -1 }

-- mining: building types that extract goods (EBuildingType) and goods to multiply (EItemType -> mult)
local MINE_TYPES = { [7] = true, [52] = true, [35] = true, [42] = true } -- Mine_Lv1, Mine_Lv2, Quarry, StoneGathererCamp
local MINING_GOODS = {}
do
    local mm = cfg.MiningMultiplier or {}
    MINING_GOODS[14] = mm.IronOre or 1
    MINING_GOODS[145] = mm.Salt or 1
    MINING_GOODS[146] = mm.Clay or 1
    MINING_GOODS[27] = mm.Stone or 1   -- RoughStone
    MINING_GOODS[15] = mm.Stone or 1   -- Rubble (stone)
end
local miningState = {}
local wildWinterHold = nil   -- last logged state of the winter refill hold
local miningSeen = {}

local function liveTick()
    local engine = getEngine()
    if not engine then return end
    local pawn = getPlayerPawn(engine)
    local st = { walk = 0, archer = 0, storage = 0, deposit = 0 }

    -- 6. militia cap
    if pawn and (cfg.MaxMilitiaSquads or 0) > 0 then
        if S(function() return pawn.maxNumOfMilitiaToSpawn end, -1) ~= cfg.MaxMilitiaSquads then
            pcall(function() pawn.maxNumOfMilitiaToSpawn = cfg.MaxMilitiaSquads end)
        end
    end

    -- 7. tree growth rate (game setup parameter)
    if (cfg.TreeGrowthRate or 0) > 0 then
        local gi = FindFirstOf("MLGameInstance")
        if valid(gi) then
            local cur = S(function() return gi.gameSetup.currentGameSetup.treeGrowthRate end)
            if type(cur) == "number" and math.abs(cur - cfg.TreeGrowthRate) > 1e-4 then
                pcall(function() gi.gameSetup.currentGameSetup.treeGrowthRate = cfg.TreeGrowthRate end)
            end
        end
    end

    -- 11. bandit camp cap (thefts are done monthly by encamped bandit squads)
    if (cfg.MaxBanditCamps or -1) >= 0 then
        local gi = FindFirstOf("MLGameInstance")
        if valid(gi) then
            local cur = S(function() return gi.gameSetup.currentGameSetup.maxBanditCamps end)
            if type(cur) == "number" and cur ~= cfg.MaxBanditCamps then
                pcall(function() gi.gameSetup.currentGameSetup.maxBanditCamps = cfg.MaxBanditCamps end)
                log(string.format("max bandit camps: %d -> %d", cur, cfg.MaxBanditCamps))
            end
        end
    end

    -- 11. raid interval: scale daysUntilNextRaid whenever a new raid gets scheduled
    local rim = cfg.RaidIntervalMultiplier or 1
    local wmaster = FindFirstOf("WeatherMaster")
    if valid(wmaster) then
        local days = S(function() return wmaster.daysUntilNextRaid end)
        if type(days) == "number" then
            local key = wmaster:GetAddress()
            if raidState.key ~= key then
                -- first sight of this world: keep the saved countdown as is (it may already be scaled)
                raidState = { key = key, last = days }
                log(string.format("raid countdown on load: %d days", days))
            end
            if days > raidState.last then
                local newDays = days
                if rim ~= 1 and days > 0 then
                    newDays = math.max(1, round(days * rim))
                    pcall(function() wmaster.daysUntilNextRaid = newDays end)
                end
                log(string.format("raid scheduled: %d days (orig %d, x%.2f)", newDays, days, rim))
                days = newDays
            end
            raidState.last = days
        end
    end

    -- player squad ids (for archers)
    local playerSquads = {}
    if pawn then
        local cs = S(function() return pawn.commandedSquads end)
        for i = 1, arrLen(cs) do playerSquads[S(function() return cs[i] end, -1)] = true end
    end

    -- 4a / 5. units
    local wm = cfg.VillagerWalkSpeedMultiplier or 1
    local am = cfg.AnimalWalkSpeedMultiplier or 1
    local dm, rm = cfg.ArcherDamageMultiplier or 1, cfg.ArcherRangeMultiplier or 1
    local units = S(function() return engine.unitArr end)
    for i = 1, arrLen(units) do
        local u = S(function() return units[i] end)
        if valid(u) and not S(function() return u.dead end, true) then
            local addr = S(function() return u:GetAddress() end, 0)
            local role = S(function() return u.currentUnitRole end, -1)
            local sm = (VILLAGER_ROLES[role] and wm) or (WORK_ANIMAL_ROLES[role] and am) or 1
            if sm ~= 1 then
                if applyTracked(u, "Speed", sm, addr .. "|Speed") then st.walk = st.walk + 1 end
            else
                restoreTracked(u, "Speed", addr .. "|Speed")
            end
            if dm ~= 1 or rm ~= 1 then
                local sq = S(function() return u.assignedSquadID end, -1)
                local sr = S(function() return u.shootingRange end, 0)
                if playerSquads[sq] and sr > 0 then
                    local a = applyTracked(u, "rangedAtt", dm, addr .. "|rangedAtt")
                    local b = applyTracked(u, "shootingRange", rm, addr .. "|shootingRange", false, "shootingRangeSq")
                    if a or b then st.archer = st.archer + 1 end
                end
            end
        end
    end

    -- 3. storage of existing buildings (player owned)
    local sm = cfg.StorageMultiplier or 1
    if sm ~= 1 then
        for _, b in ipairs(FindAllOf("SMBuildingMaster") or {}) do
            if valid(b) then
                local owner = S(function() return b.ownerPawn end)
                if pawn and valid(owner) and owner:GetAddress() == pawn:GetAddress() then
                    local addr = b:GetAddress()
                    for _, f in ipairs({ "storageLimitGeneric", "storageLimitLarge", "storageLimitPantry" }) do
                        if applyTracked(b, f, sm, addr .. "|" .. f, true) then st.storage = st.storage + 1 end
                    end
                end
            end
        end
    end

    -- 10. mining: when a mine's own stock of ore grows, add (mult-1) x the increase
    if pawn then
        local pawnAddr = pawn:GetAddress()
        for _, b in ipairs(FindAllOf("SMBuildingMaster") or {}) do
            if valid(b) then
                local bType = S(function() return b.Data.bType end, -1)
                local owner = S(function() return b.ownerPawn end)
                if MINE_TYPES[bType] and valid(owner) and owner:GetAddress() == pawnAddr then
                    local inv = S(function() return b.Inventory end)
                    local baddr = b:GetAddress()
                    for i = 1, arrLen(inv) do
                        local g = inv[i]
                        local t = S(function() return g.Type end, -1)
                        local m = MINING_GOODS[t]
                        if m and m ~= 1 then
                            local cur = S(function() return g.amt end, 0)
                            local key = baddr .. "|" .. t
                            local ms = miningState[key]
                            if not ms and miningSeen[baddr] then ms = { last = 0, frac = 0 } end
                            if ms and cur > ms.last then
                                local want = (cur - ms.last) * (m - 1) + ms.frac
                                local extra = math.floor(want)
                                ms.frac = want - extra
                                if extra > 0 and pcall(function() g.amt = cur + extra end) then
                                    cur = cur + extra
                                    st.mined = (st.mined or 0) + extra
                                end
                            end
                            miningState[key] = { last = cur, frac = ms and ms.frac or 0 }
                        end
                    end
                    miningSeen[baddr] = true
                end
            end
        end
    end

    -- 14. wild gathering spots: raise the cap and top the amount back up
    local wr = cfg.WildResources or {}
    if wr.Enabled then
        local caps = wr.Capacity or {}
        -- Winter is when the game drains the seasonal spots (the tooltip says "in decline").
        -- With StopRefillInWinter the refill stands down for those spots, so winter plays out
        -- as usual and the refill picks up again in spring.
        local holdNow = false
        if wr.Refill and wr.StopRefillInWinter then
            local wm = FindFirstOf("WeatherMaster")
            if valid(wm) then
                local iw = S(function() return wm.isWinter end)
                if iw == nil then iw = (S(function() return wm.Season end, -1) == 0) end  -- ESeason::Winter
                holdNow = iw and true or false
            end
            if wildWinterHold ~= holdNow then
                wildWinterHold = holdNow
                log("wild resources: winter refill hold " .. (holdNow and "on" or "off"))
                flushReport()
            end
        end
        for _, w in ipairs(WILD_RESOURCE_BP) do
            local want = caps[w.key] or 0
            for _, r in ipairs(FindAllOf(w.class) or {}) do
                if valid(r) then
                    local cap = S(function() return r.capacity end, 0)
                    if want > 0 and cap ~= want and pcall(function() r.capacity = want end) then
                        cap = want; st.wildCap = (st.wildCap or 0) + 1
                    end
                    -- a spot marked bSeasonal is the one winter drains; the rest keep refilling
                    local hold = holdNow and S(function() return r.bSeasonal end, false)
                    if wr.Refill and cap > 0 and not hold then
                        local amt = S(function() return r.amt end, 0)
                        if amt < cap and pcall(function() r.amt = cap end) then
                            st.wildFill = (st.wildFill or 0) + (cap - amt)
                        end
                    end
                end
            end
        end
    end

    -- 8c. rich deposits currently in the world
    if cfg.AllResourcesRich then
        local deps = S(function() return engine.deposits end)
        for i = 1, arrLen(deps) do
            local d = S(function() return deps[i] end)
            if valid(d) and not S(function() return d.bRichDeposit end, true) then
                pcall(function() d.bRichDeposit = true end); st.deposit = st.deposit + 1
            end
        end
        for _, sg in ipairs(FindAllOf("MLSaveGame") or {}) do patchSaveGame(sg, "memory") end
    end

    st.mined = st.mined or 0
    st.wildCap = st.wildCap or 0
    st.wildFill = st.wildFill or 0
    if st.walk + st.archer + st.storage + st.deposit + st.mined + st.wildCap + st.wildFill > 0 then
        log(string.format("live: walkSpeed=%d archers=%d storage=%d deposits=%d mined+%d wildCap=%d wild+%d",
            st.walk, st.archer, st.storage, st.deposit, st.mined, st.wildCap, st.wildFill))
        flushReport()
    end
end

-- ============================================================
-- Main loop
-- ============================================================
local pending = { "debugDump", "production", "storageTable", "workSpeed", "militiaSize", "resourceSettings", "wildlifeBreeding", "freeOxen", "freeLivestock", "superPerk" }

LoopAsync(3000, function()
    ExecuteInGameThread(function()
        local remaining = {}
        for _, name in ipairs(pending) do
            local ok, done = pcall(tasks[name])
            if not ok then
                log("task " .. name .. " ERROR: " .. tostring(done))
            elseif not done then
                table.insert(remaining, name)
            end
        end
        if #remaining ~= #pending then flushReport() end
        pending = remaining
        local ok, err = pcall(liveTick)
        if not ok then log("liveTick ERROR: " .. tostring(err)); flushReport() end
    end)
    return false
end)

-- ============================================================
-- Hotkeys
-- ============================================================
local function getCheatManager()
    local pc = FindFirstOf("MLPlayerController_C")
    if not valid(pc) then pc = FindFirstOf("PlayerController") end
    if not valid(pc) then return nil end
    local cm = S(function() return pc.CheatManager end)
    if valid(cm) and S(function() return cm:IsA("/Script/ManorLords.MLCheatManager") end, false) then return cm end
    local cls = StaticFindObject("/Game/CPP_BP/MLCheatManager.MLCheatManager_C")
    if not valid(cls) then cls = S(function() return pc.CheatClass end) end
    if not valid(cls) then log("cheat manager class not found"); return nil end
    cm = StaticConstructObject(cls, pc)
    if valid(cm) then
        pc.CheatManager = cm
        log("cheat manager created: " .. cm:GetFullName())
        return cm
    end
end

local function unitsSnapshot()
    local engine = getEngine()
    if not engine then return end
    local pawn = getPlayerPawn(engine)
    local pawnAddr = pawn and pawn:GetAddress() or 0
    local playerSquads = {}
    if pawn then
        local cs = S(function() return pawn.commandedSquads end)
        for i = 1, arrLen(cs) do playerSquads[S(function() return cs[i] end, -1)] = true end
    end
    local L = { "role | owner | raiding | ItemsLooted | squad | inventory | tasks" }
    local units = S(function() return engine.unitArr end)
    for i = 1, arrLen(units) do
        local u = S(function() return units[i] end)
        if valid(u) and not S(function() return u.dead end, true) then
            local inv = goodsStr(S(function() return u.Inventory end))
            local raiding = S(function() return u.raiding end, false)
            local looted = S(function() return u.ItemsLooted end, 0)
            if inv ~= "" or raiding or looted > 0 then
                local tasks = {}
                local ts = S(function() return u.Tasks end)
                for j = 1, arrLen(ts) do
                    local t = ts[j]
                    table.insert(tasks, tostring(S(function() return t.Type end, "?")) .. ":" ..
                        tostring(S(function() return t.resourceType end, "?")) .. "x" .. tostring(S(function() return t.resourceAmt end, "?")))
                end
                local sq = S(function() return u.assignedSquadID end, -1)
                table.insert(L, string.format("%s | %s | %s | %s | %s | %s | %s",
                    tostring(S(function() return u.currentUnitRole end, "?")),
                    playerSquads[sq] and "player-squad" or (sq > 0 and "other-squad" or "civil"),
                    tostring(raiding), tostring(looted), tostring(sq), inv, table.concat(tasks, ",")))
            end
        end
    end
    local sqs = S(function() return engine.squads end)
    table.insert(L, "")
    for i = 1, arrLen(sqs) do
        local s = sqs[i]
        table.insert(L, string.format("SQUAD id=%s type=%s name=%s units=%s owner=%s",
            tostring(S(function() return s.ID end, "?")), tostring(S(function() return s.squadType end, "?")),
            fstr(S(function() return s.Name end, "")), tostring(arrLen(S(function() return s.assignedRecruits end))),
            (S(function() return s.ownerPawn:GetAddress() end, 0) == pawnAddr) and "PLAYER" or "other"))
    end
    local wmaster = FindFirstOf("WeatherMaster")
    if valid(wmaster) then
        table.insert(L, string.format("WEATHER daysUntilNextRaid=%s daysSinceLastRaid=%s",
            tostring(S(function() return wmaster.daysUntilNextRaid end, "?")), tostring(S(function() return wmaster.daysSinceLastRaid end, "?"))))
    end
    -- reflected property offsets of the unit class (to map raw offsets seen in the game code)
    local function addrOf(ud)
        local s = tostring(ud)
        local h = s:match("0x(%x+)") or s:match(": (%x+)")
        return h and tonumber(h, 16) or nil
    end
    local sample
    for i = 1, arrLen(units) do
        local u = S(function() return units[i] end)
        if valid(u) and VILLAGER_ROLES[S(function() return u.currentUnitRole end, -1)] then sample = u; break end
    end
    if sample then
        table.insert(L, "")
        table.insert(L, "UNIT PROPERTY OFFSETS (" .. sample:GetFullName() .. ")")
        local base = sample:GetAddress()
        local c = sample:GetClass()
        while valid(c) do
            S(function()
                c:ForEachProperty(function(p)
                    local ptr = S(function() return p:ContainerPtrToValuePtr(sample, 0) end)
                    local a = ptr and addrOf(ptr)
                    if a then
                        local n = p:GetFName():ToString()
                        local v = S(function() return sample[n] end)
                        table.insert(L, string.format("  +0x%X %s = %s", a - base, n,
                            (type(v) == "number" or type(v) == "boolean") and tostring(v) or type(v)))
                    end
                end)
            end)
            c = S(function() return c:GetSuperStruct() end)
            if c and not S(function() return c:GetFullName():find("ManorLords", 1, true) end, false) then break end
        end
    end

    -- Player's residential buildings: occupant slots from the building table next to the
    -- families actually assigned to / living in each house (for the families-per-house feature)
    do
        local HOME_TYPES = { [3] = true, [8] = true, [60] = true, [484] = true }
        local engine = getEngine()
        local pawn = engine and getPlayerPawn(engine)
        local pawnAddr = pawn and S(function() return pawn:GetAddress() end)
        local bs = findDT("/Game/NotStronghold/Data/buildingStats.buildingStats")
        table.insert(L, "")
        table.insert(L, "== residential buildings (player) ==")
        local n = 0
        local regions, regionOrder = {}, {}
        for _, b in ipairs(FindAllOf("SMBuildingMaster") or {}) do
            if valid(b) then
                local bType = S(function() return b.Data.bType end, -1)
                local owner = S(function() return b.ownerPawn end)
                local mine = not pawnAddr or (valid(owner) and owner:GetAddress() == pawnAddr)
                local isHome = S(function() return b:isResidentialBuilding() end, false) or HOME_TYPES[bType]
                if mine and isHome then
                    local row = bs and S(function() return bs:FindRow(tostring(bType)) end)
                    -- family IDs are numbered per region, so the region tells duplicates apart
                    local reg = S(function() return b.Region end)
                    local rname = valid(reg) and fstr(S(function() return reg.regionName end, "?")) or "?"
                    if valid(reg) and not regions[rname] then regions[rname] = reg; table.insert(regionOrder, rname) end
                    table.insert(L, string.format("HOME region=%s bType=%d built=%s tableOcc=%s occupants=[%s] assigned=[%s] upgradesDone=[%s]",
                        rname, bType, tostring(S(function() return b.Data.constructed end, "?")),
                        row and vec4Str(S(function() return row.occupantTypes end)) or "?",
                        intsStr(S(function() return b.occupantFamilyIDs end)),
                        intsStr(S(function() return b.assignedFamilyIDs end)),
                        intsStr(S(function() return b.upgradesDone end))))
                    n = n + 1
                end
            end
        end
        table.insert(L, string.format("(%d residential buildings)", n))
        for _, rname in ipairs(regionOrder) do
            local reg = regions[rname]
            table.insert(L, string.format("REGION %s families=%s homeless=%s",
                rname, tostring(S(function() return reg:getTotalNumFamilies() end, "?")),
                tostring(S(function() return reg:getNumHomelessFamilies() end, "?"))))
        end
    end

    local f = io.open(MOD_DIR .. "units_snapshot.txt", "w")
    if f then f:write(table.concat(L, "\n")); f:close() end
    log("units_snapshot.txt written (" .. #L .. " lines)")
    flushReport()
end

if cfg.FreeOxHotkey then
    RegisterKeyBind(Key.O, { ModifierKey.CONTROL, ModifierKey.SHIFT }, function()
        ExecuteInGameThread(function()
            local cm = getCheatManager()
            if cm then
                local ok, err = pcall(function() cm:spawnOxen(1) end)
                log(ok and "free ox spawned" or ("spawnOxen failed: " .. tostring(err)))
            end
            flushReport()
        end)
    end)
end

if cfg.DebugHotkeys then
    RegisterKeyBind(Key.U, { ModifierKey.CONTROL, ModifierKey.SHIFT }, function()
        ExecuteInGameThread(function()
            local ok, err = pcall(unitsSnapshot)
            if not ok then log("snapshot ERROR: " .. tostring(err)); flushReport() end
        end)
    end)
    RegisterKeyBind(Key.R, { ModifierKey.CONTROL, ModifierKey.SHIFT }, function()
        ExecuteInGameThread(function()
            local cm = getCheatManager()
            if cm then
                local ok, err = pcall(function() cm:raiders() end)
                log(ok and "raiders spawned (test)" or ("raiders failed: " .. tostring(err)))
            end
            flushReport()
        end)
    end)
end

loadNativePatches()
log("MLTweaks loaded")
flushReport()
