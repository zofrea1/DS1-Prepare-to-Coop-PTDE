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
constexpr uint32_t kPlayerVtable = 0x0118A5D4;  // PlayerIns
constexpr int kUseGoodsSlot = 128;              // UseGoods(goods id, ?) thiscall, ret 8
constexpr uint32_t kUseGoodsOrig = 0x00E50C70;
constexpr uint32_t kAddItem = 0x00D664C0;       // Lua AddInventoryItem(id, category, count), stdcall
constexpr uint32_t kGoodsCategory = 0x40000000;
constexpr int kCount = kLast - kFirst + 1;
constexpr int kRowSize = 0x5C;  // EQUIP_PARAM_GOODS_ST

uint8_t g_rows[kCount][kRowSize];
bool g_rows_ready = false;
void* g_goods_orig = nullptr;
uint32_t g_use_orig = 0;
void (*g_use_fn)(int32_t) = nullptr;
bool (*g_block_fn)(int32_t) = nullptr;

struct Def {
    uint16_t icon;
    int16_t sort;
    const wchar_t* name;
    const wchar_t* info;
    const wchar_t* caption;
};

// Icons of existing items (2000 White Sign Soapstone, 2010 Blue Eye Orb, 2002 Red Eye Orb, 2132 Black Eye
// Orb, 2009 Book of the Guilty, 2129 Dried Finger, 2037 Pendant, 2030 Transient Curse, 2041 Indictment).
const Def kDefs[kCount] = {
    {2000, 1, L"Seamless Host Orb", L"Open your world to co-op players",
     L"A pale orb that shows one's own world from afar.\n\nUse it to open your world to the players who share your\n"
     L"seamless co-op password. They may join at any time,\nwherever you are."},
    {2010, 2, L"Seamless Join Orb", L"Travel to a co-op player's world",
     L"A blue orb that looks into the world of a friend.\n\nUse it to join the world of a player who shares your\n"
     L"seamless co-op password. You keep your own body, items\nand progress, and return home with them."},
    {2002, 3, L"Seamless Invasion Orb", L"Invade a seamless co-op world",
     L"A red orb clouded with spite.\n\nUse it to invade a random seamless co-op world that\n"
     L"accepts invaders. Defeat its players or be sent home."},
    {2132, 4, L"Seamless Return Orb", L"Leave the co-op session",
     L"A black orb that remembers the way home.\n\nA guest returns to their own world; a host closes the\n"
     L"session and sends everyone home."},
    {2009, 5, L"Tome of World Rules", L"Host: allow or forbid invaders",
     L"A bound list of oaths for one's own world.\n\nThe host decides whether evil spirits may invade the\n"
     L"session. Each use switches between allowing and forbidding."},
    {2129, 6, L"Withered Finger", L"Host: call invaders to your world",
     L"A finger dried to the bone.\n\nWhile the session allows invaders, using it marks the\n"
     L"world so that invaders find it first."},
    {2037, 7, L"Pendant of Misfortune", L"Host: enemies grow stronger",
     L"An old pendant that draws calamity.\n\nUntil someone rests at a bonfire, the enemies of the host's\n"
     L"world fight as if one more player were present."},
    {2030, 8, L"Crimson Blossom", L"Carried curse: less HP, more souls",
     L"A red blossom rooted in the darksign.\n\nWhile it is used, maximum HP falls and every soul earned\n"
     L"grows. It wilts at the next bonfire rest."},
    {2041, 9, L"Writ of Clemency", L"Restores certain fallen NPCs",
     L"A writ that pardons the departed.\n\nRestores some NPCs whose fate went wrong while visiting\n"
     L"other worlds (the same repair seamless co-op applies on joining)."},
};

