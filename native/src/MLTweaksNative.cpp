// MLTweaksNative: in-memory code patches for Manor Lords (0.8.065).
// Loaded from MLTweaks Lua via package.loadlib(path, "*"); all work happens in DllMain.
// Patches are located by byte signatures and only applied when the original bytes match,
// so a game update that changes the code results in "not found" instead of a bad write.
// The executable on disk is never modified.

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char* kModDir = "ue4ss\\Mods\\MLTweaks\\";
const char* kVersion = "1.1.0";

FILE* g_log = nullptr;

void Log(const char* fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

struct Patch {
    const char* group;       // config key that enables this patch
    const char* name;
    const char* signature;   // hex bytes, "??" = wildcard
    int offset;              // offset of the patched bytes inside the signature
    const char* original;    // expected original bytes at offset
    const char* replacement; // new bytes (same length as original)
};

// ---------------------------------------------------------------------------
// RichResources: every resource node / mineral deposit is created as "rich".
// ---------------------------------------------------------------------------
const Patch kPatches[] = {
    // SpawnResourceNode(type, location, bRich, ...): "movzx esi, r9b" -> "mov sil, 1; nop"
    { "RichResources", "node_spawn_flag",
      "4C 8B B5 ?? ?? ?? ?? 41 0F B6 F1 4D 8B F8 C6 44 24 20 FF", 7,
      "41 0F B6 F1", "40 B6 01 90" },
    // Save load: node->bRich = saved.bRichNode -> "mov eax, 1; nop"
    { "RichResources", "node_load_flag",
      "4C 8B C0 4D 85 C0 74 ?? 41 0F B6 44 24 FC 41 88 80 AC 02 00 00", 8,
      "41 0F B6 44 24 FC", "B8 01 00 00 00 90" },
    // New game generation: rich-amount argument for each region's node types -> "mov r8b, 1; nop"
    { "RichResources", "node_gen_amounts",
      "44 0F B6 CA 49 8B D4 48 89 44 24 20 45 0F B6 C2 49 8B CD E8", 12,
      "45 0F B6 C2", "41 B0 01 90" },
    // SpawnDeposit (iron / salt / clay): "movzx edi, byte [rbp+78h]" -> "mov dil, 1; nop"
    { "RichResources", "deposit_flag",
      "48 85 F6 0F 84 ?? ?? ?? ?? 0F B6 7D 78 41 8B 0F 40 88 BE D4 02 00 00", 9,
      "0F B6 7D 78", "40 B7 01 90" },

    // -----------------------------------------------------------------------
    // HarvestAllYear support: seasonal crops (grain) only get their per-plant yield assigned
    // on the first day of the harvest season ("cmp r14d, edx / jne" compares today with
    // HarvestSeason.X). With an all-year window that day may not come around while the crop
    // stands, so the yield stays 0 and harvesting produces nothing. NOP-ing the jne (both
    // bytes) recomputes the yield every day through the very same seasonal code path that
    // vanilla uses on the season's first day, so the values match the predicted yield.
    // -----------------------------------------------------------------------
    { "HarvestAllYear", "seasonal_yield_daily",
      "B1 01 88 4C 24 40 44 3B F2 75 0B 0F B6 F1 EB 09 32 C9", 9,
      "75 0B", "90 90" },

    // -----------------------------------------------------------------------
    // NoFertilityLoss: queued fertility change of the planted crop's channel is -0.002;
    // replace "movaps xmm0, xmm7" (the negative delta) with "xorps xmm0, xmm0" (no change).
    // Fallow regeneration (+0.005) and other channels (+0.002) are untouched.
    // -----------------------------------------------------------------------
    { "NoFertilityLoss", "planted_crop_delta",
      "44 3A CA 75 05 0F 28 C7 EB 04 41 0F 28 C0", 5,
      "0F 28 C7", "0F 57 C0" },

    // -----------------------------------------------------------------------
    // WildlifeBreed: the daily breeding step for deer (node type 4) and small game
    // (type 10) adds "herd size / 2" to a progress counter and only runs with at least
    // two animals left; when the counter passes ResourceNodeProperties.BreedingThreshold
    // (deer 162, small game 30) one animal is added and the counter resets.
    //   breed_solo          "cmp edx, 1" -> "cmp edx, 0": a herd of one still breeds
    //   breed_full_progress "sar eax, 1" -> 2x nop: progress gains the full herd size,
    //                       which doubles the speed and lets a single animal make progress
    // The threshold itself is a settings value and is scaled from Lua instead.
    // -----------------------------------------------------------------------
    { "WildlifeBreed", "breed_solo",
      "2B FA 85 FF 0F 8E ?? ?? ?? ?? 83 FA 01 0F 8E", 10,
      "83 FA 01", "83 FA 00" },
    { "WildlifeBreed", "breed_full_progress",
      "8B C2 99 2B C2 D1 F8 48 8B 19 01 83 C8 0A 00 00", 5,
      "D1 F8", "90 90" },
};

bool ParseHex(const char* s, std::vector<int>& out)
{
    out.clear();
    while (*s) {
        while (*s == ' ') ++s;
        if (!*s) break;
        if (s[0] == '?' && s[1] == '?') { out.push_back(-1); s += 2; continue; }
        char buf[3] = { s[0], s[1], 0 };
        char* end = nullptr;
        long v = strtol(buf, &end, 16);
        if (end != buf + 2) return false;
        out.push_back(static_cast<int>(v));
        s += 2;
    }
    return !out.empty();
}

bool GetTextSection(uint8_t*& begin, size_t& size)
{
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 5) == 0) {
            begin = base + sec->VirtualAddress;
            size = sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// Returns all matches (to detect ambiguous signatures).
std::vector<uint8_t*> FindAll(uint8_t* begin, size_t size, const std::vector<int>& sig)
{
    std::vector<uint8_t*> hits;
    if (sig.empty() || sig[0] < 0 || size < sig.size()) return hits;
    const uint8_t first = static_cast<uint8_t>(sig[0]);
    uint8_t* p = begin;
    uint8_t* last = begin + size - sig.size();
    while (p <= last) {
        p = static_cast<uint8_t*>(memchr(p, first, static_cast<size_t>(last - p) + 1));
        if (!p) break;
        bool ok = true;
        for (size_t i = 1; i < sig.size(); ++i) {
            if (sig[i] >= 0 && p[i] != static_cast<uint8_t>(sig[i])) { ok = false; break; }
        }
        if (ok) hits.push_back(p);
        ++p;
    }
    return hits;
}

bool WriteCode(uint8_t* at, const std::vector<int>& bytes)
{
    DWORD old;
    if (!VirtualProtect(at, bytes.size(), PAGE_EXECUTE_READWRITE, &old)) return false;
    for (size_t i = 0; i < bytes.size(); ++i) at[i] = static_cast<uint8_t>(bytes[i]);
    VirtualProtect(at, bytes.size(), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, bytes.size());
    return true;
}

// Code this DLL generates (trampolines, code caves) is written while its page is read/write
// only and then switched to execute/read, so none of our pages is ever writable and executable
// at the same time. (WriteCode above cannot do that for the game's own code: other threads may
// be running it, so its page has to stay executable for the few instructions of the write.)
bool SealCode(uint8_t* at, size_t size)
{
    DWORD old;
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READ, &old)) return false;
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return true;
}

