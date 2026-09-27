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
const char* kVersion = "1.3.0-dev";

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

// native_log.txt is closed once the patches are in, so anything a background thread wants to
// report later reopens it for a single line.
void LogLate(const char* fmt, ...)
{
    FILE* f = nullptr;
    if (fopen_s(&f, (std::string(kModDir) + "native_log.txt").c_str(), "a") != 0 || !f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

// Counters that show the owner check is doing something: "others" must stay > 0 in a game
// with an AI lord, otherwise everything would be treated as the player's.
volatile LONG g_ownProd[2] = {}, g_ownCrop[2] = {}, g_ownImm[2] = {};

// How long the hooks themselves take, to tell a slow hook from a slow game.
volatile LONG64 g_hookTicks[3] = {}, g_hookCalls[3] = {};   // 0 = plant yield, 1 = growth, 2 = craft count

struct HookTimer {
    int slot;
    LARGE_INTEGER t0;
    explicit HookTimer(int s) : slot(s) { QueryPerformanceCounter(&t0); }
    ~HookTimer() {
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        InterlockedAdd64(&g_hookTicks[slot], t1.QuadPart - t0.QuadPart);
        InterlockedIncrement64(&g_hookCalls[slot]);
    }
};

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
// TreeGrowth: saplings planted by foresters are kept in Region's growing-foliage list
// (Region+0xFA0, 0x80 bytes each: component, transform, LastGrowthDay at +0x70). The region
// updates one entry per call and grows its scale by
//     scale += (today - LastGrowthDay) * 0.03375
// moving it to the next young-tree mesh / a grown tree once the scale passes a threshold
// (vanilla: about 250-270 days from planting to a grown tree). The game setup value
// GameSetupParameters.treeGrowthRate is never read by this code.
// The patch keeps the instruction "mulss xmm6, [rip+disp32]" and only points its disp32 at
// our own read-only copy of the constant, multiplied. Nothing else uses that constant.
// ---------------------------------------------------------------------------
const char* kTreeGrowthSig =
    "66 0F 6E F0 0F 5B F6 F3 0F 59 35 ?? ?? ?? ?? F3 0F 58 F0 0F 10 46 30 0F 11 44 24 60";
const int kTreeGrowthMulss = 7;   // offset of "F3 0F 59 35 disp32" in the signature
const float kTreeGrowthVanilla = 0.03375f;
float g_treeGrowthMul = 1.0f;

bool InstallTreeGrowthPatch(uint8_t* text, size_t textSize)
{
    std::vector<int> sig;
    ParseHex(kTreeGrowthSig, sig);
    auto hits = FindAll(text, textSize, sig);
    if (hits.size() != 1) {
        Log("[TreeGrowth] sapling growth: signature found %zu times - SKIPPED", hits.size());
        return false;
    }
    uint8_t* site = hits[0] + kTreeGrowthMulss;
    uint8_t* next = site + 8;
    const float* orig = reinterpret_cast<const float*>(next + *reinterpret_cast<int32_t*>(site + 4));
    if (*orig < kTreeGrowthVanilla - 1e-6f || *orig > kTreeGrowthVanilla + 1e-6f) {
        Log("[TreeGrowth] unexpected growth constant %f - SKIPPED", *orig);
        return false;
    }
    uint8_t* mem = AllocNear(site, 16);
    if (!mem) { Log("[TreeGrowth] could not allocate near memory"); return false; }
    const float value = *orig * g_treeGrowthMul;
    memcpy(mem, &value, sizeof(value));
    DWORD old;
    if (!VirtualProtect(mem, 16, PAGE_READONLY, &old)) { Log("[TreeGrowth] VirtualProtect failed"); return false; }

    int64_t rel = reinterpret_cast<int64_t>(mem) - reinterpret_cast<int64_t>(next);
    if (rel > INT32_MAX || rel < INT32_MIN) { Log("[TreeGrowth] constant out of range"); return false; }
    std::vector<int> patch;
    for (int i = 0; i < 4; ++i) patch.push_back(static_cast<int>((static_cast<uint32_t>(rel) >> (8 * i)) & 0xFF));
    if (!WriteCode(site + 4, patch)) { Log("[TreeGrowth] VirtualProtect failed"); return false; }
    Log("[TreeGrowth] sapling growth per day %.5f -> %.5f (x%.2f) at exe+0x%llX",
        *orig, value, g_treeGrowthMul,
        static_cast<unsigned long long>(site - reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr))));
    return true;
}

// ---------------------------------------------------------------------------
// Ownership: the engine keeps the player's own pawn in RTSMultiEngineCPP.playerRef,
// and a Region / SMBuildingMaster points back at the engine. That is enough to tell
// the player's things from an AI lord's without any help from the Lua side.
// ---------------------------------------------------------------------------
const size_t kEnginePlayerRef = 0x5A0;    // RTSMultiEngineCPP.playerRef
const size_t kRegionEngine = 0x318, kRegionOwnerPawn = 0x350;        // Region.masterPtr / .ownerPawn
const size_t kBuildingEngine = 0x2E8, kBuildingOwnerPawn = 0x2E0;    // SMBuildingMaster
const size_t kBuildingCropType = 0x48C;                              // ECropType of a field

