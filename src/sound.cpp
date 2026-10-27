// Extended Sound Control (formerly Claude Sound): sound settings for The Sims 4
// (volume beyond the game's 100 %, per category, clarity). A standalone mod: its
// own menu opens with a hotkey ("." by default) when Sims4VR isn't installed;
// with Sims4VR its settings show on the VR debug menu through esc_get / esc_set
// (claudesound_get / claudesound_set kept as aliases).
//
// The DLL is also a Python extension module (_extendedsoundcontrol): the game's Python
// has no ctypes, so the script loads it with importlib.
//
// The probe (0.1.0) finds the game's audio engine objects (EA::Audio::Core
// plug-ins: SubMix, Send, Gain, Limiter1, Dac...) by their vtables and writes
// what they hold to extendedsoundcontrol_probe.txt.
//
// What the first probe showed (game 1.128, 2026-10-17): one master chain
// SubMix -> Compressor1 -> Limiter1 -> Gain (+2 dB) -> GainFader -> Dac, all
// owned by the same object (+0x20). Each category bus is a SubMix with Sends;
// one Send goes into the master SubMix (Send+0x78 = master+0x60, its input
// list), and its first parameter (Send+0x60, a float with the tag 0x7ff7fff1
// above it) is the category's level from the game's options (menu music at
// 105/255 showed as 0.41).
//
// 0.2.0: Master boost multiplies the level of every Send into the master
// SubMix, so the boost goes through the game's own compressor and limiter
// (no clipping). It hooks nothing: a worker thread writes the levels and puts
// the game's value back when the boost is 100 %. When the game changes a level
// itself (the options sliders) that becomes the new base, and the log says
// which bus moved, which tells us which bus is music, effects and so on.
//
// 0.3.0: the slider test (2026-10-17 02:52Z) matched the buses, in address
// order, to the options: bus 1 menu music (169/255 = 0.663), bus 2 UI
// (225 = 0.882), bus 5 voices (193 = 0.757), bus 6 music (237 = 0.929),
// bus 8 sound effects (218 = 0.855); level = option / 255. Buses 0, 3, 4, 7
// have no slider (ambient is one of them). The game mutes 5, 6, 8 while
// loading. A boost per category multiplies on top of the master boost; the
// buses are only named when their channel layout matches the one measured.
// Fix: a rescan kept our boosted level as the game's base (boost on boost).
// 0.4.0 Clarity: the probe showed playing sounds low-passed by the game down
// to 500-700 Hz (LowPassIir2, +0x60 = cutoff Hz, 96000 = open), and a bus with
// a high shelf cutting 5 dB above 11 kHz (HighShelfIir2, +0x60 Hz, +0x68 gain).
// Each sound's chain is HighShelf, LowPass, then HighPass at LowPass+0x110.
// Clarity keeps every low-pass at or above clarity_floor, except band-limited
// ones (high-pass above 150 Hz: the game's phone/TV/radio speaker effect), and
// opens high shelves above 4 kHz that cut.
// 0.6.1: the menu is one table (src/menu.inc) exported as esc_menu() for the VR debug menu and read by the script.
// 0.6.2: esc_quiet("music", gain) for other mods (Claude Bustin Out holds the game's music down while
// its drive runs): music and menu music at gain x their level until set back to 1; never saved.
// 0.6.0: renamed Extended Sound Control (files, settings carried over), its own
// menu with a hotkey when Sims4VR isn't installed, esc_get / esc_set.
// 0.5.0: Ambient boost. The game has no ambient slider; ambience is one of the
// four buses without one (0, 3, 4, 7). ambient_bus picks it (the player tries
// 1 .. 4: each change mutes that bus 4 s) and ambient_boost scales it.
// 0.4.1: Clarity through the engine's process functions (see below), and the
// master found among several outputs.
// 0.3.1: the address order turns round between runs, so the layout is matched
// from every starting bus (the categories were never named in the user's test).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "audio_classes.h"

namespace {

constexpr const char* kVersion = "0.6.2";
constexpr DWORD kStamp128 = 0x6aa08454;  // TS4_x64.exe 1.128.90.1030 (DX11)

// ---------------------------------------------------------------------------
// Log: extendedsoundcontrol.log next to the DLL (the package ships an empty one, so the
// mod manager keeps writes inside the mod's folder).

std::mutex g_logMutex;
FILE* g_log = nullptr;
std::wstring g_folder;

void logLine(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) {
        g_log = _wfopen((g_folder + L"extendedsoundcontrol.log").c_str(), L"w");
        if (!g_log) return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_log, fmt, args);
    va_end(args);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}
#define LOG(...) logLine(__VA_ARGS__)

// ---------------------------------------------------------------------------
// Settings: extendedsoundcontrol.ini (shipped), then extendedsoundcontrol_user.ini (changed while
// playing: the console or the Sims 4 VR debug menu, through esc_set).

struct Value {
    const char* key;
    float fallback;
    std::atomic<float> v;
};
Value g_values[] = {
    {"probe", 0, {0}},          // set to 1: look at the mixer now (never stays 1, never saved)
    {"probe_on_load", 0, {0}},  // look once after each lot load (the script asks)
    {"boost", 1, {1}},          // master boost: 1 = the game's own loudness, up to 4 (400 %)
    {"music_boost", 1, {1}},    // per category, on top of the master boost (1 .. 4)
    {"effects_boost", 1, {1}},
    {"voices_boost", 1, {1}},
    {"ui_boost", 1, {1}},
    {"menu_music_boost", 1, {1}},
    {"ambient_boost", 1, {1}},  // the bus picked by ambient_bus (1 .. 4), on top of the master boost
    {"ambient_bus", 1, {1}},    // which of the 4 buses without a slider is ambience (1 .. 4); a change mutes it 4 s
    {"menu_key", 0xBE, {0xBE}},  // virtual-key code of the menu hotkey without Sims4VR (0xBE = "."; 0 = none)
    {"clarity", 1, {1}},             // 1: lift the game's muffling of ordinary sounds (0.4.0)
    {"clarity_floor", 3000, {3000}},  // Hz: the lowest a sound's low-pass may go (500 .. 20000)
};
enum { kProbe, kProbeOnLoad, kBoost, kMusicBoost, kEffectsBoost, kVoicesBoost, kUiBoost, kMenuMusicBoost,
       kAmbientBoost, kAmbientBus, kMenuKey, kClarity, kClarityFloor, kValues };
