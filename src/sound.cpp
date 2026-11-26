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

#include <unordered_map>

#include "audio_classes.h"
#include "third_party/steamaudio/phonon.h"  // Steam Audio 4.8.1 (Apache-2.0); phonon.dll is loaded at run time
#include "audio_sigs.inc"

namespace {

constexpr const char* kVersion = "0.9.7";
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
    {"pan_log", 1, {1}},              // 1: log what the 3D panner is given (HRTF groundwork, changes no sound)
    {"hrtf", 1, {1}},                 // 1: own binaural 3D sound for positional sounds (needs the game's output set to Stereo)
    {"hrtf_sa", 1, {1}},              // 1: Steam Audio's HRTF (phonon.dll next to the mod) instead of the built-in head model
    {"hrtf_strength", 1, {1}},        // 0 .. 1: how much of the binaural effect (0 = none, the game's sound)
    {"hrtf_rear", 1, {1}},            // 0 .. 1.5: how strongly sounds behind you are dulled
    {"hrtf_stereo", 1, {1}},          // 1: also make stereo sources (music, radios, TV) binaural, from their left+right mix
    {"hrtf_mirror", 0, {0}},
    {"hrtf_close", 1, {1}},           // 1: sounds closer than 1 m (a Sim next to you, scenes) keep their direction instead of sliding into the middle of the head          // 1: mirror left and right (new key: 0.9.0 saved hrtf_flip = 1 for the then-wrong direction)
    {"muffle", 1, {1}},               // 1: sounds heard through walls, floors and closed doors are muffled
    {"muffle_amount", 1, {1}},        // 0 .. 1: how strong the muffling is
    {"muffle_treble", 400, {400}},    // Hz: how dull a fully blocked sound gets (200 .. 4000)
    {"muffle_loss", 0.6f, {0.6f}},    // 0 .. 1: how much quieter a fully blocked sound gets
    {"reverb", 1, {1}},               // 1: room reverb on every positioned sound
    {"reverb_amount", 0.25f, {0.25f}},// 0 .. 1: how much reverb is mixed in
    {"reverb_size", 0.5f, {0.5f}},    // 0 .. 1: small furnished room .. big empty hall (used when the VR mod gives no room)
    {"reverb_damp", 0.5f, {0.5f}},
    {"reverb_outdoor", 0.15f, {0.15f}},// 0 .. 1: how much reverb there is outdoors (open air has almost none)    // 0 .. 1: soft furnishings soak up treble in the tail
};
enum { kProbe, kProbeOnLoad, kBoost, kMusicBoost, kEffectsBoost, kVoicesBoost, kUiBoost, kMenuMusicBoost,
       kAmbientBoost, kAmbientBus, kMenuKey, kClarity, kClarityFloor, kPanLog, kHrtf, kHrtfSA, kHrtfStrength, kHrtfRear, kHrtfStereo, kHrtfFlip, kHrtfClose, kMuffle, kMuffleAmount, kMuffleTreble, kMuffleLoss, kReverb, kReverbAmount, kReverbSize, kReverbDamp, kReverbOutdoor, kValues };
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
    if (v == &g_values[kPanLog] || v == &g_values[kHrtf] || v == &g_values[kHrtfSA] || v == &g_values[kHrtfStereo] || v == &g_values[kHrtfFlip] || v == &g_values[kReverb] || v == &g_values[kMuffle] || v == &g_values[kHrtfClose]) f = f != 0 ? 1.0f : 0.0f;
    if (v == &g_values[kMuffleAmount] || v == &g_values[kMuffleLoss]) f = std::min(1.0f, std::max(0.0f, f));
    if (v == &g_values[kMuffleTreble]) f = std::min(4000.0f, std::max(200.0f, f));
    if (v == &g_values[kReverbAmount] || v == &g_values[kReverbSize] || v == &g_values[kReverbDamp] || v == &g_values[kReverbOutdoor]) f = std::min(1.0f, std::max(0.0f, f));
    if (v == &g_values[kHrtfStrength]) f = std::min(1.0f, std::max(0.0f, f));
    if (v == &g_values[kHrtfRear]) f = std::min(1.5f, std::max(0.0f, f));
    if (v == &g_values[kClarityFloor]) f = std::min(20000.0f, std::max(500.0f, f));
    if (v->v.exchange(f) != f) {
        LOG("Set %s = %g", v->key, f);
        saveSoon();
    }
    return true;
}

// ---------------------------------------------------------------------------
// The game

constexpr int kClassCount = static_cast<int>(sizeof(kAudioClasses128) / sizeof(kAudioClasses128[0]));
uint8_t* g_exe = nullptr;
size_t g_exeSize = 0;
bool g_known = false;  // the exe is the build the vtable table is for

uint32_t g_vtRva[kClassCount];  // vtable rva per class in THIS build (0 = not found)
uint32_t g_lowPassSlot = 0, g_lowPassFn = 0, g_shelfSlot = 0, g_shelfFn = 0;  // rvas in this build
uint32_t g_panSlot = 0, g_panFn = 0;  // Pan2D1 process function and its descriptor slot

// --- Another build (1.129 and later): rvas by signature. The 1.129 exe is encrypted on disk, but its
// image in memory is plain, so everything is found in memory: a constructor of each class by its masked
// signature (src/audio_sigs.inc, made from 1.128 by tools/gen_audio_sigs.py), the vtable from the lea
// inside it, and the filter process slots in .data as the qword that holds the process function.

struct Pat {
    std::vector<uint8_t> bytes, mask;
};
Pat parsePat(const char* sig) {
    Pat p;
    for (const char* c = sig; *c;) {
        while (*c == ' ') ++c;
        if (!*c) break;
        if (c[0] == '?') {
            p.bytes.push_back(0);
            p.mask.push_back(0);
            c += 2;
        } else {
            p.bytes.push_back(static_cast<uint8_t>(std::strtoul(std::string(c, 2).c_str(), nullptr, 16)));
            p.mask.push_back(1);
            c += 2;
        }
    }
    return p;
}

struct Region {
    const uint8_t* at;
    size_t size;
};
// Committed, readable regions of the exe section called `name`.
std::vector<Region> sectionRegions(const char* name) {
    std::vector<Region> out;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_exe);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_exe + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::strncmp(reinterpret_cast<const char*>(sec[i].Name), name, 8) != 0) continue;
        uint8_t* lo = g_exe + sec[i].VirtualAddress;
        uint8_t* hi = lo + std::max(sec[i].Misc.VirtualSize, sec[i].SizeOfRawData);
        for (uint8_t* p = lo; p < hi;) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
            uint8_t* end = std::min(hi, static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize);
            bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
            if (ok) out.push_back({p, static_cast<size_t>(end - p)});
            p = end;
        }
    }
    return out;
}

