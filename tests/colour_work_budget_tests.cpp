#include "refunct/colour_work_budget.hpp"
#include <cassert>
#include <limits>

int main() {
    Counter() = 100;
    refunct::ColourWorkBudget budget(700);
    assert(!budget.expired());
    Counter() += 699;
    assert(!budget.expired());
    ++Counter();
    assert(budget.expired());
    // New frame gets its own full time budget.
    refunct::ColourWorkBudget frame2(1000);
    assert(!frame2.expired());
    Counter() += 1000;
    assert(frame2.expired());
}