static_assert(sizeof(g_values) / sizeof(g_values[0]) == kValues, "g_values and its enum differ");
float val(int i) { return g_values[i].v.load(std::memory_order_relaxed); }

Value* findValue(const char* key) {
    if (!key) return nullptr;
    for (Value& v : g_values)
        if (!_stricmp(v.key, key)) return &v;
    return nullptr;
}

bool readIni(const std::wstring& path, const char* key, float& out) {
    char buffer[64];
    GetPrivateProfileStringA("sound", key, "", buffer, sizeof(buffer), std::string(path.begin(), path.end()).c_str());
    char* end = nullptr;
    float x = std::strtof(buffer, &end);
    if (!buffer[0] || end == buffer || !std::isfinite(x)) return false;
    out = x;
    return true;
}

void saveSoon();

void readSettings() {
    std::wstring shipped = g_folder + L"extendedsoundcontrol.ini", user = g_folder + L"extendedsoundcontrol_user.ini";
    // Settings saved while the mod was called Claude Sound (same folder, or a claudesound folder next to it).
    std::wstring old = g_folder + L"claudesound_user.ini", oldNext = g_folder + L"..\\claudesound\\claudesound_user.ini";
    WIN32_FILE_ATTRIBUTE_DATA info;  // missing, or the empty one the package ships
    bool fresh = !GetFileAttributesExW(user.c_str(), GetFileExInfoStandard, &info) ||
                 (info.nFileSizeHigh == 0 && info.nFileSizeLow < 8);
    const std::wstring* carried = nullptr;
    for (const std::wstring* o : {&old, &oldNext})
        if (fresh && !carried && GetFileAttributesW(o->c_str()) != INVALID_FILE_ATTRIBUTES) carried = o;
    for (Value& v : g_values) {
        float f = v.fallback;
        for (const std::wstring* path : {static_cast<const std::wstring*>(&shipped), carried ? carried : &user})
            readIni(*path, v.key, f);
        v.v = f;
    }
    if (carried) {
        LOG("Settings: carried over from Claude Sound (%ls)", carried->c_str());
        saveSoon();
    }
    g_values[kProbe].v = 0;
    LOG("Settings: master boost %.0f %%, probe after lot load %.0f", val(kBoost) * 100, val(kProbeOnLoad));
}

// Saves changed values a second after the last change, on a worker thread
// that stays for the rest of the game.
std::atomic<ULONGLONG> g_saveAt{0};
std::atomic<bool> g_saver{false};
void saveSoon() {
    g_saveAt = GetTickCount64() + 1000;
    if (g_saver.exchange(true)) return;
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        for (;;) {
            Sleep(250);
            ULONGLONG at = g_saveAt.load();
            if (!at || GetTickCount64() < at || !g_saveAt.compare_exchange_strong(at, 0)) continue;
            std::string path(g_folder.begin(), g_folder.end());
            path += "extendedsoundcontrol_user.ini";
            for (Value& v : g_values) {
                if (&v == &g_values[kProbe]) continue;
                char text[32];
                std::snprintf(text, sizeof(text), "%g", v.v.load());
                WritePrivateProfileStringA("sound", v.key, text, path.c_str());
            }
            LOG("Saved to extendedsoundcontrol_user.ini");
        }
    }, nullptr, 0, nullptr);
}

void startProbe(const char* why);
std::atomic<ULONGLONG> g_muteUntil{0};  // the picked ambient bus is silent until then

bool setValue(const char* key, float f) {
    Value* v = findValue(key);
    if (!v || !std::isfinite(f)) return false;
    if (v == &g_values[kProbe]) {
        if (f != 0) startProbe("asked");
        return true;
    }
    if (v >= &g_values[kBoost] && v <= &g_values[kMenuMusicBoost]) f = std::min(4.0f, std::max(1.0f, f));
    if (v == &g_values[kClarity]) f = f != 0 ? 1.0f : 0.0f;
    if (v == &g_values[kAmbientBoost]) f = std::min(4.0f, std::max(1.0f, f));
    if (v == &g_values[kMenuKey]) f = std::min(255.0f, std::max(0.0f, std::round(f)));
    if (v == &g_values[kAmbientBus]) {
        f = std::min(4.0f, std::max(1.0f, std::round(f)));
        g_muteUntil = GetTickCount64() + 4000;  // let the player hear which sounds drop out
    }
    if (v == &g_values[kClarityFloor]) f = std::min(20000.0f, std::max(500.0f, f));
    if (v->v.exchange(f) != f) {
        LOG("Set %s = %g", v->key, f);
        saveSoon();
    }
    return true;
}

// ---------------------------------------------------------------------------
// The game

uint8_t* g_exe = nullptr;
size_t g_exeSize = 0;
bool g_known = false;  // the exe is the build the vtable table is for

bool checkExe() {
    g_exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_exe);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_exe + dos->e_lfanew);
    g_exeSize = nt->OptionalHeader.SizeOfImage;
    DWORD stamp = nt->FileHeader.TimeDateStamp;
    g_known = stamp == kStamp128;
    LOG("Game exe stamp 0x%08lx: %s", static_cast<unsigned long>(stamp),
        g_known ? "1.128.90.1030, known" : "unknown build: the mod stays off (needs an update for this patch)");
    return g_known;
}

// ---------------------------------------------------------------------------
// Probe: finds every audio plug-in object in the game's memory by its vtable
// pointer (its first 8 bytes) and writes them to extendedsoundcontrol_probe.txt.
// Memory is read with ReadProcessMemory, so a page freed meanwhile only fails
// the read instead of crashing the game.

constexpr int kClassCount = static_cast<int>(sizeof(kAudioClasses128) / sizeof(kAudioClasses128[0]));
struct VtableOf {
    uint64_t address;
    int cls;
};
VtableOf g_vtables[kClassCount];  // sorted by address; in the DLL image, not the heap the probe scans
uint64_t g_vtMin = 0, g_vtMax = 0;

void buildVtables() {
    for (int i = 0; i < kClassCount; ++i) g_vtables[i] = {reinterpret_cast<uint64_t>(g_exe) + kAudioClasses128[i].vtable, i};
    std::sort(g_vtables, g_vtables + kClassCount, [](const VtableOf& a, const VtableOf& b) { return a.address < b.address; });
    g_vtMin = g_vtables[0].address;
    g_vtMax = g_vtables[kClassCount - 1].address;
}

int classOfVtable(uint64_t q) {
    if (q < g_vtMin || q > g_vtMax) return -1;
    int lo = 0, hi = kClassCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_vtables[mid].address == q) return g_vtables[mid].cls;
        if (g_vtables[mid].address < q) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

