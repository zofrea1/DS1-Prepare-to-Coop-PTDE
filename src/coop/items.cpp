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
constexpr uint32_t kAddItem = 0x00D664C0;       // Lua AddInventoryItem(id, category, count), stdcall
constexpr uint32_t kRemoveItem = 0x00D6D670;    // stdcall(category, id, max; 0 = all)
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
    {-100, 40, 2008, L"Blessed Eye Orb", L"Create a seamless co-operative session",
     L"A small, pale eye orb, warm in the hand.\n\nUse it to create a seamless co-operative session. Players\n"
     L"who share your password may then join your world at any\ntime, wherever you are."},
    {-99, 80, 2010, L"Crystal Eye Orb", L"Join a seamless co-operative session",
     L"A small blue eye orb of clouded crystal that hums faintly.\n\nUse it to join the world of a player who "
     L"shares your\npassword. You travel in your own body and keep all\nthat you gain."},
    {-98, 81, 2002, L"Chaos Eye Orb", L"Invade a seamless co-operative session",
     L"A small eye orb, red with heat and spite.\n\nUse it to invade, as an evil spirit, a seamless\n"
     L"co-operative session that welcomes invaders."},
    {-97, 0, 2132, L"Abyssal Eye Orb", L"Disband a seamless session",
     L"A small black eye orb whose gaze never wavers.\n\nA guest uses it to return home. A host uses it to\n"
     L"disband the session."},
    {-96, 0, 2007, L"Ominous Tome", L"Change the world rules",
     L"Loose pages of faded writing that describe a rite to\nlay bare one's true nature before one's allies.\n\n"
     L"The host changes the rules of the world with each use."},
    {-95, 0, 2129, L"Dried Fingers", L"Taunt invaders into joining",
     L"Withered fingers.\n\nThe host uses them to taunt invaders into joining."},
    {-94, 0, 2037, L"Cursed Pendant", L"Cause disasters in one's own world",
     L"An old pendant, scratched and dull. Some say it was\ntaken from the tomb of the Gravelord.\n\n"
     L"The host uses it to bring disaster upon their own world."},
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
    __try {
        using Fn = uint32_t(__stdcall*)(int32_t, uint32_t, int32_t);
        reinterpret_cast<Fn>(kAddItem)(goods_id, kGoodsCategory, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[coop] Giving item %d failed", goods_id);
    }
}

void remove_all(int32_t goods_id) {
    if (!game::in_game()) return;
    __try {
        using Fn = uint8_t(__stdcall*)(uint32_t, int32_t, int32_t);
        reinterpret_cast<Fn>(kRemoveItem)(kGoodsCategory, goods_id, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[coop] Removing item %d failed", goods_id);
    }
}

void grant_missing() {
    if (!game::in_game()) return;
    for (int32_t id = kFirst; id <= kLast; ++id) {
        if (id == kBlossom || count(id) > 0) continue;
        give(id, 1);
        LOG_INFO("[coop] Gave co-op item %d", id);
    }
}

void set_can_use(int (*fn)(int32_t)) { g_can_use_fn = fn; }
void set_use_handler(bool (*fn)(int32_t)) { g_use_fn = fn; }
void set_dialog_text(const wchar_t* (*fn)()) { g_dialog_fn = fn; }

}  // namespace coop::items