bool IsPlayerOwned(uint8_t* obj, size_t engineOffset, size_t ownerOffset)
{
    __try {
        if (!obj) return false;
        uint8_t* engine = *reinterpret_cast<uint8_t**>(obj + engineOffset);
        uint8_t* owner = *reinterpret_cast<uint8_t**>(obj + ownerOffset);
        if (!engine || !owner) return false;
        return owner == *reinterpret_cast<uint8_t**>(engine + kEnginePlayerRef);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsPlayerBuilding(void* b)
{
    return IsPlayerOwned(static_cast<uint8_t*>(b), kBuildingEngine, kBuildingOwnerPawn);
}

// ECropType of a field, or -1 when it cannot be read
int CropTypeOf(void* b)
{
    __try {
        return b ? *(static_cast<uint8_t*>(b) + kBuildingCropType) : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// ---------------------------------------------------------------------------
// Immigration: Region::<monthly population growth>() sums the very map the game's own
// GetMonthlyPopGrowthModifiersWithValues hands the UI (base, approval, free housing, ...).
// The total is how many families move into that region this month: growPopulation spreads
// that many arrival days evenly over the month, and on such a day a family moves into a
// plot that still has room - the room getFamilyCapacity reports, so extra BurgageFamilies
// slots are filled as well. A negative total means families leave instead, so the
// multiplier is applied to arrivals only.
// Arrivals cannot be closer together than one a day, so a month cannot take more than
// about 30 however high the multiplier is.
// ---------------------------------------------------------------------------
const char* kImmigrationSig =
    "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 48 8D 6C 24 A9 48 81 EC F0 00 00 00 "
    "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 47 48 8D 55 F7 33 DB";
const int kImmigrationStolen = 15;   // three 8-byte-register spills, 5 bytes each

using MonthlyGrowthFn = int(__fastcall*)(void*);
MonthlyGrowthFn g_origMonthlyGrowth = nullptr;
float g_immigrationMul = 1.0f;

int __fastcall MonthlyGrowthHook(void* region)
{
    int v = g_origMonthlyGrowth(region);
    bool mine = IsPlayerOwned(static_cast<uint8_t*>(region), kRegionEngine, kRegionOwnerPawn);
    InterlockedIncrement(&g_ownImm[mine ? 0 : 1]);
    if (v <= 0 || !mine) return v;
    int scaled = static_cast<int>(v * g_immigrationMul + 0.5f);
    if (scaled < v) scaled = v;          // never fewer arrivals than vanilla
    if (scaled > 1000) scaled = 1000;
    return scaled;
}

bool InstallImmigrationHook(uint8_t* text, size_t textSize)
{
    std::vector<int> sig;
    ParseHex(kImmigrationSig, sig);
    auto hits = FindAll(text, textSize, sig);
    if (hits.size() != 1) {
        Log("[Immigration] monthly growth: signature found %zu times - SKIPPED", hits.size());
        return false;
    }
    uint8_t* target = hits[0];

    auto tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!tramp) { Log("[Immigration] VirtualAlloc failed"); return false; }
    memcpy(tramp, target, kImmigrationStolen);
    uint8_t* j = tramp + kImmigrationStolen;
    j[0] = 0xFF; j[1] = 0x25; *reinterpret_cast<uint32_t*>(j + 2) = 0;
    *reinterpret_cast<uint64_t*>(j + 6) = reinterpret_cast<uint64_t>(target + kImmigrationStolen);
    if (!SealCode(tramp, 64)) { Log("[Immigration] VirtualProtect failed"); return false; }
    g_origMonthlyGrowth = reinterpret_cast<MonthlyGrowthFn>(tramp);

    std::vector<int> patch = { 0xFF, 0x25, 0, 0, 0, 0 };
    uint64_t hookAddr = reinterpret_cast<uint64_t>(&MonthlyGrowthHook);
    for (int i = 0; i < 8; ++i) patch.push_back(static_cast<int>((hookAddr >> (8 * i)) & 0xFF));
    while (static_cast<int>(patch.size()) < kImmigrationStolen) patch.push_back(0x90);
    if (!WriteCode(target, patch)) { Log("[Immigration] VirtualProtect failed"); return false; }
    Log("[Immigration] monthly population growth hooked at exe+0x%llX (x%.2f, player region only)",
        static_cast<unsigned long long>(target - reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr))),
        g_immigrationMul);
    return true;
}

// ---------------------------------------------------------------------------
// Production output, for the player's buildings only. When a craft finishes, the game asks
// the building how many units this one produced: normally 1, 2 on the bonus craft a yield
// perk grants, 0 on a botched one (SMBuildingMaster::<units produced>, the function that
// reads GetSummedYieldModifierValue). Scaling that result multiplies the output while the
// inputs, which are consumed per craft, stay the same - the same effect the Lua side used to
// get by editing the item table, but without touching an AI lord's workshops.
// Whether a building makes food or crafted goods comes from the game's building table, which
// only exists once a game is loaded, so Lua writes the list to native_runtime.cfg and a small
// thread here picks it up. Until it arrives, output is left at vanilla.
// ---------------------------------------------------------------------------
const char* kCraftCountSig =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 0F 29 74 24 30 48 8B F1 0F 29 7C 24 20 E8";
const int kCraftCountStolen = 15;

using CraftCountFn = int(__fastcall*)(void*);
CraftCountFn g_origCraftCount = nullptr;
float g_prodMulFood = 1.0f, g_prodMulOther = 1.0f;
bool g_prodFoodBuilding[512] = {};
volatile LONG g_prodListReady = 0;

int __fastcall CraftCountHook(void* b)
{
    int v = g_origCraftCount(b);
    HookTimer timer(2);
    if (v <= 0 || !g_prodListReady) return v;
    bool mine = IsPlayerBuilding(b);
    InterlockedIncrement(&g_ownProd[mine ? 0 : 1]);
    if (!mine) return v;
    int t = BuildingType(static_cast<uint8_t*>(b));
    float m = (t >= 0 && t < 512 && g_prodFoodBuilding[t]) ? g_prodMulFood : g_prodMulOther;
    if (m <= 1.0f) return v;
    int scaled = static_cast<int>(v * m + 0.5f);
    return scaled < v ? v : scaled;
}

// Waits for the building list Lua writes once the game's data tables are up.
DWORD WINAPI ProdListThread(LPVOID)
{
    const std::string path = std::string(kModDir) + "native_runtime.cfg";
    for (int i = 0; i < 1200; ++i) {          // up to ten minutes
        Sleep(500);
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) continue;
        std::string text;
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        fclose(f);
        const char* key = "FoodBuildings=";
        size_t pos = text.find(key);
        if (pos == std::string::npos) continue;
        size_t end = text.find_first_of("\r\n", pos);
        std::string list = text.substr(pos + strlen(key), end == std::string::npos ? std::string::npos : end - pos - strlen(key));
        int count = 0;
        const char* p = list.c_str();
        while (*p) {
            char* e = nullptr;
            long v = strtol(p, &e, 10);
            if (e == p) { ++p; continue; }
            if (v >= 0 && v < 512) { g_prodFoodBuilding[v] = true; ++count; }
            p = e;
        }
        InterlockedExchange(&g_prodListReady, 1);
        LogLate("[Production] food buildings from native_runtime.cfg: %d types (food x%.2f, other x%.2f)",
                count, g_prodMulFood, g_prodMulOther);
        // Twice, so the owner check can be seen working: "others" counts the things this
        // build deliberately leaves alone (an AI lord's buildings, fields and regions).
        for (int k = 0; k < 2; ++k) {
            Sleep(k == 0 ? 60000 : 240000);
            LogLate("[Owner check] scaled/left alone - production %ld/%ld, crop yield %ld/%ld, immigration %ld/%ld",
                    g_ownProd[0], g_ownProd[1], g_ownCrop[0], g_ownCrop[1], g_ownImm[0], g_ownImm[1]);
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            const char* names[3] = { "plant yield", "growth", "craft count" };
            for (int h = 0; h < 3; ++h)
                LogLate("[Hook cost] %-12s %lld calls, %.1f ms total", names[h],
                        g_hookCalls[h], 1000.0 * (double)g_hookTicks[h] / (double)freq.QuadPart);
        }
        return 0;
    }
    Log("[Production] native_runtime.cfg never appeared - output left at vanilla");
    return 0;
}

