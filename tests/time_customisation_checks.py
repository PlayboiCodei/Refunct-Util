"""Time-of-day menu / native-clock safety checks (not in-game validation)."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
menu = (root/'src/menu.cpp').read_text(encoding='utf-8')
code = (root/'src/time_of_day.cpp').read_text(encoding='utf-8')
header = (root/'include/refunct/features/time_of_day.hpp').read_text(encoding='utf-8')
records = (root/'src/records.cpp').read_text(encoding='utf-8')
state = (root/'include/refunct/state.hpp').read_text(encoding='utf-8')
for source in ('CMakeLists.txt','include/refunct/internal_api.hpp'):
    assert 'time_of_day' in (root/source).read_text(encoding='utf-8'), source
assert 'UpdateCustomTimeOfDay(player);' in (root/'src/game_tick.cpp').read_text(encoding='utf-8')
assert 'CustomisationTime' in state
assert 'case Page::CustomisationTime:' in menu
assert 'else if (i == 8) SetPage(Page::CustomisationTime);' in menu
assert 'else if (i == 16) ResetWorldCustomisation();' in menu
assert 'AdjustCustomTimeMinutes(key == VK_RIGHT ? +1 : -1);' in menu
assert 'SetCustomTimeOfDayEnabled(false);' in menu
assert '24-hour time' in menu
assert 'CurrentMinute' in code and 'TimeSpeed' in code and 'TimeOfDay' in code
assert 'FindClassPropertyOffset(klass, property)' in code
assert 'L"IntProperty"' in code and 'L"FloatProperty"' in code
assert 'objects->objects[index].object != object' in code
assert 'serial_number == s_clock.serial' in code
assert 's_clock.original_minute' in code and 's_clock.original_speed' in code
assert 'kDiscoveryBudgetMs = 2' in code and 'kApplyIntervalMs = 100' in code
assert 'kSaveDelayMs = 350' in code
assert 'TimeCustomisation' in code and 'MinuteOfDay' in code
assert 'CheatMask(CheatBit::TimeOfDay)' in records
assert 'set(CheatBit::TimeOfDay, IsCustomTimeOfDayEnabled());' in records
assert 'void UpdateCustomTimeOfDay(MyCharacter* player);' in header
print('Time customisation menu, persistence, native validation, cosmetic tracking passed')