bool readMemory(uint64_t address, void* out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), out, size, &got) && got == size;
}

struct Found {
    uint64_t address;
    int cls;
};

// Players and decoders exist once per playing sound: a few are enough to learn
// their layout. Buses, gains and effects are dumped in full.
bool isPerSound(const char* name) {
    static const char* const kinds[] = {"Player", "Dec", "Pan3D", "Pan2D", "Resample", "Layer3", "Decoder", "Pause",
                                        "TimeStretch", "MatrixPanner", "Rechannel", "MapChannels"};
    for (const char* k : kinds)
        if (std::strstr(name, k)) return true;
    return false;
}

constexpr size_t kDump = 0x180;  // bytes of each object written out

std::atomic<bool> g_probing{false};
std::atomic<int> g_probes{0};

void describe(FILE* f, uint64_t q, const std::vector<Found>& found) {
    uint64_t exe = reinterpret_cast<uint64_t>(g_exe);
    if (q >= exe && q < exe + g_exeSize) {
        int c = classOfVtable(q);
        if (c >= 0) std::fprintf(f, "  vtable %s", kAudioClasses128[c].name);
        else std::fprintf(f, "  exe+0x%llx", static_cast<unsigned long long>(q - exe));
        return;
    }
    // Points at (or into) another object found: the graph's links.
    auto it = std::upper_bound(found.begin(), found.end(), q, [](uint64_t a, const Found& b) { return a < b.address; });
    if (it != found.begin()) {
        --it;
        uint64_t off = q - it->address;
        if (off < 0x400) {
            int index = static_cast<int>(it - found.begin());
            if (off) std::fprintf(f, "  -> #%d %s+0x%llx", index, kAudioClasses128[it->cls].name, static_cast<unsigned long long>(off));
            else std::fprintf(f, "  -> #%d %s", index, kAudioClasses128[it->cls].name);
            return;
        }
    }
    float lo, hi;
    uint32_t a = static_cast<uint32_t>(q), b = static_cast<uint32_t>(q >> 32);
    std::memcpy(&lo, &a, 4);
    std::memcpy(&hi, &b, 4);
    auto plausible = [](float x, uint32_t bits) { return bits == 0 || (std::isfinite(x) && std::fabs(x) > 1e-4f && std::fabs(x) < 1e6f); };
    if (q && plausible(lo, a) && plausible(hi, b)) std::fprintf(f, "  f %g %g", lo, hi);
    else if (q && q < 0x100000000ull) std::fprintf(f, "  i %u %u", a, b);
}

// Every audio object in the game's memory (only the classes `want` accepts, when given), sorted by address.
std::vector<Found> scanMemory(uint64_t& scanned, bool (*want)(int cls) = nullptr) {
    std::vector<Found> found;
    constexpr size_t kChunk = 1 << 20;
    auto* buffer = static_cast<uint64_t*>(VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buffer) return found;
    scanned = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uint64_t at = 0x10000; at < 0x7fffffff0000ull && VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi));
         at = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || mbi.Protect != PAGE_READWRITE) continue;
        if (mbi.AllocationBase == buffer) continue;  // our own copy of the memory
        uint64_t base = reinterpret_cast<uint64_t>(mbi.BaseAddress), end = base + mbi.RegionSize;
        for (uint64_t p = base; p < end; p += kChunk) {
            size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk, end - p));
            if (!readMemory(p, buffer, n)) continue;
            scanned += n;
            for (size_t i = 0; i < n / 8; ++i) {
                uint64_t q = buffer[i];
                if (q < g_vtMin || q > g_vtMax) continue;
                int c = classOfVtable(q);
                if (c >= 0 && (!want || want(c)) && found.size() < 20000) found.push_back({p + i * 8, c});
            }
        }
    }
    VirtualFree(buffer, 0, MEM_RELEASE);
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.address < b.address; });
    return found;
}

void census(const std::vector<Found>& found, const char* why);

void probe(const char* why) {
    ULONGLONG start = GetTickCount64();
    LOG("Probe (%s): scanning the game's memory for audio objects...", why);
    uint64_t scanned = 0;
    std::vector<Found> found = scanMemory(scanned);

    int counts[kClassCount] = {};
    for (const Found& x : found) ++counts[x.cls];
    std::string summary;
    for (int c = 0; c < kClassCount; ++c)
        if (counts[c]) summary += std::string(kAudioClasses128[c].name) + " " + std::to_string(counts[c]) + ", ";
    LOG("Probe: %zu audio objects in %.0f MB scanned (%.1f s): %s", found.size(), scanned / 1048576.0,
        (GetTickCount64() - start) / 1000.0, summary.c_str());

    census(found, why);

    // Two looks 3 s apart: what changes on its own (levels, meters, fades) shows as a change.
    std::vector<std::vector<uint64_t>> first(found.size());
    int perSound[kClassCount] = {};
    std::vector<bool> dump(found.size());
    for (size_t i = 0; i < found.size(); ++i) {
        const char* name = kAudioClasses128[found[i].cls].name;
        dump[i] = !isPerSound(name) || perSound[found[i].cls]++ < 4;
        if (!dump[i]) continue;
        first[i].resize(kDump / 8);
        if (!readMemory(found[i].address, first[i].data(), kDump)) {
            first[i].resize(0x40 / 8);
            if (!readMemory(found[i].address, first[i].data(), 0x40)) first[i].clear();
        }
    }
    Sleep(3000);

    FILE* f = _wfopen((g_folder + L"extendedsoundcontrol_probe.txt").c_str(), g_probes.load() ? L"a" : L"w");
    if (!f) {
        LOG("Probe: can't write extendedsoundcontrol_probe.txt");
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(f, "==== Extended Sound Control %s probe (%s) at %02d:%02d:%02d: %zu audio objects, exe base 0x%llx\n", kVersion, why,
                 t.wHour, t.wMinute, t.wSecond, found.size(), static_cast<unsigned long long>(reinterpret_cast<uint64_t>(g_exe)));
    std::fprintf(f, "Counts: %s\n", summary.c_str());
    std::fprintf(f, "Per-sound kinds (players, decoders, panners) only the first 4 of each are shown.\n\n");
    for (size_t i = 0; i < found.size(); ++i) {
        if (!dump[i] || first[i].empty()) continue;
        std::vector<uint64_t> now(first[i].size());
        bool again = readMemory(found[i].address, now.data(), now.size() * 8);
        std::fprintf(f, "#%zu %s at 0x%llx%s\n", i, kAudioClasses128[found[i].cls].name,
                     static_cast<unsigned long long>(found[i].address), again ? "" : " (gone after 3 s)");
        for (size_t k = 0; k < first[i].size(); ++k) {
            uint64_t q = first[i][k];
            std::fprintf(f, "  +%03zx %016llx", k * 8, static_cast<unsigned long long>(q));
            describe(f, q, found);
            if (again && now[k] != q) {
                std::fprintf(f, "   CHANGED -> %016llx", static_cast<unsigned long long>(now[k]));
                describe(f, now[k], found);
            }
            std::fputc('\n', f);
        }
    }
    std::fprintf(f, "\n");
    std::fclose(f);
    ++g_probes;
    LOG("Probe: written to extendedsoundcontrol_probe.txt");
}

void startProbe(const char* why) {
    if (!g_known) {
        LOG("Probe (%s) skipped: unknown game build", why);
        return;
    }
    if (g_probing.exchange(true)) {
        LOG("Probe (%s) skipped: one is already running", why);
        return;
    }
    static char reason[64];
    std::snprintf(reason, sizeof(reason), "%s", why);
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        probe(reason);
        g_probing = false;
        return 0;
    }, nullptr, 0, nullptr);
}