// Matches (first two) of every pattern in .text, in one pass: patterns are bucketed by their first 4 bytes.
std::vector<std::vector<const uint8_t*>> scanPatterns(const std::vector<Pat>& pats) {
    std::vector<std::vector<const uint8_t*>> hits(pats.size());
    std::unordered_multimap<uint32_t, int> byKey;
    for (size_t i = 0; i < pats.size(); ++i) {
        if (pats[i].bytes.size() < 8 || !pats[i].mask[0] || !pats[i].mask[1] || !pats[i].mask[2] || !pats[i].mask[3]) continue;
        uint32_t k;
        std::memcpy(&k, pats[i].bytes.data(), 4);
        byKey.emplace(k, static_cast<int>(i));
    }
    for (const Region& r : sectionRegions(".text")) {
        for (size_t o = 0; o + 4 <= r.size; ++o) {
            uint32_t k;
            std::memcpy(&k, r.at + o, 4);
            auto range = byKey.equal_range(k);
            for (auto it = range.first; it != range.second; ++it) {
                const Pat& p = pats[it->second];
                if (o + p.bytes.size() > r.size || hits[it->second].size() >= 2) continue;
                bool ok = true;
                for (size_t j = 4; j < p.bytes.size() && ok; ++j)
                    if (p.mask[j] && r.at[o + j] != p.bytes[j]) ok = false;
                if (ok) hits[it->second].push_back(r.at + o);
            }
        }
    }
    return hits;
}

bool inSection(const char* name, const void* p) {
    for (const Region& r : sectionRegions(name))
        if (p >= r.at && p < r.at + r.size) return true;
    return false;
}

// The qword equal to `value` in .data (exactly one), as an rva; 0 = none or several.
uint32_t dataSlotHolding(uint64_t value) {
    uint32_t found = 0;
    int n = 0;
    for (const Region& r : sectionRegions(".data")) {
        for (size_t o = 0; o + 8 <= r.size; o += 8) {
            uint64_t q;
            std::memcpy(&q, r.at + o, 8);
            if (q == value) {
                found = static_cast<uint32_t>(r.at + o - g_exe);
                ++n;
            }
        }
    }
    return n == 1 ? found : 0;
}

bool resolveByScan() {
    const int nCls = static_cast<int>(sizeof(kAudioSigs) / sizeof(kAudioSigs[0]));
    const int nProc = static_cast<int>(sizeof(kProcSigs) / sizeof(kProcSigs[0]));
    std::vector<Pat> pats;
    for (int i = 0; i < nCls; ++i) pats.push_back(parsePat(kAudioSigs[i].sig));
    for (int i = 0; i < nProc; ++i) pats.push_back(parsePat(kProcSigs[i].sig));
    ULONGLONG t0 = GetTickCount64();
    auto hits = scanPatterns(pats);
    int resolved = 0;
    for (int i = 0; i < nCls; ++i) {
        const AudioSig& a = kAudioSigs[i];
        int cls = -1;
        for (int c = 0; c < kClassCount; ++c)
            if (!std::strcmp(kAudioClasses128[c].name, a.name)) cls = c;
        if (cls < 0) continue;
        if (hits[i].size() != 1) {
            LOG("Build scan: %s constructor %s", a.name, hits[i].empty() ? "not found" : "signature not unique");
            continue;
        }
        const uint8_t* fn = hits[i][0];
        if ((fn[a.dispOff - 3] != 0x48 && fn[a.dispOff - 3] != 0x4c) || fn[a.dispOff - 2] != 0x8d) {
            LOG("Build scan: %s constructor has no lea where expected", a.name);
            continue;
        }
        int32_t disp;
        std::memcpy(&disp, fn + a.dispOff, 4);
        const uint8_t* vt = fn + a.endOff + disp;
        uint64_t slot0 = 0;
        std::memcpy(&slot0, vt, 8);
        if (!inSection(".rdata", vt) || !inSection(".text", reinterpret_cast<void*>(slot0))) {
            LOG("Build scan: %s vtable 0x%llx fails the sanity check", a.name, static_cast<unsigned long long>(vt - g_exe));
            continue;
        }
        g_vtRva[cls] = static_cast<uint32_t>(vt - g_exe);
        ++resolved;
        LOG("Build scan: %s vtable 0x%x (1.128: 0x%x, shift %+lld)", a.name, g_vtRva[cls], a.vtable128,
            static_cast<long long>(static_cast<int64_t>(g_vtRva[cls]) - static_cast<int64_t>(a.vtable128)));
    }
    int procs = 0;
    for (int i = 0; i < nProc; ++i) {
        const ProcSig& p = kProcSigs[i];
        auto& h = hits[nCls + i];
        if (h.size() != 1) {
            LOG("Build scan: %s process function %s", p.name, h.empty() ? "not found" : "signature not unique");
            continue;
        }
        uint32_t fnRva = static_cast<uint32_t>(h[0] - g_exe);
        uint32_t slot = dataSlotHolding(reinterpret_cast<uint64_t>(h[0]));
        if (!slot) {
            LOG("Build scan: %s process function 0x%x is not held by exactly one .data slot", p.name, fnRva);
            continue;
        }
        LOG("Build scan: %s process 0x%x in slot 0x%x (1.128: 0x%x / 0x%x)", p.name, fnRva, slot, p.fn128, p.slot128);
        if (!std::strcmp(p.name, "LowPassIir2")) g_lowPassFn = fnRva, g_lowPassSlot = slot;
        else if (!std::strcmp(p.name, "Pan2D1")) g_panFn = fnRva, g_panSlot = slot;
        else g_shelfFn = fnRva, g_shelfSlot = slot;
        ++procs;
    }
    LOG("Build scan: %d of %d classes, %d of %d process slots, %llu ms", resolved, nCls, procs, nProc,
        static_cast<unsigned long long>(GetTickCount64() - t0));
    return resolved > 0;
}

// The classes the mixer and Clarity cannot work without.
bool coreResolved() {
    for (const char* n : {"SubMix", "Send", "Dac", "GainFader", "HighPassIir2"})
        for (int c = 0; c < kClassCount; ++c)
            if (!std::strcmp(kAudioClasses128[c].name, n) && !g_vtRva[c]) return false;
    return true;
}

