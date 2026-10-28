#include "SifCdvdServer.h"

#include <cstring>

namespace ps2_sif
{
    std::atomic<uint32_t> SifCdvdServer::s_pendingNcmdSignals{0u};
    namespace
    {
        constexpr uint32_t kSidInit = 0x80000592u;
        constexpr uint32_t kSidSCommand = 0x80000593u;
        constexpr uint32_t kSidNCommand = 0x80000595u;
        constexpr uint32_t kSidPowerOff = 0x80000596u;
        constexpr uint32_t kSidSearchFile = 0x80000597u;
        constexpr uint32_t kSidDiskReady = 0x8000059Au;
        constexpr uint32_t kSidDiskReady2 = 0x8000059Cu;

        constexpr uint32_t kDiskReady = 2u;
        constexpr uint32_t kDiskTypePs2Cd = 0x12u;
        constexpr uint32_t kDiskTypePs2Dvd = 0x14u;
        // Largest CD-ROM (80 minutes, mode 1).
        constexpr uint64_t kCdCapacityBytes = 360000ull * 2048ull;
        constexpr uint32_t kStatusPaused = 0x0Au;
        constexpr uint32_t kStatusSeek = 0x12u;
        constexpr uint32_t kEeRamMask = 0x01FFFFFFu;
        constexpr uint32_t kChainTerminator = ~0u;
        constexpr size_t kChainMaxTuples = 64u;

        uint32_t argument(const std::vector<uint8_t> &send, size_t index)
        {
            uint32_t value = 0u;
            if ((index + 1u) * 4u <= send.size())
                std::memcpy(&value, send.data() + index * 4u, sizeof(value));
            return value;
        }

        bool read(const std::function<bool(uint32_t, uint32_t, uint32_t)> &reader, uint32_t lbn, uint32_t count,
                  uint32_t destination)
        {
            return reader && reader(lbn, count, destination);
        }
    }

    bool SifCdvdServer::serves(uint32_t sid)
    {
        switch (sid)
        {
        case kSidInit:
        case kSidSCommand:
        case kSidNCommand:
        case kSidPowerOff:
        case kSidSearchFile:
        case kSidDiskReady:
        case kSidDiskReady2:
            return true;
        default:
            return false;
        }
    }

    bool SifCdvdServer::isAsyncNCommand(uint32_t function)
    {
        switch (function)
        {
        case 0x01u: // Read
        case 0x04u: // GetToc
        case 0x0Du: // ReadIopMem
        case 0x0Fu: // ReadChain
            return true;
        default:
            return false;
        }
    }

    uint32_t SifCdvdServer::diskTypeForSize(uint64_t bytes)
    {
        return bytes <= kCdCapacityBytes ? kDiskTypePs2Cd : kDiskTypePs2Dvd;
    }

    std::vector<uint32_t> SifCdvdServer::invoke(const Request &request, const Disc &disc)
    {
        switch (request.sid)
        {
        case kSidInit:
            // Words 1 and 2 are the cdvdfsv and cdvdman versions; word 3 must be 0xFF.
            return {0u, 0u, 0u, 0xFFu};
        case kSidSCommand:
            return invokeSCommand(request.function, disc);
        case kSidNCommand:
            return invokeNCommand(request, disc);
        case kSidSearchFile:
            return searchFile(request, disc);
        case kSidDiskReady:
        case kSidDiskReady2:
            return {kDiskReady};
        case kSidPowerOff:
        default:
            return {};
        }
    }

    std::vector<uint32_t> SifCdvdServer::invokeSCommand(uint32_t function, const Disc &disc)
    {
        switch (function)
        {
        case 0x01u: // ReadClock: result, then an all-zero clock
            return {1u, 0u, 0u};
        case 0x03u: // GetDiskType
            return {disc.diskType ? disc.diskType() : kDiskTypePs2Dvd};
        case 0x04u: // GetError
            return {0u};
        case 0x05u: // TrayReq: result, tray check
            return {1u, 0u};
        case 0x0Cu: // Status
            return {m_streaming ? kStatusSeek : kStatusPaused};
        case 0x16u: // Break
        case 0x22u: // SetMediaMode
            return {1u};
        case 0x27u: // ReadDvdDualInfo: single layer
            return {1u, 0u, 0u};
        default:
            return {};
        }
    }

    std::vector<uint32_t> SifCdvdServer::invokeNCommand(const Request &request, const Disc &disc)
    {
        const auto &send = request.send;
        switch (request.function)
        {
        case 0x01u: // Read(sector, count, EE destination, mode)
            (void)read(disc.readToEe, argument(send, 0), argument(send, 1), argument(send, 2) & kEeRamMask);
            return {0u};
        case 0x04u: // GetToc
            return {1u};
        case 0x09u:
            return streamCommand(request, disc);
        case 0x0Du: // ReadIopMem(sector, count, IOP destination, mode)
            (void)read(disc.readToIop, argument(send, 0), argument(send, 1), argument(send, 2));
            return {0u};
        case 0x0Eu: // NDiskReady
            return {kDiskReady};
        case 0x0Fu:
            return readChain(request, disc);
        case 0x05u: // Seek
        case 0x08u: // Pause
        default:
            return {};
        }
    }

    std::vector<uint32_t> SifCdvdServer::streamCommand(const Request &request, const Disc &disc)
    {
        const auto &send = request.send;
        const uint32_t sector = argument(send, 0);
        const uint32_t count = argument(send, 1);
        switch (argument(send, 3))
        {
        case 1u: // Start
            m_streamPosition = sector;
            m_streaming = true;
            return {1u};
        case 2u: // Read
            (void)read(disc.readToEe, m_streamPosition, count, argument(send, 2) & kEeRamMask);
            m_streamPosition += count;
            return {count};
        case 3u: // Stop
            m_streaming = false;
            return {1u};
        case 5u: // Init(buffer size, buffer count, buffer)
            m_streamBufferSize = sector;
            return {1u};
        case 6u: // Status
            return {m_streamBufferSize};
        case 4u: // Seek
        case 9u:
            m_streamPosition = sector;
            return {1u};
        default:
            return {};
        }
    }

    std::vector<uint32_t> SifCdvdServer::readChain(const Request &request, const Disc &disc)
    {
        for (size_t tuple = 0u; tuple < kChainMaxTuples; ++tuple)
        {
            const uint32_t sector = argument(request.send, tuple * 3u);
            const uint32_t count = argument(request.send, tuple * 3u + 1u);
            const uint32_t destination = argument(request.send, tuple * 3u + 2u);
            if (sector == kChainTerminator || count == kChainTerminator || destination == kChainTerminator ||
                (tuple * 3u + 3u) * 4u > request.send.size())
                break;
            (void)read(disc.readToEe, sector, count, destination & kEeRamMask);
        }
        return {};
    }

    std::vector<uint32_t> SifCdvdServer::searchFile(const Request &request, const Disc &disc)
    {
        // The request is a sceCdlFILE the server fills in place, followed by the path.
        const uint32_t pathOffset = request.send.size() == 0x124u ? 0x20u : 0x24u;
        if (!disc.searchFile)
            return {0u};
        return {static_cast<uint32_t>(disc.searchFile(request.sendAddress, request.sendAddress + pathOffset))};
    }
}