// ---------------------------------------------------------------------------
// Carry capacity: detour of the "new transport task" function
//   void NewTransportTask(SMUnit* unit, SMBuildingMaster* from, SMBuildingMaster* to, int goodType, int amount)
// Vanilla passes amount = 1 for hand carrying and min(stock, cartCap, need) for handcarts.
// The hook raises the amount to the configured value, clamped to the source building's stock
// so that no goods are created out of thin air.
// ---------------------------------------------------------------------------
const char* kTransportSig =
    "4C 8B DC 49 89 5B 18 49 89 73 20 55 57 41 57 49 8D AB 38 FF FF FF 48 81 EC B0 01 00 00 45 8B F9 49 8B D8 48 8B F2";
const int kStolenBytes = 15; // mov r11,rsp / mov [r11+18h],rbx / mov [r11+20h],rsi / push rbp / push rdi / push r15

// SMBuildingMaster::Inventory (TArray<FGood>): data at +0x438, num at +0x440; FGood = { int Type; int amt; ... } stride 0x18
const size_t kInvData = 0x438, kInvNum = 0x440, kGoodStride = 0x18;

using TransportFn = uint64_t(__fastcall*)(void*, uint8_t*, uint8_t*, int, int);
TransportFn g_origTransport = nullptr;
int g_normalCarry = 0;
int g_cartCarry = 0;

