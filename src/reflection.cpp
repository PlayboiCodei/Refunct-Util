#include "refunct/internal_api.hpp"

namespace refunct {
namespace {
// FName indices and instance numbers are immutable within a loaded process.
// Cache only successful conversions; fixed size prevents long-session growth.
struct NameCacheEntry { uint64_t key = 0; std::wstring value; };
thread_local std::array<NameCacheEntry, 2048> s_names{};
struct FieldIdentity {
    void* object = nullptr;
    void* klass = nullptr;
    void* outer = nullptr;
    FNameView name{};
    int32_t index = -1;
    int32_t serial = 0;
};
// UE can recycle addresses. Check the registry slot and serial before using
// a cached class or field, and fall back to traversal if identity is uncertain.
bool MetadataIdentity(void* object, FieldIdentity& out) {
    if (!g_game_module || !IsReadable(object, sizeof(UObjectHeader))) return false;
    const auto* header = static_cast<const UObjectHeader*>(object);
    const auto* objects = reinterpret_cast<const GlobalObjectArray*>(
        reinterpret_cast<uintptr_t>(g_game_module) + kGlobalObjectArrayRva);
    if (!IsReadable(objects, sizeof(*objects)) || !objects->objects ||
        header->index < 0 || header->index >= objects->num_elements ||
        objects->num_elements > objects->max_elements || objects->num_elements > 500000) return false;
    const auto* slot = objects->objects + header->index;
    if (!IsReadable(slot, sizeof(*slot)) || slot->object != object ||
        (static_cast<uint32_t>(slot->flags) & 0x30000000u)) return false;
    out = {object, header->class_ptr, header->outer, header->name, header->index, slot->serial_number};
    return true;
}
bool SameIdentity(const FieldIdentity& saved) {
    FieldIdentity live{};
    return MetadataIdentity(saved.object, live) && live.index == saved.index &&
        live.serial == saved.serial && live.klass == saved.klass && live.outer == saved.outer &&
        live.name.index == saved.name.index && live.name.number == saved.name.number;
}
struct FieldCacheEntry {
    FieldIdentity klass{}, field{};
    std::wstring name;
    void* root_children = nullptr;
    void* root_super = nullptr;
    int limit = 0;
};
thread_local std::array<FieldCacheEntry, 1024> s_fields{};
} // namespace

void* FindCachedClassField(void* klass, const wchar_t* name, int limit) {
    if (!name || !IsReadable(klass, sizeof(UStructHeader))) return nullptr;
    size_t hash = reinterpret_cast<uintptr_t>(klass) >> 4;
    for (const wchar_t* c = name; *c; ++c) hash = hash * 16777619u ^ static_cast<size_t>(*c);
    auto& entry = s_fields[hash % s_fields.size()];
    const auto* root = static_cast<const UStructHeader*>(klass);
    if (entry.klass.object == klass && entry.limit == limit && entry.name == name &&
        entry.root_children == root->children && entry.root_super == root->super_struct &&
        SameIdentity(entry.klass) && SameIdentity(entry.field)) return entry.field.object;
    void* current = klass;
    for (int depth = 0; depth < 16 && IsReadable(current, sizeof(UStructHeader)); ++depth) {
        const auto* cls = static_cast<const UStructHeader*>(current);
        void* field = cls->children;
        for (int count = 0; count < limit && IsReadable(field, sizeof(UFieldHeader)); ++count) {
            const auto* child = static_cast<const UFieldHeader*>(field);
            if (UnrealName(child->object.name) == name) {
                FieldIdentity class_id{}, field_id{};
                if (MetadataIdentity(klass, class_id) && MetadataIdentity(field, field_id))
                    entry = {class_id, field_id, name, root->children, root->super_struct, limit};
                return field;
            }
            field = child->next;
        }
        current = cls->super_struct;
    }
    return nullptr; // Do not cache misses while reflection metadata is loading.
}
std::wstring UnrealName(FNameView name) {
    if (!g_fname_append || !g_memory_free) return L"";
    const uint64_t key = (static_cast<uint64_t>(name.index) << 32) | name.number;
    const size_t bucket = (static_cast<size_t>(name.index) * 2654435761u ^ name.number) % s_names.size();
    auto& entry = s_names[bucket];
    if (entry.key == key && !entry.value.empty()) return entry.value;
    FStringView str{};
    g_fname_append(&name, &str);
    std::wstring result;
    if (str.data && str.count > 0 && str.count < 1024 && IsReadable(str.data, static_cast<size_t>(str.count) * sizeof(wchar_t))) {
        result.assign(str.data, str.data + str.count - 1);
    }
    if (str.data) g_memory_free(str.data);
    if (!result.empty()) entry = {key, result};
    return result;
}

void* FindRestartFunction(void* instance) {
    if (!IsReadable(instance, sizeof(UObjectHeader))) return nullptr;
    auto* klass = static_cast<UObjectHeader*>(instance)->class_ptr;
    return FindCachedClassField(klass, L"RestartGame", 2048);
}

// Reflected property offsets are version-specific; never assume the layout
// of a Blueprint actor. Only the native UProperty::Offset_Internal is fixed.
int32_t FindClassPropertyOffset(void* klass, const wchar_t* field_name) {
    constexpr size_t kOffsetInternal = 0x3C;
    void* field = FindCachedClassField(klass, field_name, 4096);
    if (!IsReadable(field, kOffsetInternal + sizeof(int32_t))) return -1;
    const int32_t value = *reinterpret_cast<const int32_t*>(
        static_cast<const uint8_t*>(field) + kOffsetInternal);
    return value >= 0 && value < 0x4000 ? value : -1;
}

// Resolve a native/Blueprint float property only if its value is plausible.
// A failed lookup is inert: never write to an unverified object offset.
TunedFloatProperty ResolveTunedFloat(void* object, const wchar_t* property,
                                    float min_value, float max_value) {
    TunedFloatProperty field{};
    if (!IsReadable(object, sizeof(UObjectHeader))) return field;
    void* klass = static_cast<const UObjectHeader*>(object)->class_ptr;
    if (!IsReadable(klass, sizeof(UStructHeader))) return field;
    const int32_t offset = FindClassPropertyOffset(klass, property);
    if (offset < 0) return field;
    auto* address = reinterpret_cast<float*>(static_cast<uint8_t*>(object) + offset);
    if (!IsReadable(address, sizeof(float)) || !std::isfinite(*address) ||
        *address < min_value || *address > max_value) return field;
    field.address = address;
    field.original = *address;
    field.last_written = *address;
    return field;
}

// Avoid recursively multiplying a value already written on an earlier tick.
// When switching back to 100%, restore only if our value is still present;
// otherwise let the game retain a value it changed independently.

} // namespace refunct