// ---------------------------------------------------------------------------
// Backyard animals: chickens, goats and pigs on a burgage plot hand in a by-product every so
// many days - eggs every 15, milk every 49, pork every 73 in vanilla (the amount per delivery
// is 1, or 2 for pork). The wait is computed as
//     days = round(base days / GetSummedYieldModifierValue(plot))
// which is also how the game's own backyard-animal perk speeds it up. The three calls to that
// function inside the by-product step are redirected here, so the wait can be shortened for
// the player's plots only; every other use of the function is untouched.
//
// The same step hands the goods over with eight calls of the shape
//     mov rcx,[plot+360h] / mov edx,<good> / <amount in r8d> / call <add goods>
// for eggs, chicken, pork, milk, chevon, hides, honey and wax. Those calls are redirected too,
// and the amount is scaled there. The plot comes straight out of rcx (rcx - 360h), so the
// owner is checked per call without remembering anything between calls.
// ---------------------------------------------------------------------------
const char* kByproductAnchorSig = "C7 03 DD 00 00 00 0F 57 C0 C7 43 04 01 00 00 00";  // milk
const int kByproductWindow = 0x800;   // the three calls sit a little before the anchor

using YieldModifierFn = float(__fastcall*)(void*);
using AddGoodsFn = void*(__fastcall*)(void*, int, int, void*);
YieldModifierFn g_origYieldModifier = nullptr;
AddGoodsFn g_origAddGoods = nullptr;
float g_backyardMul = 1.0f;        // how much more often
float g_backyardAmount = 1.0f;     // how much per delivery
const size_t kBuildingStore = 0x360;   // what the by-product step passes as rcx

float __fastcall ByproductModifierHook(void* building)
{
    float v = g_origYieldModifier(building);
    if (g_backyardMul > 1.0f && v > 0.0f && IsPlayerBuilding(building)) v *= g_backyardMul;
    return v;
}

void* __fastcall ByproductAddGoodsHook(void* store, int goodType, int amount, void* rest)
{
    if (g_backyardAmount > 1.0f && amount > 0 && store) {
        uint8_t* plot = static_cast<uint8_t*>(store) - kBuildingStore;
        if (IsPlayerBuilding(plot)) {
            int scaled = static_cast<int>(amount * g_backyardAmount + 0.5f);
            if (scaled > amount) amount = (scaled > 10000) ? 10000 : scaled;
        }
    }
    return g_origAddGoods(store, goodType, amount, rest);
}