int StockOf(uint8_t* building, int goodType)
{
    __try {
        if (!building) return -1;
        uint8_t* items = *reinterpret_cast<uint8_t**>(building + kInvData);
        int num = *reinterpret_cast<int*>(building + kInvNum);
        if (!items || num < 0 || num > 4096) return -1;
        for (int i = 0; i < num; ++i) {
            uint8_t* g = items + i * kGoodStride;
            if (*reinterpret_cast<int*>(g) == goodType) return *reinterpret_cast<int*>(g + 4);
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// SMBuildingMaster::Data.bType (EBuildingType) is at +0x3A8
const size_t kBuildingType = 0x3A8;
bool g_bulkDest[512] = {};

int BuildingType(uint8_t* building)
{
    __try {
        return building ? *reinterpret_cast<int*>(building + kBuildingType) : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

uint64_t __fastcall TransportHook(void* unit, uint8_t* from, uint8_t* to, int goodType, int amount)
{
    // Only bulk-haul into storage-like buildings. Deliveries to workshops (tools, inputs)
    // are demand-driven; raising them just makes the surplus get hauled back.
    int destType = BuildingType(to);
    if (destType < 0 || destType >= 512 || !g_bulkDest[destType])
        return g_origTransport(unit, from, to, goodType, amount);

    int target = (amount <= 1) ? g_normalCarry : g_cartCarry;
    if (target > amount) {
        int stock = StockOf(from, goodType);
        // guard against reading a non-building / bogus pointer
        if (stock > amount && stock < 100000) amount = (stock < target) ? stock : target;
    }
    return g_origTransport(unit, from, to, goodType, amount);
}

bool InstallTransportHook(uint8_t* text, size_t textSize)
{
    std::vector<int> sig;
    ParseHex(kTransportSig, sig);
    auto hits = FindAll(text, textSize, sig);
    if (hits.size() != 1) {
        Log("[Carry] transport task: signature found %zu times - SKIPPED", hits.size());
        return false;
    }
    uint8_t* target = hits[0];

    // trampoline: stolen bytes + jmp [rip] back to target+kStolenBytes
    auto tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!tramp) { Log("[Carry] VirtualAlloc failed"); return false; }
    memcpy(tramp, target, kStolenBytes);
    uint8_t* j = tramp + kStolenBytes;
    j[0] = 0xFF; j[1] = 0x25; *reinterpret_cast<uint32_t*>(j + 2) = 0;
    *reinterpret_cast<uint64_t*>(j + 6) = reinterpret_cast<uint64_t>(target + kStolenBytes);
    if (!SealCode(tramp, 64)) { Log("[Carry] VirtualProtect failed"); return false; }
    g_origTransport = reinterpret_cast<TransportFn>(tramp);

    // detour: jmp [rip] -> TransportHook, pad with nop
    std::vector<int> patch = { 0xFF, 0x25, 0, 0, 0, 0 };
    uint64_t hookAddr = reinterpret_cast<uint64_t>(&TransportHook);
    for (int i = 0; i < 8; ++i) patch.push_back(static_cast<int>((hookAddr >> (8 * i)) & 0xFF));
    while (static_cast<int>(patch.size()) < kStolenBytes) patch.push_back(0x90);
    if (!WriteCode(target, patch)) { Log("[Carry] VirtualProtect failed"); return false; }
    Log("[Carry] transport task hooked at exe+0x%llX (hand=%d, cart=%d)",
        static_cast<unsigned long long>(target - reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr))),
        g_normalCarry, g_cartCarry);
    return true;
}

// ---------------------------------------------------------------------------
// Plant harvest yield: in the harvest handler the per-plant yield is loaded into r12d
// ("mov r12d, [rdi+4Ch]", rdi = FPlant*), then "test r12d, r12d / jle skip".
// We replace that test+jle (9 bytes, not a branch target) with a jump to a cave that
// multiplies r12d by a per-crop-type factor (FPlant+0 = ECropType) and then re-does the test.
// eax/rcx are free at this point (both overwritten before being read on every path).
// ---------------------------------------------------------------------------
const char* kHarvestSig = "44 8B 67 4C 85 DB 0F 8E ?? ?? ?? ?? 45 85 E4 0F 8E ?? ?? ?? ??";
const int kHarvestOffset = 12;   // "45 85 E4 0F 8E rel32"
int g_cropYieldMul[16] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };

uint8_t* AllocNear(uint8_t* target, size_t size)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t t = reinterpret_cast<uintptr_t>(target);
    for (uintptr_t d = gran; d < 0x70000000; d += gran) {
        for (int sign = -1; sign <= 1; sign += 2) {
            uintptr_t a = (sign < 0) ? (t > d ? t - d : 0) : t + d;
            if (!a) continue;
            a &= ~(gran - 1);
            void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (p) return static_cast<uint8_t*>(p);
        }
    }
    return nullptr;
}

