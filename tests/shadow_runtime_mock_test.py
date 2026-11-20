"""Compile and execute the actual native shadow light/post-process update functions.

This is a mocked UE4 memory test; not a Windows x86 or in-game test.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'src/shadows.cpp').read_text().split('\n', 2)[2]
fog = (root/'tests/fog_cpp_syntax_compile_test.py').read_text()
stub = fog.split("stub = r'''", 1)[1].split("'''", 1)[0]
stub = stub.replace('bool IsReadable(const void*,size_t);',
                    'bool IsReadable(const void*,size_t);\nbool IsWritable(const void*,size_t);')
stub = stub.replace('namespace refunct {',
                    'namespace refunct {\nvoid SetShadowAmbientColourHex(uint32_t);', 1)
mock = r'''
#include <cassert>
namespace refunct {
void* g_game_module=nullptr;
void (*g_process_event)(void*,void*,void*)=nullptr;
std::wstring g_settings_path;
bool IsReadable(const void* address,size_t) { return address!=nullptr; }
bool IsWritable(const void* address,size_t) { return address!=nullptr; }
void Log(const wchar_t*) {}
void Notify(const std::wstring&) {}
}
ULONGLONG GetTickCount64() { return 5000; }

static bool ignore_setter=false;
static void MockCastShadows(void* component, void*, void* args) {
    if (!ignore_setter) static_cast<uint8_t*>(component)[0] =
        *static_cast<uint8_t*>(args) ? 1 : 0;
}
int main() {
    using namespace refunct;
    // Actual UpdateLight() with a fake directional light BoolProperty byte.
    uint8_t native_shadow=1;
    s_light={};
    s_light.component.ptr=&native_shadow;
    s_light.setter=reinterpret_cast<void*>(1);
    s_light.setter_arg=0;
    s_light.prop_offset=0;
    s_light.byte_offset=0;
    s_light.mask=1;
    g_process_event=&MockCastShadows;
    s_config.visible=false;
    UpdateLight();
    assert(native_shadow==0 && s_light.changed && s_light.original);
    s_config.visible=true;
    UpdateLight();
    assert(native_shadow==1 && !s_light.changed);
    // Failed ProcessEvent must not be counted as successfully hidden.
    s_light={};
    s_light.component.ptr=&native_shadow;
    s_light.setter=reinterpret_cast<void*>(1);
    s_light.setter_arg=0;
    s_light.prop_offset=0;
    s_light.mask=1;
    ignore_setter=true;
    s_config.visible=false;
    UpdateLight();
    assert(native_shadow==1 && !s_light.changed && s_light.retry_after>5000);
    ignore_setter=false;

    // Real UpdatePost() with an in-memory PostNormal.Settings substitute.
    struct PostStorage { uint8_t flag; LinearColor tint; } post{};
    post.tint={0.3f,0.4f,0.5f,1.0f};
    const LinearColor original=post.tint;
    s_post={};
    s_post.volume.ptr=&post;
    s_post.settings_offset=0;
    s_post.tint_offset=static_cast<int32_t>(offsetof(PostStorage,tint));
    s_post.override_offset=static_cast<int32_t>(offsetof(PostStorage,flag));
    s_post.override_mask=1;
    s_config.tint_enabled=true;
    s_config.tint_rgb=0xFF0000;
    UpdatePost();
    assert(s_post.changed && (post.flag&1)==1);
    assert(post.tint.r>0.99f && post.tint.g<0.01f && post.tint.b<0.01f);
    s_config.tint_enabled=false;
    UpdatePost();
    assert(!s_post.changed && (post.flag&1)==0);
    assert(Similar(post.tint,original));
    return 0;
}
'''
with tempfile.TemporaryDirectory() as temp:
    exe=Path(temp)/'shadow_mock'
    cpp=Path(temp)/'shadow_mock.cpp'
    cpp.write_text(stub+'\n'+source+'\n'+mock)
    subprocess.run(['g++','-std=c++20','-O1','-ffunction-sections','-fdata-sections',
                    str(cpp),'-Wl,--gc-sections','-o',str(exe)],check=True,timeout=30)
    subprocess.run([str(exe)],check=True,timeout=10)
print('Shadow native update runtime mock: visibility, restoration, retry and tint passed')
