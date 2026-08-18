#ifndef XGPU_PLUME_RECORDED_RESOURCES_H
#define XGPU_PLUME_RECORDED_RESOURCES_H

#include <cstdint>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xgpu {
namespace plume {

/* Stable, pointer-free identity and sampling metadata for one hosted Xbox
 * texture generation. A recorded batch may carry this across threads; the
 * render owner resolves it against RecordedTextureVersions at replay time. */
struct RecordedTextureBinding {
    uint32_t guest = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 0;
    uint32_t levels = 0;
    uint32_t dimensionality = 0;
    uint32_t bytes = 0;
    uint32_t format = 0;
    uint64_t version = 0;
    uint32_t cube = 0;
    uint32_t unnormalizedCoords = 0;

    bool valid() const { return guest != 0; }

    bool sameResource(const RecordedTextureBinding &other) const
    {
        return guest == other.guest && width == other.width &&
               height == other.height && depth == other.depth &&
               levels == other.levels &&
               dimensionality == other.dimensionality &&
               bytes == other.bytes && format == other.format &&
               version == other.version && cube == other.cube &&
               unnormalizedCoords == other.unnormalizedCoords;
    }
};

/* Resource command payload owned by the recording. `pixels` is a snapshot;
 * it never borrows the caller's guest/temporary upload pointer. Gate 3 routes
 * this command through the render owner. Gate 2 records it while retaining the
 * existing synchronous upload for exact disabled-path behavior. */
struct RecordedTextureUpload {
    RecordedTextureBinding binding;
    uint32_t pitch = 0;
    std::vector<uint8_t> pixels;
};

/* Current resource by guest identity plus superseded generations that older
 * recordings can still name. Retired resources move into the submission slot
 * only after replay has resolved every key in that recording. */
template <class Resource>
class RecordedTextureVersions {
public:
    struct Entry {
        RecordedTextureBinding binding;
        Resource resource;

        Entry(RecordedTextureBinding binding_, Resource &&resource_)
            : binding(binding_), resource(std::move(resource_)) {}

        Entry(Entry &&) = default;
        Entry &operator=(Entry &&) = default;
        Entry(const Entry &) = delete;
        Entry &operator=(const Entry &) = delete;
    };

    Entry *current(uint32_t guest)
    {
        auto it = m_current.find(guest);
        return it == m_current.end() ? nullptr : &it->second;
    }

    const Entry *current(uint32_t guest) const
    {
        auto it = m_current.find(guest);
        return it == m_current.end() ? nullptr : &it->second;
    }

    Entry *resolve(const RecordedTextureBinding &binding)
    {
        if (!binding.valid())
            return nullptr;
        Entry *active = current(binding.guest);
        if (active && active->binding.sameResource(binding))
            return active;
        for (auto it = m_retired.rbegin(); it != m_retired.rend(); ++it) {
            if (it->binding.sameResource(binding))
                return &*it;
        }
        return nullptr;
    }

    const Entry *resolve(const RecordedTextureBinding &binding) const
    {
        if (!binding.valid())
            return nullptr;
        const Entry *active = current(binding.guest);
        if (active && active->binding.sameResource(binding))
            return active;
        for (auto it = m_retired.rbegin(); it != m_retired.rend(); ++it) {
            if (it->binding.sameResource(binding))
                return &*it;
        }
        return nullptr;
    }

    Resource *replace(const RecordedTextureBinding &binding,
                      Resource &&resource)
    {
        auto old = m_current.find(binding.guest);
        if (old != m_current.end()) {
            m_retired.push_back(std::move(old->second));
            m_current.erase(old);
        }
        auto inserted = m_current.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(binding.guest),
            std::forward_as_tuple(binding, std::move(resource)));
        return &inserted.first->second.resource;
    }

    void releaseRetiredResources(std::vector<Resource> &destination)
    {
        destination.reserve(destination.size() + m_retired.size());
        for (Entry &entry : m_retired)
            destination.push_back(std::move(entry.resource));
        m_retired.clear();
    }

    void discardRetiredResources() { m_retired.clear(); }

private:
    std::unordered_map<uint32_t, Entry> m_current;
    std::vector<Entry> m_retired;
};

} /* namespace plume */
} /* namespace xgpu */

#endif /* XGPU_PLUME_RECORDED_RESOURCES_H */