bool InstallHarvestHook(uint8_t* text, size_t textSize)
{
    std::vector<int> sig;
    ParseHex(kHarvestSig, sig);
    auto hits = FindAll(text, textSize, sig);
    if (hits.size() != 1) {
        Log("[CropYield] harvest handler: signature found %zu times - SKIPPED", hits.size());
        return false;
    }
    uint8_t* site = hits[0] + kHarvestOffset;          // test r12d,r12d ; jle rel32
    uint8_t* cont = site + 9;                           // fall-through
    uint8_t* skip = site + 9 + *reinterpret_cast<int32_t*>(site + 5); // jle target

    uint8_t* cave = AllocNear(site, 256);
    if (!cave) { Log("[CropYield] could not allocate near memory"); return false; }

    std::vector<uint8_t> c;
    auto emit = [&](std::initializer_list<uint8_t> b) { c.insert(c.end(), b); };
    auto emitAbsJmp = [&](uint8_t* to) {
        emit({ 0xFF, 0x25, 0, 0, 0, 0 });
        uint64_t v = reinterpret_cast<uint64_t>(to);
        for (int i = 0; i < 8; ++i) c.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    emit({ 0x0F, 0xB6, 0x07 });                         // movzx eax, byte [rdi]   (crop type)
    emit({ 0x83, 0xE0, 0x0F });                         // and eax, 15
    size_t leaPos = c.size();
    emit({ 0x48, 0x8D, 0x0D, 0, 0, 0, 0 });             // lea rcx, [rip+table]
    emit({ 0x44, 0x0F, 0xAF, 0x24, 0x81 });             // imul r12d, [rcx+rax*4]
    emit({ 0x45, 0x85, 0xE4 });                         // test r12d, r12d
    emit({ 0x7E, 14 });                                 // jle +14 (over the next abs jmp)
    emitAbsJmp(cont);                                   // 14 bytes
    emitAbsJmp(skip);
    while (c.size() % 4) c.push_back(0xCC);
    size_t tablePos = c.size();
    for (int v : g_cropYieldMul)
        for (int i = 0; i < 4; ++i) c.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i)));
    *reinterpret_cast<int32_t*>(&c[leaPos + 3]) = static_cast<int32_t>(tablePos - (leaPos + 7));
    memcpy(cave, c.data(), c.size());
    if (!SealCode(cave, c.size())) { Log("VirtualProtect on a code cave failed"); return false; }

    int64_t rel = reinterpret_cast<int64_t>(cave) - reinterpret_cast<int64_t>(site + 5);
    if (rel > INT32_MAX || rel < INT32_MIN) { Log("[CropYield] cave out of range"); return false; }
    std::vector<int> patch = { 0xE9 };
    for (int i = 0; i < 4; ++i) patch.push_back(static_cast<int>((static_cast<uint32_t>(rel) >> (8 * i)) & 0xFF));
    while (patch.size() < 9) patch.push_back(0x90);
    if (!WriteCode(site, patch)) { Log("[CropYield] VirtualProtect failed"); return false; }

    std::string t;
    for (int i = 0; i < 16; ++i) t += std::to_string(g_cropYieldMul[i]) + (i < 15 ? "," : "");
    Log("[CropYield] harvest handler hooked at exe+0x%llX, multipliers by crop type [%s]",
        static_cast<unsigned long long>(site - reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr))), t.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Fetch capacity: the "go fetch goods" task builder (used for construction materials and
// other demand-driven deliveries) computes
//     amount = min(capacityArg > 0 ? capacityArg : unit->carryCapacity(+0x300), maxNeeded)
// Hand carrying has capacity 1, a handcart more. We raise the capacity read from the unit
// (hand -> HandCarry, cart -> CartCarry); the min() with the needed amount stays intact,
// so nothing is over-delivered.
// Patched instruction: "mov ecx, [rsi+300h]" (6 bytes, not a branch target).
// ---------------------------------------------------------------------------
const char* kFetchCapSig = "8B 8D 70 02 00 00 85 C9 7F 06 8B 8E 00 03 00 00 45 85 E4";
const int kFetchCapOffset = 10;

bool InstallFetchCapacityHook(uint8_t* text, size_t textSize)
{
    std::vector<int> sig;
    ParseHex(kFetchCapSig, sig);
    auto hits = FindAll(text, textSize, sig);
    if (hits.size() != 1) {
        Log("[Carry] fetch capacity: signature found %zu times - SKIPPED", hits.size());
        return false;
    }
    uint8_t* site = hits[0] + kFetchCapOffset;
    uint8_t* back = site + 6;
    uint8_t* cave = AllocNear(site, 128);
    if (!cave) { Log("[Carry] fetch capacity: could not allocate near memory"); return false; }

    std::vector<uint8_t> c;
    auto emit = [&](std::initializer_list<uint8_t> b) { c.insert(c.end(), b); };
    auto emit32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) c.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    emit({ 0x8B, 0x8E, 0x00, 0x03, 0x00, 0x00 });            // mov ecx, [rsi+300h]   (original)
    // only bulk-fetch for a building that is still under construction:
    // arg6 = target building ([rbp+248h]), FBuildingDataStruct.constructed = +0x3B1
    emit({ 0x48, 0x8B, 0x85, 0x48, 0x02, 0x00, 0x00 });      // mov rax, [rbp+248h]
    emit({ 0x48, 0x85, 0xC0 });                              // test rax, rax
    size_t jz = c.size(); emit({ 0x74, 0x00 });              // je  -> keep
    emit({ 0x80, 0xB8, 0xB1, 0x03, 0x00, 0x00, 0x00 });      // cmp byte [rax+3B1h], 0
    size_t jnz = c.size(); emit({ 0x75, 0x00 });             // jne -> keep (already built)
    emit({ 0xB8 }); emit32(static_cast<uint32_t>(g_normalCarry)); // mov eax, HandCarry
    emit({ 0x83, 0xF9, 0x01 });                              // cmp ecx, 1
    emit({ 0x7E, 0x05 });                                    // jle +5
    emit({ 0xB8 }); emit32(static_cast<uint32_t>(g_cartCarry));   // mov eax, CartCarry
    emit({ 0x39, 0xC1 });                                    // cmp ecx, eax
    emit({ 0x0F, 0x4C, 0xC8 });                              // cmovl ecx, eax  (only ever raise)
    size_t keep = c.size();
    c[jz + 1] = static_cast<uint8_t>(keep - (jz + 2));
    c[jnz + 1] = static_cast<uint8_t>(keep - (jnz + 2));
    emit({ 0xFF, 0x25, 0, 0, 0, 0 });                        // jmp [rip] -> back
    uint64_t v = reinterpret_cast<uint64_t>(back);
    for (int i = 0; i < 8; ++i) c.push_back(static_cast<uint8_t>(v >> (8 * i)));
    memcpy(cave, c.data(), c.size());
    if (!SealCode(cave, c.size())) { Log("VirtualProtect on a code cave failed"); return false; }

    int64_t rel = reinterpret_cast<int64_t>(cave) - reinterpret_cast<int64_t>(site + 5);
    if (rel > INT32_MAX || rel < INT32_MIN) { Log("[Carry] fetch capacity: cave out of range"); return false; }
    std::vector<int> patch = { 0xE9 };
    for (int i = 0; i < 4; ++i) patch.push_back(static_cast<int>((static_cast<uint32_t>(rel) >> (8 * i)) & 0xFF));
    patch.push_back(0x90);
    if (!WriteCode(site, patch)) { Log("[Carry] fetch capacity: VirtualProtect failed"); return false; }
    Log("[Carry] fetch capacity hooked (construction sites only) at exe+0x%llX (hand=%d, cart=%d)",
        static_cast<unsigned long long>(site - reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr))),
        g_normalCarry, g_cartCarry);
    return true;
}