// ---------------------------------------------------------------------------
// Mixer: the buses into the master SubMix and their levels.

int classIndex(const char* name) {
    for (int c = 0; c < kClassCount; ++c)
        if (!std::strcmp(kAudioClasses128[c].name, name)) return c;
    return -1;
}
int g_clsSubMix = -1, g_clsSend = -1, g_clsDac = -1, g_clsGainFader = -1;

bool writeMemory(uint64_t address, const void* data, size_t size) {
    SIZE_T done = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), data, size, &done) && done == size;
}
uint64_t readQ(uint64_t address) {
    uint64_t q = 0;
    return readMemory(address, &q, 8) ? q : 0;
}
float readF(uint64_t address) {
    float f = -1;
    return readMemory(address, &f, 4) ? f : -1;
}
uint64_t vtableOf(int cls) { return reinterpret_cast<uint64_t>(g_exe) + kAudioClasses128[cls].vtable; }

constexpr size_t kOwner = 0x20, kChannels = 0x58, kParams = 0x60, kSendTarget = 0x78;
constexpr uint32_t kFloatTag = 0x7ff7fff1;  // above a float parameter

struct Bus {
    uint64_t send = 0;    // its Send into the master SubMix
    uint64_t source = 0;  // the SubMix it belongs to (same owner), 0 if none
    float base = 1;       // the game's own level
    float written = -1;   // what we last wrote (-1: nothing)
    int resets = 0;       // times the game wrote over our level
};
std::mutex g_busMutex;
std::vector<Bus> g_buses;
uint64_t g_master = 0, g_fader = 0;
float g_faderLevel = -1;

bool sendIntoMaster(uint64_t send) {
    return readQ(send) == vtableOf(g_clsSend) && readQ(send + kSendTarget) == g_master + kParams &&
           static_cast<uint32_t>(readQ(send + kParams) >> 32) == kFloatTag;
}

// The buses in address order (from the 1-channel bus minus 4) as measured in game 1.128 (channels of their SubMix) and the boost of each.
constexpr uint32_t kBusChannels[] = {8, 8, 8, 8, 1, 8, 8, 2, 8};
constexpr int kBusBoost[] = {-1, kMenuMusicBoost, kUiBoost, -1, -1, kVoicesBoost, kMusicBoost, -1, kEffectsBoost};
constexpr size_t kHiddenBus[] = {0, 3, 4, 7};  // the buses without a slider (ambient_bus 1 .. 4)
constexpr const char* kBusName[] = {"(no slider)", "menu music", "UI", "(no slider)", "(no slider)", "voices", "music",
                                    "(no slider)", "sound effects"};
constexpr size_t kKnownBuses = sizeof(kBusChannels) / sizeof(kBusChannels[0]);
std::atomic<bool> g_named{false};  // the buses match the measured layout: the category boosts apply
std::atomic<float> g_quietMusic{1};  // esc_quiet: the music and menu music buses at this (0 .. 1), not saved

bool findBuses() {
    uint64_t scanned = 0;
    std::vector<Found> found = scanMemory(scanned, [](int c) {
        return c == g_clsSubMix || c == g_clsSend || c == g_clsDac || c == g_clsGainFader;
    });
    // Several outputs can exist (0.4.0 log: 2-3 after a lot load): the master is the SubMix next to an
    // output (same owner) that the most Sends feed.
    int dacs = 0, bestSends = 0;
    uint64_t master = 0, fader = 0;
    for (const Found& d : found) {
        if (d.cls != g_clsDac) continue;
        ++dacs;
        uint64_t owner = readQ(d.address + kOwner), mix = 0, gf = 0;
        for (const Found& x : found) {
            if (!owner || readQ(x.address + kOwner) != owner) continue;
            if (x.cls == g_clsSubMix) mix = x.address;
            if (x.cls == g_clsGainFader) gf = x.address;
        }
        int sends = 0;
        for (const Found& x : found)
            if (mix && x.cls == g_clsSend && readQ(x.address + kSendTarget) == mix + kParams) ++sends;
        if (sends > bestSends) {
            bestSends = sends;
            master = mix;
            fader = gf;
        }
    }
    if (!master) {
        LOG("Mixer: master bus not found (%d outputs, %zu objects); trying again later", dacs, found.size());
        return false;
    }
    std::lock_guard<std::mutex> lock(g_busMutex);
    g_master = master;
    g_fader = fader;
    g_faderLevel = fader ? readF(fader + kParams) : -1;
    std::vector<Bus> old;
    old.swap(g_buses);
    for (const Found& x : found) {
        if (x.cls != g_clsSend || !sendIntoMaster(x.address)) continue;
        Bus b;
        b.send = x.address;
        uint64_t owner = readQ(x.address + kOwner);
        for (const Found& y : found)
            if (y.cls == g_clsSubMix && readQ(y.address + kOwner) == owner) b.source = y.address;
        b.base = readF(x.address + kParams);
        // Still holding our boosted level: the game's own is the one we kept.
        for (const Bus& o : old)
            if (o.send == b.send && o.written >= 0 && b.base == o.written) {
                b.base = o.base;
                b.written = o.written;
            }
        g_buses.push_back(b);
    }
    // The address order turns round between runs (2026-10-17 05:27Z: every bus one further on), so the
    // layout is matched at every starting point; the 1- and 2-channel buses make the match unique.
    bool named = false;
    size_t start = 0;
    for (size_t r = 0; !named && g_buses.size() == kKnownBuses && r < kKnownBuses; ++r) {
        named = true;
        for (size_t i = 0; named && i < kKnownBuses; ++i) {
            const Bus& b = g_buses[(i + r) % kKnownBuses];
            named = b.source && (static_cast<uint32_t>(readQ(b.source + kChannels)) >> 16) == kBusChannels[i];
        }
        if (named) start = r;
    }
    if (named) std::rotate(g_buses.begin(), g_buses.begin() + start, g_buses.end());
    g_named = named;
    LOG("Mixer: master bus at 0x%llx, %zu buses feed it, master fader %.2f", static_cast<unsigned long long>(master),
        g_buses.size(), g_faderLevel);
    for (size_t i = 0; i < g_buses.size(); ++i) {
        const Bus& b = g_buses[i];
        uint32_t channels = b.source ? static_cast<uint32_t>(readQ(b.source + kChannels)) >> 16 : 0;
        LOG("Mixer: bus %zu %s (send 0x%llx, %u channels) level %.3f", i, named ? kBusName[i] : "",
            static_cast<unsigned long long>(b.send), channels, b.base);
    }
    if (!named) LOG("Mixer: the buses don't match the layout measured in 1.128: only the master boost applies");
    return !g_buses.empty();
}

