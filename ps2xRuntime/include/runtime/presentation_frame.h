#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <limits>
#include <optional>
#include <vector>

// Host snapshots only. Each pixel is four consecutive bytes: R, G, B, A.
// Rows start strideBytes apart; callers must provide the entire declared span.
struct PS2PresentationFrameView
{
    const uint8_t* pixels;
    uint32_t width, height, strideBytes;
};

struct PS2OwnedPresentationFrame
{
    std::shared_ptr<const std::vector<uint8_t>> pixels;
    uint32_t width, height, strideBytes;
};

inline uint64_t PS2PresentationFrameBytes(PS2PresentationFrameView frame)
{
    if (!frame.pixels || !frame.width || !frame.height ||
        uint64_t(frame.strideBytes) < uint64_t(frame.width) * 4u)
        return 0;
    return uint64_t(frame.height - 1u) * frame.strideBytes + uint64_t(frame.width) * 4u;
}

// Shared desktop/Switch preparation step. Keep pixels and geometry together when
// no new snapshot arrives. Uploaders consume tightly packed host RGBA bytes.
class PS2PresentationFrameCache
{
public:
    template<class Compose>
    bool update(PS2PresentationFrameView source, Compose compose)
    {
        const uint64_t sourceBytes = PS2PresentationFrameBytes(source);
        if (!sourceBytes || sourceBytes > std::numeric_limits<size_t>::max()) return false;
        auto result = compose(source);
        if (result)
        {
            if (!result->pixels) return false;
            const PS2PresentationFrameView view{result->pixels->data(), result->width, result->height, result->strideBytes};
            const uint64_t bytes = PS2PresentationFrameBytes(view);
            if (!bytes || bytes > result->pixels->size() || view.width > 1920u || view.height > 1080u)
                return false;
            source = view;
        }
        std::vector<uint8_t> packed(size_t(source.width) * source.height * 4u);
        for (uint32_t y = 0; y < source.height; ++y)
            std::memcpy(packed.data() + size_t(y) * source.width * 4u,
                        source.pixels + size_t(y) * source.strideBytes, size_t(source.width) * 4u);
        pixels = std::move(packed);
        width = source.width;
        height = source.height;
        return true;
    }

    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
};