// ---------------------------------------------------------------------------
// Free animal orders: SMRegion::GetRegionalWealthCostForUpgrade(upgradeID) returns the
// regional wealth price of an upgrade. For the six "order an animal" upgrades
// (13 OrderOxen, 19 OrderHorse, 21 OrderMule, 26 OrderHuntingHound, 27 OrderPig,
// 28 OrderGoat) it ignores the DT_Upgrades cost entirely and computes
//     max(1, round(importPrice * regionalImportModifier)) + UTradeSettings::importFee
// (importFee = CDO+0x3C = 10), which is why clearing the table cost and zeroing the
// animal's item value still left a price of 11 (= max(1, 0) + 10).
// The very same function is used for the "can the region afford it" check and for the
// actual deduction ("if (cost > 0) SubtractRegionalWealth(cost)"), so returning 0 for the
// selected upgrade IDs makes those orders genuinely free. All other upgrades keep their
// vanilla price and importFee itself is left alone, so general trade is unaffected.
// ---------------------------------------------------------------------------
const char* kUpgradeCostSig =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 8B DA 48 8B E9 85 D2 "
    "0F 84 ?? ?? ?? ?? 8B CA E8 ?? ?? ?? ?? 84 C0";
// three "mov [rsp+N], reg" of the prologue - 15 bytes, no branch target inside
const int kUpgradeCostStolen = 15;

using UpgradeCostFn = int(__fastcall*)(void*, int);
UpgradeCostFn g_origUpgradeCost = nullptr;
bool g_freeUpgrade[64] = {};

int __fastcall UpgradeCostHook(void* region, int upgradeID)
{
    if (upgradeID > 0 && upgradeID < 64 && g_freeUpgrade[upgradeID]) return 0;
    return g_origUpgradeCost(region, upgradeID);
}

bool InstallUpgradeCostHook(uint8_t* text, size_t textSize)
{
    std::vector<int> sig;
    ParseHex(kUpgradeCostSig, sig);
    auto hits = FindAll(text, textSize, sig);
    if (hits.size() != 1) {
        Log("[FreeUpgrade] wealth cost: signature found %zu times - SKIPPED", hits.size());
        return false;
    }
    uint8_t* target = hits[0];

    // trampoline: stolen bytes + jmp [rip] back to target+kUpgradeCostStolen
    auto tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!tramp) { Log("[FreeUpgrade] VirtualAlloc failed"); return false; }
    memcpy(tramp, target, kUpgradeCostStolen);
    uint8_t* j = tramp + kUpgradeCostStolen;
    j[0] = 0xFF; j[1] = 0x25; *reinterpret_cast<uint32_t*>(j + 2) = 0;
    *reinterpret_cast<uint64_t*>(j + 6) = reinterpret_cast<uint64_t>(target + kUpgradeCostStolen);
    if (!SealCode(tramp, 64)) { Log("[FreeUpgrade] VirtualProtect failed"); return false; }
    g_origUpgradeCost = reinterpret_cast<UpgradeCostFn>(tramp);

    // detour: jmp [rip] -> UpgradeCostHook, pad with nop
    std::vector<int> patch = { 0xFF, 0x25, 0, 0, 0, 0 };
    uint64_t hookAddr = reinterpret_cast<uint64_t>(&UpgradeCostHook);
    for (int i = 0; i < 8; ++i) patch.push_back(static_cast<int>((hookAddr >> (8 * i)) & 0xFF));
    while (static_cast<int>(patch.size()) < kUpgradeCostStolen) patch.push_back(0x90);
    if (!WriteCode(target, patch)) { Log("[FreeUpgrade] VirtualProtect failed"); return false; }

    std::string ids;
    for (int i = 0; i < 64; ++i)
        if (g_freeUpgrade[i]) ids += (ids.empty() ? "" : ",") + std::to_string(i);
    Log("[FreeUpgrade] upgrade wealth cost hooked at exe+0x%llX, free upgrade IDs [%s]",
        static_cast<unsigned long long>(target - reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr))),
        ids.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// WildlifeMax: in the same daily breeding step the room left in a herd is
//     room = node->maxAnimals (record+0x60) - herdSize (record+0x20)
// and breeding stops once room <= 0, so maxAnimals is the per-node population cap
// (from ResourceNodeProperties.Max[Rich]ResourceAmount when the map was generated).
// The hook multiplies the cap used for that comparison, which lets a herd grow past
// the vanilla cap without writing anything into the saved node data - switching the
// mod off simply stops the growth, it does not corrupt a save.
// Patched instructions: "mov edi, [rax+60h]" + "mov r12, [rax+1F8h]" (10 bytes,
// nothing jumps into them).
// ---------------------------------------------------------------------------
const char* kWildlifeCapSig = "8B 78 60 4C 8B A0 F8 01 00 00 2B FA 85 FF";
int g_wildlifeMax = 1;

