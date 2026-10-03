#pragma once

// Text for the seamless co-op module, through the game's own message system: the FMG lookup every menu,
// item and message uses (0xD175E0) answers our ids with our strings, and our messages appear in the game's
// action box at the bottom of the screen (the one "Phantom X summoned" uses), which fades by itself.

#include <cstdint>
#include <string>

namespace coop::text {

// Which kind of item text an FMG lookup asks for, by the FMG it searches.
enum class Fmg { Other, GoodsName, GoodsInfo, GoodsCaption };

bool install();
// Item text provider: (id, kind) -> text or null. Set by the items module.
void set_item_text(const wchar_t* (*fn)(uint32_t id, Fmg kind));
// Dialog text provider for our ids in any other FMG (the yes/no dialogs of items): id -> text or null.
void set_dialog_text(const wchar_t* (*fn)(uint32_t id));
// Shows a line in the action box at the bottom of the screen (no button press needed). Game thread only.
void show(const std::wstring& line);
std::wstring widen(const std::string& utf8);

// While a seamless session is active (and for a minute after it ends) the game's own multiplayer notices
// that do not fit seamless play are dropped: Remastered Seamless Co-op's list of summon texts in the action box
// ("You are being summoned...", "Phantom X summoned", ...), and the modal boxes about phantoms returning home
// or being sent home. Called every frame by the session.
void set_seamless_active(bool active);
// Every frame on the game thread: opens an information box the game asked for and nothing claimed.
void tick();

}  // namespace coop::text
