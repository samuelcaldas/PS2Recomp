#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ps2_sif
{
    // IOP LOADFILE RPC server, which EE libraries call to load IRX modules. Request layout and
    // replies follow CLoadcore in Play! (Source/iop/Iop_Loadcore.cpp, BSD 2-Clause).
    class SifLoadfileServer
    {
    public:
        static constexpr uint32_t kSid = 0x80000006u;

        struct Modules
        {
            // Loads and starts a module; returns its id, or a negative value on failure.
            std::function<int32_t(const std::string &path, const std::vector<uint8_t> &arguments)> load;
        };

        // Runs one RPC function; returns the words for the client's receive buffer.
        [[nodiscard]] static std::vector<uint32_t> invoke(uint32_t function,
                                                          const std::vector<uint8_t> &send,
                                                          const Modules &modules);
    };
}
