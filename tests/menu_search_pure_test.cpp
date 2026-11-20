#include "refunct/menu_search.hpp"
#include <cassert>
#include <string>

int main() {
    using refunct::MenuSearchTerms;
    assert(MenuSearchTerms(L"water").matches(L"World / Water", L"Remove water: OFF"));
    assert(MenuSearchTerms(L"HUD BUTTON").matches(L"HUD / Side HUD Elements", L"Current button number: ON"));
    assert(MenuSearchTerms(L"button hud").matches(L"HUD / Side HUD Elements", L"Current button number: ON"));
    assert(MenuSearchTerms(L"section pb").matches(L"HUD / Splits / Popup Rows", L"Section Time vs PB: Time"));
    assert(MenuSearchTerms(L"  pB    splits  ").matches(L"HUD / Comparisons", L"Other profile reference: PB splits"));
    assert(!MenuSearchTerms(L"ghost button").matches(L"World / Water", L"Remove Water"));
    assert(!MenuSearchTerms(L"   ").matches(L"HUD / Side HUD Elements", L"FPS"));
    assert(!MenuSearchTerms(L"").matches(L"Settings", L"Save personal bests"));
}