bool checkExe() {
    g_exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_exe);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_exe + dos->e_lfanew);
    g_exeSize = nt->OptionalHeader.SizeOfImage;
    DWORD stamp = nt->FileHeader.TimeDateStamp;
    if (stamp == kStamp128) {
        for (int c = 0; c < kClassCount; ++c) g_vtRva[c] = kAudioClasses128[c].vtable;
        g_lowPassSlot = 0x2859b30, g_lowPassFn = 0x1aef230, g_shelfSlot = 0x2859660, g_shelfFn = 0x1aee640;
        g_panSlot = 0x285ad20, g_panFn = 0x1af0220;
        g_known = true;
        LOG("Game exe stamp 0x%08lx: 1.128.90.1030, table addresses", static_cast<unsigned long>(stamp));
        return true;
    }
    LOG("Game exe stamp 0x%08lx is not 1.128: looking for the audio classes by signature in memory",
        static_cast<unsigned long>(stamp));
    g_known = resolveByScan() && coreResolved();
    LOG(g_known ? "Found the audio classes by signature: the mod is on" : "The audio classes were not all found: the mod stays off");
    return g_known;
}

// ---------------------------------------------------------------------------
// Probe: finds every audio plug-in object in the game's memory by its vtable
// pointer (its first 8 bytes) and writes them to extendedsoundcontrol_probe.txt.
// Memory is read with ReadProcessMemory, so a page freed meanwhile only fails
// the read instead of crashing the game.

struct VtableOf {
    uint64_t address;
    int cls;
};
VtableOf g_vtables[kClassCount];
int g_vtCount = 0;  // sorted by address; in the DLL image, not the heap the probe scans
uint64_t g_vtMin = 0, g_vtMax = 0;

void buildVtables() {
    g_vtCount = 0;
    for (int i = 0; i < kClassCount; ++i)
        if (g_vtRva[i]) g_vtables[g_vtCount++] = {reinterpret_cast<uint64_t>(g_exe) + g_vtRva[i], i};
    std::sort(g_vtables, g_vtables + g_vtCount, [](const VtableOf& a, const VtableOf& b) { return a.address < b.address; });
    g_vtMin = g_vtables[0].address;
    g_vtMax = g_vtables[g_vtCount - 1].address;
}

