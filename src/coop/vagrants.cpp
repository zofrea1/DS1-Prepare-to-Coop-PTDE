// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "vagrants.h"

#include "game.h"
#include "hook.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

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

void __cdecl hk_check(int map_index) {
    Look before{};
    safe_look(map_index, &before);
    reinterpret_cast<void(__cdecl*)(int)>(g_check)(map_index);
    InterlockedExchange(&g_last_index, map_index);
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
int g_test_index = -1, g_test_wanders = 2;
bool g_test_humanity = false;

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
    const uint32_t item = g_test_humanity ? kHumanityItem : kTestItem;
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
             added ? "added" : "NOT added", g_test_humanity ? "humanity" : "Prism Stone", index, g_test_wanders, pos.x,
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

void add_test(int map_index, int wanders, bool humanity) {
    g_test_index = map_index;
    g_test_wanders = (std::max)(1, (std::min)(wanders, 999));
    g_test_humanity = humanity;
    InterlockedExchange(&g_test_pending, 1);
}

}  // namespace coop::vagrants