// Puts boost x the game's level on every bus; follows the game's own changes.
// Returns false when the buses are gone (a rescan is needed).
bool applyBoost() {
    std::lock_guard<std::mutex> lock(g_busMutex);
    float boost = val(kBoost);
    if (g_fader) {
        float f = readF(g_fader + kParams);
        if (f >= 0 && f != g_faderLevel) {
            LOG("Mixer: master fader %.3f -> %.3f (the game)", g_faderLevel, f);
            g_faderLevel = f;
        }
    }
    for (size_t i = 0; i < g_buses.size(); ++i) {
        Bus& b = g_buses[i];
        if (!sendIntoMaster(b.send)) return false;
        float now = readF(b.send + kParams);
        if (b.written >= 0 && now != b.written) {  // the game wrote over ours
            if (++b.resets == 20) LOG("Mixer: the game keeps resetting bus %zu (the boost may not hold there)", i);
            b.written = -1;
        }
        if (b.written < 0 && now != b.base) {
            LOG("Mixer: bus %zu level %.3f -> %.3f (the game)", i, b.base, now);
            b.base = now;
        }
        float factor = boost;
        if (g_named && i < kKnownBuses && kBusBoost[i] >= 0) factor *= val(kBusBoost[i]);
        bool ambient = g_named && i == kHiddenBus[std::min(4, std::max(1, static_cast<int>(val(kAmbientBus)))) - 1];
        if (ambient) factor *= val(kAmbientBoost);
        float want = factor > 1.0001f ? b.base * factor : b.base;
        if (ambient && GetTickCount64() < g_muteUntil) want = 0;
        if (g_named && i < kKnownBuses && (kBusBoost[i] == kMusicBoost || kBusBoost[i] == kMenuMusicBoost))
            want *= g_quietMusic.load();
        if (b.written < 0 && want == b.base) continue;  // nothing of ours there
        if (want != now && !writeMemory(b.send + kParams, &want, 4)) return false;
        b.written = want == b.base ? -1 : want;
    }
    return true;
}

// Which bus each playing sound goes to (0.5.1): every player's chain ends in a Send; follow Sends from
// SubMix to SubMix until one goes into the master. Tells which hidden bus carries ambience (outside vs inside).
void census(const std::vector<Found>& found, const char* why) {
    std::lock_guard<std::mutex> lock(g_busMutex);
    if (!g_master || g_buses.empty()) {
        LOG("Census (%s): the buses aren't known yet", why);
        return;
    }
    std::vector<uint64_t> sends, players;
    for (const Found& x : found) {
        if (readQ(x.address) != vtableOf(x.cls)) continue;  // released
        const char* name = kAudioClasses128[x.cls].name;
        if (x.cls == g_clsSend) sends.push_back(x.address);
        else if (std::strstr(name, "Player")) players.push_back(x.address);
    }
    // A SubMix's owner -> the Send that owner sends on (its way towards the master).
    auto sendOf = [&](uint64_t owner) -> uint64_t {
        for (uint64_t s : sends)
            if (readQ(s + kOwner) == owner) return s;
        return 0;
    };
    auto subMixAt = [&](uint64_t target) -> uint64_t {  // Send+0x78 points at SubMix+0x60
        for (const Found& x : found)
            if (x.cls == g_clsSubMix && x.address + kParams == target) return x.address;
        return 0;
    };
    std::vector<int> count(g_buses.size(), 0);
    int lost = 0;
    for (uint64_t pl : players) {
        uint64_t send = sendOf(readQ(pl + kOwner));
        int bus = -1;
        for (int hop = 0; send && hop < 8 && bus < 0; ++hop) {
            for (size_t i = 0; i < g_buses.size(); ++i)
                if (g_buses[i].send == send) bus = static_cast<int>(i);
            if (bus >= 0) break;
            uint64_t mix = subMixAt(readQ(send + kSendTarget));
            send = mix ? sendOf(readQ(mix + kOwner)) : 0;
        }
        if (bus >= 0) ++count[bus];
        else ++lost;
    }
    std::string text;
    for (size_t i = 0; i < count.size(); ++i) {
        char part[96];
        int group = 0;
        for (int h = 0; h < 4; ++h)
            if (kHiddenBus[h] == i) group = h + 1;
        if (g_named && group) std::snprintf(part, sizeof(part), "group %d: %d, ", group, count[i]);
        else std::snprintf(part, sizeof(part), "%s: %d, ", g_named ? kBusName[i] : ("bus " + std::to_string(i)).c_str(),
                           count[i]);
        text += part;
    }
    LOG("Census (%s): %zu playing sounds by bus: %sunknown: %d", why, players.size(), text.c_str(), lost);
}

std::atomic<bool> g_mixerOn{false};
std::atomic<bool> g_rescan{false};
std::atomic<int> g_busCount{0};

