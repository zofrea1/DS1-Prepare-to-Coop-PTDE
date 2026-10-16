// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "items.h"

#include "game.h"
#include "hook.h"
#include "log.h"
#include "text.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>

namespace coop::items {

namespace {

constexpr uint32_t kGoodsGetter = 0x00D1E3A0;   // edi = goods id, push out {id, row}; ret 4
constexpr uint32_t kCanUse = 0x00EADCA0;        // cdecl(goods, chr, ?, chr type, weapon, ?) -> al
constexpr uint32_t kPlayerVtable = 0x0118A5D4;  // PlayerIns
constexpr int kUseGoodsSlot = 128;              // UseGoods(goods id, ?) thiscall, ret 8
constexpr uint32_t kUseGoodsOrig = 0x00E50C70;
// The inventory's own add: stdcall(inventory, category, id, count, 1, 0) -> slot, or -1 when full. The Lua
// AddInventoryItem (0xD664C0) wraps it with the "items obtained" box (0xD4BFA0; the next frame 0xD4BD80 puts it
// in the dialog slots at MenuMan+0x9EC). Called by the mod, that box is queued and never drawn or dismissed, in
// the rest (2026-10-12/13 tests) as well as standing in the world with no menu open (2026-10-13 11:12: slot 0
// requested for good); the action buttons (bonfires, doors, ladders) and every later dialog (Quit Game) waited
// behind it until the game was closed. A new character's first rest hands out every co-op item, so only new
// characters met it. The co-op items go straight into the inventory, and a line in the action box says so.
constexpr uint32_t kInventoryAdd = 0x00C0B700;
constexpr uint32_t kMenuManVar = 0x013786D0;
constexpr uint32_t kNoEventAnim[2] = {0x012E0C64, 0x012E0C68};  // the "none" handle pair (PlayerIns+0xFC/+0x100)
constexpr uint32_t kRemoveItem = 0x00D6D670;    // stdcall(category, id, count). count <= 0 removes one stack.
constexpr uint32_t kGoodsCategory = 0x40000000;
constexpr int kCount = kLast - kFirst + 1;
constexpr int kRowSize = 0x5C;  // EQUIP_PARAM_GOODS_ST

uint8_t g_rows[kCount][kRowSize];
bool g_rows_ready = false;
void* g_goods_orig = nullptr;
void* g_can_use_orig = nullptr;
uint32_t g_use_orig = 0;
int (*g_can_use_fn)(int32_t) = nullptr;
bool (*g_use_fn)(int32_t) = nullptr;
const wchar_t* (*g_dialog_fn)() = nullptr;

// Remastered Seamless Co-op's item rows, field for field (its param layout puts enable_pvp at 0x44 bit 5, so
// the flag bytes are translated to PTDE's layout: isEquip 0x44 bit 6, isOnlyOne 0x45 bit 2,
// disableMultiDropShare 0x46 bit 3). enable_* and the covenant bits stay 0: the game's item check is answered
// for these ids by the co-op rules instead (see hk_can_use).
struct Def {
    int32_t ref_id;
    int32_t sfx;
    uint16_t icon;
    const wchar_t* name;
    const wchar_t* info;
    const wchar_t* caption;
};

const Def kDefs[kCount] = {
    {-100, 40, 2008, L"Blessed Eye Orb", L"Create a co-operative session",
     L"A small, pale eye orb, warm in the hand.\n\nUse it to create a co-operative session. Players\n"
     L"who share your password may then join your world at any\ntime, wherever you are."},
    {-99, 80, 2010, L"Crystal Eye Orb", L"Join a co-operative session",
     L"A small blue eye orb of clouded crystal that hums faintly.\n\nUse it to join the world of a player who "
     L"shares your\npassword. You travel in your own body and keep all\nthat you gain."},
    {-98, 81, 2002, L"Chaos Eye Orb", L"Invade a co-operative session",
     L"A small eye orb, red with heat and spite.\n\nUse it to invade, as an evil spirit, a\n"
     L"co-operative session that welcomes invaders."},
    {-97, 0, 2132, L"Abyssal Eye Orb", L"Disband a co-operative session",
     L"A small black eye orb whose gaze never wavers.\n\nA guest uses it to return home. A host uses it to\n"
     L"disband the session."},
    {-96, 0, 2007, L"Ominous Tome", L"Change the world rules",
     L"Loose pages of faded writing that describe a rite to\nlay bare one's true nature before one's allies.\n\n"
     L"The host changes the rules of the world with each use."},
    {-95, 0, 2129, L"Dried Fingers", L"Taunt invaders into joining",
     L"Withered fingers.\n\nThe host uses them to taunt invaders into joining."},
    {-94, 0, 2037, L"Cursed Pendant", L"Cause disasters in one's own world",
     L"An old pendant, scratched and dull. Some say it was\ntaken from the tomb of the Gravelord.\n\n"
     L"Use it in one's own world to bring disaster upon it,\nalone or as host, and again to lift the curse."},
    {0, 0, 2033, L"Crimson Blossom", L"Accrues curse",
     L"A blood-red flower rooted in the Darksign, feeding\non its bearer.\n\nOne blooms with each death and saps "
     L"the bearer's\nmaximum HP. They wither at a bonfire."},
    {-93, 0, 2067, L"Parchment of Deliverance", L"Revives world allies",
     L"A parchment that calls back certain allies of the\nworld who have fallen.\n\nCosts souls, and returns you "
     L"to your last bonfire."},
};

void put32(uint8_t* r, int off, int32_t v) { std::memcpy(r + off, &v, 4); }
void put16(uint8_t* r, int off, int16_t v) { std::memcpy(r + off, &v, 2); }

void build_rows() {
    for (int i = 0; i < kCount; ++i) {
        uint8_t* r = g_rows[i];
        const Def& d = kDefs[i];
        std::memset(r, 0, kRowSize);
        put32(r, 0x00, d.ref_id);   // refId
        put32(r, 0x04, d.sfx);      // sfxVariationId
        put32(r, 0x18, -1);         // replaceItemId
        put32(r, 0x20, -1);         // qwcId
        put32(r, 0x28, -1);         // magicId
        put16(r, 0x2C, static_cast<int16_t>(d.icon));
        put16(r, 0x2E, -1);         // modelId
        put16(r, 0x30, -1);         // shopLv
        put16(r, 0x32, -1);         // compTrophySedId
        put16(r, 0x34, -1);         // trophySeqId
        put16(r, 0x36, 1);          // maxNum
        r[0x39] = 50;               // overDexterity
        r[0x46] = 0x08;             // disableMultiDropShare
        if (kFirst + i == kBlossom) {
            put16(r, 0x36, 5);      // up to five, one per death
            r[0x3A] = 1;            // goodsType: key item (the game never lets it be used)
            continue;
        }
        r[0x3B] = 2;                // refCategory
        r[0x3E] = 6;                // goodsUseAnim: the eye orb animation
        r[0x44] = 0x40;             // isEquip
        r[0x45] = 0x04;             // isOnlyOne
        if (kFirst + i == kParchment) {
            r[0x3F] = 1;            // opmeMenuType: yes/no dialog first
            put32(r, 0x24, static_cast<int32_t>(kParchmentDialog));
        }
    }
    g_rows_ready = true;
    LOG_INFO("[coop] Co-op item rows ready");
}

void* __stdcall our_row(int32_t id) {
    if (id < kFirst || id > kLast) return nullptr;
    if (!g_rows_ready) build_rows();
    return g_rows[id - kFirst];
}

__declspec(naked) void hk_goods_row() {
    __asm {
        cmp edi, 389000
        jl orig
        cmp edi, 389008
        jg orig
        push edi
        call our_row
        test eax, eax
        jz orig
        mov ecx, [esp + 4]
        mov [ecx], edi
        mov [ecx + 4], eax
        ret 4
    orig:
        jmp g_goods_orig
    }
}

const wchar_t* item_text(uint32_t id, text::Fmg kind) {
    if (id < static_cast<uint32_t>(kFirst) || id > static_cast<uint32_t>(kLast)) return nullptr;
    const Def& d = kDefs[id - kFirst];
    switch (kind) {
        case text::Fmg::GoodsName: return d.name;
        case text::Fmg::GoodsInfo: return d.info;
        case text::Fmg::GoodsCaption: return d.caption;
        default: return nullptr;
    }
}

const wchar_t* dialog_text(uint32_t id) {
    return (id == kParchmentDialog && g_dialog_fn) ? g_dialog_fn() : nullptr;
}

// ---------------------------------------------------------------- can it be used?
// The game asks this before an item can be selected and used (quick slot, menu). Remastered Seamless Co-op
// answers it for its own items and a few vanilla ones by the session state; the rest is the game's rule.
using CanUseFn = char(__cdecl*)(int32_t, uint32_t, uint32_t, int, int, char);

char __cdecl hk_can_use(int32_t goods, uint32_t chr, uint32_t p3, int type, int weapon, char flag) {
    const auto orig = reinterpret_cast<CanUseFn>(g_can_use_orig);
    const uint32_t me = game::player();
    if (g_can_use_fn && (chr == 0 || chr == me)) {
        const int decision = g_can_use_fn(goods);
        if (decision == 0 || decision == 1) return static_cast<char>(decision);
        if (decision == 2) return orig(goods, chr, p3, type, weapon, flag);
    }
    if (goods >= kFirst && goods <= kLast) return 0;  // never through the game's rule (refId -100..)
    return orig(goods, chr, p3, type, weapon, flag);
}

// ---------------------------------------------------------------- use
char __fastcall hk_use_goods(void* chr, void*, int32_t goods, int32_t p2) {
    const uint32_t me = game::player();
    const bool ours = goods >= kFirst && goods <= kLast;
    if (me && reinterpret_cast<uint32_t>(chr) == me) {
        if (ours) LOG_INFO("[coop] Used co-op item %d", goods);
        const bool handled = g_use_fn ? g_use_fn(goods) : false;
        if (handled || ours) return 1;
    } else if (ours) {
        return 1;
    }
    using Fn = char(__fastcall*)(void*, void*, int32_t, int32_t);
    return reinterpret_cast<Fn>(g_use_orig)(chr, nullptr, goods, p2);
}

}  // namespace

bool install() {
    // xor eax,eax ; push ebx ; mov ebx,[esp+8]
    static const uint8_t kGetter[7] = {0x33, 0xC0, 0x53, 0x8B, 0x5C, 0x24, 0x08};
    g_goods_orig = detour(kGoodsGetter, kGetter, sizeof(kGetter), reinterpret_cast<void*>(&hk_goods_row));
    if (!g_goods_orig) {
        LOG_ERROR("[coop] Goods param getter does not match this build; co-op items unavailable");
        return false;
    }
    // push ebp ; mov ebp,esp ; and esp,-8
    static const uint8_t kCheck[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
    g_can_use_orig = detour(kCanUse, kCheck, sizeof(kCheck), reinterpret_cast<void*>(&hk_can_use));
    if (!g_can_use_orig) {
        LOG_ERROR("[coop] Item use check does not match this build; co-op items unavailable");
        return false;
    }
    if (!swap_pointer(kPlayerVtable + kUseGoodsSlot * 4, kUseGoodsOrig, reinterpret_cast<void*>(&hk_use_goods),
                      &g_use_orig)) {
        LOG_ERROR("[coop] PlayerIns::UseGoods slot does not match this build; co-op items unavailable");
        return false;
    }
    text::set_item_text(&item_text);
    text::set_dialog_text(&dialog_text);
    return true;
}

int count(int32_t goods_id) {
    const uint32_t pgd = game::player_game_data();
    if (!pgd) return 0;
    const uint32_t inv = pgd + 0x1B8;
    uint32_t n = 0, keys = 0, last = 0, a = 0, b = 0;
    if (!read32(inv + 0x110, &n) || !read32(inv + 0x118, &keys) || !read32(inv + 0x13C, &last) ||
        !read32(inv + 0x120, &a) || !read32(inv + 0x124, &b)) {
        return 0;
    }
    const uint32_t end = (last + 1 < n) ? last + 1 : n;
    for (uint32_t i = 0; i < end && i < 4096; ++i) {
        const uint32_t e = (i < keys ? a : b) + i * 0x1C;
        uint32_t cat = 0, id = 0, num = 0;
        if (!read32(e, &cat) || !read32(e + 4, &id) || !read32(e + 8, &num)) return 0;
        if (cat == kGoodsCategory && static_cast<int32_t>(id) == goods_id) return static_cast<int>(num);
    }
    return 0;
}

void give(int32_t goods_id, int n) {
    if (!game::in_game() || n <= 0) return;
    const uint32_t pgd = game::player_game_data();
    if (!pgd) return;
    int slot = -1;
    __try {
        using Fn = int(__stdcall*)(uint32_t, uint32_t, int32_t, int32_t, int32_t, int32_t);
        slot = reinterpret_cast<Fn>(kInventoryAdd)(pgd + 0x1B8, kGoodsCategory, goods_id, n, 1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[coop] Giving item %d failed", goods_id);
        return;
    }
    if (slot == -1) LOG_INFO("[coop] Item %d did not fit in the inventory", goods_id);
}

namespace {

bool g_told = false;  // the "items added" line waits for the player to be back in control
ULONGLONG g_free_since = 0;

// Standing in the world: alive, no event animation (a bonfire's sit, rest and stand-up hold one), no menu, no
// dialog requested or open.
bool in_control() {
    if (!game::in_game()) return false;
    const uint32_t me = game::player();
    uint32_t hp = 0, h0 = 0, h1 = 0, n0 = 0, n1 = 0;
    if (!read32(me + 0x2D4, &hp) || static_cast<int>(hp) <= 0) return false;
    if (!read32(me + 0xFC, &h0) || !read32(me + 0x100, &h1) || !read32(kNoEventAnim[0], &n0) ||
        !read32(kNoEventAnim[1], &n1) || h0 != n0 || h1 != n1) {
        return false;
    }
    uint32_t mm = 0, request = 0, dialog = 0;
    if (!read32(kMenuManVar, &mm) || mm < 0x10000 || !read32(mm + 0x60, &request) || request != 0) return false;
    static const uint32_t kOpen[] = {0x40, 0x4C, 0x78, 0x84, 0x80, 0x50, 0xAC};
    for (uint32_t off : kOpen) {
        uint8_t open = 1;
        if (!read8(mm + off, &open) || open) return false;
    }
    if (read32(mm + 0x9E8, &dialog) && dialog > 0x10000) {
        uint32_t text = 0;
        if (read32(dialog, &text) && text != 0) return false;
    }
    return true;
}

}  // namespace

void tick() {
    if (!g_told) return;
    if (!game::player_game_data()) {
        g_told = false;
        return;
    }
    if (!in_control()) {
        g_free_since = 0;
        return;
    }
    const ULONGLONG t = GetTickCount64();
    if (!g_free_since) g_free_since = t;
    if (t - g_free_since < 1000) return;
    g_told = false;
    text::show(L"Prepare to Co-op items added to your inventory");
}

// A key item (goodsType 1, the Crimson Blossom) sits in the key range and its slot byte at +0x10 is 0.
// The game's erase (0xC09770) refuses that byte, so a rest never withered the stack. Marking the slot
// lets the same erase clear it, including the key-item counter at inventory+0x12C.
void mark_key_item_removable(int32_t goods_id) {
    const uint32_t pgd = game::player_game_data();
    if (!pgd) return;
    const uint32_t inv = pgd + 0x1B8;
    uint32_t n = 0, keys = 0, last = 0, a = 0, b = 0;
    if (!read32(inv + 0x110, &n) || !read32(inv + 0x118, &keys) || !read32(inv + 0x13C, &last) ||
        !read32(inv + 0x120, &a) || !read32(inv + 0x124, &b)) {
        return;
    }
    const uint32_t end = (last + 1 < n) ? last + 1 : n;
    for (uint32_t i = 0; i < end && i < 4096; ++i) {
        const uint32_t base = i < keys ? a : b;
        if (base < 0x10000) continue;
        const uint32_t e = base + i * 0x1C;
        uint32_t cat = 0, id = 0;
        uint8_t flag = 0;
        if (!read32(e, &cat) || !read32(e + 4, &id) || !read8(e + 0x10, &flag)) return;
        if (cat == kGoodsCategory && static_cast<int32_t>(id) == goods_id && flag == 0) {
            *ptr<uint8_t>(e + 0x10) = 1;
        }
    }
}

void blank_slot(uint32_t e) {
    *ptr<int32_t>(e) = -1;
    *ptr<int32_t>(e + 4) = -1;
    *ptr<int32_t>(e + 8) = 0;
    *ptr<uint8_t>(e + 0x10) = 0;
}

bool slot_is_goods(uint32_t base, uint32_t index, int32_t goods_id, uint32_t* slot) {
    if (base < 0x10000) return false;
    const uint32_t e = base + index * 0x1C;
    uint32_t cat = 0, id = 0;
    if (!read32(e, &cat) || !read32(e + 4, &id)) return false;
    if (cat != kGoodsCategory || static_cast<int32_t>(id) != goods_id) return false;
    *slot = e;
    return true;
}

// The game left the stack. Blank whichever copy remains. The key counter is decremented only when the
// copy the game's own lookup reads was still there (a successful erase already decremented it).
int blank_matching_slots(int32_t goods_id) {
    const uint32_t pgd = game::player_game_data();
    if (!pgd) return 0;
    const uint32_t inv = pgd + 0x1B8;
    uint32_t n = 0, keys = 0, last = 0, a = 0, b = 0;
    if (!read32(inv + 0x110, &n) || !read32(inv + 0x118, &keys) || !read32(inv + 0x13C, &last) ||
        !read32(inv + 0x120, &a) || !read32(inv + 0x124, &b)) {
        return 0;
    }
    int cleared = 0;
    const uint32_t end = (last + 1 < n) ? last + 1 : n;
    for (uint32_t i = 0; i < end && i < 4096; ++i) {
        const uint32_t primary_base = i < keys ? a : b;
        const uint32_t mirror_base = i < keys ? b : a;
        uint32_t primary = 0, mirror = 0;
        const bool have_primary = slot_is_goods(primary_base, i, goods_id, &primary);
        const bool have_mirror = a != b && slot_is_goods(mirror_base, i, goods_id, &mirror);
        if (!have_primary && !have_mirror) continue;
        if (have_primary) blank_slot(primary);
        if (have_mirror) blank_slot(mirror);
        if (have_primary && i < keys) {
            uint32_t counter = 0, held = 0;
            if (read32(inv + 0x134, &counter) && counter > 0x10000 && read32(counter, &held) && held > 0 &&
                held < 100000) {
                *ptr<uint32_t>(counter) = held - 1;
            }
        }
        if (have_primary && i == last) {
            --last;
            *ptr<uint32_t>(inv + 0x13C) = last;
        }
        ++cleared;
    }
    return cleared;
}

void remove_all(int32_t goods_id) {
    if (!game::in_game()) return;
    const int before = count(goods_id);
    mark_key_item_removable(goods_id);
    __try {
        using Fn = uint8_t(__stdcall*)(uint32_t, int32_t, int32_t);
        const int stacks = before > 0 ? before : 1;
        reinterpret_cast<Fn>(kRemoveItem)(kGoodsCategory, goods_id, stacks);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[coop] Removing item %d failed", goods_id);
    }
    const int cleared = blank_matching_slots(goods_id);
    const int after = count(goods_id);
    if (before > 0 || cleared > 0 || after > 0) {
        LOG_INFO("[coop] Crimson Blossoms withered (%d held, %d slot(s) cleared, %d left)", before, cleared, after);
    }
}

void grant_missing() {
    if (!game::in_game()) return;
    int given = 0;
    for (int32_t id = kFirst; id <= kLast; ++id) {
        if (id == kBlossom || count(id) > 0) continue;
        give(id, 1);
        if (count(id) <= 0) continue;
        LOG_INFO("[coop] Gave co-op item %d", id);
        ++given;
    }
    if (given) {
        g_told = true;
        g_free_since = 0;
    }
}

void set_can_use(int (*fn)(int32_t)) { g_can_use_fn = fn; }
void set_use_handler(bool (*fn)(int32_t)) { g_use_fn = fn; }
void set_dialog_text(const wchar_t* (*fn)()) { g_dialog_fn = fn; }

}  // namespace coop::items
