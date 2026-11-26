// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "vagrants.h"

#include "game.h"
#include "hook.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace coop::vagrants {

namespace {

// How the game runs them (PTDE, 2026-11-25; the developers' debug menu still names each value, 0x11D97A4..):
//  * Made in the world of the player who loses something, in their own world ([0x13784A0]+0xB4F): a bloodstain with five or
//    more humanity that is lost (0xD78B81 -> 0xBD3480: the "humanity vagrant", goods 350), and each item left lying
//    when its drop goes away whose parameters allow it (0xDC5F2A -> 0xBD3160). Its wander count starts at 1 and it
//    leaves at once.
//  * A list in every game (FrpgNetItemVagrantDbIf, [[0x137E204]+0xB54]: +0x24 the list's sentinel, +0x28 its count;
//    +0x10 the linked games it can pass to, each with a budget at +0x40). One that came from another game stays for
//    "item re-send time" (0x12DE224, 300 s; its countdown at item+0x24), then the network tick (0xBD3F90, from
//    0xBF1B2C) passes the list's first to one linked game with its count raised by one (0xBD40FA) and deletes its own
//    copy: one vagrant, never a copy. A budget allows one every 120 s (0x12DE234, at most 1: 0x12DE238). The list is
//    memory only: a game that closes holding one ends it.
//  * When a map loads (0xDDE8D2 -> 0xBD41A0 with the map's index), each vagrant from another game (item+0x20 bit 30)
//    lost in that map ((item+0x14 >> 20) & 0xFF) may appear at one of the map's vagrant places: an item once its
//    count is 10 ("item enemy A", 0x12DE228) or 20 ("B", 0x12DE22C); humanity (bit 31) from [0x1378708] (0) or 20
//    (0x12DE230). An item below its count is put on the ground in that world instead (up to 8 a load: 0xDC9140 ->
//    0xBD2660 -> 0xDC7F70), for the player to pick up.
// VagrantWanders sets the item's first count (its second, and humanity's, at twice it: the game's 10 and 20), and
// VagrantStay the 300 s. VagrantPassOnAtQuit runs the hand-off before the game leaves its links: every vagrant's
// countdown to 0 and every linked game's budget up, at Quit Game (or a network error's return to the title), and
// for a close of the window (held back up to 3 s).
constexpr uint32_t kItemA = 0x012DE228;
constexpr uint32_t kItemB = 0x012DE22C;
constexpr uint32_t kHumanityB = 0x012DE230;
constexpr uint32_t kStay = 0x012DE224;
constexpr uint32_t kNetMan = 0x0137E204;
constexpr uint32_t kDbOffset = 0xB54;
constexpr uint32_t kReturnState = 0x013784A4;  // [[it]] != 0: a return to the title is asked for (world.cpp)
// Quit Game's OK: the menu's request ([MenuMan+0x60]) is 21 until the title (some seconds while the game leaves its
// online session; the return state above stays 0 for it, 2026-11-25).
constexpr uint32_t kMenuManVar = 0x013786D0;
constexpr uint32_t kQuitRequest = 21;
constexpr uint32_t kCheckCall = 0x00DDE8D2;
constexpr uint32_t kCheck = 0x00BD41A0;
constexpr uint32_t kAdd = 0x00BD2CC0;          // stdcall(db, data[8], from_another_game): add to the list
constexpr uint32_t kSendCall = 0x00BD4136;     // the hand-off: stdcall 0xBD29A0(linked game, data[8]), ret 8
constexpr uint32_t kSend = 0x00BD29A0;
constexpr uint32_t kHumanityItem = 0x4000015E; // goods 350: what the game's humanity vagrant carries
constexpr uint32_t kTestItem = 0x40000172;     // goods 370, the Prism Stone
constexpr double kPassWait = 4.0;              // a hand-off at most this long
constexpr UINT_PTR kCloseTimer = 0x56A9;
constexpr UINT kCloseWaitMs = 3000;

Settings g_s;
bool g_installed = false;
uint32_t g_check = kCheck;

double now() { return GetTickCount64() / 1000.0; }

// Each vagrant this game passes on (the network tick's hand-off), counted and in the log.
volatile LONG g_sent = 0;
int g_sent_lines = 0;

void __stdcall hk_send(uint32_t target, uint32_t data) {
    InterlockedIncrement(&g_sent);
    uint32_t map = 0, bits = 0;
    if (g_s.log && g_sent_lines < 200 && data && read32(data + 0x10, &map) && read32(data + 0x1C, &bits)) {
        ++g_sent_lines;
        LOG_INFO("[net] Vagrants: one passed on to a linked game (lost in map index %u, through %u game(s) with this "
                 "one)", (map >> 20) & 0xFF, (bits >> 20) & 0x3FF);
    }
    reinterpret_cast<void(__stdcall*)(uint32_t, uint32_t)>(kSend)(target, data);
}

bool put32(uint32_t addr, uint32_t v) {
    __try {
        *reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(addr)) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t db() {
    uint32_t nm = 0, d = 0;
    return read32(kNetMan, &nm) && nm && read32(nm + kDbOffset, &d) ? d : 0;
}

// A list kept by its sentinel node ([owner+offset]): each node +0 the next, +8 its element.
template <class F>
int each(uint32_t owner, uint32_t offset, F fn) {
    uint32_t sentinel = 0, node = 0;
    if (!owner || !read32(owner + offset, &sentinel) || !sentinel || !read32(sentinel, &node)) return -1;
    int n = 0;
    while (node && node != sentinel && n < 4096) {
        uint32_t e = 0;
        if (read32(node + 8, &e) && e) fn(e);
        ++n;
        if (!read32(node, &node)) break;
    }
    return n;
}

uint32_t return_state() {
    uint32_t p = 0, v = 0;
    return read32(kReturnState, &p) && p && read32(p, &v) ? v : 0;
}

bool quitting() {
    uint32_t mm = 0, request = 0;
    return read32(kMenuManVar, &mm) && mm >= 0x10000 && read32(mm + 0x60, &request) && request == kQuitRequest;
}

// ---------------------------------------------------------------- the hand-off before leaving
struct Pass {
    bool on = false;
    double start = 0;
    double emptied = 0;
    int count = 0;
    LONG sent = 0;  // g_sent when it began
    const char* why = "";
} g_pass;

HWND g_window = nullptr;
WNDPROC g_prev = nullptr;
bool g_close_waits = false;  // a close held back for the hand-off
bool g_close_go = false;     // ours: the close goes through

void push_frame(uint32_t d) {
    uint32_t count = 0;
    read32(d + 0x28, &count);
    each(d, 0x10, [&](uint32_t t) {
        uint32_t budget = 0;
        if (read32(t + 0x40, &budget) && budget < count) put32(t + 0x40, count);
    });
    const uint32_t zero = 0;  // 0.0f
    each(d, 0x24, [&](uint32_t item) { put32(item + 0x24, zero); });
}

// Starts a hand-off of what we hold; false when there is nothing to pass on or nobody to take it.
bool begin(const char* why) {
    if (g_pass.on) return true;
    const uint32_t d = db();
    const int count = held(), linked = targets();
    if (!d || count <= 0) return false;
    if (linked <= 0) {
        LOG_INFO("[net] Vagrants: %d held before %s, and no linked game to pass them to: they end here", count, why);
        return false;
    }
    g_pass = Pass{true, now(), 0, count, g_sent, why};
    LOG_INFO("[net] Vagrants: passing the %d this game holds to its %d linked game(s) before %s (VagrantPassOnAtQuit)",
             count, linked, why);
    push_frame(d);
    return true;
}

void close_now() {
    if (!g_close_waits || !g_window) return;
    g_close_waits = false;
    g_close_go = true;
    KillTimer(g_window, kCloseTimer);
    PostMessageW(g_window, WM_CLOSE, 0, 0);
}

void pass_step() {
    if (!g_pass.on) return;
    const uint32_t d = db();
    const int left = d ? held() : -1;
    const double t = now();
    if (left > 0 && targets() > 0 && t - g_pass.start < kPassWait) {
        push_frame(d);
        return;
    }
    // A moment for the network to send the last of them.
    if (left <= 0 && g_pass.emptied == 0) g_pass.emptied = t;
    if (left <= 0 && t - g_pass.emptied < 0.5 && t - g_pass.start < kPassWait) return;
    g_pass.on = false;
    const LONG passed = g_sent - g_pass.sent;
    LOG_INFO("[net] Vagrants: %ld of %d passed on before %s%s", passed, g_pass.count, g_pass.why,
             left > 0 ? " (the rest end with this game's links)" : "");
    close_now();
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_CLOSE && !g_close_go && !g_close_waits && g_s.pass_on_at_quit && game::player() &&
        begin("the game closing")) {
        g_close_waits = true;
        SetTimer(window, kCloseTimer, kCloseWaitMs, nullptr);
        return 0;
    }
    if (message == WM_TIMER && wp == kCloseTimer) {
        close_now();
        return 0;
    }
    return CallWindowProcW(g_prev, window, message, wp, lp);
}

struct WindowSearch {
    DWORD pid;
    HWND best;
    long area;
};

BOOL CALLBACK find_window(HWND window, LPARAM parameter) {
    auto* s = reinterpret_cast<WindowSearch*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != s->pid || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) return TRUE;
    RECT r{};
    if (!GetWindowRect(window, &r)) return TRUE;
    const long area = (r.right - r.left) * (r.bottom - r.top);
    if (area > s->area) {
        s->area = area;
        s->best = window;
    }
    return TRUE;
}

