#pragma once

// Text for the seamless co-op module, through the game's own message system: the FMG lookup every menu,
// item and message uses (0xD175E0) answers our ids with our strings.

#include <cstdint>
#include <string>

namespace coop::text {

// Our message ids (no FMG of the game uses them).
constexpr uint32_t kMessageBase = 99900000;  // ring of on-screen messages
constexpr int kMessageSlots = 16;

// Which kind of item text an FMG lookup asks for, by the FMG it searches.
enum class Fmg { Other, GoodsName, GoodsInfo, GoodsCaption };

bool install();
// Item text provider: (id, kind) -> text or null. Set by the items module.
void set_item_text(const wchar_t* (*fn)(uint32_t id, Fmg kind));
// Dialog text provider for our ids in any other FMG (the yes/no dialogs of items): id -> text or null.
void set_dialog_text(const wchar_t* (*fn)(uint32_t id));
// Shows a line in the game's information list (top left, like "X has been summoned"). Game thread only.
void show(const std::wstring& line);
std::wstring widen(const std::string& utf8);

}  // namespace coop::text
