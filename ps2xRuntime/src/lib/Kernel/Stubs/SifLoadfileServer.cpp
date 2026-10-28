#include "SifLoadfileServer.h"

#include <algorithm>
#include <cstring>

namespace ps2_sif
{
    namespace
    {
        constexpr uint32_t kLoadModule = 0x00u;
        constexpr uint32_t kSearchModuleByName = 0x09u;
        constexpr uint32_t kInitialize = 0xFFu;
        constexpr size_t kPathOffset = 8u;
        constexpr size_t kPathMaxSize = 252u;
        constexpr size_t kArgumentsOffset = kPathOffset + kPathMaxSize;
        constexpr size_t kArgumentsMaxSize = 252u;
        constexpr size_t kRequestSize = kArgumentsOffset + kArgumentsMaxSize;
        constexpr uint32_t kLoadcoreVersionMarker = 0x2E2E2E2Eu;
        constexpr int32_t kFailure = -1;

        uint32_t failure()
        {
            return static_cast<uint32_t>(kFailure);
        }

        std::vector<uint32_t> loadModule(const std::vector<uint8_t> &send, const SifLoadfileServer::Modules &modules)
        {
            if (send.size() < kRequestSize || !modules.load)
                return {failure(), 0u};
            uint32_t argumentSize = 0u;
            std::memcpy(&argumentSize, send.data(), sizeof(argumentSize));
            const char *path = reinterpret_cast<const char *>(send.data() + kPathOffset);
            const std::string modulePath(path, strnlen(path, kPathMaxSize));
            const auto arguments = send.begin() + kArgumentsOffset;
            const std::vector<uint8_t> moduleArguments(arguments, arguments + std::min<size_t>(argumentSize, kArgumentsMaxSize));
            return {static_cast<uint32_t>(modules.load(modulePath, moduleArguments)), 0u};
        }
    }

    std::vector<uint32_t> SifLoadfileServer::invoke(uint32_t function,
                                                    const std::vector<uint8_t> &send,
                                                    const Modules &modules)
    {
        switch (function)
        {
        case kLoadModule:
            return loadModule(send, modules);
        case kInitialize:
            return {kLoadcoreVersionMarker};
        case kSearchModuleByName:
        default:
            // No module registry to search; callers treat a negative id as "not loaded".
            return {failure()};
        }
    }
}