// Point a set of "call rel32" sites at a hook, through one small block allocated near them.
bool RedirectCalls(const std::vector<uint8_t*>& sites, void* hook, const char* what)
{
    if (sites.empty()) return false;
    uint8_t* cave = AllocNear(sites[0], 32);
    if (!cave) { Log("%s could not allocate near memory", what); return false; }
    cave[0] = 0xFF; cave[1] = 0x25; *reinterpret_cast<uint32_t*>(cave + 2) = 0;
    *reinterpret_cast<uint64_t*>(cave + 6) = reinterpret_cast<uint64_t>(hook);
    if (!SealCode(cave, 32)) { Log("%s VirtualProtect on the cave failed", what); return false; }

    for (size_t i = 0; i < sites.size(); ++i) {
        int64_t rel = reinterpret_cast<int64_t>(cave) - reinterpret_cast<int64_t>(sites[i] + 5);
        if (rel > INT32_MAX || rel < INT32_MIN) { Log("%s cave out of range", what); return false; }
        std::vector<int> patch;
        for (int k = 0; k < 4; ++k) patch.push_back(static_cast<int>((static_cast<uint32_t>(rel) >> (8 * k)) & 0xFF));
        if (!WriteCode(sites[i] + 1, patch)) { Log("%s VirtualProtect failed", what); return false; }
    }
    return true;
}

bool InstallByproductHook(uint8_t* text, size_t textSize)
{
    std::vector<int> sig;
    ParseHex(kByproductAnchorSig, sig);
    auto hits = FindAll(text, textSize, sig);
    if (hits.size() != 1) {
        Log("[Backyard] by-product step: anchor found %zu times - SKIPPED", hits.size());
        return false;
    }
    uint8_t* anchor = hits[0];
    uint8_t* from = (anchor - text > kByproductWindow) ? anchor - kByproductWindow : text;

    // the function the by-product step keeps calling in that window is the yield modifier
    std::vector<uint8_t*> sites;
    uint8_t* best = nullptr;
    int candidates = 0;
    for (uint8_t* p = from; p < anchor; ++p) {
        if (*p != 0xE8) continue;
        uint8_t* target = p + 5 + *reinterpret_cast<int32_t*>(p + 1);
        bool seen = false;
        for (uint8_t* q = from; q < p; ++q)
            if (*q == 0xE8 && q + 5 + *reinterpret_cast<int32_t*>(q + 1) == target) { seen = true; break; }
        if (seen) continue;                      // count each target once
        int n = 0;
        for (uint8_t* q = from; q < anchor; ++q)
            if (*q == 0xE8 && q + 5 + *reinterpret_cast<int32_t*>(q + 1) == target) ++n;
        if (n == 3) { best = target; ++candidates; }
    }
    if (!best || candidates != 1) {
        Log("[Backyard] by-product step: %d candidates for the yield modifier call - SKIPPED", candidates);
        return false;
    }
    for (uint8_t* p = from; p < anchor; ++p)
        if (*p == 0xE8 && p + 5 + *reinterpret_cast<int32_t*>(p + 1) == best) sites.push_back(p);
    if (sites.size() != 3) { Log("[Backyard] by-product step: %zu call sites - SKIPPED", sites.size()); return false; }

    g_origYieldModifier = reinterpret_cast<YieldModifierFn>(best);
    if (!RedirectCalls(sites, &ByproductModifierHook, "[Backyard]")) return false;
    Log("[Backyard] by-product wait hooked at exe+0x%llX and 2 more (x%.2f, player plots only)",
        static_cast<unsigned long long>(sites[0] - reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr))),
        g_backyardMul);
    return true;
}

// The handover calls: "mov rcx,[rdi+360h]" a few bytes before "call <add goods>".
const char* kByproductStoreSig = "48 8B 8F 60 03 00 00";
const int kByproductAfter = 0x400;    // the last handovers sit past the anchor

bool InstallByproductAmountHook(uint8_t* text, size_t textSize)
{
    std::vector<int> sig;
    ParseHex(kByproductAnchorSig, sig);
    auto hits = FindAll(text, textSize, sig);
    if (hits.size() != 1) {
        Log("[Backyard] amount: anchor found %zu times - SKIPPED", hits.size());
        return false;
    }
    uint8_t* anchor = hits[0];
    uint8_t* from = (anchor - text > kByproductWindow) ? anchor - kByproductWindow : text;
    uint8_t* to = anchor + kByproductAfter;
    if (to > text + textSize - 8) to = text + textSize - 8;

    std::vector<int> storeSig;
    ParseHex(kByproductStoreSig, storeSig);
    std::vector<uint8_t*> sites;
    uint8_t* target = nullptr;
    for (uint8_t* p = from; p < to; ++p) {
        bool match = true;
        for (size_t i = 0; i < storeSig.size(); ++i)
            if (p[i] != static_cast<uint8_t>(storeSig[i])) { match = false; break; }
        if (!match) continue;
        for (uint8_t* q = p + 7; q < p + 40; ++q) {
            if (*q != 0xE8) continue;
            uint8_t* t = q + 5 + *reinterpret_cast<int32_t*>(q + 1);
            if (!target) target = t;
            if (t == target) sites.push_back(q);
            break;
        }
    }
    if (!target || sites.size() < 4) {
        Log("[Backyard] amount: %zu handover calls found - SKIPPED", sites.size());
        return false;
    }
    g_origAddGoods = reinterpret_cast<AddGoodsFn>(target);
    if (!RedirectCalls(sites, &ByproductAddGoodsHook, "[Backyard]")) return false;
    Log("[Backyard] by-product amount hooked at %zu handovers (x%.2f, player plots only)",
        sites.size(), g_backyardAmount);
    return true;
}

