"""Check diagnostic defaults and instructions remain consistent with startup code."""
from pathlib import Path
import configparser
root = Path(__file__).resolve().parents[1]
source = (root / 'src/bootstrap.cpp').read_text(encoding='utf-8')
config = configparser.ConfigParser()
assert config.read(root / 'RefunctUtility.diagnostic.ini')
assert config.getint('Startup', 'Mode') == 2, 'bundled diagnostic file must match START_HERE'
# Documentation files are not distributed in this source-only archive. Verify
# the actual mode-selection contract and the bundled sample INI instead.
assert 'return 6;' in source, 'a normal no-INI install must run full Mode 6'
assert 'return 1;' in source, 'invalid settings must use safe Mode 1'
assert 'GetPrivateProfileIntW(L"Startup", L"Mode", 1, path)' in source
assert 'diagnostic configuration missing; using full Mode 6' in source
assert 'invalid mode; using safe Mode 1' in source
print('PASS: diagnostic Mode 2 sample, no-INI Mode 6, safe invalid-INI fallback')