bool InstallWildlifeCapHook(uint8_t* text, size_t textSize)
{
    std::vector<int> sig;
    ParseHex(kWildlifeCapSig, sig);
    auto hits = FindAll(text, textSize, sig);
    if (hits.size() != 1) {
        Log("[WildlifeMax] herd cap: signature found %zu times - SKIPPED", hits.size());
        return false;
    }
    uint8_t* site = hits[0];
    uint8_t* back = site + 10;
    uint8_t* cave = AllocNear(site, 64);
    if (!cave) { Log("[WildlifeMax] could not allocate near memory"); return false; }

    std::vector<uint8_t> c;
    auto emit = [&](std::initializer_list<uint8_t> b) { c.insert(c.end(), b); };
    emit({ 0x8B, 0x78, 0x60 });                              // mov edi, [rax+60h]   (original)
    emit({ 0x69, 0xFF });                                    // imul edi, edi, mult
    for (int i = 0; i < 4; ++i) c.push_back(static_cast<uint8_t>(static_cast<uint32_t>(g_wildlifeMax) >> (8 * i)));
    emit({ 0x4C, 0x8B, 0xA0, 0xF8, 0x01, 0x00, 0x00 });      // mov r12, [rax+1F8h]  (original)
    emit({ 0xFF, 0x25, 0, 0, 0, 0 });                        // jmp [rip] -> back
    uint64_t v = reinterpret_cast<uint64_t>(back);
    for (int i = 0; i < 8; ++i) c.push_back(static_cast<uint8_t>(v >> (8 * i)));
    memcpy(cave, c.data(), c.size());
    if (!SealCode(cave, c.size())) { Log("VirtualProtect on a code cave failed"); return false; }

    int64_t rel = reinterpret_cast<int64_t>(cave) - reinterpret_cast<int64_t>(site + 5);
    if (rel > INT32_MAX || rel < INT32_MIN) { Log("[WildlifeMax] cave out of range"); return false; }
    std::vector<int> patch = { 0xE9 };
    for (int i = 0; i < 4; ++i) patch.push_back(static_cast<int>((static_cast<uint32_t>(rel) >> (8 * i)) & 0xFF));
    while (patch.size() < 10) patch.push_back(0x90);
    if (!WriteCode(site, patch)) { Log("[WildlifeMax] VirtualProtect failed"); return false; }
    Log("[WildlifeMax] herd cap hooked at exe+0x%llX (cap x%d)",
        static_cast<unsigned long long>(site - reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr))),
        g_wildlifeMax);
    return true;
}

// ---------------------------------------------------------------------------
// Diagnostics: dump UCropSettings (16 entries per array, incl. Oats = index 15).
// The getter caches the UClass in a global; the CDO sits at UClass+0x110.
// Read-only, runs on a background thread once the settings exist.
// ---------------------------------------------------------------------------
// SMBuildingMaster::getDailyPlantGrowth: calls the crop-settings getter and reads
// BaseDailyGrowthRate[cropType] (+0x38) - unique, unlike the generic getter prologue.
const char* kCropSettingsGetterSig =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 E8 ?? ?? ?? ?? 48 8B D8 48 83 B8 10 01 00 00 00 "
    "75 08 48 8B C8 E8 ?? ?? ?? ?? 48 8B 83 10 01 00 00 0F B6 8F 8C 04 00 00";
uint8_t** g_cropSettingsClassPtr = nullptr;

// UCropSettings layout (from the generated property table): 16 entries per array
const size_t kGrowthArr = 0x38, kYieldArr = 0x78, kSeasonArr = 0xB8, kThreshold = 0x138;
float g_cropGrowthMul[16], g_cropYieldMulSet[16];
bool g_harvestAllYear = false;
float g_harvestThreshold = 0.0f;

