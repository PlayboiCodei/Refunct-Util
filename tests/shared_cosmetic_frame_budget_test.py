"""Exercise the production cosmetic budget header with a fake 1 MHz Win32 QPC.

The test controls QPC deterministically; it does not claim to measure in-game FPS.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which('g++') or shutil.which('clang++')
if not compiler:
    print('SKIP: portable C++20 compiler unavailable')
    raise SystemExit(0)

code = r'''
#include <cassert>
#include "refunct/colour_work_budget.hpp"
int main() {
    using namespace refunct;
    Counter() = 1000000;
    // Without an enclosing game-tick scope, old standalone budgets still work.
    {
        ColourWorkBudget budget(1000);
        Counter() += 900;
        assert(!budget.expired());
        Counter() += 101;
        assert(budget.expired());
    }
    // 60 FPS -> 1333 us combined budget. Charge only scoped cosmetic work.
    {
        ScopedColourFrameBudget frame(1.0f / 60.0f);
        assert(colour_work_detail::frame.active);
        assert(colour_work_detail::frame.remaining_ticks >= 1300);
        {
            ColourWorkBudget first(1000);
            Counter() += 900;
            assert(!first.expired());
        }
        // Non-cosmetic native game work is not charged.
        Counter() += 20000;
        assert(colour_work_detail::frame.remaining_ticks >= 400);
        {
            ColourWorkBudget second(1100);
            Counter() += 400;
            assert(!second.expired());
            Counter() += 40;
            assert(second.expired());
        }
        assert(colour_work_detail::frame.remaining_ticks < 0);
        {
            ColourWorkBudget third(1100);
            assert(third.expired()); // later cosmetic queues yield after one item
        }
    }
    assert(!colour_work_detail::frame.active);
    {
        ScopedColourFrameBudget high_fps(1.0f / 240.0f);
        assert(colour_work_detail::frame.remaining_ticks == 400);
    }
    {
        ScopedColourFrameBudget stalled(0.2f);
        assert(colour_work_detail::frame.remaining_ticks <= 1400);
    }
    // Existing standalone behaviour remains valid after the frame closes.
    {
        ColourWorkBudget budget(1000);
        Counter() += 800;
        assert(!budget.expired());
    }
}
'''
with tempfile.TemporaryDirectory() as folder:
    src = Path(folder) / 'budget_test.cpp'
    binary = Path(folder) / 'budget_test'
    src.write_text(code, encoding='utf-8')
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'tests/win32_stub'),
                    '-I', str(root / 'include'), str(src), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: shared cosmetic frame budget enforces aggregate deadlines')
