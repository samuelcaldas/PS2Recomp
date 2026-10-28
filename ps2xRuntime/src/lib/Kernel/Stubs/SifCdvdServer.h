#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace ps2_sif
{
    // IOP cdvdfsv RPC servers (SIDs 0x80000592-0x8000059C), which the EE CDVD library calls
    // for disc access. Functions and replies follow CCdvdfsv in Play! (Source/iop/Iop_Cdvdfsv.cpp,
    // BSD 2-Clause); disc access is delegated to the runtime's virtual disc.
    class SifCdvdServer
    {
    public:
        struct Disc
        {
            // Fills the sceCdlFILE at fileAddress for the path at pathAddress (EE addresses); 1 if found.
            std::function<int32_t(uint32_t fileAddress, uint32_t pathAddress)> searchFile;
            std::function<bool(uint32_t lbn, uint32_t count, uint32_t eeDestination)> readToEe;
            std::function<bool(uint32_t lbn, uint32_t count, uint32_t iopDestination)> readToIop;
            std::function<uint32_t()> diskType;
        };

        struct Request
        {
            uint32_t sid = 0u;
            uint32_t function = 0u;
            uint32_t sendAddress = 0u;
            std::vector<uint8_t> send;
        };

        [[nodiscard]] static bool serves(uint32_t sid);

        // Returns true for NCmd ordinals that are asynchronous on real PS2 hardware:
        // the IOP fires a DMA completion interrupt after the transfer, which the EE polls
        // via a VBLANK_END handler. The HLE does the I/O synchronously, so callers must
        // simulate that completion signal after invoking one of these commands.
        [[nodiscard]] static bool isAsyncNCommand(uint32_t function);

        // SCECdPS2CD for discs that fit on a CD-ROM, SCECdPS2DVD otherwise. Games check
        // the type and wait for the right disc when it does not match.
        [[nodiscard]] static uint32_t diskTypeForSize(uint64_t bytes);

        // Runs one RPC function; returns the words for the client's receive buffer.
        std::vector<uint32_t> invoke(const Request &request, const Disc &disc);

        // Count of async NCmd completions that fired before the EE NCmd-completion sema
        // (D_4ED348) was created. The game's CDVD init races: GetToc fires before
        // func_0012B8A8 writes the sema ID. Hook the first WaitSema caller to drain this.
        static std::atomic<uint32_t> s_pendingNcmdSignals;

    private:
        std::vector<uint32_t> invokeSCommand(uint32_t function, const Disc &disc);
        std::vector<uint32_t> invokeNCommand(const Request &request, const Disc &disc);
        std::vector<uint32_t> streamCommand(const Request &request, const Disc &disc);
        static std::vector<uint32_t> readChain(const Request &request, const Disc &disc);
        static std::vector<uint32_t> searchFile(const Request &request, const Disc &disc);

        uint32_t m_streamPosition = 0u;
        uint32_t m_streamBufferSize = 0u;
        bool m_streaming = false;
    };
}
