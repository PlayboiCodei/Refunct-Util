"""Compile production shadows.cpp with a small fake Win32/UE4 interface.

No Win32 x86 ABI or in-game claim is made by this test.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
code = (root/'src/shadows.cpp').read_text()
assert code.startswith('#include "refunct/internal_api.hpp"\n#include <commdlg.h>\n')
body = code.split('\n', 2)[2]
# Use the exact Win32/UE4 test declarations already exercised by the fog module.
fog = (root/'tests/fog_cpp_syntax_compile_test.py').read_text()
stub = fog.split("stub = r'''", 1)[1].split("'''", 1)[0]
stub = stub.replace('bool IsReadable(const void*,size_t);',
                    'bool IsReadable(const void*,size_t);\nbool IsWritable(const void*,size_t);')
stub = stub.replace('namespace refunct {', 'namespace refunct {\nvoid SetShadowAmbientColourHex(uint32_t);', 1)
with tempfile.TemporaryDirectory() as temp:
    path = Path(temp)/'shadow_syntax.cpp'
    path.write_text(stub + '\n' + body)
    subprocess.run(['g++', '-std=c++20', '-fsyntax-only', '-Wall', '-Wextra',
                    '-Werror', str(path)], check=True, timeout=30)
print('Shadows C++20 syntax test passed (fake Win32/UE4 ABI)')
