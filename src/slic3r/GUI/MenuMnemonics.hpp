#pragma once

class wxMenu;

namespace Slic3r { namespace GUI {

// Gives every item of `menu` (submenus included) a unique '&' mnemonic taken from its
// translated label, so translators never place ampersands and no two items of one menu
// compete for a letter. Idempotent: earlier mnemonics are stripped before reassignment,
// which makes it safe to run each time a menu is rebuilt or popped up. No-op on macOS,
// where menus have no mnemonics.
void assign_menu_mnemonics(wxMenu* menu);

}} // namespace Slic3r::GUI