// ---------------------------------------------------------------------------
// BurgageFamilies: how many families a house holds. The building table is NOT used for this
// (buildingStats.occupantTypes.Y is ignored); two small functions compute it:
//   SMBuildingMaster::getFamilyCapacity()            8 callers: living space, homeless
//                                                    assignment, save load, ...
//   SMBuildingMaster::getMaxOccupantsOfRole(role)    Blueprint, the residents panel
// Both do: worker camp (bType 111) -> 5; not residential (+0x3BC != 1) -> 0; otherwise
//   extensions (+0x430, byte) + base, base = 3 for house level 4, 2 for level 3, else 1
//   (house level at +0xCA8, set to 1 on construction and 2/3/4 by the plot upgrades).
// getMaxOccupantsOfRole returns that for roles 0..2 and 0 for any other role.
// Both are replaced outright - a jump to one implementation here, the original body never
// runs - so the panel and the game logic can never disagree. A level left at 0 in the
// config keeps its vanilla base.
// ---------------------------------------------------------------------------
const char* kFamilyCapSig =        // "CC CC" = padding before the function; the body alone
    "CC CC 83 B9 A8 03 00 00 6F 75 06 B8 05 00 00 00 C3 80 B9 BC 03 00 00 01 75 ?? "
    "8B 91 A8 0C 00 00 0F B6 81 30 04 00 00 83 FA 04";   // also occurs inside the role variant
const int kFamilyCapOffset = 2;
const char* kOccupantsOfRoleSig =
    "44 0F B6 C2 84 D2 74 ?? 41 83 E8 01 74 ?? 41 83 E8 01 75 ?? 83 B9 A8 03 00 00 6F";
const size_t kBuildingTypeOff = 0x3A8, kIsResidential = 0x3BC, kExtensions = 0x430, kHouseLevel = 0xCA8;
int g_familiesPerLevel[5] = {};    // index = house level 1..4, 0 = vanilla

int VanillaFamilyBase(int level) { return level == 4 ? 3 : (level == 3 ? 2 : 1); }

int __fastcall FamilyCapacityHook(uint8_t* b)
{
    if (*reinterpret_cast<int*>(b + kBuildingTypeOff) == 111) return 5;   // worker camp
    if (b[kIsResidential] != 1) return 0;
    int level = *reinterpret_cast<int*>(b + kHouseLevel);
    int base = (level >= 1 && level <= 4 && g_familiesPerLevel[level] > 0)
        ? g_familiesPerLevel[level] : VanillaFamilyBase(level);
    return b[kExtensions] + base;
}

int __fastcall OccupantsOfRoleHook(uint8_t* b, uint8_t role)
{
    return role <= 2 ? FamilyCapacityHook(b) : 0;
}

// Overwrites the start of a function with "jmp [rip] -> hook" (14 bytes).
bool ReplaceFunction(uint8_t* text, size_t textSize, const char* sigText, int offset, void* hook, const char* what)
{
    std::vector<int> sig;
    ParseHex(sigText, sig);
    auto hits = FindAll(text, textSize, sig);
    if (hits.size() != 1) {
        Log("[BurgageFamilies] %s: signature found %zu times - SKIPPED", what, hits.size());
        return false;
    }
    uint8_t* site = hits[0] + offset;
    std::vector<int> patch = { 0xFF, 0x25, 0, 0, 0, 0 };
    uint64_t addr = reinterpret_cast<uint64_t>(hook);
    for (int i = 0; i < 8; ++i) patch.push_back(static_cast<int>((addr >> (8 * i)) & 0xFF));
    if (!WriteCode(site, patch)) { Log("[BurgageFamilies] %s: VirtualProtect failed", what); return false; }
    Log("[BurgageFamilies] %s replaced at exe+0x%llX", what,
        static_cast<unsigned long long>(site - reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr))));
    return true;
}