void build_rows() {
    // Template: White Sign Soapstone (goods 100).
    uint32_t out[2] = {0, 0};
    void* orig = g_goods_orig;
    uint32_t* pout = out;
    __asm {
        push edi
        mov edi, 100
        push pout
        call orig
        pop edi
    }
    const uint8_t* tpl = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(out[1]));
    for (int i = 0; i < kCount; ++i) {
        uint8_t* r = g_rows[i];
        if (tpl) {
            std::memcpy(r, tpl, kRowSize);
        } else {
            std::memset(r, 0, kRowSize);
        }
        const int32_t ref = -1;
        std::memcpy(r + 0x00, &ref, 4);              // refId: no behaviour (use is caught in UseGoods)
        const int32_t none = -1;
        std::memcpy(r + 0x04, &none, 4);             // sfxVariationId
        const float weight = 0.0f;
        std::memcpy(r + 0x08, &weight, 4);
        const int32_t zero = 0;
        std::memcpy(r + 0x0C, &zero, 4);             // basicPrice
        std::memcpy(r + 0x10, &zero, 4);             // sellValue
        std::memcpy(r + 0x18, &none, 4);             // replaceItemId
        const int32_t sort = 900000 + kDefs[i].sort;
        std::memcpy(r + 0x1C, &sort, 4);             // sortId
        std::memcpy(r + 0x20, &none, 4);             // qwcId
        std::memcpy(r + 0x24, &none, 4);             // yesNoDialogMessageId
        std::memcpy(r + 0x28, &none, 4);             // magicId
        std::memcpy(r + 0x2C, &kDefs[i].icon, 2);    // iconId
        const int16_t max_num = 1;
        std::memcpy(r + 0x36, &max_num, 2);          // maxNum
        r[0x38] = 0;                                 // consumeHeroPoint
        r[0x3A] = 0;                                 // goodsType: normal
        r[0x3B] = 0;                                 // refCategory
        r[0x3E] = 6;                                 // goodsUseAnim (like the soapstones)
        r[0x3F] = 0;                                 // no menu
        // enable_live, enable_gray, enable_white, enable_black, enable_multi, isEquip; not consumed
        r[0x44] = 0x01 | 0x02 | 0x04 | 0x08 | 0x10 | 0x40;
        // isAutoEquip, isOnlyOne; cannot be dropped or deposited
        r[0x45] = 0x01 | 0x04;
        r[0x46] = 0;
        std::memset(r + 0x50, 0, 12);                // vagrant item lots
    }
    g_rows_ready = true;
    LOG_INFO("[coop] Co-op item rows ready (template %s)", tpl ? "goods 100" : "blank");
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

char __fastcall hk_use_goods(void* chr, void*, int32_t goods, int32_t p2) {
    const uint32_t me = game::player();
    if (me && reinterpret_cast<uint32_t>(chr) == me) {
        if (goods >= kFirst && goods <= kLast) {
            LOG_INFO("[coop] Used co-op item %d", goods);
            if (g_use_fn) g_use_fn(goods);
            return 1;
        }
        if (g_block_fn && g_block_fn(goods)) return 0;
    }
    using Fn = char(__fastcall*)(void*, void*, int32_t, int32_t);
    return reinterpret_cast<Fn>(g_use_orig)(chr, nullptr, goods, p2);
}

}  // namespace

bool install() {
    // xor eax,eax ; push ebx ; mov ebx,[esp+8]
    static const uint8_t k[7] = {0x33, 0xC0, 0x53, 0x8B, 0x5C, 0x24, 0x08};
    g_goods_orig = detour(kGoodsGetter, k, sizeof(k), reinterpret_cast<void*>(&hk_goods_row));
    if (!g_goods_orig) {
        LOG_ERROR("[coop] Goods param getter does not match this build; co-op items unavailable");
        return false;
    }
    if (!swap_pointer(kPlayerVtable + kUseGoodsSlot * 4, kUseGoodsOrig, reinterpret_cast<void*>(&hk_use_goods),
                      &g_use_orig)) {
        LOG_ERROR("[coop] PlayerIns::UseGoods slot does not match this build; co-op items unavailable");
        return false;
    }
    text::set_item_text(&item_text);
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

void grant_missing() {
    if (!game::in_game() || !game::own_world()) return;
    for (int32_t id = kFirst; id <= kLast; ++id) {
        if (count(id) > 0) continue;
        uint32_t ok = 0;
        __try {
            using Fn = uint32_t(__stdcall*)(int32_t, uint32_t, int32_t);
            ok = reinterpret_cast<Fn>(kAddItem)(id, kGoodsCategory, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG_ERROR("[coop] Giving item %d failed", id);
            return;
        }
        LOG_INFO("[coop] Gave co-op item %d (%u)", id, ok);
    }
}

void set_use_handler(void (*fn)(int32_t)) { g_use_fn = fn; }
void set_block_filter(bool (*fn)(int32_t)) { g_block_fn = fn; }

}  // namespace coop::items