// The game's window, once a character is loaded (made long before): its closes come to window_proc first.
void watch_window() {
    static bool tried = false;
    if (tried || !g_s.pass_on_at_quit || !game::player()) return;
    tried = true;
    WindowSearch s{GetCurrentProcessId(), nullptr, 0};
    EnumWindows(find_window, reinterpret_cast<LPARAM>(&s));
    if (!s.best) {
        LOG_INFO("[net] Vagrants: the game window was not found; a close does not wait for the hand-off");
        return;
    }
    g_window = s.best;
    g_prev = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_proc)));
    if (!g_prev) {
        g_window = nullptr;
        LOG_ERROR("[net] Vagrants: could not watch the game window's close (error %lu)", GetLastError());
    }
}

// ---------------------------------------------------------------- the map load's check, in the log
struct Look {
    int held = 0;    // the whole list
    int here = 0;    // from another game, lost in this map
    int ready = 0;   // of them, far enough to come as a vagrant
    int minc = 0, maxc = 0;
};

Look look(int map_index) {
    Look l;
    const uint32_t d = db();
    uint32_t a = 10, b = 20, hb = 20;
    read32(kItemA, &a);
    read32(kItemB, &b);
    read32(kHumanityB, &hb);
    each(d, 0x24, [&](uint32_t item) {
        ++l.held;
        uint32_t map = 0, bits = 0;
        if (!read32(item + 0x14, &map) || !read32(item + 0x20, &bits)) return;
        if (!(bits & 0x40000000) || static_cast<int>((map >> 20) & 0xFF) != map_index) return;
        const int c = static_cast<int>((bits >> 20) & 0x3FF);
        const bool humanity = (bits & 0x80000000) != 0;
        const uint32_t need = humanity ? 0 : (std::min)(a, b);
        l.minc = l.here ? (std::min)(l.minc, c) : c;
        l.maxc = l.here ? (std::max)(l.maxc, c) : c;
        ++l.here;
        if (static_cast<uint32_t>(c) >= need) ++l.ready;
        (void)hb;
    });
    return l;
}