bool InstallFamilyCapacityHooks(uint8_t* text, size_t textSize)
{
    // resolve both sites first, so a partial install (one hooked, one not) cannot happen
    std::vector<int> a, b;
    ParseHex(kFamilyCapSig, a);
    ParseHex(kOccupantsOfRoleSig, b);
    if (FindAll(text, textSize, a).size() != 1 || FindAll(text, textSize, b).size() != 1) {
        Log("[BurgageFamilies] signatures not unique - SKIPPED (game version changed?)");
        return false;
    }
    bool ok = ReplaceFunction(text, textSize, kFamilyCapSig, kFamilyCapOffset,
                              reinterpret_cast<void*>(&FamilyCapacityHook), "family capacity");
    ok = ReplaceFunction(text, textSize, kOccupantsOfRoleSig, 0,
                         reinterpret_cast<void*>(&OccupantsOfRoleHook), "max occupants of role") && ok;
    std::string v;
    for (int lv = 1; lv <= 4; ++lv)
        v += "Lv" + std::to_string(lv) + "=" + (g_familiesPerLevel[lv] > 0 ? std::to_string(g_familiesPerLevel[lv])
                                                                              : std::to_string(VanillaFamilyBase(lv)) + "(vanilla)") + (lv < 4 ? " " : "");
    Log("[BurgageFamilies] base families per house level: %s, +1 per extension", v.c_str());
    return ok;
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

            // The growth rate and the yield per 100 plants are NOT changed here any more:
            // they are scaled per field in the hooks below, so only the player's fields change.
            if (g_harvestAllYear) { season[0] = 1; season[1] = 365; }

            if (f) fprintf(f, "%-6d %-22s %-16s %s\n", c,
                           (std::to_string(g0) + " x" + std::to_string(g_cropGrowthMul[c])).c_str(),
                           (std::to_string(y0) + " x" + std::to_string(g_cropYieldMulSet[c])).c_str(),
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

// ---------------------------------------------------------------------------
// Crops, for the player's fields only. Two per-building functions carry the numbers:
//   SMBuildingMaster::getDailyPlantGrowth()                       -> growth per day
//   SMBuildingMaster::<per-plant yield>(plant, ...)               -> yield of one plant
// Both are hooked and their result is scaled by the crop group's multiplier when the
// field belongs to the player, so an AI lord's fields are left at vanilla. Harvest
// season and the harvest threshold stay global: they are not multipliers.
// The per-plant yield returns an int in eax; the UI's predicted yield uses the same
// function, so the tooltip keeps matching what is harvested.
// ---------------------------------------------------------------------------
const char* kPlantYieldSig = "48 8B C4 56 41 57 48 83 EC 58 48 8B B4 24 98 00 00 00";
const int kPlantYieldStolen = 18;

using PlantYieldFn = int(__fastcall*)(void*, int, unsigned char, unsigned char, int, void*);
PlantYieldFn g_origPlantYield = nullptr;
bool g_harvestHookActive = false;   // the older global patch for garden / orchard plants

float CropMul(const float* table, void* building)
{
    if (!IsPlayerBuilding(building)) return 1.0f;
    int c = CropTypeOf(building);
    if (c < 0 || c > 15) return 1.0f;
    float m = table[c];
    return (m > 0.0f) ? m : 1.0f;
}

// Called from the growth cave with the field in rcx; returns the growth multiplier.
float __fastcall GrowthMulFor(void* building)
{
    HookTimer timer(1);
    return CropMul(g_cropGrowthMul, building);
}

int __fastcall PlantYieldHook(void* building, int plant, unsigned char a, unsigned char b, int c, void* out)
{
    int v = g_origPlantYield(building, plant, a, b, c, out);
    HookTimer timer(0);
    if (v <= 0) return v;
    // Garden and orchard plants are handled by the harvest-handler patch below; scaling them
    // here as well would apply the multiplier twice.
    int type = CropTypeOf(building);
    if (type < 0 || type > 15) return v;
    if (g_harvestHookActive && g_cropYieldMul[type] != 1) return v;
    float m = g_cropYieldMulSet[type];
    if (m <= 0.0f || m == 1.0f) return v;
    bool mine = IsPlayerBuilding(building);
    InterlockedIncrement(&g_ownCrop[mine ? 0 : 1]);
    if (!mine) return v;
    int scaled = static_cast<int>(v * m + 0.5f);
    return scaled < 1 ? 1 : scaled;
}

// jmp [rip] detour over `stolen` bytes, with a trampoline that runs them and jumps back
uint8_t* DetourFunction(uint8_t* target, void* hook, int stolen, const char* what)
{
    auto tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!tramp) { Log("%s: VirtualAlloc failed", what); return nullptr; }
    memcpy(tramp, target, stolen);
    uint8_t* j = tramp + stolen;
    j[0] = 0xFF; j[1] = 0x25; *reinterpret_cast<uint32_t*>(j + 2) = 0;
    *reinterpret_cast<uint64_t*>(j + 6) = reinterpret_cast<uint64_t>(target + stolen);
    if (!SealCode(tramp, 64)) { Log("%s: VirtualProtect failed", what); return nullptr; }

    std::vector<int> patch = { 0xFF, 0x25, 0, 0, 0, 0 };
    uint64_t addr = reinterpret_cast<uint64_t>(hook);
    for (int i = 0; i < 8; ++i) patch.push_back(static_cast<int>((addr >> (8 * i)) & 0xFF));
    while (static_cast<int>(patch.size()) < stolen) patch.push_back(0x90);
    if (!WriteCode(target, patch)) { Log("%s: VirtualProtect failed", what); return nullptr; }
    return tramp;
}

// Same, but the detour is a 5-byte relative jump to a small block allocated near the game
// module, which then jumps to the hook. Used where the stolen bytes have to stay short.
uint8_t* DetourFunctionNear(uint8_t* target, void* hook, int stolen, const char* what)
{
    if (stolen < 5) return nullptr;
    auto tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!tramp) { Log("%s: VirtualAlloc failed", what); return nullptr; }
    memcpy(tramp, target, stolen);
    uint8_t* j = tramp + stolen;
    j[0] = 0xFF; j[1] = 0x25; *reinterpret_cast<uint32_t*>(j + 2) = 0;
    *reinterpret_cast<uint64_t*>(j + 6) = reinterpret_cast<uint64_t>(target + stolen);
    if (!SealCode(tramp, 64)) { Log("%s: VirtualProtect failed", what); return nullptr; }

    uint8_t* cave = AllocNear(target, 32);
    if (!cave) { Log("%s: could not allocate near memory", what); return nullptr; }
    cave[0] = 0xFF; cave[1] = 0x25; *reinterpret_cast<uint32_t*>(cave + 2) = 0;
    *reinterpret_cast<uint64_t*>(cave + 6) = reinterpret_cast<uint64_t>(hook);
    if (!SealCode(cave, 32)) { Log("%s: VirtualProtect failed", what); return nullptr; }

    int64_t rel = reinterpret_cast<int64_t>(cave) - reinterpret_cast<int64_t>(target + 5);
    if (rel > INT32_MAX || rel < INT32_MIN) { Log("%s: cave out of range", what); return nullptr; }
    std::vector<int> patch = { 0xE9 };
    for (int i = 0; i < 4; ++i) patch.push_back(static_cast<int>((static_cast<uint32_t>(rel) >> (8 * i)) & 0xFF));
    while (static_cast<int>(patch.size()) < stolen) patch.push_back(0x90);
    if (!WriteCode(target, patch)) { Log("%s: VirtualProtect failed", what); return nullptr; }
    return tramp;
}