// Applied from a background thread as soon as the settings object exists, i.e. on the
// untouched values - no hardcoded vanilla numbers, so a game update cannot skew the factors.
DWORD WINAPI CropSettingsThread(LPVOID)
{
    for (int i = 0; i < 300; ++i) {
        Sleep(500);
        uint8_t* cls = g_cropSettingsClassPtr ? *g_cropSettingsClassPtr : nullptr;
        if (!cls) continue;
        uint8_t* cdo = *reinterpret_cast<uint8_t**>(cls + 0x110);
        if (!cdo) continue;

        FILE* f = nullptr;
        fopen_s(&f, (std::string(kModDir) + "crop_settings.txt").c_str(), "w");
        if (f) fprintf(f, "UCropSettings CDO = %p\n\n%-6s %-22s %-16s %s\n",
                       cdo, "index", "growthRate", "yield/100plants", "harvestSeason");
        for (int c = 0; c < 16; ++c) {
            float* growth = reinterpret_cast<float*>(cdo + kGrowthArr + c * 4);
            int* yield = reinterpret_cast<int*>(cdo + kYieldArr + c * 4);
            int* season = reinterpret_cast<int*>(cdo + kSeasonArr + c * 8);
            float g0 = *growth;
            int y0 = *yield, s0 = season[0], s1 = season[1];

            if (g_cropGrowthMul[c] > 0.0f && g_cropGrowthMul[c] != 1.0f) *growth = g0 * g_cropGrowthMul[c];
            if (g_cropYieldMulSet[c] > 0.0f && g_cropYieldMulSet[c] != 1.0f) {
                int v = static_cast<int>(y0 * g_cropYieldMulSet[c] + 0.5f);
                *yield = v < 1 ? 1 : v;
            }
            if (g_harvestAllYear) { season[0] = 1; season[1] = 365; }

            if (f) fprintf(f, "%-6d %-22s %-16s %s\n", c,
                           (std::to_string(g0) + " -> " + std::to_string(*growth)).c_str(),
                           (std::to_string(y0) + " -> " + std::to_string(*yield)).c_str(),
                           ("(" + std::to_string(s0) + "," + std::to_string(s1) + ") -> (" +
                            std::to_string(season[0]) + "," + std::to_string(season[1]) + ")").c_str());
        }
        float* th = reinterpret_cast<float*>(cdo + kThreshold);
        float t0 = *th;
        if (g_harvestThreshold > 0.0f) *th = g_harvestThreshold;
        if (f) {
            fprintf(f, "\nHarvestGrowthThreshold %.3f -> %.3f\n", t0, *th);
            fclose(f);
        }
        return 0;
    }
    return 0;
}

float ConfigFloat(const std::string& cfg, const char* key, float def)
{
    std::string k = std::string(key) + "=";
    size_t pos = 0;
    while ((pos = cfg.find(k, pos)) != std::string::npos) {
        if (pos == 0 || cfg[pos - 1] == '\n' || cfg[pos - 1] == '\r')
            return static_cast<float>(atof(cfg.c_str() + pos + k.size()));
        ++pos;
    }
    return def;
}

void SetupCropSettingsDump(uint8_t* text, size_t textSize)
{
    std::vector<int> sig;
    ParseHex(kCropSettingsGetterSig, sig);
    auto hits = FindAll(text, textSize, sig);
    if (hits.size() != 1) { Log("[Dump] crop settings access found %zu times - skipped", hits.size()); return; }
    // "call getter" at +13, then inside the getter: "mov rax, [rip+rel32]" at +7
    uint8_t* getter = hits[0] + 18 + *reinterpret_cast<int32_t*>(hits[0] + 14);
    uint8_t* mov = getter + 7;
    g_cropSettingsClassPtr = reinterpret_cast<uint8_t**>(mov + 7 + *reinterpret_cast<int32_t*>(mov + 3));
    Log("[Dump] crop settings class ptr at exe+0x%llX",
        static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(g_cropSettingsClassPtr) - reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr))));
    CloseHandle(CreateThread(nullptr, 0, CropSettingsThread, nullptr, 0, nullptr));
    Log("[CropSettings] applied on the untouched values; see crop_settings.txt");
}

int ConfigInt(const std::string& cfg, const char* key, int def)
{
    std::string k = std::string(key) + "=";
    size_t pos = 0;
    while ((pos = cfg.find(k, pos)) != std::string::npos) {
        if (pos == 0 || cfg[pos - 1] == '\n' || cfg[pos - 1] == '\r') return atoi(cfg.c_str() + pos + k.size());
        ++pos;
    }
    return def;
}

bool GroupEnabled(const std::string& cfg, const char* group)
{
    // native.cfg format: one "Key=1" / "Key=0" per line
    std::string key = std::string(group) + "=1";
    size_t pos = 0;
    while ((pos = cfg.find(key, pos)) != std::string::npos) {
        if (pos == 0 || cfg[pos - 1] == '\n' || cfg[pos - 1] == '\r') return true;
        ++pos;
    }
    return false;
}