// ---------------------------------------------------------------- Firelink Shrine's vagrant spots (the user's, 2026-11-25)
// Where a vagrant appears: each floor collision of a map names three vagrant spots, the entities of the vagrant
// enemies standing ready in that map (MSB1 Collision type data +0x18 items (c3490), +0x1C and +0x20 humanity (c3491));
// a vagrant lost on that floor takes the spot its kind and wander count pick (0xBD44BB: 0xDD1310 the floor's record,
// 0xDD6830/6870/68B0 the spot's entity) when that spot is in the list of the vagrant enemies made (db+0x3C), and the
// enemy goes to where the vagrant was lost. Every map has spots but Firelink Shrine (m10_02): no c3490/c3491 there and
// every collision's three entities -1, so a vagrant lost there never came. Its map (the MSB, read by 0xD24460 from a
// copy of the file: ecx = its size, the second argument the file's bytes) now gets three spots of the Undead Burg's
// kind (vagrant_spots.inc) at the places the user chose, on the collisions under them, and every collision names one
// (the shrine's by default). The parts of each kind lie together in an MSB (the loader keeps each kind's first index
// and count, 0xD24BBB), so the two models go after its enemy models and the nine parts after its enemies, and every
// index past them is moved on: each event's part (base +0), a treasure's and an object action's (+4 in their data),
// an object's and an enemy's collision (+4 / +0x1C, their dummies too) and a connecting collision's (+0). At each
// Firelink check, a vagrant lost there from another game is moved to the free point of the place nearest to where it
// was lost (a point with a character on it is passed over: Petrus's ruin has NPCs) and its floor names that place.
#include "vagrant_spots.inc"

constexpr uint32_t kMsbLoad = 0x00D24460;  // ecx = size, stack (map's MSB object, file bytes)
constexpr uint32_t kVagrantMaps = 0x0137D6B8;  // the per-map floors' vagrant records (0xDD1310)
constexpr int kFirelinkIndex = 2;
constexpr float kClear = 1.6f;
constexpr int kSpotBase = 1023900;  // place p: entities kSpotBase + 10 p + 0..2
struct Place {
    const char* name;
    const char* collision;
    game::Vec3 at[3];
    int n;
};
const Place kPlaces[] = {
    {"the bridge by the Ring of Sacrifice", "h0028B2_0000", {{-32.1f, -50.0f, 3.1f}, {-29.6f, -50.0f, 4.6f}, {-34.6f, -50.0f, 1.6f}}, 3},
    {"Petrus's ruin", "h0007B2_0000", {{-25.0f, -53.27f, 83.5f}, {-26.1f, -53.27f, 83.1f}, {-24.0f, -53.27f, 84.0f}}, 3},
    {"between the Firekeeper and Lautrec", "h0001B2_0000", {{-55.6f, -64.57f, 47.6f}, {-56.7f, -64.74f, 45.8f}, {}}, 2},
};
constexpr int kPlaceCount = 3;
constexpr int kDefaultPlace = 2;
bool g_spots = true;          // VagrantSpotsFirelink
volatile LONG g_spots_in = 0;  // Firelink's map got them (this load)
void* g_msb_load_orig = nullptr;

uint32_t rd32(const std::vector<uint8_t>& e, size_t at) {
    uint32_t v = 0;
    if (at + 4 <= e.size()) std::memcpy(&v, &e[at], 4);
    return v;
}
void wr32(std::vector<uint8_t>& e, size_t at, uint32_t v) {
    if (at + 4 <= e.size()) std::memcpy(&e[at], &v, 4);
}
std::string entry_name(const std::vector<uint8_t>& e) {
    const uint32_t o = rd32(e, 0);
    std::string s;
    for (size_t i = o; i < e.size() && e[i]; ++i) s.push_back(static_cast<char>(e[i]));
    return s;
}

struct MsbSection {
    uint32_t version = 0;
    std::string name;
    std::vector<std::vector<uint8_t>> entries;
};

// MSB1: each section is {version, name offset, count of offsets} with that many offsets (the entries', then the next
// section's, 0 for the last), absolute; an entry's own offsets are relative to it.
bool parse_msb(const uint8_t* d, uint32_t size, std::vector<MsbSection>* out) {
    std::vector<uint32_t> starts;
    struct Raw {
        uint32_t version, name;
        std::vector<uint32_t> entries;
    };
    std::vector<Raw> raw;
    uint32_t off = 0;
    for (int guard = 0; guard < 16; ++guard) {
        if (off + 12 > size) return false;
        uint32_t ver, name, count;
        std::memcpy(&ver, d + off, 4);
        std::memcpy(&name, d + off + 4, 4);
        std::memcpy(&count, d + off + 8, 4);
        if (count == 0 || count > 100000 || off + 12 + 4ull * count > size || name >= size) return false;
        Raw r{ver, name, {}};
        for (uint32_t i = 0; i + 1 < count; ++i) {
            uint32_t e;
            std::memcpy(&e, d + off + 12 + 4 * i, 4);
            if (e >= size) return false;
            r.entries.push_back(e);
            starts.push_back(e);
        }
        starts.push_back(off);
        raw.push_back(r);
        uint32_t next;
        std::memcpy(&next, d + off + 12 + 4 * (count - 1), 4);
        if (!next) break;
        if (next <= off || next >= size) return false;
        off = next;
    }
    starts.push_back(size);
    std::sort(starts.begin(), starts.end());
    for (const Raw& r : raw) {
        MsbSection s;
        s.version = r.version;
        for (uint32_t i = r.name; i < size && d[i]; ++i) s.name.push_back(static_cast<char>(d[i]));
        for (uint32_t e : r.entries) {
            const uint32_t end = *std::upper_bound(starts.begin(), starts.end(), e);
            s.entries.emplace_back(d + e, d + end);
        }
        out->push_back(std::move(s));
    }
    return true;
}