bool InstallProductionHook(uint8_t* text, size_t textSize)
{
    std::vector<int> sig;
    ParseHex(kCraftCountSig, sig);
    auto hits = FindAll(text, textSize, sig);
    if (hits.size() != 1) {
        Log("[Production] units produced: signature found %zu times - SKIPPED", hits.size());
        return false;
    }
    g_origCraftCount = reinterpret_cast<CraftCountFn>(
        DetourFunction(hits[0], &CraftCountHook, kCraftCountStolen, "[Production]"));
    if (!g_origCraftCount) return false;
    Log("[Production] units produced per craft hooked at exe+0x%llX (player buildings only)",
        static_cast<unsigned long long>(hits[0] - reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr))));
    CloseHandle(CreateThread(nullptr, 0, ProdListThread, nullptr, 0, nullptr));
    return true;
}

// Growth: the daily growth step loads BaseDailyGrowthRate[cropType] straight out of the crop
// settings (SMBuildingMaster::getDailyPlantGrowth is only a Blueprint helper - the game never
// calls it). The load is redirected to a cave that runs it and then multiplies the value by
// this field's factor, so nothing changes for an AI lord.
//   movzx ecx,[rbx+48Ch] / mov rax,[rdi+110h] / movss xmm9,[rax+rcx*4+38h]   rbx = the field
const char* kGrowthRateSig =
    "0F B6 8B 8C 04 00 00 48 8B 87 10 01 00 00 F3 44 0F 10 4C 88 38";
const int kGrowthRateOffset = 14;   // the movss itself
const int kGrowthRateLen = 7;

bool InstallGrowthPatch(uint8_t* text, size_t textSize)
{
    std::vector<int> sig;
    ParseHex(kGrowthRateSig, sig);
    auto hits = FindAll(text, textSize, sig);
    if (hits.size() != 1) {
        Log("[CropGrowth] growth rate read: signature found %zu times - SKIPPED", hits.size());
        return false;
    }
    uint8_t* site = hits[0] + kGrowthRateOffset;
    uint8_t* back = site + kGrowthRateLen;
    uint8_t* cave = AllocNear(site, 128);
    if (!cave) { Log("[CropGrowth] could not allocate near memory"); return false; }

    std::vector<uint8_t> c;
    auto emit = [&](std::initializer_list<uint8_t> b) { c.insert(c.end(), b); };
    emit({ 0xF3, 0x44, 0x0F, 0x10, 0x4C, 0x88, 0x38 });   // movss xmm9,[rax+rcx*4+38h] (original)
    emit({ 0x48, 0x8B, 0xC4 });                           // mov rax, rsp
    emit({ 0x48, 0x83, 0xEC, 0x38 });                     // sub rsp, 38h
    emit({ 0x48, 0x83, 0xE4, 0xF0 });                     // and rsp, -16   (align for the call)
    emit({ 0x48, 0x89, 0x44, 0x24, 0x28 });               // mov [rsp+28h], rax   (old rsp)
    emit({ 0x48, 0x8B, 0xCB });                           // mov rcx, rbx         (the field)
    emit({ 0x48, 0xB8 });                                 // mov rax, &GrowthMulFor
    uint64_t fn = reinterpret_cast<uint64_t>(&GrowthMulFor);
    for (int i = 0; i < 8; ++i) c.push_back(static_cast<uint8_t>(fn >> (8 * i)));
    emit({ 0xFF, 0xD0 });                                 // call rax
    emit({ 0xF3, 0x44, 0x0F, 0x59, 0xC8 });               // mulss xmm9, xmm0
    emit({ 0x48, 0x8B, 0x64, 0x24, 0x28 });               // mov rsp, [rsp+28h]
    emit({ 0xFF, 0x25, 0, 0, 0, 0 });                     // jmp [rip] -> back
    uint64_t b = reinterpret_cast<uint64_t>(back);
    for (int i = 0; i < 8; ++i) c.push_back(static_cast<uint8_t>(b >> (8 * i)));
    memcpy(cave, c.data(), c.size());
    if (!SealCode(cave, c.size())) { Log("[CropGrowth] VirtualProtect on the cave failed"); return false; }

    int64_t rel = reinterpret_cast<int64_t>(cave) - reinterpret_cast<int64_t>(site + 5);
    if (rel > INT32_MAX || rel < INT32_MIN) { Log("[CropGrowth] cave out of range"); return false; }
    std::vector<int> patch = { 0xE9 };
    for (int i = 0; i < 4; ++i) patch.push_back(static_cast<int>((static_cast<uint32_t>(rel) >> (8 * i)) & 0xFF));
    while (static_cast<int>(patch.size()) < kGrowthRateLen) patch.push_back(0x90);
    if (!WriteCode(site, patch)) { Log("[CropGrowth] VirtualProtect failed"); return false; }
    Log("[CropGrowth] daily growth rate patched at exe+0x%llX (player fields only)",
        static_cast<unsigned long long>(site - reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr))));
    return true;
}

