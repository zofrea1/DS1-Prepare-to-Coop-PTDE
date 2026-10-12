#include "text.h"

#include "hook.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <intrin.h>

namespace coop::text {

namespace {

constexpr uint32_t kFmgLookup = 0x00D175E0;   // edx = id, [esp+4] = FMG; returns wchar_t* or 0 (cdecl)
constexpr uint32_t kMsgRepoVar = 0x013785DC;  // MsgRepository: FMG of category N at +4 + 4*N, patch FMGs above
constexpr uint32_t kMenuManVar = 0x013786D0;

// The action box (MenuMan+0xEC0): two channels of three rotating lines (channel at +4 + 12*n: lines, index,
// in-use flags at +8..+10). 0x515B80 queues a line (ecx = box, eax = channel, push text; ret 4) and
// MenuMan+0xF4 = 1 shows it, as RecallMenuEvent (0xD74950) does.
constexpr uint32_t kActionBox = 0xEC0;
constexpr uint32_t kActionBoxShow = 0xF4;
constexpr uint32_t kActionBoxAdd = 0x00515B80;

// RecallMenuEvent's body (0xD74950, stdcall(channel, event text id) -> bool): an event text in the action box.
// The Lua native and the game's own summon notices (0xEA2510) both end here.
constexpr uint32_t kRecallMenuEvent = 0x00D74950;

// The information box: 0xD4C090 opens it (cdecl(type, -1, 0, -1, 1); type 0 is the modal list that waits for
// a button), 0xD4C190 adds a line by message id (ecx = id, cdecl(push category)), 0xD4C290 adds a line from
// the message buffer (edi = id). The game always opens it right before adding a line.
constexpr uint32_t kInfoOpen = 0x00D4C090;
constexpr uint32_t kInfoAdd = 0x00D4C190;
constexpr uint32_t kInfoAddBuffer = 0x00D4C290;
// Two more ways into the box. 0xD4BB30 adds a list line (the "items / souls obtained" rows, through 0xD4BE50,
// and the Lua AddInfomationList) to whatever box [0x1378548] holds, and 0xD4BFA0 opens the box for the items
// AddInventoryItem hands out. A box this module holds back must be opened before either of them runs: a list
// line would otherwise go into the box before it (one already closed), and the held-back box would open a frame
// later with nothing in it, a modal list that waits for a button and has nothing to show.
constexpr uint32_t kInfoAddList = 0x00D4BB30;
constexpr uint32_t kInfoOpenItems = 0x00D4BFA0;
// The return address of the box opened by the member-left notice (0xD79380 calls 0xD4C090 at 0xD79418). That
// notice is always 20001020 or 20001025, both dropped in a seamless session. Its line is only added when the
// leaving member's record can still be found (0xD5E820); when it cannot, the held-back box opened empty.
constexpr uint32_t kRetMemberLeftBox = 0x00D7941D;

// FMG pointers in MsgRepository (base, then the 1.0.2 patch FMG the game checks first).
constexpr uint32_t kGoodsName[2] = {0x2C, 0x1C0};
constexpr uint32_t kGoodsInfo[2] = {0x54, 0x1BC};
constexpr uint32_t kGoodsCaption[2] = {0x64, 0x194};

// Remastered Seamless Co-op's RecallMenuEvent block list: being summoned, phantom/dark spirit summoned,
// summoned to the world of X, phantom vanquished (returning home), dark spirit vanquished.
constexpr uint32_t kBlockedEventTexts[] = {140013, 140014, 140023, 140024, 140030, 140031,
                                           140033, 140034, 140153, 140154, 140192};
// The game's modal notices about leaving that seamless play replaces with its own lines: phantom returned to
// world of origin, sent home by summoner, phantom X has returned home, returning to your world, you were sent
// home, disconnected from master of world (returning to your world), connection unstable (leaving session),
// failed to create/join session, P2P connection lost. A seamless session handles all of these itself; the box
// waits for a button and some of them were left on screen.
constexpr uint32_t kBlockedNotices[] = {20000415, 20000416, 20000420, 20000421, 20000425, 20000430,
                                        20000435, 20000450, 20001000, 20001001, 20001003, 20001004,
                                        20001015, 20001020, 20001025};

const wchar_t* (*g_item_text)(uint32_t, Fmg) = nullptr;
const wchar_t* (*g_dialog_text)(uint32_t) = nullptr;
void* g_fmg_orig = nullptr;
bool g_box_ok = false;

using RecallFn = bool(__stdcall*)(int, uint32_t);
RecallFn g_recall_orig = nullptr;
using InfoOpenFn = void(__cdecl*)(int, int, int, int, int);
InfoOpenFn g_info_open_orig = nullptr;
void* g_info_add_orig = nullptr;
void* g_info_buffer_orig = nullptr;
void* g_info_list_orig = nullptr;
void* g_info_items_orig = nullptr;
bool g_member_left_box = false;  // the member-left notice's box was dropped this frame (its line goes too)

bool g_seamless = false;
ULONGLONG g_seamless_until = 0;
struct PendingOpen {
    bool pending = false;
    int args[5] = {};
} g_open;
int g_dropped = 0;

Fmg classify(uint32_t fmg) {
    uint32_t repo = 0;
    if (!read32(kMsgRepoVar, &repo) || !repo) return Fmg::Other;
    auto is = [&](const uint32_t (&offs)[2]) {
        for (uint32_t o : offs) {
            uint32_t p = 0;
            if (read32(repo + o, &p) && p == fmg) return true;
        }
        return false;
    };
    if (is(kGoodsName)) return Fmg::GoodsName;
    if (is(kGoodsInfo)) return Fmg::GoodsInfo;
    if (is(kGoodsCaption)) return Fmg::GoodsCaption;
    return Fmg::Other;
}

const wchar_t* __stdcall custom_text(uint32_t id, uint32_t fmg) {
    const Fmg kind = classify(fmg);
    if (kind != Fmg::Other) return g_item_text ? g_item_text(id, kind) : nullptr;
    return g_dialog_text ? g_dialog_text(id) : nullptr;
}

// Only ids from 300000 up are ours to answer (items 389000+, dialogs 93129700).
__declspec(naked) void hk_fmg() {
    __asm {
        cmp edx, 300000
        jb orig
        push edx
        push dword ptr [esp + 8]
        push edx
        call custom_text
        pop edx
        test eax, eax
        jz orig
        ret
    orig:
        jmp g_fmg_orig
    }
}

bool filtering() { return g_seamless || GetTickCount64() < g_seamless_until; }

template <size_t N>
bool listed(const uint32_t (&list)[N], uint32_t id) {
    for (uint32_t v : list) {
        if (v == id) return true;
    }
    return false;
}

bool __stdcall hk_recall(int channel, uint32_t id) {
    if (filtering() && listed(kBlockedEventTexts, id)) {
        if (++g_dropped <= 50) LOG_INFO("[coop] Game notice %u not shown (seamless session)", id);
        return false;
    }
    return g_recall_orig(channel, id);
}

void open_pending() {
    if (!g_open.pending) return;
    g_open.pending = false;
    g_info_open_orig(g_open.args[0], g_open.args[1], g_open.args[2], g_open.args[3], g_open.args[4]);
}

// In a seamless session the box is opened when its first line arrives, so a dropped notice leaves no empty box.
void __cdecl hk_info_open(int type, int a, int b, int c, int d) {
    open_pending();
    if (!filtering()) {
        g_info_open_orig(type, a, b, c, d);
        return;
    }
    if (reinterpret_cast<uint32_t>(_ReturnAddress()) == kRetMemberLeftBox) {
        // Never shown in a seamless session, with or without its line (see kRetMemberLeftBox).
        g_member_left_box = true;
        return;
    }
    g_open.pending = true;
    g_open.args[0] = type;
    g_open.args[1] = a;
    g_open.args[2] = b;
    g_open.args[3] = c;
    g_open.args[4] = d;
}

// true: drop this line (and the box opened for it)
bool __stdcall info_line(uint32_t id) {
    if (g_member_left_box && listed(kBlockedNotices, id)) {
        g_member_left_box = false;
        if (++g_dropped <= 50) LOG_INFO("[coop] Game notice %u not shown (seamless session)", id);
        return true;
    }
    if (g_open.pending && listed(kBlockedNotices, id)) {
        g_open.pending = false;
        if (++g_dropped <= 50) LOG_INFO("[coop] Game notice %u not shown (seamless session)", id);
        return true;
    }
    open_pending();
    return false;
}

void __stdcall info_buffer_line() { open_pending(); }

__declspec(naked) void hk_info_add() {
    __asm {
        pushad
        push ecx
        call info_line
        test al, al
        popad
        jz pass
        ret
    pass:
        jmp g_info_add_orig
    }
}

__declspec(naked) void hk_info_buffer() {
    __asm {
        pushad
        call info_buffer_line
        popad
        jmp g_info_buffer_orig
    }
}

__declspec(naked) void hk_info_list() {
    __asm {
        pushad
        call info_buffer_line
        popad
        jmp g_info_list_orig
    }
}

__declspec(naked) void hk_info_items() {
    __asm {
        pushad
        call info_buffer_line
        popad
        jmp g_info_items_orig
    }
}

int pick_channel(uint32_t box) {
    for (int ch = 0; ch < 2; ++ch) {
        for (int line = 0; line < 3; ++line) {
            uint8_t used = 1;
            if (read8(box + 4 + ch * 12 + 8 + line, &used) && !used) return ch;
        }
    }
    return 0;
}

}  // namespace

bool install() {
    int failed = 0;
    {
        // push ebx ; mov ebx,[esp+8] ; push ebp ; push esi
        static const uint8_t k[7] = {0x53, 0x8B, 0x5C, 0x24, 0x08, 0x55, 0x56};
        g_fmg_orig = detour(kFmgLookup, k, sizeof(k), reinterpret_cast<void*>(&hk_fmg));
        if (!g_fmg_orig) {
            LOG_ERROR("[coop] FMG lookup at %08X does not match this build; co-op texts are unavailable", kFmgLookup);
            ++failed;
        }
    }
    {
        // cmp eax,1 ; ja ... : the action box queue we call
        static const uint8_t k[9] = {0x53, 0x8B, 0x5C, 0x24, 0x08, 0x57, 0x83, 0xF8, 0x01};
        g_box_ok = bytes_match(kActionBoxAdd, k, sizeof(k));
        if (!g_box_ok) {
            LOG_ERROR("[coop] Action box code at %08X does not match this build; messages go to the log only", kActionBoxAdd);
            ++failed;
        }
    }
    {
        // push ebp ; mov ebp,esp ; and esp,-8
        static const uint8_t k[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
        g_recall_orig = reinterpret_cast<RecallFn>(detour(kRecallMenuEvent, k, sizeof(k), reinterpret_cast<void*>(&hk_recall)));
        if (!g_recall_orig) {
            LOG_ERROR("[coop] RecallMenuEvent at %08X does not match this build", kRecallMenuEvent);
            ++failed;
        }
    }
    {
        // push -1 ; push 010687C0h (SEH frame)
        static const uint8_t k[7] = {0x6A, 0xFF, 0x68, 0xC0, 0x87, 0x06, 0x01};
        g_info_open_orig = reinterpret_cast<InfoOpenFn>(detour(kInfoOpen, k, sizeof(k), reinterpret_cast<void*>(&hk_info_open)));
        // push ebp ; mov ebp,esp ; and esp,-8 (both line adders)
        static const uint8_t k2[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
        g_info_add_orig = detour(kInfoAdd, k2, sizeof(k2), reinterpret_cast<void*>(&hk_info_add));
        g_info_buffer_orig = detour(kInfoAddBuffer, k2, sizeof(k2), reinterpret_cast<void*>(&hk_info_buffer));
        // push -1 ; push <SEH handler> (both)
        static const uint8_t k3[7] = {0x6A, 0xFF, 0x68, 0x1C, 0xF1, 0x05, 0x01};
        g_info_list_orig = detour(kInfoAddList, k3, sizeof(k3), reinterpret_cast<void*>(&hk_info_list));
        static const uint8_t k4[7] = {0x6A, 0xFF, 0x68, 0x92, 0x87, 0x06, 0x01};
        g_info_items_orig = detour(kInfoOpenItems, k4, sizeof(k4), reinterpret_cast<void*>(&hk_info_items));
        if (!g_info_open_orig || !g_info_add_orig || !g_info_buffer_orig || !g_info_list_orig || !g_info_items_orig) {
            LOG_ERROR("[coop] Information box code does not match this build (open %d, add %d, buffer %d, list %d, "
                      "items %d)",
                      g_info_open_orig != nullptr, g_info_add_orig != nullptr, g_info_buffer_orig != nullptr,
                      g_info_list_orig != nullptr, g_info_items_orig != nullptr);
            ++failed;
        }
    }
    return failed == 0;
}

void set_item_text(const wchar_t* (*fn)(uint32_t, Fmg)) { g_item_text = fn; }
void set_dialog_text(const wchar_t* (*fn)(uint32_t)) { g_dialog_text = fn; }

void show(const std::wstring& line) {
    if (!g_box_ok) return;
    uint32_t mm = 0;
    if (!read32(kMenuManVar, &mm) || !mm) return;
    static wchar_t buf[256];
    wcsncpy_s(buf, line.c_str(), _TRUNCATE);
    const uint32_t box = mm + kActionBox;
    const int channel = pick_channel(box);
    const wchar_t* text = buf;
    __try {
        __asm {
            push text
            mov ecx, box
            mov eax, channel
            mov edx, kActionBoxAdd
            call edx
        }
        *ptr<uint32_t>(mm + kActionBoxShow) = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[coop] Showing a message failed");
    }
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &w[0], n);
    return w;
}

void set_seamless_active(bool active) {
    if (g_seamless && !active) g_seamless_until = GetTickCount64() + 60000;
    g_seamless = active;
}

void tick() {
    if (g_info_open_orig) open_pending();
    g_member_left_box = false;
}

void debug_state(char* out, size_t size) {
    _snprintf_s(out, size, _TRUNCATE, "filter %d pending %d", filtering() ? 1 : 0, g_open.pending ? 1 : 0);
}

}  // namespace coop::text
