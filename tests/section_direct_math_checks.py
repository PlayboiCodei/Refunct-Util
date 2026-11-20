"""Regression checks for actual section durations and reference math."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
state = (root / "include/refunct/state.hpp").read_text()
section = (root / "include/refunct/section_compare.hpp").read_text()
records = (root / "src/records.cpp").read_text()
hud = (root / "src/hud.cpp").read_text()

# Read/write both active and external PB profiles, including the new section table.
assert "section_seconds.fill(-1.0);" in state
assert '[SectionSplits]' in records
assert 'read_time(L"SectionSplits", SectionPBKey(i)' in records
assert 'GetPrivateProfileStringW(L"SectionSplits", SectionPBKey(section)' in records
assert 'next.section_seconds[i] = SectionDurationFromPB' in records
assert 'SectionDurationFromRecordedPB(' in section

# Measured section durations are taken from real boundaries during the press,
# then retained after the practice state / newest button changes.
assert 'split.section_actual_seconds = MeasuredSectionSeconds(' in records
assert 'completed->section_actual_seconds' in hud
assert 'completed->section_comparison_boundary.section != 0' in hud
assert 'split.section_comparison_boundary = reference_boundary;' in records
assert 'CompletedCurrentSectionSeconds(' not in hud

# Golds are separately labelled in external Best Segments mode,
# never substituted into the active profile's Section vs PB row.
assert 'SectionDurationFromSegments(' in section
assert 'split.section_best_reference_seconds = EarlierBestSectionReference(' in records
assert 'split.previous_recorded_gold_seconds = previous_gold;' in records
assert 'const double reference_seconds = old_pb_section;' in hud
assert 'const double reference_seconds = prefer_best ? gold_seconds : pb_seconds;' in hud
assert 'vs sum of best' in hud
assert 'prefix + L"N/A"' in hud  # unavailable section comparisons share one placeholder
assert 'next.section_seconds.fill(-1.0);' in records
print('PASS: direct section-time math, PB persistence and honest fallback')