bool InstallCropHooks(uint8_t* text, size_t textSize)
{
    bool ok = InstallGrowthPatch(text, textSize);

    std::vector<int> sig;
    ParseHex(kPlantYieldSig, sig);
    auto hits = FindAll(text, textSize, sig);
    if (hits.size() != 1) {
        Log("[CropYield] per-plant yield: signature found %zu times - SKIPPED", hits.size());
        return false;
    }
    g_origPlantYield = reinterpret_cast<PlantYieldFn>(
        DetourFunction(hits[0], &PlantYieldHook, kPlantYieldStolen, "[CropYield]"));
    if (!g_origPlantYield) return false;
    Log("[CropYield] per-plant yield hooked at exe+0x%llX (player fields only)",
        static_cast<unsigned long long>(hits[0] - reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr))));
    return ok;
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
    Log("[CropSettings] harvest season / threshold applied; vanilla values in crop_settings.txt");
    // The multipliers themselves are applied per field, so they only affect the player.
    // This has to come last: the pointer above is read out of the very bytes it patches.
    bool anyCrop = false;
    for (int c = 0; c < 16; ++c)
        if (g_cropGrowthMul[c] != 1.0f || g_cropYieldMulSet[c] != 1.0f) anyCrop = true;
    if (anyCrop) InstallCropHooks(text, textSize);
    else Log("[CropGrowth] and [CropYield] per-field multipliers disabled");
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

    // The per-plant yield hook (player fields only) already covers the harvest, so the older
    // global harvest-handler patch is only used when that hook could not be installed.
    // Garden and orchard plants: the harvest handler is the path that works for them, so it
    // stays. It is global - it has no building to check the owner of - and the per-plant hook
    // skips the crop types it covers.
    if (anyYield) g_harvestHookActive = InstallHarvestHook(text, textSize);
    else Log("[CropYieldPlants] disabled");

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

    // ProdMulFood / ProdMulOther=F : output multipliers for the player's own workshops
    g_prodMulFood = ConfigFloat(cfg, "ProdMulFood", 1.0f);
    g_prodMulOther = ConfigFloat(cfg, "ProdMulOther", 1.0f);
    if ((g_prodMulFood > 1.0f || g_prodMulOther > 1.0f) &&
        g_prodMulFood <= 1000.0f && g_prodMulOther <= 1000.0f)
        InstallProductionHook(text, textSize);
    else Log("[Production] disabled");

    // BackyardSpeed / BackyardAmount=F : how often, and how much, backyard animals hand in
    g_backyardMul = ConfigFloat(cfg, "BackyardSpeed", 1.0f);
    g_backyardAmount = ConfigFloat(cfg, "BackyardAmount", 1.0f);
    if (g_backyardMul > 1.0f && g_backyardMul <= 100.0f) InstallByproductHook(text, textSize);
    else Log("[Backyard] wait unchanged");
    if (g_backyardAmount > 1.0f && g_backyardAmount <= 1000.0f) InstallByproductAmountHook(text, textSize);
    else Log("[Backyard] amount unchanged");

    // Immigration=F : multiplier for the families that move into the player's region
    g_immigrationMul = ConfigFloat(cfg, "Immigration", 1.0f);
    if (g_immigrationMul > 1.0f && g_immigrationMul <= 100.0f) InstallImmigrationHook(text, textSize);
    else Log("[Immigration] disabled");

    // TreeGrowth=F : growth speed multiplier for saplings planted by foresters
    g_treeGrowthMul = ConfigFloat(cfg, "TreeGrowth", 1.0f);
    if (g_treeGrowthMul > 0.0f && g_treeGrowthMul != 1.0f && g_treeGrowthMul <= 1000.0f)
        InstallTreeGrowthPatch(text, textSize);
    else Log("[TreeGrowth] disabled");

    // FamiliesLvN=M : base families per house of level N (0 = vanilla)
    {
        bool any = false;
        for (int lv = 1; lv <= 4; ++lv) {
            int v = ConfigInt(cfg, ("FamiliesLv" + std::to_string(lv)).c_str(), 0);
            g_familiesPerLevel[lv] = (v >= 1 && v <= 50) ? v : 0;
            if (g_familiesPerLevel[lv] > 0) any = true;
        }
        if (any) InstallFamilyCapacityHooks(text, textSize);
        else Log("[BurgageFamilies] disabled");
    }

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
