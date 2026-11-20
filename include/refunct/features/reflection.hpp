#pragma once
// Internal functions owned by this feature's .cpp file.
#include "refunct/state.hpp"
namespace refunct {
std::wstring UnrealName(FNameView name);
void* FindCachedClassField(void* klass, const wchar_t* name, int limit);
void* FindRestartFunction(void* instance);
int32_t FindClassPropertyOffset(void* klass, const wchar_t* field_name);
TunedFloatProperty ResolveTunedFloat(void* object, const wchar_t* property,
                                    float min_value, float max_value);
} // namespace refunct