int classOfVtable(uint64_t q) {
    if (q < g_vtMin || q > g_vtMax) return -1;
    int lo = 0, hi = g_vtCount - 1;
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
uint64_t vtableOf(int cls) { return reinterpret_cast<uint64_t>(g_exe) + g_vtRva[cls]; }

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
    if (!(game > 1) || game >= knee || floor <= kGameLowest) return game;
    if (floor >= knee) return std::max(game, floor);  // floor at the top of the range: muffling fully off
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
    if (hp < 0 || !g_vtRva[hp] || !g_lowPassSlot || !g_shelfSlot) {
        LOG("Clarity: slots or classes not found in this build; off");
        return;
    }
    g_vtHighPass = vtableOf(hp);
    bool lp = swapSlot(g_lowPassSlot, g_lowPassFn, reinterpret_cast<void*>(&lowPassProcess), g_lowPassProcess);
    bool shelf = swapSlot(g_shelfSlot, g_shelfFn, reinterpret_cast<void*>(&shelfProcess), g_shelfProcess);
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
// 3D panner probe (0.8.0, HRTF groundwork): the engine's Pan2D1 process function (descriptor slot, found like the
// filters') is wrapped. It changes nothing: it counts calls and, a few times a second, copies what one call was
// given (the panner's angle/distance parameters at +0x60/+0x68/+0x70, output and input channel counts at
// +0x324/+0x328, the voice context's input buffer at ctx+0xc490: data pointer at +8, channel stride (ushort) at
// +0x12) into a small ring that a logger thread writes out. The context swaps its two buffers (ctx+0xc490 /
// +0xc498) after the call, so an HRTF renderer would read the mono input before the call and write the stereo
// output after it. Read with ReadProcessMemory, never dereferenced on the audio thread.

using PanFn = uint64_t (*)(uint8_t* plugin, void* context, char flag);
ProcessFn g_panProcess = nullptr;  // the game's own function (called as a PanFn)
std::atomic<uint32_t> g_panCalls{0};
constexpr size_t kPanRing = 48;
struct PanSample {
    uint64_t panner = 0;
    uint32_t in = 0, out = 0, stride = 0, flag = 0;
    float angle = 0, distance = 0, third = 0, peak = 0;
};
PanSample g_panRing[kPanRing];
std::atomic<uint32_t> g_panTaken{0};
std::atomic<ULONGLONG> g_panNext{0};

void samplePanner(uint8_t* p, uint8_t* ctx, char flag) {
    uint32_t slot = g_panTaken.load(std::memory_order_relaxed);
    if (slot >= kPanRing) return;
    PanSample s;
    s.panner = reinterpret_cast<uint64_t>(p);
    s.flag = static_cast<uint32_t>(static_cast<uint8_t>(flag));
    float f[3] = {};
    uint32_t ch[2] = {};
    readMemory(reinterpret_cast<uint64_t>(p + 0x60), &f[0], 4);
    readMemory(reinterpret_cast<uint64_t>(p + 0x68), &f[1], 4);
    readMemory(reinterpret_cast<uint64_t>(p + 0x70), &f[2], 4);
    readMemory(reinterpret_cast<uint64_t>(p + 0x324), &ch[0], 8);  // in, out
    s.angle = f[0], s.distance = f[1], s.third = f[2], s.in = ch[0], s.out = ch[1];  // +0x324 input, +0x328 output channels
    uint64_t buf = readQ(reinterpret_cast<uint64_t>(ctx + 0xc490));
    if (buf) {
        uint64_t data = readQ(buf + 8);
        uint16_t stride = 0;
        readMemory(buf + 0x12, &stride, 2);
        s.stride = stride;
        float block[256];
        if (data && readMemory(data, block, sizeof(block))) {
            float peak = 0;
            for (float x : block) peak = std::max(peak, std::fabs(x));
            s.peak = peak;
        }
    }
    g_panRing[slot] = s;
    g_panTaken.store(slot + 1, std::memory_order_release);
}

// --- Binaural 3D sound (0.8.0). A positional sound's Pan2D1 gets its direction as an angle (+0x60, degrees,
// 0 = ahead, growing towards the listener's first axis = right) and a "directness" (+0x68: distance across the
// listener's plane, capped at the sound's near radius; under 1 the game spreads the sound towards the middle,
// 0 = middle of the head). With the game's output set to Stereo (2 output channels) and a mono source, the
// engine's own pan result is replaced by a head model: interaural time difference (Woodworth, fractional
// delay), head shadow (far ear lower and duller), and a dulling of sounds behind you (the ear's shadow:
// front/back). Height is not known to the panner (its third parameter is 0): the VR mod's pan step would
// have to pass it. Blocks are 256 samples, planar, channel stride = ushort at buffer+0x12; the input buffer
// is ctx+0xc490 and the output ctx+0xc498 before the call (swapped after it).

constexpr int kHrtfVoices = 256, kBlock = 256, kHist = 64;

// --- Steam Audio (phonon.dll, Apache-2.0, loaded from the mod folder; a missing or failing DLL leaves the built-in
// head model below in charge). One binaural effect per voice slot, all made on a worker thread at start.
// Directions are in the listener's frame: +x right, +y up, -z ahead (Steam Audio's own convention, and the
// game's: the pan angle is atan2(x, -z)). A hrtf.sofa file in the mod folder is loaded instead of the built-in HRTF.
struct SteamAudio {
    decltype(&iplContextCreate) contextCreate = nullptr;
    decltype(&iplHRTFCreate) hrtfCreate = nullptr;
    decltype(&iplBinauralEffectCreate) effectCreate = nullptr;
    decltype(&iplBinauralEffectApply) effectApply = nullptr;
    decltype(&iplBinauralEffectReset) effectReset = nullptr;
    IPLContext context = nullptr;
    IPLHRTF hrtf = nullptr;
    IPLAudioSettings audio{48000, kBlock};
};
SteamAudio g_sa;
std::atomic<bool> g_saReady{false};

struct HrtfVoice {
    std::atomic<uint64_t> key{0};
    std::atomic<IPLBinauralEffect> fx{nullptr};
    uint32_t last = 0;
    bool fresh = true;
    float hist[kHist] = {};
    struct Reverb* rev = nullptr;  // made when the voice first gets reverb
    float lpL = 0, lpR = 0, elvLp = 0, occLp = 0, occG = 1;  // occ*: the muffle state  // elvLp: the height cue's low-pass state
    float gL = 1, gR = 1, dL = 0, dR = 0, aL = 1, aR = 1;  // the values at the end of the last block
};
HrtfVoice g_hrtf[kHrtfVoices];

// Room reverb: a small Schroeder reverb per voice (4 combs with a damping low-pass, then an all-pass per ear), so
// every positioned sound gets a tail without a shared bus. The comb lengths follow the room size when the voice
// starts. The VR mod can give the real room through esc_set_room (volume in m3, absorption 0..1); the menu's size and
// damping apply when it hasn't for 2 s.
constexpr int kCombBase[4] = {1116, 1188, 1277, 1356}, kApBase[2] = {556, 441}, kRevMax = 3 * 1400;
struct Reverb {
    float comb[4][kRevMax] = {};
    float ap[2][2][1200] = {};
    int clen[4] = {1116, 1188, 1277, 1356}, cpos[4] = {}, apos[2] = {};
    float damp[4] = {};
};
std::atomic<float> g_roomVolume{0}, g_roomAbsorb{0};
std::atomic<uint32_t> g_roomTick{0};
std::atomic<float> g_pyRoomVolume{0}, g_pyRoomAbsorb{0};  // the script's own estimate (used when the VR mod gives none)
std::atomic<uint32_t> g_pyRoomTick{0};

std::atomic<float> g_smSize{-1}, g_smDamp{0}, g_smOpen{0};  // eased room values (rooms change smoothly, not in jumps)
std::atomic<uint32_t> g_smTick{0};

void roomParams(float& size01, float& damp01, float* openAir = nullptr) {
    size01 = val(kReverbSize), damp01 = val(kReverbDamp);
    float open = 0;
    // The VR mod's room (esc_set_room) wins, then this mod's own script estimate, then the menu.
    const uint32_t now = GetTickCount();
    float vol = 0, ab = 0;
    if (g_roomVolume.load(std::memory_order_relaxed) > 0 && now - g_roomTick.load(std::memory_order_relaxed) < 2000)
        vol = g_roomVolume.load(std::memory_order_relaxed), ab = g_roomAbsorb.load(std::memory_order_relaxed);
    else if (g_pyRoomVolume.load(std::memory_order_relaxed) > 0 && now - g_pyRoomTick.load(std::memory_order_relaxed) < 5000)
        vol = g_pyRoomVolume.load(std::memory_order_relaxed), ab = g_pyRoomAbsorb.load(std::memory_order_relaxed);
    if (vol > 0) {
        // log scale: 12 m3 (a closet) .. 1000 m3 (a hall) is 0 .. 1; a room over 1500 m3 is the open air
        size01 = std::min(1.0f, std::max(0.0f, (std::log2(vol) - 3.5f) / 6.5f));
        damp01 = std::min(1.0f, std::max(0.0f, ab));
        open = vol >= 1500.0f ? 1.0f : 0.0f;
    }
    // ease towards the target (about a second), so the tail follows you through a door instead of jumping
    float sz = g_smSize.load(std::memory_order_relaxed);
    const uint32_t t = GetTickCount(), last = g_smTick.load(std::memory_order_relaxed);
    if (sz < 0) sz = size01, g_smDamp.store(damp01), g_smOpen.store(open);
    else if (t != last) {
        float k = std::min(1.0f, static_cast<float>(t - last) / 1000.0f);
        sz += (size01 - sz) * k;
        g_smDamp.store(g_smDamp.load() + (damp01 - g_smDamp.load()) * k);
        g_smOpen.store(g_smOpen.load() + (open - g_smOpen.load()) * k);
    }
    g_smSize.store(sz);
    g_smTick.store(t);
    size01 = sz, damp01 = g_smDamp.load();
    if (openAir) *openAir = g_smOpen.load();
}

void startReverb(HrtfVoice& v) {
    if (!v.rev) v.rev = new Reverb();
    else *v.rev = Reverb();
    float size01, damp01;
    roomParams(size01, damp01);
    float scale = 0.45f + 1.55f * size01;  // 0.45 .. 2.0 of the base lengths
    for (int i = 0; i < 4; ++i) v.rev->clen[i] = std::min(kRevMax, std::max(64, static_cast<int>(kCombBase[i] * scale)));
}

// Adds the tail of `in` to left/right (already binaural).
void addReverb(HrtfVoice& v, const float* in, float* left, float* right, float directness) {
    if (!v.rev) startReverb(v);
    Reverb& r = *v.rev;
    float size01, damp01, open;
    roomParams(size01, damp01, &open);
    float fb = 0.62f + 0.30f * size01, d = 0.15f + 0.75f * damp01;
    // farther sounds carry more reverb relative to the direct sound
    float wet = val(kReverbAmount) * (1.0f - open * (1.0f - val(kReverbOutdoor))) * 0.35f * (1.0f + 0.8f * (1.0f - std::min(1.0f, std::max(0.0f, directness))));
    for (int n = 0; n < kBlock; ++n) {
        float x = in[n] * 0.05f, c[4];
        for (int i = 0; i < 4; ++i) {
            float y = r.comb[i][r.cpos[i]];
            r.damp[i] = y * (1.0f - d) + r.damp[i] * d;
            r.comb[i][r.cpos[i]] = x + r.damp[i] * fb;
            if (++r.cpos[i] >= r.clen[i]) r.cpos[i] = 0;
            c[i] = y;
        }
        float w[2] = {c[0] - c[1] + c[2] - c[3], c[0] + c[1] - c[2] - c[3]};
        for (int ch = 0; ch < 2; ++ch) {
            float* b = r.ap[ch][0];
            int len = kApBase[0], p = r.apos[0];
            float z = b[p], o = -w[ch] + z;
            b[p] = w[ch] + z * 0.5f;
            if (++p >= len) p = 0;
            if (ch == 1) r.apos[0] = p;
            w[ch] = o;
        }
        left[n] += w[0] * wet * 4.0f;
        right[n] += w[1] * wet * 4.0f;
    }
}
std::atomic<uint32_t> g_hrtfBlockCount{0}, g_hrtfRendered{0}, g_hrtfSkipped{0}, g_hrtfLastOut{0}, g_hrtfLastIn{0},
    g_hrtfSteam{0};

void resetVoice(HrtfVoice& v) {
    if (IPLBinauralEffect fx = v.fx.load()) g_sa.effectReset(fx);
    v.fresh = true;
    std::memset(v.hist, 0, sizeof(v.hist));
    v.lpL = v.lpR = v.elvLp = v.occLp = 0, v.occG = 1;
    if (v.rev) { delete v.rev; v.rev = nullptr; }
}

HrtfVoice* voiceFor(uint64_t key) {
    uint32_t h = static_cast<uint32_t>((key >> 4) * 2654435761u);
    HrtfVoice* oldest = nullptr;
    for (int i = 0; i < 8; ++i) {
        HrtfVoice& v = g_hrtf[(h + i) % kHrtfVoices];
        uint64_t k = v.key.load(std::memory_order_relaxed);
        if (k == key) return &v;
        uint64_t zero = 0;
        if (k == 0 && v.key.compare_exchange_strong(zero, key)) {
            resetVoice(v);
            return &v;
        }
        if (!oldest || v.last < oldest->last) oldest = &v;
    }
    oldest->key.store(key);  // the table is crowded here: the voice unused the longest gives way
    resetVoice(*oldest);
    return oldest;
}

float onePole(float cutoff, float fs) { return cutoff >= 18000.0f ? 1.0f : 1.0f - std::exp(-6.2831853f * cutoff / fs); }

// in: 256 mono samples; out: 256 left + 256 right samples (separate arrays).
// How much of the binaural effect a sound gets. The game's "directness" falls below 1 for sounds closer than their
// near radius (it spreads them to the middle of the head); with hrtf_close on, only sounds practically inside the
// head (under ~5 cm) are spread, so a Sim 30 cm away still plays from where they are.
float spatialAmount(float directness) {
    float d = std::min(1.0f, std::max(0.0f, directness));
    if (val(kHrtfClose) != 0) d = std::min(1.0f, std::max(0.0f, (d - 0.05f) / 0.2f));
    return d;
}

void renderBinaural(HrtfVoice& v, const float* in, float* left, float* right, float angleDeg, float elevDeg, float directness) {
    constexpr float kFs = 48000.0f, kHeadRadius = 0.0875f, kSpeed = 343.0f, kBase = 0.75f;
    float th = angleDeg * 0.017453292f;  // the game's angle grows towards the right (tested in game 2026-11-04)
    if (val(kHrtfFlip) != 0) th = -th;
    float s = spatialAmount(directness) * val(kHrtfStrength);
    float sinT = std::sin(th), cosT = std::cos(th);
    float lat = sinT * s;  // + = the sound is on the right
    float aLat = std::fabs(lat);
    float rear = std::max(0.0f, -cosT) * s * val(kHrtfRear);
    float phi = std::fabs(std::asin(std::min(1.0f, std::max(-1.0f, sinT))));
    float itd = (kHeadRadius / kSpeed) * (phi + std::sin(phi)) * kFs * s;  // samples, at most ~32
    float dL = lat > 0 ? itd : 0, dR = lat < 0 ? itd : 0;
    float nearGain = std::pow(10.0f, 1.5f * aLat / 20.0f), farGain = std::pow(10.0f, -5.0f * aLat / 20.0f);
    float rearGain = std::pow(10.0f, -2.0f * rear / 20.0f);
    float gL = kBase * rearGain * (lat > 0 ? farGain : nearGain), gR = kBase * rearGain * (lat > 0 ? nearGain : farGain);
    float farCut = std::exp(std::log(20000.0f) + (std::log(2000.0f) - std::log(20000.0f)) * aLat);
    float rearCut = std::exp(std::log(20000.0f) + (std::log(4500.0f) - std::log(20000.0f)) * std::min(1.0f, rear));
    float cL = std::min(lat > 0 ? farCut : 20000.0f, rearCut), cR = std::min(lat > 0 ? 20000.0f : farCut, rearCut);
    float aL = onePole(cL, kFs), aR = onePole(cR, kFs);
    if (v.fresh) {
        v.gL = gL, v.gR = gR, v.dL = dL, v.dR = dR, v.aL = aL, v.aR = aR;
        v.fresh = false;
    }
    // Height without a measured HRTF: a sound above you gains treble, one below you loses it (the main spectral cue
    // the ear has for elevation), the more the further up or down it is.
    float fin[kBlock];
    {
        const float se = std::sin(elevDeg * 0.017453292f);
        const float k = (se > 0 ? 0.9f * se : 0.6f * se) * s;
        const float a = onePole(5000.0f, kFs);
        float lp = v.elvLp;
        for (int n = 0; n < kBlock; ++n) {
            lp += a * (in[n] - lp);
            fin[n] = in[n] + k * (in[n] - lp);
        }
        v.elvLp = std::isfinite(lp) ? lp : 0.0f;
    }
    float ext[kHist + kBlock + 1];
    std::memcpy(ext, v.hist, sizeof(v.hist));
    std::memcpy(ext + kHist, fin, kBlock * sizeof(float));
    ext[kHist + kBlock] = 0;
    auto tap = [&](int n, float d) {
        float pos = static_cast<float>(kHist + n) - d;
        int i0 = static_cast<int>(pos);
        float fr = pos - static_cast<float>(i0);
        return fr > 0 ? ext[i0] + (ext[i0 + 1] - ext[i0]) * fr : ext[i0];
    };
    float lpL = v.lpL, lpR = v.lpR;
    for (int n = 0; n < kBlock; ++n) {
        float t = static_cast<float>(n + 1) / kBlock;
        float xl = tap(n, v.dL + (dL - v.dL) * t), xr = tap(n, v.dR + (dR - v.dR) * t);
        lpL += (v.aL + (aL - v.aL) * t) * (xl - lpL);
        lpR += (v.aR + (aR - v.aR) * t) * (xr - lpR);
        float l = lpL * (v.gL + (gL - v.gL) * t), r = lpR * (v.gR + (gR - v.gR) * t);
        left[n] = std::isfinite(l) ? l : 0.0f;
        right[n] = std::isfinite(r) ? r : 0.0f;
    }
    v.lpL = std::isfinite(lpL) ? lpL : 0.0f;
    v.lpR = std::isfinite(lpR) ? lpR : 0.0f;
    std::memcpy(v.hist, ext + kBlock, sizeof(v.hist));
    v.gL = gL, v.gR = gR, v.dL = dL, v.dR = dR, v.aL = aL, v.aR = aR;
    v.last = g_hrtfBlockCount.fetch_add(1, std::memory_order_relaxed);
}

// Height: the panner only knows a horizontal angle. The VR mod's pan step (it sees the sound's position and the
// listener's frame) tells us each panner's elevation in degrees through esc_set_elevation (+ up, - down); a value
// not refreshed for 2 s is taken as 0 (level with you).
struct ElevSlot {
    std::atomic<uint64_t> key{0};
    std::atomic<float> degrees{0};
    std::atomic<uint32_t> tick{0};
    std::atomic<float> occl{0};       // 0 clear .. 1 fully behind walls
    std::atomic<uint32_t> occlTick{0};
};
constexpr int kElevSlots = 512;
ElevSlot g_elev[kElevSlots];
std::atomic<uint32_t> g_elevUpdates{0};
std::atomic<float> g_elevLast{0};
std::atomic<uint32_t> g_skipIn{0}, g_skipOut{0};

ElevSlot* slotFor(uint64_t key);
void setOcclusion(const void* panner, float amount) {
    if (!panner || !std::isfinite(amount)) return;
    ElevSlot* s = slotFor(reinterpret_cast<uint64_t>(panner));
    s->occl.store(std::min(1.0f, std::max(0.0f, amount)), std::memory_order_relaxed);
    s->occlTick.store(GetTickCount(), std::memory_order_relaxed);
}

ElevSlot* slotFor(uint64_t key) {
    uint32_t h = static_cast<uint32_t>((key >> 4) * 2654435761u);
    ElevSlot* use = &g_elev[h % kElevSlots];
    for (int i = 0; i < 8; ++i) {
        ElevSlot& s = g_elev[(h + i) % kElevSlots];
        uint64_t k = s.key.load(std::memory_order_relaxed), zero = 0;
        if (k == key || (k == 0 && s.key.compare_exchange_strong(zero, key))) {
            use = &s;
            break;
        }
        // a slot nobody has refreshed for 5 s belongs to a sound that is gone: take it over (before, dead sounds
        // kept their slots for ever, the table filled up and live sounds shared a slot, so one sound could get
        // another's height or muffling)
        const uint32_t now = GetTickCount();
        if (now - s.tick.load(std::memory_order_relaxed) > 5000 && now - s.occlTick.load(std::memory_order_relaxed) > 5000 &&
            s.key.compare_exchange_strong(k, key)) {
            s.occl.store(0, std::memory_order_relaxed);
            s.degrees.store(0, std::memory_order_relaxed);
            use = &s;
            break;
        }
    }
    use->key.store(key, std::memory_order_relaxed);
    return use;
}

float occlusionFor(uint64_t key) {
    uint32_t h = static_cast<uint32_t>((key >> 4) * 2654435761u);
    for (int i = 0; i < 8; ++i) {
        const ElevSlot& s = g_elev[(h + i) % kElevSlots];
        if (s.key.load(std::memory_order_relaxed) != key) continue;
        return GetTickCount() - s.occlTick.load(std::memory_order_relaxed) < 2000 ? s.occl.load(std::memory_order_relaxed) : 0.0f;
    }
    return 0.0f;
}

void setElevation(const void* panner, float degrees) {
    if (!panner || !std::isfinite(degrees)) return;
    uint64_t key = reinterpret_cast<uint64_t>(panner);
    ElevSlot* use = slotFor(key);
    use->degrees.store(std::min(90.0f, std::max(-90.0f, degrees)), std::memory_order_relaxed);
    use->tick.store(GetTickCount(), std::memory_order_relaxed);
    g_elevUpdates.fetch_add(1, std::memory_order_relaxed);
    g_elevLast.store(degrees, std::memory_order_relaxed);
}

float elevationFor(uint64_t key) {
    uint32_t h = static_cast<uint32_t>((key >> 4) * 2654435761u);
    for (int i = 0; i < 8; ++i) {
        const ElevSlot& s = g_elev[(h + i) % kElevSlots];
        if (s.key.load(std::memory_order_relaxed) != key) continue;
        return GetTickCount() - s.tick.load(std::memory_order_relaxed) < 2000 ? s.degrees.load(std::memory_order_relaxed) : 0.0f;
    }
    return 0.0f;
}

// Steam Audio render of one block; false = not available for this voice (the built-in model takes it).
bool renderSteam(HrtfVoice& v, const float* in, float* left, float* right, float angleDeg, float elevDeg, float directness) {
    IPLBinauralEffect fx = v.fx.load(std::memory_order_relaxed);
    if (!fx || !g_saReady.load(std::memory_order_relaxed)) return false;
    float az = angleDeg * 0.017453292f, el = elevDeg * 0.017453292f;  // az grows towards the right
    if (val(kHrtfFlip) != 0) az = -az;
    IPLBinauralEffectParams params{};
    params.direction = {std::sin(az) * std::cos(el), std::sin(el), -std::cos(az) * std::cos(el)};
    params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
    params.spatialBlend = spatialAmount(directness) * val(kHrtfStrength);
    params.hrtf = g_sa.hrtf;
    float* inCh[1] = {const_cast<float*>(in)};
    float* outCh[2] = {left, right};
    IPLAudioBuffer inBuf{1, kBlock, inCh}, outBuf{2, kBlock, outCh};
    g_sa.effectApply(fx, &params, &inBuf, &outBuf);
    for (int n = 0; n < kBlock; ++n) {
        left[n] = std::isfinite(left[n]) ? left[n] * 0.8f : 0.0f;
        right[n] = std::isfinite(right[n]) ? right[n] * 0.8f : 0.0f;
    }
    v.last = g_hrtfBlockCount.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void startSteamAudio() {
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        ULONGLONG t0 = GetTickCount64();
        std::wstring path = g_folder + L"phonon.dll";
        HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m) {
            LOG("Steam Audio: phonon.dll is not next to the mod or cannot be loaded (error %lu): the built-in head model is used",
                GetLastError());
            return 0;
        }
        g_sa.contextCreate = reinterpret_cast<decltype(g_sa.contextCreate)>(GetProcAddress(m, "iplContextCreate"));
        g_sa.hrtfCreate = reinterpret_cast<decltype(g_sa.hrtfCreate)>(GetProcAddress(m, "iplHRTFCreate"));
        g_sa.effectCreate = reinterpret_cast<decltype(g_sa.effectCreate)>(GetProcAddress(m, "iplBinauralEffectCreate"));
        g_sa.effectApply = reinterpret_cast<decltype(g_sa.effectApply)>(GetProcAddress(m, "iplBinauralEffectApply"));
        g_sa.effectReset = reinterpret_cast<decltype(g_sa.effectReset)>(GetProcAddress(m, "iplBinauralEffectReset"));
        if (!g_sa.contextCreate || !g_sa.hrtfCreate || !g_sa.effectCreate || !g_sa.effectApply || !g_sa.effectReset) {
            LOG("Steam Audio: phonon.dll lacks expected functions (not the 4.8 API?): the built-in head model is used");
            return 0;
        }
        IPLContextSettings cs{};
        cs.version = STEAMAUDIO_VERSION;
        cs.simdLevel = IPL_SIMDLEVEL_AVX2;
        IPLContext context = nullptr;
        IPLerror err = g_sa.contextCreate(&cs, &context);
        if (err != IPL_STATUS_SUCCESS || !context) {
            LOG("Steam Audio: context creation failed (status %d): the built-in head model is used", static_cast<int>(err));
            return 0;
        }
        g_sa.context = context;
        // hrtf.sofa next to the mod replaces the built-in HRTF (a SOFA file in the SimpleFreeFieldHRIR convention).
        std::wstring sofaPath = g_folder + L"hrtf.sofa";
        std::string sofa;
        if (GetFileAttributesW(sofaPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            int n = WideCharToMultiByte(CP_UTF8, 0, sofaPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
            sofa.resize(n > 0 ? n - 1 : 0);
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, sofaPath.c_str(), -1, &sofa[0], n, nullptr, nullptr);
        }
        IPLHRTFSettings hs{};
        hs.volume = 1.0f;
        hs.normType = IPL_HRTFNORMTYPE_RMS;
        IPLHRTF hrtf = nullptr;
        if (!sofa.empty()) {
            hs.type = IPL_HRTFTYPE_SOFA;
            hs.sofaFileName = sofa.c_str();
            if (g_sa.hrtfCreate(context, &g_sa.audio, &hs, &hrtf) != IPL_STATUS_SUCCESS) {
                LOG("Steam Audio: hrtf.sofa could not be loaded: the built-in HRTF is used");
                hrtf = nullptr;
            }
        }
        if (!hrtf) {
            hs.type = IPL_HRTFTYPE_DEFAULT;
            hs.sofaFileName = nullptr;
            if (g_sa.hrtfCreate(context, &g_sa.audio, &hs, &hrtf) != IPL_STATUS_SUCCESS || !hrtf) {
                LOG("Steam Audio: HRTF creation failed: the built-in head model is used");
                return 0;
            }
        }
        g_sa.hrtf = hrtf;
        int made = 0;
        IPLBinauralEffectSettings es{hrtf};
        for (HrtfVoice& v : g_hrtf) {
            IPLBinauralEffect fx = nullptr;
            if (g_sa.effectCreate(context, &g_sa.audio, &es, &fx) == IPL_STATUS_SUCCESS && fx) {
                v.fx.store(fx);
                ++made;
            }
        }
        g_saReady = made > 0;
        LOG("Steam Audio %d.%d.%d: %s HRTF, %d of %d voice effects ready in %llu ms", STEAMAUDIO_VERSION_MAJOR,
            STEAMAUDIO_VERSION_MINOR, STEAMAUDIO_VERSION_PATCH, hs.type == IPL_HRTFTYPE_SOFA ? "hrtf.sofa" : "built-in", made,
            kHrtfVoices, static_cast<unsigned long long>(GetTickCount64() - t0));
        return 0;
    }, nullptr, 0, nullptr);
}

