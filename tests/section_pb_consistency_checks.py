"""Guard strict Section vs PB semantics after the math audit."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
hud = (root / 'src/hud.cpp').read_text(encoding='utf-8')
records = (root / 'src/records.cpp').read_text(encoding='utf-8')
section = (root / 'include/refunct/section_compare.hpp').read_text(encoding='utf-8')
section_compat = (root / 'include/refunct/features/section_compare.hpp').read_text(encoding='utf-8')
assert section == section_compat, 'Duplicate compatibility header drifted'

# Prefer PB cumulative endpoints; use saved sections only if they are absent.
assert 'const double derived = SectionDurationFromPB(cumulative_button_seconds, boundary);' in section
assert 'if (derived >= 0.0) return derived;' in section
assert 'DisplayedDurationSeconds(start, end)' in section
assert 'return DisplayedDurationSeconds(0.0, stored);' in section
assert 'full.previous_end_button != boundary.previous_end_button' in section

# No fabricated zero-length references, including independent best segments.
assert 'duration <= 0.0' in section
assert 'end_ms > start_ms' in section

# Active PB and external-file PB mode NEVER substitute independent gold records.
assert 'const double reference_seconds = old_pb_section;' in hud
assert 'const double reference_seconds = prefer_best ? gold_seconds : pb_seconds;' in hud
assert 'const double reference_seconds = use_pb' not in hud
assert 'has_pb ? pb_seconds : gold_seconds' not in hud
assert 'current_section_seconds - reference_seconds' in hud
assert 'split.cumulative_pb_reference_seconds' in hud
assert 'DisplayedTimestampDeltaSeconds(split.run_seconds, best)' in hud
assert 'SectionDurationFromRecordedPB(' in records
print('PASS: Section vs PB uses consistent recorded PB references only')