std::vector<uint8_t> write_msb(const std::vector<MsbSection>& secs) {
    std::vector<uint8_t> out;
    std::vector<size_t> heads;
    std::vector<std::vector<uint32_t>> offs(secs.size());
    std::vector<uint32_t> names(secs.size());
    for (size_t k = 0; k < secs.size(); ++k) {
        const MsbSection& s = secs[k];
        heads.push_back(out.size());
        const size_t n = s.entries.size() + 1;
        out.resize(out.size() + 12 + 4 * n);
        names[k] = static_cast<uint32_t>(out.size());
        out.insert(out.end(), s.name.begin(), s.name.end());
        out.push_back(0);
        while (out.size() % 4) out.push_back(0);
        for (const auto& e : s.entries) {
            offs[k].push_back(static_cast<uint32_t>(out.size()));
            out.insert(out.end(), e.begin(), e.end());
        }
    }
    for (size_t k = 0; k < secs.size(); ++k) {
        const uint32_t count = static_cast<uint32_t>(secs[k].entries.size() + 1);
        const uint32_t next = k + 1 < secs.size() ? static_cast<uint32_t>(heads[k + 1]) : 0;
        uint8_t* h = &out[heads[k]];
        std::memcpy(h, &secs[k].version, 4);
        std::memcpy(h + 4, &names[k], 4);
        std::memcpy(h + 8, &count, 4);
        for (size_t i = 0; i < offs[k].size(); ++i) std::memcpy(h + 12 + 4 * i, &offs[k][i], 4);
        std::memcpy(h + 12 + 4 * offs[k].size(), &next, 4);
    }
    return out;
}