void startMixer() {
    g_clsSubMix = classIndex("SubMix");
    g_clsSend = classIndex("Send");
    g_clsDac = classIndex("Dac");
    g_clsGainFader = classIndex("GainFader");
    if (g_clsSubMix < 0 || g_clsSend < 0 || g_clsDac < 0 || g_mixerOn.exchange(true)) return;
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        Sleep(5000);  // the audio engine builds its buses at start
        bool have = false;
        ULONGLONG nextScan = 0;
        double lastBoost = -1;
        for (;;) {
            ULONGLONG now = GetTickCount64();
            if ((!have || g_rescan.exchange(false)) && now >= nextScan) {
                have = findBuses();
                g_busCount = have ? static_cast<int>(g_buses.size()) : 0;
                nextScan = now + 10000;
            }
            if (have) {
                if (!applyBoost()) {
                    LOG("Mixer: the buses moved; looking for them again");
                    have = false;
                    g_busCount = 0;
                    nextScan = 0;
                } else if (double sum = val(kBoost) + 10 * val(kMusicBoost) + 100 * val(kEffectsBoost) +
                                       1000 * val(kVoicesBoost) + 10000 * val(kUiBoost) + 100000 * val(kMenuMusicBoost) +
                                       1e6f * val(kAmbientBoost) + 1e7f * val(kAmbientBus);
                           sum != lastBoost) {
                    lastBoost = sum;
                    LOG("Mixer: boost master %.0f %%, music %.0f %%, effects %.0f %%, voices %.0f %%, UI %.0f %%, menu "
                        "music %.0f %%, ambient %.0f %% (bus %d) on %d buses%s", val(kBoost) * 100, val(kMusicBoost) * 100,
                        val(kEffectsBoost) * 100, val(kVoicesBoost) * 100, val(kUiBoost) * 100,
                        val(kMenuMusicBoost) * 100, val(kAmbientBoost) * 100,
                        static_cast<int>(kHiddenBus[std::min(4, std::max(1, static_cast<int>(val(kAmbientBus)))) - 1]),
                        g_busCount.load(), g_named ? "" : " (categories unknown: master only)");
                }
            }
            Sleep(250);
        }
    }, nullptr, 0, nullptr);
}

// ---------------------------------------------------------------------------
// Clarity (0.4.1): the game sets a sound's low-pass cutoff many times a second,
// so writing the parameter fought it (0.4.0: heard as bad sound in muffled
// rooms). Instead the engine's per-plugin process functions are swapped in the
// plugin descriptors (exe .data, the engine calls through them): ours adjusts
// the parameter for the one call and puts the game's value back. No code is
// patched; sims4vr hooks none of these.
//   LowPassIir2 descriptor 0x2859a80, process table +0x18 -> 0x2859b28, slot 1 = 0x1aef230
//     (reads cutoff +0x60, w = cutoff*2pi/rate(+0x100), recomputes when w != cached +0xfc).
//   HighShelfIir2 descriptor 0x2859560 -> table 0x2859658, slot 1 = 0x1aee640
//     (freq +0x60, gain +0x68; gain 1 = bypass).

constexpr uint32_t kLowPassSlot = 0x2859b30, kLowPassProcess = 0x1aef230;
constexpr uint32_t kShelfSlot = 0x2859660, kShelfProcess = 0x1aee640;
constexpr size_t kHighPassAfterLowPass = 0x110;  // a sound's HighPassIir2 follows its LowPassIir2
constexpr float kBandLimited = 150;               // high-pass above this: a deliberate speaker effect, left alone
constexpr float kGameLowest = 500, kOpen = 20000;  // the game's muffling range (Hz)

using ProcessFn = uint64_t (*)(uint8_t* plugin, void* context, void* a3, void* a4);
ProcessFn g_lowPassProcess = nullptr, g_shelfProcess = nullptr;
uint64_t g_vtHighPass = 0;
std::atomic<uint32_t> g_lowPassCalls{0}, g_lowPassLifted{0}, g_shelfOpened{0};

// The game's heavy muffling (500 Hz .. a knee at twice the floor, 6 kHz at least) mapped onto (floor .. knee)
// on a log scale, so rooms further away still sound more muffled, only never below the floor; lighter
// muffling above the knee stays the game's.
float clearCutoff(float game, float floor) {
    float knee = std::min(kOpen, std::max(6000.0f, floor * 2));
    if (!(game > 1) || game >= knee || floor <= kGameLowest || floor >= knee) return game;
    float t = std::log(std::max(game, kGameLowest) / kGameLowest) / std::log(knee / kGameLowest);
    return std::max(game, floor * std::pow(knee / floor, t));
}

uint64_t lowPassProcess(uint8_t* p, void* context, void* a3, void* a4) {
    g_lowPassCalls.fetch_add(1, std::memory_order_relaxed);
    if (val(kClarity) == 0) return g_lowPassProcess(p, context, a3, a4);
    auto* cutoff = reinterpret_cast<float*>(p + kParams);
    float game = *cutoff, want = clearCutoff(game, val(kClarityFloor));
    if (want == game) return g_lowPassProcess(p, context, a3, a4);
    const uint8_t* hp = p + kHighPassAfterLowPass;
    if (*reinterpret_cast<const uint64_t*>(hp) == g_vtHighPass &&
        *reinterpret_cast<const uint64_t*>(hp + kOwner) == *reinterpret_cast<const uint64_t*>(p + kOwner) &&
        *reinterpret_cast<const float*>(hp + kParams) > kBandLimited)
        return g_lowPassProcess(p, context, a3, a4);  // a phone/TV/radio speaker sound
    g_lowPassLifted.fetch_add(1, std::memory_order_relaxed);
    *cutoff = want;
    uint64_t r = g_lowPassProcess(p, context, a3, a4);
    if (*cutoff == want) *cutoff = game;  // unless the game set a new one meanwhile
    return r;
}

uint64_t shelfProcess(uint8_t* p, void* context, void* a3, void* a4) {
    auto* gain = reinterpret_cast<float*>(p + kParams + 8);
    float hz = *reinterpret_cast<const float*>(p + kParams), game = *gain;
    if (val(kClarity) == 0 || !(game > 0 && game < 1) || hz < 4000 || hz > 30000)
        return g_shelfProcess(p, context, a3, a4);
    g_shelfOpened.fetch_add(1, std::memory_order_relaxed);
    *gain = 1;  // the engine bypasses a shelf at gain 1
    uint64_t r = g_shelfProcess(p, context, a3, a4);
    if (*gain == 1) *gain = game;
    return r;
}