uint64_t panProcess(uint8_t* p, void* ctx, char flag) {
    g_panCalls.fetch_add(1, std::memory_order_relaxed);
    uint8_t *inBuf = nullptr, *outBuf = nullptr;
    float input[kBlock];
    float angle = 0, directness = 0;
    uint32_t outChannels = 0;
    if (val(kHrtf) != 0 && val(kHrtfStrength) > 0) {
        // +0x324 = the panner's input channels (1 mono source, 2 stereo source), +0x328 = its output channels: the
        // voice buses are 8-channel (1.129 log: "in 1/2 out 8"), the game folds them to the output mode afterwards.
        uint32_t in = *reinterpret_cast<uint32_t*>(p + 0x324), out = *reinterpret_cast<uint32_t*>(p + 0x328);
        g_hrtfLastOut = out, g_hrtfLastIn = in;
        auto* c = static_cast<uint8_t*>(ctx);
        uint8_t* ib = *reinterpret_cast<uint8_t**>(c + 0xc490);
        uint8_t* ob = *reinterpret_cast<uint8_t**>(c + 0xc498);
        if ((in == 1 || (in >= 2 && in <= 8 && val(kHrtfStereo) != 0)) && out >= 2 && out <= 8 && ib && ob) {
            auto* id = *reinterpret_cast<float**>(ib + 8);
            auto* od = *reinterpret_cast<float**>(ob + 8);
            uint16_t os = *reinterpret_cast<uint16_t*>(ob + 0x12);
            if (id && od && os >= kBlock) {
                if (in >= 2) {
                    // every channel except a lone LFE (8-channel layouts keep it last) is mixed into the one source
                    uint16_t is = *reinterpret_cast<uint16_t*>(ib + 0x12);
                    uint32_t used = in == 8 ? 7 : in;
                    if (is < kBlock) id = nullptr;
                    else for (int n = 0; n < kBlock; ++n) {
                        float sum = 0;
                        for (uint32_t k = 0; k < used; ++k) sum += id[static_cast<size_t>(is) * k + n];
                        input[n] = sum / static_cast<float>(used);
                    }
                } else
                    std::memcpy(input, id, sizeof(input));
            }
            if (id && od && os >= kBlock) {
                angle = *reinterpret_cast<float*>(p + 0x60);
                directness = *reinterpret_cast<float*>(p + 0x68);
                inBuf = ib, outBuf = ob;
                outChannels = out;
            }
        }
        if (!outBuf) {
            g_hrtfSkipped.fetch_add(1, std::memory_order_relaxed);
            g_skipIn = in, g_skipOut = out;  // what the last left-over panner looked like (shown in the log)
        }
    }
    uint64_t result = reinterpret_cast<PanFn>(g_panProcess)(p, ctx, flag);
    if (outBuf) {
        auto* od = *reinterpret_cast<float**>(outBuf + 8);
        uint16_t os = *reinterpret_cast<uint16_t*>(outBuf + 0x12);
        // The binaural pair goes to the front left and front right of the bus (the game's speaker order: 8/6
        // channels FL, C, FR, ...; 4 and 2 channels FL, FR) and the rest is silent, so the game's fold-down to
        // Stereo passes it through as it is.
        const uint32_t rightIndex = outChannels >= 6 ? 2 : 1;
        float* left = od;
        float* right = od + static_cast<size_t>(os) * rightIndex;
        HrtfVoice& voice = *voiceFor(reinterpret_cast<uint64_t>(p));
        if (val(kMuffle) != 0 && val(kMuffleAmount) > 0) {
            // Occlusion comes from the VR mod's pan step (it knows the walls between the sound and you): a one-pole
            // low-pass whose cutoff falls from open to muffle_treble, plus a level drop, eased over the block.
            float occ = occlusionFor(reinterpret_cast<uint64_t>(p)) * val(kMuffleAmount);
            if (occ > 0.001f || voice.occG < 0.999f) {
                float cutoff = 16000.0f * std::pow(val(kMuffleTreble) / 16000.0f, occ);
                float a = 1.0f - std::exp(-6.2831853f * cutoff / 48000.0f);
                float g1 = 1.0f - val(kMuffleLoss) * occ, g0 = voice.occG;
                for (int n = 0; n < kBlock; ++n) {
                    float g = g0 + (g1 - g0) * (static_cast<float>(n) / kBlock);
                    voice.occLp += a * (input[n] - voice.occLp);
                    input[n] = voice.occLp * g;
                }
                voice.occG = g1;
            } else voice.occLp = input[kBlock - 1];
        }
        if (val(kHrtfSA) != 0 &&
            renderSteam(voice, input, left, right, angle, elevationFor(reinterpret_cast<uint64_t>(p)), directness))
            g_hrtfSteam.fetch_add(1, std::memory_order_relaxed);
        else
            renderBinaural(voice, input, left, right, angle, elevationFor(reinterpret_cast<uint64_t>(p)), directness);
        if (val(kReverb) != 0 && val(kReverbAmount) > 0) addReverb(voice, input, left, right, directness);
        for (uint32_t c = 1; c < outChannels; ++c)
            if (c != rightIndex) std::memset(od + static_cast<size_t>(os) * c, 0, kBlock * sizeof(float));
        g_hrtfRendered.fetch_add(1, std::memory_order_relaxed);
    }
    (void)inBuf;
    if (val(kPanLog) != 0) {
        ULONGLONG now = GetTickCount64(), next = g_panNext.load(std::memory_order_relaxed);
        if (now >= next && g_panNext.compare_exchange_strong(next, now + 250)) samplePanner(p, static_cast<uint8_t*>(ctx), flag);
    }
    return result;
}