MsbSection* section(std::vector<MsbSection>& secs, const char* name) {
    for (auto& s : secs) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

// The maps given spots, kept for the rest of the run: the loader copies one at once, but nothing it was handed is ever
// freed (a load of the same map gives the same bytes, which are handed again).
std::vector<std::vector<uint8_t>*> g_new_msbs;
std::string g_spots_line;

// Firelink Shrine's map: its parts' entities are 102xxxx and its collisions include the three under the places.
bool is_firelink(std::vector<MsbSection>& secs) {
    MsbSection* parts = section(secs, "PARTS_PARAM_ST");
    if (!parts) return false;
    int found = 0, firelink_ids = 0;
    for (const auto& e : parts->entries) {
        const std::string n = entry_name(e);
        for (const Place& p : kPlaces) found += n == p.collision ? 1 : 0;
        const uint32_t eo = rd32(e, 0x58);
        const int ent = eo ? static_cast<int>(rd32(e, eo)) : -1;
        firelink_ids += ent / 10000 == 102 ? 1 : 0;
    }
    return found == kPlaceCount && firelink_ids >= 3;
}

bool add_spots(std::vector<MsbSection>& secs, std::string* why) {
    MsbSection* models = section(secs, "MODEL_PARAM_ST");
    MsbSection* events = section(secs, "EVENT_PARAM_ST");
    MsbSection* parts = section(secs, "PARTS_PARAM_ST");
    if (!models || !events || !parts) return *why = "a section is missing", false;
    int m_at = -1, enemy_models = 0, p_at = -1, enemies = 0;
    for (size_t i = 0; i < models->entries.size(); ++i) {
        const std::string n = entry_name(models->entries[i]);
        if (n == "c3490" || n == "c3491") return *why = "it has vagrant models already", false;
        if (rd32(models->entries[i], 4) == 2) m_at = static_cast<int>(i) + 1, ++enemy_models;
    }
    std::vector<std::string> names;
    for (size_t i = 0; i < parts->entries.size(); ++i) {
        names.push_back(entry_name(parts->entries[i]));
        if (rd32(parts->entries[i], 4) == 2) p_at = static_cast<int>(i) + 1, ++enemies;
        const uint32_t eo = rd32(parts->entries[i], 0x58);
        const int ent = eo ? static_cast<int>(rd32(parts->entries[i], eo)) : -1;
        if (ent >= kSpotBase && ent < kSpotBase + 10 * kPlaceCount) return *why = "an entity of the spots is taken", false;
    }
    if (m_at < 0 || p_at < 0) return *why = "no enemies", false;
    for (const auto& e : events->entries) {
        if (rd32(e, 8) == 5) return *why = "it has generators", false;
    }
    int collision[kPlaceCount];
    for (int p = 0; p < kPlaceCount; ++p) {
        auto it = std::find(names.begin(), names.end(), kPlaces[p].collision);
        if (it == names.end()) return *why = "a place's collision is missing", false;
        collision[p] = static_cast<int>(it - names.begin());
    }
    const int added = 3 * kPlaceCount;
    auto moved = [&](uint32_t v) { return static_cast<int>(v) >= p_at ? v + added : v; };
    // Every index past the insertion moves on.
    for (auto& e : parts->entries) {
        const uint32_t typ = rd32(e, 4);
        const uint32_t model = rd32(e, 0xC);
        if (static_cast<int>(model) >= m_at) wr32(e, 0xC, model + 2);
        const uint32_t to = rd32(e, 0x5C);
        int at = -1;
        if (typ == 1 || typ == 9) at = 4;
        if (typ == 2 || typ == 10) at = 0x1C;
        if (typ == 11) at = 0;
        if (to && at >= 0) wr32(e, to + at, moved(rd32(e, to + at)));
        // Each collision names a place's spots (the shrine's unless it is under another place).
        if (typ == 5 && to) {
            int place = kDefaultPlace;
            for (int p = 0; p < kPlaceCount; ++p) place = entry_name(e) == kPlaces[p].collision ? p : place;
            for (int k = 0; k < 3; ++k) wr32(e, to + 0x18 + 4 * k, static_cast<uint32_t>(kSpotBase + 10 * place + k));
        }
    }
    for (auto& e : events->entries) {
        const uint32_t typ = rd32(e, 8), bo = rd32(e, 0x10), to = rd32(e, 0x14);
        if (bo) wr32(e, bo, moved(rd32(e, bo)));
        if ((typ == 4 || typ == 7) && to) wr32(e, to + 4, moved(rd32(e, to + 4)));
    }
    // The models after the enemy models, the parts after the enemies.
    std::vector<uint8_t> mi(kModelItem, kModelItem + sizeof(kModelItem));
    std::vector<uint8_t> mh(kModelHumanity, kModelHumanity + sizeof(kModelHumanity));
    wr32(mi, 8, enemy_models);
    wr32(mh, 8, enemy_models + 1);
    wr32(mi, 0x10, kPlaceCount);
    wr32(mh, 0x10, 2 * kPlaceCount);
    models->entries.insert(models->entries.begin() + m_at, {mi, mh});
    std::vector<std::vector<uint8_t>> fresh;
    for (int p = 0; p < kPlaceCount; ++p) {
        const int col = collision[p] >= p_at ? collision[p] + added : collision[p];
        const uint8_t* tpl[3] = {kPartItem, kPartHumanity, kPartHumanity2};
        for (int k = 0; k < 3; ++k) {
            std::vector<uint8_t> e(tpl[k], tpl[k] + sizeof(kPartItem));
            wr32(e, 8, enemies + static_cast<uint32_t>(fresh.size()));
            wr32(e, 0xC, m_at + (k ? 1 : 0));
            std::memcpy(&e[0x14], &kPlaces[p].at[0], 12);
            const float rot[3] = {0.0f, 0.0f, 0.0f};
            std::memcpy(&e[0x20], rot, 12);
            char name[16];
            std::snprintf(name, sizeof(name), "c349%d_%04d", k ? 1 : 0, 9000 + 3 * p + k);
            std::memcpy(&e[rd32(e, 0)], name, 10);  // the template's names are 10 characters too
            wr32(e, rd32(e, 0x58), static_cast<uint32_t>(kSpotBase + 10 * p + k));
            wr32(e, rd32(e, 0x5C) + 0x1C, static_cast<uint32_t>(col));
            fresh.push_back(std::move(e));
        }
    }
    parts->entries.insert(parts->entries.begin() + p_at, fresh.begin(), fresh.end());
    char line[160];
    std::snprintf(line, sizeof(line), "%d vagrant enemies at %d places (parts %d.., models %d..)", added, kPlaceCount,
                  p_at, m_at);
    *why = line;
    return true;
}

// The parse copies what it needs; a bad read leaves the map as it is.
bool safe_parse(const uint8_t* bytes, uint32_t size, std::vector<MsbSection>* secs) {
    __try {
        return parse_msb(bytes, size, secs);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// cdecl(&size, &bytes) at the loader's start: Firelink's map with its spots.
void __cdecl msb_load(uint32_t* size, const uint8_t** bytes) {
    if (!g_spots || !size || !bytes || !*bytes || *size < 64 || *size > (64u << 20)) return;
    std::vector<MsbSection> secs;
    if (!safe_parse(*bytes, *size, &secs) || !is_firelink(secs)) return;
    std::string why;
    if (!add_spots(secs, &why)) {
        LOG_INFO("[net] Vagrants: Firelink Shrine's map left as it is (%s)", why.c_str());
        return;
    }
    std::vector<uint8_t> built = write_msb(secs);
    std::vector<uint8_t>* keep = nullptr;
    for (std::vector<uint8_t>* b : g_new_msbs) keep = *b == built ? b : keep;
    if (!keep) {
        keep = new std::vector<uint8_t>(std::move(built));
        g_new_msbs.push_back(keep);
    }
    *bytes = keep->data();
    *size = static_cast<uint32_t>(keep->size());
    if (!InterlockedExchange(&g_spots_in, 1)) {
        LOG_INFO("[net] Vagrants: Firelink Shrine's map gets vagrant spots: %s", why.c_str());
    }
}

__declspec(naked) void hk_msb_load() {
    __asm {
        pushad
        lea eax, [esp + 0x28]  // the file's bytes (second argument)
        lea edx, [esp + 0x18]  // the saved ecx (size)
        push eax
        push edx
        call msb_load
        add esp, 8
        popad
        jmp dword ptr [g_msb_load_orig]
    }
}

// Characters standing near a point (not our spots' own, which wait around their place's first point: the three of a
// place push one another up to a few tenths of a metre apart).
bool someone_near(const game::Vec3& at) {
    uint32_t wcm = 0, sets_info = 0, nsets = 0;
    if (!read32(game::kWorldChrManVar, &wcm) || !wcm || !read32(wcm + 0x10, &sets_info) || !sets_info ||
        !read32(sets_info + 0xC, &nsets)) {
        return false;
    }
    for (uint32_t k = 0; k < nsets + 2 && k < 64; ++k) {
        uint32_t set = 0, n = 0, entries = 0;
        if (!read32(wcm + 0x240 + k * 4, &set) || !set || !read32(set, &n) || !read32(set + 4, &entries)) continue;
        for (uint32_t i = 0; i < n && i < 4096; ++i) {
            uint32_t chr = 0, hp = 0;
            if (!read32(entries + i * 0x20, &chr) || !chr || !read32(chr + 0x2D4, &hp) || static_cast<int32_t>(hp) <= 0) {
                continue;
            }
            uint32_t entity = 0;
            read32(chr + 0x208, &entity);
            if (static_cast<int>(entity) >= kSpotBase && static_cast<int>(entity) < kSpotBase + 10 * kPlaceCount) continue;
            game::Vec3 p{};
            if (!game::chr_position(chr, &p)) continue;
            const float dx = p.x - at.x, dz = p.z - at.z;
            if (std::fabs(p.y - at.y) < 3.0f && dx * dx + dz * dz < kClear * kClear) return true;
        }
    }
    return false;
}

// The free point of the place nearest to `lost`, or -1. `used`: points taken in this check.
int firelink_point(const game::Vec3& lost, std::vector<game::Vec3>& used, game::Vec3* out) {
    int order[kPlaceCount] = {0, 1, 2};
    float dist[kPlaceCount];
    for (int i = 0; i < kPlaceCount; ++i) {
        const game::Vec3& a = kPlaces[i].at[0];
        dist[i] = (a.x - lost.x) * (a.x - lost.x) + (a.y - lost.y) * (a.y - lost.y) + (a.z - lost.z) * (a.z - lost.z);
    }
    std::sort(order, order + kPlaceCount, [&](int a, int b) { return dist[a] < dist[b]; });
    for (int i : order) {
        for (int k = 0; k < kPlaces[i].n; ++k) {
            const game::Vec3& pt = kPlaces[i].at[k];
            bool taken = false;
            for (const auto& u : used) {
                const float dx = u.x - pt.x, dz = u.z - pt.z;
                taken = taken || dx * dx + dz * dz < kClear * kClear;
            }
            if (taken || someone_near(pt)) continue;
            used.push_back(pt);
            *out = pt;
            return i;
        }
    }
    return -1;
}

// The collision a vagrant was lost on (its map word: the map index in bits 20..27, the floor's record below; as
// 0xDD1310 and 0xDD6830 read it), for its vagrant spots: its type data, or 0.
uint32_t lost_floor_data(uint32_t map_word) {
    uint32_t tbl = 0, maps = 0, nmaps = 0, recs = 0, nrecs = 0, msb = 0, idx = 0, cols = 0, ncols = 0, part = 0, td = 0;
    const uint32_t m = (map_word >> 20) & 0xFF, sub = map_word & 0xFFFFF;
    if (!read32(kVagrantMaps, &tbl) || !tbl || !read32(tbl + 0x20, &nmaps) || m >= nmaps || !read32(tbl + 0x24, &maps) ||
        !maps || !read32(maps + m * 0x68 + 0x3C, &nrecs) || sub >= nrecs || !read32(maps + m * 0x68 + 0x40, &recs) ||
        !recs || !read32(recs + sub * 0x84 + 8, &msb) || !msb || !read32(recs + sub * 0x84 + 0xC, &idx) ||
        !read32(msb + 0x180, &ncols) || !read32(msb + 0x188, &cols) || !cols) {
        return 0;
    }
    const int i = static_cast<int16_t>(idx & 0xFFFF);
    if (i < 0 || static_cast<uint32_t>(i) >= ncols || !read32(cols + i * 4, &part) || !part || !read32(part + 0x5C, &td)) {
        return 0;
    }
    return td;
}

// Before Firelink's check: each vagrant lost there by another player to a free point of the nearest place.
void place_firelink(std::string* where, int* count) {
    std::vector<game::Vec3> used;
    std::vector<uint32_t> items;
    each(db(), 0x24, [&](uint32_t item) { items.push_back(item); });
    for (uint32_t item : items) {
        uint32_t map = 0, bits = 0, pos[3] = {};
        if (!read32(item + 0x14, &map) || !read32(item + 0x20, &bits)) continue;
        if (!(bits & 0x40000000) || static_cast<int>((map >> 20) & 0xFF) != kFirelinkIndex) continue;
        if (!read32(item + 4, &pos[0]) || !read32(item + 8, &pos[1]) || !read32(item + 0xC, &pos[2])) continue;
        game::Vec3 lost{}, at{};
        std::memcpy(&lost.x, &pos[0], 4);
        std::memcpy(&lost.y, &pos[1], 4);
        std::memcpy(&lost.z, &pos[2], 4);
        const int place = firelink_point(lost, used, &at);
        ++*count;
        if (place < 0) {
            *where += (where->empty() ? "" : "; ") + std::string("one with no free point");
            continue;
        }
        const uint32_t td = lost_floor_data(map);
        if (td) {
            for (int k = 0; k < 3; ++k) put32(td + 0x18 + 4 * k, static_cast<uint32_t>(kSpotBase + 10 * place + k));
        }
        uint32_t v[3];
        std::memcpy(&v[0], &at.x, 4);
        std::memcpy(&v[1], &at.y, 4);
        std::memcpy(&v[2], &at.z, 4);
        put32(item + 4, v[0]);
        put32(item + 8, v[1]);
        put32(item + 0xC, v[2]);
        char line[200];
        std::snprintf(line, sizeof(line), "%s at (%.1f, %.1f, %.1f), lost at (%.1f, %.1f, %.1f)%s", kPlaces[place].name,
                      at.x, at.y, at.z, lost.x, lost.y, lost.z, td ? "" : " (its floor not found)");
        *where += (where->empty() ? "" : "; ") + std::string(line);
    }
}

// The vagrant enemies made for Firelink's spots (db+0x3C: +0 entity, +6 flags).
std::string spot_slots() {
    std::string s;
    each(db(), 0x3C, [&](uint32_t slot) {
        uint32_t ent = 0, f = 0;
        if (!read32(slot, &ent) || static_cast<int>(ent) < kSpotBase || static_cast<int>(ent) >= kSpotBase + 30) return;
        read32(slot + 4, &f);
        char b[32];
        std::snprintf(b, sizeof(b), "%s%u/%X", s.empty() ? "" : " ", ent, (f >> 16) & 0xFFFF);
        s += b;
    });
    return s;
}

volatile LONG g_last_index = -1;
std::mutex g_indices_lock;
std::set<int> g_load_indices;  // the indices one load checked (told on the next frame with a character)
int g_lines = 0;

void safe_look(int map_index, Look* out) {
    __try {
        *out = look(map_index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void note_index(int map_index) {
    std::lock_guard<std::mutex> lock(g_indices_lock);
    if (g_load_indices.size() < 32) g_load_indices.insert(map_index);
}

// Firelink's check after its load (2026-11-25): a load runs the check before the map's characters are made, and when
// Firelink was loaded already (a death or a warp there, the hub's usual loads) the spot reserved was the old vagrant
// enemy's, made again a moment later with its reservation gone: the vagrant was passed on instead of coming. So the
// check for Firelink waits for the load to end and our character to stand there (kFirelinkCheckDelay), as a check run
// then did come every time; a load that ends away from Firelink's map leaves it to Firelink's next load.
constexpr ULONGLONG kFirelinkCheckDelay = 1500;
volatile LONG g_firelink_waiting = 0;
ULONGLONG g_firelink_ready_at = 0;
bool g_firelink_now = false;  // the check that waited, run now

void __cdecl hk_check(int map_index) {
    if (map_index == kFirelinkIndex && g_spots_in && !g_firelink_now) {
        InterlockedExchange(&g_firelink_waiting, 1);
        g_firelink_ready_at = 0;
        note_index(map_index);
        return;
    }
    std::string where;
    int placed = 0;
    if (map_index == kFirelinkIndex && g_spots_in) place_firelink(&where, &placed);
    Look before{};
    safe_look(map_index, &before);
    reinterpret_cast<void(__cdecl*)(int)>(g_check)(map_index);
    InterlockedExchange(&g_last_index, map_index);
    if (map_index == kFirelinkIndex && g_spots_in && placed) {
        LOG_INFO("[net] Vagrants: Firelink's check: %d vagrant(s) lost there by another player%s%s; its spots' enemies "
                 "(entity/flags): %s", placed, placed ? ": " : "", where.c_str(), spot_slots().c_str());
    }
    if (!g_s.log) return;
    note_index(map_index);
    if (before.here <= 0 || g_lines >= 200) return;
    ++g_lines;
    const int after = held();
    LOG_INFO("[net] Vagrants: map index %d loaded with %d of the %d we hold lost there by another player (wander count "
             "%d..%d; %d far enough to come as vagrants, the rest as items on the ground): %d left the list",
             map_index, before.here, before.held, before.minc, before.maxc, before.ready,
             after >= 0 ? (std::max)(before.held - after, 0) : 0);
}

// ---------------------------------------------------------------- test
volatile LONG g_test_pending = 0;
volatile LONG g_check_pending = -1;
int g_test_index = -1, g_test_wanders = 2;
bool g_test_humanity = false;
uint32_t g_test_item = 0;

uint32_t call_add(uint32_t d, uint32_t* data) {
    __try {
        return reinterpret_cast<uint32_t(__stdcall*)(uint32_t, uint32_t*, int)>(kAdd)(d, data, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void run_test() {
    const uint32_t d = db();
    game::Vec3 pos{};
    float angle = 0;
    const int index = g_test_index >= 0 ? g_test_index : static_cast<int>(g_last_index);
    if (!d || !game::position(&pos, &angle) || index < 0) {
        LOG_INFO("[net] Vagrant test: not added (database %08X, map index %d, a character %s)", d, index,
                 game::player() ? "yes" : "no");
        return;
    }
    const uint32_t item = g_test_humanity ? kHumanityItem : g_test_item ? g_test_item : kTestItem;
    uint32_t data[8];
    std::memcpy(&data[0], &pos.x, 4);
    std::memcpy(&data[1], &pos.y, 4);
    std::memcpy(&data[2], &pos.z, 4);
    std::memcpy(&data[3], &angle, 4);
    data[4] = static_cast<uint32_t>(index & 0xFF) << 20;
    data[5] = item;
    data[6] = item;
    data[7] = (static_cast<uint32_t>(g_test_wanders & 0x3FF) << 20) | 1u;
    const uint32_t added = call_add(d, data);
    uint32_t bits = 0;
    if (added) read32(added + 0x20, &bits);
    LOG_INFO("[net] Vagrant test: %s (%s, map index %d, wander count %d) at (%.1f, %.1f, %.1f): item %08X, flags %08X; "
             "%d held",
             added ? "added" : "NOT added", g_test_humanity ? "humanity" : g_test_item ? "the item asked" : "Prism Stone",
             index, g_test_wanders, pos.x,
             pos.y, pos.z, added, bits, held());
}

}  // namespace

int held() {
    const uint32_t d = db();
    uint32_t n = 0;
    return d && read32(d + 0x28, &n) && n < 100000 ? static_cast<int>(n) : -1;
}

int targets() { return (std::max)(each(db(), 0x10, [](uint32_t) {}), 0); }

void install(const Settings& s) {
    g_s = s;
    g_s.wanders = (std::max)(2, (std::min)(s.wanders, 10));
    g_s.stay_seconds = (std::max)(10, (std::min)(s.stay_seconds, 300));
    // The game's own values first (another build would have others: then nothing is changed).
    uint32_t a = 0, b = 0, hb = 0, stay = 0;
    const bool ours = read32(kItemA, &a) && read32(kItemB, &b) && read32(kHumanityB, &hb) && read32(kStay, &stay) &&
                      a == 10 && b == 20 && hb == 20 && stay == 0x43960000;  // 300.0f
    if (!ours) {
        LOG_ERROR("[net] Vagrants: the game's values are not the ones known (%u %u %u %08X); left alone", a, b, hb, stay);
    } else {
        const float stay_f = static_cast<float>(g_s.stay_seconds);
        uint32_t stay_bits = 0;
        std::memcpy(&stay_bits, &stay_f, 4);
        put32(kItemA, static_cast<uint32_t>(g_s.wanders));
        put32(kItemB, static_cast<uint32_t>(2 * g_s.wanders));
        put32(kHumanityB, static_cast<uint32_t>(2 * g_s.wanders));
        put32(kStay, stay_bits);
    }
    const bool check = redirect_call(kCheckCall, kCheck, reinterpret_cast<void*>(&hk_check));
    g_spots = s.firelink_spots;
    if (g_spots) {
        // push ebp ; mov ebp,esp ; and esp,-16
        static const uint8_t k[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
        g_msb_load_orig = detour(kMsbLoad, k, sizeof(k), reinterpret_cast<void*>(&hk_msb_load));
        if (!g_msb_load_orig) {
            g_spots = false;
            LOG_ERROR("[net] Vagrants: the map loader at %08X does not match this build; Firelink Shrine gets no "
                      "vagrant spots", kMsbLoad);
        }
    }
    const bool send = redirect_call(kSendCall, kSend, reinterpret_cast<void*>(&hk_send));
    g_installed = true;
    LOG_INFO("[net] Vagrants: lost items come as vagrants after %d games (VagrantWanders; the game's own 10), each game "
             "keeps one %d s (VagrantStay; the game's 300), passed on before leaving: %s%s",
             g_s.wanders, g_s.stay_seconds, g_s.pass_on_at_quit ? "yes" : "no (VagrantPassOnAtQuit)",
             check && send ? "" : "; the map loads' checks or the hand-offs are not logged (their sites do not match)");
}

void tick() {
    if (!g_installed) return;
    if (InterlockedExchange(&g_test_pending, 0)) run_test();
    const LONG check_index = InterlockedExchange(&g_check_pending, -1);
    if (check_index >= 0) hk_check(static_cast<int>(check_index));
    if (g_firelink_waiting) {
        if (game::loading() || !game::player()) {
            g_firelink_ready_at = 0;
        } else if (!g_firelink_ready_at) {
            g_firelink_ready_at = GetTickCount64() + kFirelinkCheckDelay;
        } else if (GetTickCount64() >= g_firelink_ready_at) {
            InterlockedExchange(&g_firelink_waiting, 0);
            // Only where Firelink's map is loaded: Firelink, or the Undead Burg it loads with (the check also lays the
            // vagrants not yet far enough on its ground, which needs its map).
            const uint32_t here = static_cast<uint32_t>(game::map_id());
            const bool run = here == 0x0A020000u || here == 0x0A010000u;
            if (g_s.log) {
                LOG_INFO("[net] Vagrants: Firelink's check, held until its load ended: %s (we are in map %08X, %d held)",
                         run ? "run now" : "left to Firelink's next load", here, held());
            }
            if (run) {
                g_firelink_now = true;
                hk_check(kFirelinkIndex);
                g_firelink_now = false;
            }
        }
    }
    watch_window();
    if (g_s.log && game::player()) {
        std::string list;
        {
            std::lock_guard<std::mutex> lock(g_indices_lock);
            for (int i : g_load_indices) list += (list.empty() ? "" : ", ") + std::to_string(i);
            g_load_indices.clear();
        }
        static int told = 0;
        if (!list.empty() && ++told <= 30) {
            const int m = game::map_id();
            LOG_INFO("[net] Vagrants: the load checked map index(es) %s; we are in m%02d_%02d (%d held, %d linked games "
                     "to pass them to)",
                     list.c_str(), (m >> 24) & 0xFF, (m >> 16) & 0xFF, held(), targets());
        }
    }
    if (g_s.pass_on_at_quit) {
        static bool asked = false;
        const bool quit = quitting();
        const bool now_asked = quit || return_state() != 0;
        if (now_asked && !asked) begin(quit ? "Quit Game" : "the return to the title");
        asked = now_asked;
    }
    pass_step();
}

void check_test(int map_index) { InterlockedExchange(&g_check_pending, map_index); }

void add_test(int map_index, int wanders, bool humanity, uint32_t item) {
    g_test_item = item;
    g_test_index = map_index;
    g_test_wanders = (std::max)(1, (std::min)(wanders, 999));
    g_test_humanity = humanity;
    InterlockedExchange(&g_test_pending, 1);
}

}  // namespace coop::vagrants