bool swapSlot(uint32_t slot, uint32_t expected, void* ours, ProcessFn& original) {
    auto** at = reinterpret_cast<void**>(g_exe + slot);
    void* game = g_exe + expected;
    if (*at != game) {
        LOG("Clarity: the engine's slot 0x%x holds something unexpected; left alone", slot);
        return false;
    }
    original = reinterpret_cast<ProcessFn>(game);
    InterlockedExchangePointer(at, ours);
    return true;
}

void startClarity() {
    int hp = classIndex("HighPassIir2");
    if (hp < 0) return;
    g_vtHighPass = vtableOf(hp);
    bool lp = swapSlot(kLowPassSlot, kLowPassProcess, reinterpret_cast<void*>(&lowPassProcess), g_lowPassProcess);
    bool shelf = swapSlot(kShelfSlot, kShelfProcess, reinterpret_cast<void*>(&shelfProcess), g_shelfProcess);
    LOG("Clarity: %s, muffling kept at %.0f Hz or above (scaled, speaker effects left alone); low-pass %s, treble "
        "shelf %s", val(kClarity) != 0 ? "on" : "off", val(kClarityFloor), lp ? "ready" : "NOT ready",
        shelf ? "ready" : "NOT ready");
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        float lastOn = val(kClarity), lastFloor = val(kClarityFloor);
        for (;;) {
            Sleep(10000);
            uint32_t calls = g_lowPassCalls.exchange(0), lifted = g_lowPassLifted.exchange(0),
                     opened = g_shelfOpened.exchange(0);
            if (val(kClarity) != lastOn || val(kClarityFloor) != lastFloor || lifted || opened) {
                lastOn = val(kClarity);
                lastFloor = val(kClarityFloor);
                LOG("Clarity: %s at %.0f Hz; in 10 s: %u low-pass runs, %u made clearer, %u treble cuts opened",
                    lastOn != 0 ? "on" : "off", lastFloor, calls, lifted, opened);
            }
        }
    }, nullptr, 0, nullptr);
}

// ---------------------------------------------------------------------------
// Menu hotkey (0.6.0): without Sims4VR the mod opens its own menu (the script's dialogs) on a key,
// "." by default. Read while the game's window has focus; the script asks for presses twice a second.

std::atomic<int> g_menuPresses{0};

std::atomic<bool> g_capturing{false};
std::atomic<int> g_captured{0};
bool g_held[256] = {};

// The settings menu (src/menu.inc): the VR debug menu reads it through esc_menu(), the script through menu().
const wchar_t kMenu[] =
#include "menu.inc"
    ;

bool vrPresent() { return GetModuleHandleW(L"sims4vr.dll") != nullptr; }

void startHotkey() {
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        bool down = false;
        for (;;) {
            Sleep(30);
            int key = static_cast<int>(val(kMenuKey));
            HWND fg = GetForegroundWindow();
            DWORD pid = 0;
            if (fg) GetWindowThreadProcessId(fg, &pid);
            bool focused = pid == GetCurrentProcessId();
            if (g_capturing.load()) {
                // Choosing a new menu key: the first key that goes down (mouse buttons ignored).
                for (int vk = 0x08; vk < 0xFF; ++vk) {
                    bool held = focused && (GetAsyncKeyState(vk) & 0x8000);
                    if (held && !g_held[vk] && !g_captured.load()) g_captured = vk;
                    g_held[vk] = held;
                }
                down = true;
                continue;
            }
            bool now = key > 0 && key < 256 && focused && (GetAsyncKeyState(key) & 0x8000);
            if (now && !down && !vrPresent()) g_menuPresses.fetch_add(1);
            down = now;
        }
    }, nullptr, 0, nullptr);
}

// ---------------------------------------------------------------------------
// Python (3.7, the game's), minimal ABI subset.

struct PyObject {
    ptrdiff_t ob_refcnt;
    void* ob_type;
};
using PyCFunction = PyObject* (*)(PyObject*, PyObject*);
struct PyMethodDef {
    const char* ml_name;
    PyCFunction ml_meth;
    int ml_flags;
    const char* ml_doc;
};
struct PyModuleDef_Base {
    PyObject ob_base;
    PyObject* (*m_init)();
    ptrdiff_t m_index;
    PyObject* m_copy;
};
struct PyModuleDef {
    PyModuleDef_Base m_base;
    const char* m_name;
    const char* m_doc;
    ptrdiff_t m_size;
    PyMethodDef* m_methods;
    void* m_slots;
    void* m_traverse;
    void* m_clear;
    void* m_free;
};
constexpr int METH_VARARGS = 0x0001;
constexpr int METH_NOARGS = 0x0004;

struct PythonApi {
    PyObject* (*PyModule_Create2)(PyModuleDef*, int);
    int (*PyArg_ParseTuple)(PyObject*, const char*, ...);
    PyObject* (*Py_BuildValue)(const char*, ...);
    void (*Py_IncRef)(PyObject*);
    PyObject* None;
} py;

PyObject* none() {
    py.Py_IncRef(py.None);
    return py.None;
}

PyObject* pySet(PyObject*, PyObject* args) {
    const char* name = nullptr;
    float v = 0;
    if (!py.PyArg_ParseTuple(args, "sf", &name, &v)) return nullptr;
    return py.Py_BuildValue("i", setValue(name, v) ? 1 : 0);
}

PyObject* pyGet(PyObject*, PyObject* args) {
    const char* name = nullptr;
    if (!py.PyArg_ParseTuple(args, "s", &name)) return nullptr;
    Value* v = findValue(name);
    if (!v) return none();
    return py.Py_BuildValue("d", static_cast<double>(v->v.load()));
}

// status() -> (known game build, probe running, probes written, buses found, master boost)
PyObject* pyStatus(PyObject*, PyObject*) {
    return py.Py_BuildValue("(iiiid)", g_known ? 1 : 0, g_probing.load() ? 1 : 0, g_probes.load(), g_busCount.load(),
                            static_cast<double>(val(kBoost)));
}

// rescan(): look for the buses again (after a lot load).
PyObject* pyRescan(PyObject*, PyObject*) {
    g_rescan = true;
    return none();
}

PyObject* pyProbe(PyObject*, PyObject* args) {
    const char* why = "console";
    if (!py.PyArg_ParseTuple(args, "|s", &why)) return nullptr;
    startProbe(why);
    return none();
}

PyObject* pyLog(PyObject*, PyObject* args) {
    const char* text = nullptr;
    if (!py.PyArg_ParseTuple(args, "s", &text)) return nullptr;
    LOG("script: %s", text);
    return none();
}

