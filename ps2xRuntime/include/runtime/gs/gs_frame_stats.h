#pragma once
// Per-presentation GS workload summary, enabled with PS2X_GS_FRAME_STATS=<latches per report>.
// Groups primitives by texture/target and image transfers by destination so a log shows
// how a game composes its frames without describing every primitive.
#include "runtime/gs/gs_types.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>

class GSFrameStats
{
public:
    static GSFrameStats &instance()
    {
        static GSFrameStats stats;
        return stats;
    }

    bool enabled() const { return m_interval != 0u; }

    void recordPrimitive(const GSPrimitiveBatch &batch)
    {
        if (!enabled())
            return;
        const auto &ctx = batch.state.context;
        const bool textured = batch.state.prim.tme;
        const PrimKey key{static_cast<uint32_t>(batch.state.prim.type), textured ? 1u : 0u, ctx.frame.fbp,
                          textured ? ctx.tex0.tbp0 : 0u, textured ? static_cast<uint32_t>(ctx.tex0.psm) : 0u,
                          textured ? (1u << ctx.tex0.tw) : 0u, textured ? (1u << ctx.tex0.th) : 0u};
        std::lock_guard<std::mutex> lock(m_mutex);
        PrimStats &stats = m_prims[key];
        const float offsetX = static_cast<float>(ctx.xyoffset.ofx >> 4);
        const float offsetY = static_cast<float>(ctx.xyoffset.ofy >> 4);
        for (uint8_t i = 0; i < batch.vertexCount && i < batch.vertices.size(); ++i)
            stats.extend(batch.vertices[i].x - offsetX, batch.vertices[i].y - offsetY);
        ++stats.count;
    }

    void recordTransfer(const GSTransferCommand &command)
    {
        if (!enabled())
            return;
        const TransferKey key{command.direction, command.bitbltbuf.dbp, static_cast<uint32_t>(command.bitbltbuf.dpsm),
                              command.trxreg.rrw, command.trxreg.rrh};
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_transfers[key];
    }

    // GIF DMA payloads of at least kLargeGifQwc quadwords, by guest source address.
    void recordGifSource(uint32_t address, uint32_t qwc)
    {
        if (!enabled() || qwc < kLargeGifQwc)
            return;
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_gifSources[{address, qwc}];
    }

    // Returns a report every interval latches (empty otherwise) and starts a new window.
    std::string latch(const GSPresentationRequest &request)
    {
        if (!enabled())
            return {};
        std::lock_guard<std::mutex> lock(m_mutex);
        if (++m_latches % m_interval != 0u)
            return {};
        std::ostringstream out;
        out << "[gs:frame-stats] latch=" << m_latches << std::hex << " pmode=0x" << request.pmode
            << " smode2=0x" << request.smode2 << " dispfb1=0x" << request.dispfb1 << " display1=0x"
            << request.display1 << " dispfb2=0x" << request.dispfb2 << " display2=0x" << request.display2
            << std::dec;
        for (const auto &[key, stats] : m_prims)
            out << " prim(type=" << std::get<0>(key) << " tme=" << std::get<1>(key) << " fbp=" << std::get<2>(key)
                << " tbp=" << std::get<3>(key) << " psm=0x" << std::hex << std::get<4>(key) << std::dec
                << " tex=" << std::get<5>(key) << "x" << std::get<6>(key) << " n=" << stats.count << " bbox=("
                << stats.minX << "," << stats.minY << ")-(" << stats.maxX << "," << stats.maxY << "))";
        for (const auto &[key, count] : m_transfers)
            out << " xfer(dir=" << std::get<0>(key) << " dbp=" << std::get<1>(key) << " psm=0x" << std::hex
                << std::get<2>(key) << std::dec << " size=" << std::get<3>(key) << "x" << std::get<4>(key)
                << " n=" << count << ")";
        for (const auto &[key, count] : m_gifSources)
            out << " gifsrc(addr=0x" << std::hex << key.first << std::dec << " qwc=" << key.second << " n=" << count
                << ")";
        m_prims.clear();
        m_transfers.clear();
        m_gifSources.clear();
        return out.str();
    }

private:
    using PrimKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    using TransferKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;

    struct PrimStats
    {
        uint64_t count = 0;
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        void extend(float x, float y)
        {
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    };

    GSFrameStats()
    {
        const char *text = std::getenv("PS2X_GS_FRAME_STATS");
        m_interval = text != nullptr ? static_cast<uint32_t>(std::strtoul(text, nullptr, 10)) : 0u;
    }

    static constexpr uint32_t kLargeGifQwc = 1024;
    uint32_t m_interval = 0;
    uint64_t m_latches = 0;
    std::mutex m_mutex;
    std::map<PrimKey, PrimStats> m_prims;
    std::map<TransferKey, uint64_t> m_transfers;
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> m_gifSources;
};