std::string ReadFileText(const std::string& path)
{
    std::string s;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

void ApplyPatches()
{
    std::string dir = kModDir;
    fopen_s(&g_log, (dir + "native_log.txt").c_str(), "w");
    Log("MLTweaksNative %s loaded", kVersion);

    std::string cfg = ReadFileText(dir + "native.cfg");
    if (cfg.empty()) Log("native.cfg not found or empty - nothing enabled");

    uint8_t* text = nullptr;
    size_t textSize = 0;
    if (!GetTextSection(text, textSize)) { Log("ERROR: .text section not found"); return; }

    for (const Patch& p : kPatches) {
        if (!GroupEnabled(cfg, p.group)) { Log("[%s] %s: disabled", p.group, p.name); continue; }
        std::vector<int> sig, orig, repl;
        if (!ParseHex(p.signature, sig) || !ParseHex(p.original, orig) || !ParseHex(p.replacement, repl) ||
            orig.size() != repl.size()) {
            Log("[%s] %s: bad patch definition", p.group, p.name);
            continue;
        }
        auto hits = FindAll(text, textSize, sig);
        if (hits.size() != 1) {
            Log("[%s] %s: signature found %zu times - SKIPPED (game version changed?)", p.group, p.name, hits.size());
            continue;
        }
        uint8_t* at = hits[0] + p.offset;
        bool match = true;
        for (size_t i = 0; i < orig.size(); ++i) if (at[i] != static_cast<uint8_t>(orig[i])) match = false;
        if (!match) { Log("[%s] %s: original bytes differ - SKIPPED", p.group, p.name); continue; }
        if (WriteCode(at, repl))
            Log("[%s] %s: patched at exe+0x%llX", p.group, p.name,
                static_cast<unsigned long long>(at - reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr))));
        else
            Log("[%s] %s: VirtualProtect failed", p.group, p.name);
    }
    // CarryDest=72,99,... : destination building types that receive bulk hauls
    {
        std::string k = "CarryDest=";
        size_t pos = cfg.find(k);
        std::string list;
        if (pos != std::string::npos) {
            size_t end = cfg.find_first_of("\r\n", pos);
            list = cfg.substr(pos + k.size(), end == std::string::npos ? std::string::npos : end - pos - k.size());
        }
        const char* p = list.c_str();
        while (*p) {
            char* e = nullptr;
            long v = strtol(p, &e, 10);
            if (e == p) { ++p; continue; }
            if (v >= 0 && v < 512) g_bulkDest[v] = true;
            p = e;
        }
        Log("[Carry] bulk destinations: %s", list.empty() ? "(none)" : list.c_str());
    }
    g_normalCarry = ConfigInt(cfg, "HandCarry", 0);
    g_cartCarry = ConfigInt(cfg, "CartCarry", 0);
    if (g_normalCarry > 1 || g_cartCarry > 1) {
        InstallTransportHook(text, textSize);
        // Raising the generic fetch capacity affects every demand-driven delivery
        // (construction materials, workshop inputs, ...) and is off unless asked for.
        if (GroupEnabled(cfg, "BulkFetch")) InstallFetchCapacityHook(text, textSize);
        else Log("[Carry] fetch capacity: disabled");
    } else
        Log("[Carry] disabled");

    // CropYieldN=M : harvest multiplier for crop type N (ECropType)
    bool anyYield = false;
    for (int i = 0; i < 16; ++i) {
        int v = ConfigInt(cfg, ("CropYield" + std::to_string(i)).c_str(), 1);
        g_cropYieldMul[i] = (v >= 1 && v <= 1000) ? v : 1;
        if (g_cropYieldMul[i] != 1) anyYield = true;
    }
    // Crop settings: per crop type growth / yield multipliers, harvest season and threshold
    {
        bool any = false;
        for (int c = 0; c < 16; ++c) {
            g_cropGrowthMul[c] = ConfigFloat(cfg, ("CropGrowthMul" + std::to_string(c)).c_str(), 1.0f);
            g_cropYieldMulSet[c] = ConfigFloat(cfg, ("CropYieldMul" + std::to_string(c)).c_str(), 1.0f);
            if (g_cropGrowthMul[c] != 1.0f || g_cropYieldMulSet[c] != 1.0f) any = true;
        }
        g_harvestAllYear = GroupEnabled(cfg, "HarvestAllYear");
        g_harvestThreshold = ConfigFloat(cfg, "HarvestThreshold", 0.0f);
        if (any || g_harvestAllYear || g_harvestThreshold > 0.0f) SetupCropSettingsDump(text, textSize);
        else Log("[CropSettings] disabled");
    }

    if (anyYield) InstallHarvestHook(text, textSize);
    else Log("[CropYield] disabled");

    // FreeUpgrade=13,19,... : upgrade IDs (EUpgradeType) whose regional wealth cost becomes 0
    {
        std::string k = "FreeUpgrade=";
        size_t pos = cfg.find(k);
        std::string list;
        if (pos != std::string::npos) {
            size_t end = cfg.find_first_of("\r\n", pos);
            list = cfg.substr(pos + k.size(), end == std::string::npos ? std::string::npos : end - pos - k.size());
        }
        bool any = false;
        const char* p = list.c_str();
        while (*p) {
            char* e = nullptr;
            long v = strtol(p, &e, 10);
            if (e == p) { ++p; continue; }
            if (v > 0 && v < 64) { g_freeUpgrade[v] = true; any = true; }
            p = e;
        }
        if (any) InstallUpgradeCostHook(text, textSize);
        else Log("[FreeUpgrade] disabled");
    }

    // WildlifeMax=N : multiplier for the per-node animal population cap
    g_wildlifeMax = ConfigInt(cfg, "WildlifeMax", 1);
    if (g_wildlifeMax > 1 && g_wildlifeMax <= 1000) InstallWildlifeCapHook(text, textSize);
    else Log("[WildlifeMax] disabled");

    Log("done");
    if (g_log) { fclose(g_log); g_log = nullptr; }
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}

// Entry point, called from Lua as package.loadlib(dll, "MLTweaks_Init")(). The signature
// matches lua_CFunction (int f(lua_State*)); the state is unused and nothing is returned.
// Doing the work here instead of in DllMain keeps it out of the loader lock.
extern "C" __declspec(dllexport) int MLTweaks_Init(void*)
{
    static bool done = false;
    if (!done) {
        done = true;
        ApplyPatches();
    }
    return 0;
}