// menu_presses() -> menu hotkey presses since the last call; has_vr() -> 1 when Sims4VR is loaded.
PyObject* pyMenuPresses(PyObject*, PyObject*) { return py.Py_BuildValue("i", g_menuPresses.exchange(0)); }
PyObject* pyHasVr(PyObject*, PyObject*) { return py.Py_BuildValue("i", vrPresent() ? 1 : 0); }

// menu() -> the menu rows (jiggle/docs/MENU-EXPORT.md) as text.
PyObject* pyMenu(PyObject*, PyObject*) { return py.Py_BuildValue("u", kMenu); }

// capture(1) starts listening for a new menu key; capture(0) stops and returns the key pressed (0 = none).
PyObject* pyCapture(PyObject*, PyObject* args) {
    int on = 0;
    if (!py.PyArg_ParseTuple(args, "i", &on)) return nullptr;
    if (on) {
        for (int vk = 0; vk < 256; ++vk) g_held[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        g_captured = 0;
        g_capturing = true;
        return py.Py_BuildValue("i", 0);
    }
    g_capturing = false;
    return py.Py_BuildValue("i", g_captured.exchange(0));
}

// key_name(vk) -> the key's name on this keyboard.
PyObject* pyKeyName(PyObject*, PyObject* args) {
    int vk = 0;
    if (!py.PyArg_ParseTuple(args, "i", &vk)) return nullptr;
    wchar_t name[64] = L"None";
    if (vk > 0 && vk < 256) {
        LONG scan = static_cast<LONG>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16;
        if (vk >= VK_PRIOR && vk <= VK_DELETE) scan |= 1 << 24;  // the arrow/navigation block, not the numpad
        if (!scan || !GetKeyNameTextW(scan, name, 64)) swprintf(name, 64, L"key %d", vk);
    }
    return py.Py_BuildValue("u", name);
}

PyMethodDef methods[] = {
    {"menu", pyMenu, METH_NOARGS, "menu() -> the menu rows as text"},
    {"capture", pyCapture, METH_VARARGS, "capture(1) listen for a new menu key; capture(0) -> the key pressed"},
    {"key_name", pyKeyName, METH_VARARGS, "key_name(vk) -> the key's name"},
    {"menu_presses", pyMenuPresses, METH_NOARGS, "menu hotkey presses since the last call"},
    {"has_vr", pyHasVr, METH_NOARGS, "1 when Sims4VR is loaded (it shows these settings itself)"},
    {"set", pySet, METH_VARARGS, "set(name, value) -> 1 for a known setting"},
    {"get", pyGet, METH_VARARGS, "get(name) -> value of an extendedsoundcontrol.ini setting, or None"},
    {"status", pyStatus, METH_NOARGS, "(known game build, probe running, probes written, buses, boost)"},
    {"rescan", pyRescan, METH_NOARGS, "rescan(): find the mixer buses again"},
    {"probe", pyProbe, METH_VARARGS, "probe([why]): look at the mixer in the background"},
    {"log", pyLog, METH_VARARGS, "log(text)"},
    {nullptr, nullptr, 0, nullptr},
};
PyModuleDef moduleDef = {{{1, nullptr}, nullptr, 0, nullptr}, "_extendedsoundcontrol", "Extended Sound Control", -1, methods,
                         nullptr, nullptr, nullptr, nullptr};

template <typename T>
bool resolveFn(HMODULE m, T& out, const char* name) {
    out = reinterpret_cast<T>(GetProcAddress(m, name));
    return out != nullptr;
}

bool loadApi() {
    HMODULE m = nullptr;
    for (const wchar_t* name : {L"python37_x64.dll", L"python37.dll", L"python3.dll"})
        if ((m = GetModuleHandleW(name))) break;
    if (!m) return false;
    return resolveFn(m, py.PyModule_Create2, "PyModule_Create2") && resolveFn(m, py.PyArg_ParseTuple, "PyArg_ParseTuple") &&
           resolveFn(m, py.Py_BuildValue, "Py_BuildValue") && resolveFn(m, py.Py_IncRef, "Py_IncRef") &&
           resolveFn(m, py.None, "_Py_NoneStruct");
}

bool g_started = false;
void start() {
    if (g_started) return;
    g_started = true;
    LOG("Extended Sound Control %s", kVersion);
    readSettings();
    if (checkExe()) {
        buildVtables();
        startMixer();
        startClarity();
        startHotkey();
    }
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(instance, path, MAX_PATH);
        std::wstring p(path, n);
        g_folder = p.substr(0, p.find_last_of(L"\\/") + 1);
    }
    return TRUE;
}

// For other mods (the Sims 4 VR debug menu): read and change a setting by its
// extendedsoundcontrol.ini key. 1 = known key.
extern "C" __declspec(dllexport) int esc_get(const char* key, float* out) {
    Value* v = findValue(key);
    if (!v || !out) return 0;
    *out = v->v.load();
    return 1;
}
extern "C" __declspec(dllexport) int esc_set(const char* key, float value) { return setValue(key, value) ? 1 : 0; }
// For other mods (Claude Bustin Out's drive): the game's music ("music": the music and menu music buses) at
// gain (0 .. 1) of its level until set back to 1; not a setting, never saved. 1 = the buses are known (it
// applies within a quarter second), 0 = they aren't (yet).
extern "C" __declspec(dllexport) int esc_quiet(const char* what, float gain) {
    if (!what || std::strcmp(what, "music") != 0 || !std::isfinite(gain)) return 0;
    gain = std::fmin(1.0f, std::fmax(0.0f, gain));
    if (g_quietMusic.exchange(gain) != gain) LOG("Music held at %.0f %% by another mod (esc_quiet)", gain * 100);
    return g_named.load() && g_busCount.load() > 0 ? 1 : 0;
}
// The settings menu as rows of text (src/menu.inc, format in jiggle/docs/MENU-EXPORT.md); values through esc_get / esc_set.
extern "C" __declspec(dllexport) const wchar_t* esc_menu() { return kMenu; }
// The names used while the mod was called Claude Sound.
extern "C" __declspec(dllexport) int claudesound_get(const char* key, float* out) { return esc_get(key, out); }
extern "C" __declspec(dllexport) int claudesound_set(const char* key, float value) { return esc_set(key, value); }

extern "C" __declspec(dllexport) PyObject* PyInit__extendedsoundcontrol() {
    if (!loadApi()) return nullptr;
    start();
    return py.PyModule_Create2(&moduleDef, 1013);
}