void startPanProbe() {
    if (!g_panSlot) {
        LOG("Pan probe: the panner's process slot was not found in this build; off");
        return;
    }
    bool ok = swapSlot(g_panSlot, g_panFn, reinterpret_cast<void*>(&panProcess), g_panProcess);
    LOG("Pan probe: Pan2D1 process (slot 0x%x, function 0x%x) %s; %s", g_panSlot, g_panFn, ok ? "wrapped" : "NOT wrapped",
        val(kPanLog) != 0 ? "logging" : "log off");
    if (!ok) return;
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        int lines = 0;
        for (;;) {
            Sleep(10000);
            uint32_t rendered = g_hrtfRendered.exchange(0), skipped = g_hrtfSkipped.exchange(0), steam = g_hrtfSteam.exchange(0);
            if (val(kHrtf) != 0 && (rendered || skipped) && lines < 60)
                LOG("3D sound: %u panner blocks made binaural (%u by Steam Audio), %u left to the game (last seen: %u in, %u out channels); "
                    "height from the VR mod: %u updates (last %.1f deg); last left over: %u in, %u out%s",
                    rendered, steam, skipped, g_hrtfLastIn.load(), g_hrtfLastOut.load(),
                    g_elevUpdates.exchange(0), g_elevLast.load(), g_skipIn.load(), g_skipOut.load(),
                    !rendered && skipped ? " - only mono sources are made binaural; stereo sources keep the game's pan" : "");
            uint32_t calls = g_panCalls.exchange(0), n = std::min<uint32_t>(g_panTaken.load(std::memory_order_acquire), kPanRing);
            if (val(kPanLog) == 0 || !calls) {
                g_panTaken.store(0);
                continue;
            }
            if (lines < 40) {
                ++lines;
                LOG("Pan probe: %u panner runs in 10 s, %u sampled", calls, n);
                for (uint32_t i = 0; i < n && i < 12; ++i) {
                    const PanSample& s = g_panRing[i];
                    LOG("Pan probe:   panner 0x%llx in %u out %u angle %.3f distance %.3f third %.3f stride %u peak %.4f flag %u",
                        static_cast<unsigned long long>(s.panner), s.in, s.out, s.angle, s.distance, s.third, s.stride, s.peak,
                        s.flag);
                }
            }
            g_panTaken.store(0);
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

// set_room(volume_m3, absorption): the script's own estimate of the room around the listener.
PyObject* pySetRoom(PyObject*, PyObject* args) {
    double vol = 0, ab = 0;
    if (!py.PyArg_ParseTuple(args, "dd", &vol, &ab)) return nullptr;
    if (std::isfinite(vol) && std::isfinite(ab)) {
        g_pyRoomVolume.store(static_cast<float>(vol));
        g_pyRoomAbsorb.store(static_cast<float>(ab));
        g_pyRoomTick.store(GetTickCount());
    }
    return none();
}

PyMethodDef methods[] = {
    {"set_room", pySetRoom, METH_VARARGS, "set_room(volume_m3, absorption 0..1): the room around the listener"},
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
        startPanProbe();
        startSteamAudio();
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
// For the VR mod's pan step: a panner's elevation in degrees (+ up, - down) for Steam Audio's height cue.
// The room the listener is in: volume in cubic metres, absorption 0 (bare, hard) .. 1 (carpets, furniture, curtains).
// Refresh it at least every 2 s, otherwise the menu's size and damping apply.
extern "C" __declspec(dllexport) void esc_set_room(float volumeM3, float absorption) {
    if (!std::isfinite(volumeM3) || !std::isfinite(absorption)) return;
    g_roomVolume.store(volumeM3, std::memory_order_relaxed);
    g_roomAbsorb.store(absorption, std::memory_order_relaxed);
    g_roomTick.store(GetTickCount(), std::memory_order_relaxed);
}
// How much of the sound between this panner and the listener is walls, floors or closed doors: 0 clear .. 1 fully
// blocked. Refresh at least every 2 s, otherwise 0 applies.
extern "C" __declspec(dllexport) void esc_set_occlusion(const void* panner, float amount) { setOcclusion(panner, amount); }
extern "C" __declspec(dllexport) void esc_set_elevation(const void* panner, float degrees) { setElevation(panner, degrees); }
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
