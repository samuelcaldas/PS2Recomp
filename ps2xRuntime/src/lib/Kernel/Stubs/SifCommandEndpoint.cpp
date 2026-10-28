#include "SifCommandEndpoint.h"

#include <cstring>

namespace ps2_sif
{
    namespace
    {
        constexpr uint32_t kCmdChangeAddress = 0x80000000u;
        constexpr uint32_t kCmdSetSreg = 0x80000001u;
        constexpr uint32_t kCmdInit = 0x80000002u;
        constexpr uint32_t kCmdReset = 0x80000003u;
        constexpr uint32_t kCmdRequestEnd = 0x80000008u;
        constexpr uint32_t kCmdBind = 0x80000009u;
        constexpr uint32_t kCmdCall = 0x8000000Au;
        constexpr uint32_t kCmdOtherData = 0x8000000Cu;

        // Packet sizes in 32-bit words: 4-word header, then the command body.
        constexpr size_t kHeaderWords = 4u;
        constexpr size_t kInitWords = 5u;
        constexpr size_t kBindWords = 9u;
        constexpr size_t kCallWords = 14u;
        constexpr size_t kOtherDataWords = 11u;
        constexpr size_t kRpcPacketBytes = 64u;

        // EE addresses arrive with segment bits (uncached 0x20000000, 0x30000000).
        constexpr uint32_t kEeRamMask = 0x01FFFFFFu;
        // Server handles must be nonzero (zero means the bind failed), and some games use SID 0.
        constexpr uint32_t kServerHandleXor = 0xACACACACu;
        constexpr uint32_t kSregRpcInit = 0u;

        size_t wordCount(const std::vector<uint8_t> &packet)
        {
            return packet.size() / 4u;
        }

        uint32_t word(const std::vector<uint8_t> &packet, size_t index)
        {
            uint32_t value = 0u;
            std::memcpy(&value, packet.data() + index * 4u, sizeof(value));
            return value;
        }

        std::vector<uint8_t> build(uint32_t dest, uint32_t commandId, const std::vector<uint32_t> &body)
        {
            std::vector<uint32_t> words{0u, dest, commandId, 0u};
            words.insert(words.end(), body.begin(), body.end());
            std::vector<uint8_t> bytes(words.size() * 4u);
            std::memcpy(bytes.data(), words.data(), bytes.size());
            bytes[0] = static_cast<uint8_t>(bytes.size());
            return bytes;
        }
    }

    void SifCommandEndpoint::reset()
    {
        m_eeReceiveAddress = 0u;
        m_rpcDataSource = 0u;
        m_awaitingAcknowledge = false;
        m_replies.clear();
    }

    void SifCommandEndpoint::setRpcBuffer(uint32_t iopAddress)
    {
        m_rpcBuffer = iopAddress;
    }

    void SifCommandEndpoint::noteRpcData(uint32_t eeSource)
    {
        m_rpcDataSource = eeSource;
    }

    void SifCommandEndpoint::receive(const std::vector<uint8_t> &packet, const Services &services)
    {
        if (wordCount(packet) < kHeaderWords)
            return;

        switch (word(packet, 2))
        {
        case kCmdChangeAddress:
        case kCmdInit:
            receiveInit(packet);
            break;
        case kCmdReset:
            // The EE reboots the IOP: nothing in flight survives, and the EE re-runs the
            // INIT handshake against the new IOP.
            m_awaitingAcknowledge = false;
            m_replies.clear();
            if (services.rebootIop)
                services.rebootIop();
            break;
        case kCmdBind:
            receiveBind(packet, services);
            break;
        case kCmdCall:
            receiveCall(packet, services);
            break;
        case kCmdOtherData:
            receiveOtherData(packet, services);
            break;
        default:
            break;
        }
    }

    std::optional<SifCommandEndpoint::Reply> SifCommandEndpoint::nextReply()
    {
        if (!hasDeliverableReply())
            return std::nullopt;

        Reply reply = std::move(m_replies.front());
        m_replies.pop_front();
        if (reply.eeAddress == 0u)
            reply.eeAddress = m_eeReceiveAddress;
        m_awaitingAcknowledge = true;
        return reply;
    }

    bool SifCommandEndpoint::hasDeliverableReply() const
    {
        return !m_awaitingAcknowledge && !m_replies.empty() && m_eeReceiveAddress != 0u;
    }

    void SifCommandEndpoint::acknowledge()
    {
        m_awaitingAcknowledge = false;
    }

    void SifCommandEndpoint::receiveInit(const std::vector<uint8_t> &packet)
    {
        // opt 1 is SifInitRpc asking for the RPCINIT software register; anything else
        // carries the EE receive buffer (INIT opt 0, or CHANGE_SADDR).
        if (word(packet, 2) == kCmdInit && word(packet, 3) == 1u)
        {
            queue(0u, build(0u, kCmdSetSreg, {kSregRpcInit, 1u}));
            return;
        }
        if (wordCount(packet) < kInitWords)
            return;
        m_eeReceiveAddress = word(packet, 4) & kEeRamMask;
    }

    void SifCommandEndpoint::receiveBind(const std::vector<uint8_t> &packet, const Services &services)
    {
        if (wordCount(packet) < kBindWords)
            return;
        const uint32_t sid = word(packet, 8);
        const bool bound = services.canBind && services.canBind(sid);
        queueRequestEnd(packet, word(packet, 7), kCmdBind, bound ? (sid ^ kServerHandleXor) : 0u);
    }

    void SifCommandEndpoint::receiveCall(const std::vector<uint8_t> &packet, const Services &services)
    {
        if (wordCount(packet) < kCallWords)
            return;
        RpcCall call;
        call.sid = word(packet, 13) ^ kServerHandleXor;
        call.function = word(packet, 8);
        call.clientAddress = word(packet, 7);
        call.sendAddress = m_rpcDataSource;
        call.sendSize = word(packet, 9);
        call.receiveAddress = word(packet, 10) & kEeRamMask;
        call.receiveSize = word(packet, 11);
        call.mode = word(packet, 12);
        if (services.call)
            services.call(call);
        if (call.mode == 0u)
        {
            const uint32_t packetAddress = word(packet, 5) & kEeRamMask;
            if (packetAddress != 0u)
                queue(packetAddress, std::vector<uint8_t>(kRpcPacketBytes, 0u));
            return;
        }
        queueRequestEnd(packet, call.clientAddress, kCmdCall, 0u);
    }

    void SifCommandEndpoint::receiveOtherData(const std::vector<uint8_t> &packet, const Services &services)
    {
        if (wordCount(packet) < kOtherDataWords)
            return;
        if (services.copyIopToEe)
            services.copyIopToEe(word(packet, 9) & kEeRamMask, word(packet, 8), word(packet, 10));
        queueRequestEnd(packet, word(packet, 7), kCmdOtherData, 0u);
    }

    void SifCommandEndpoint::queueRequestEnd(const std::vector<uint8_t> &request, uint32_t clientData,
                                             uint32_t endedCommand, uint32_t serverHandle)
    {
        const uint32_t buffer = endedCommand == kCmdBind ? m_rpcBuffer : 0u;
        queue(m_eeReceiveAddress, build(word(request, 1), kCmdRequestEnd,
                                         {word(request, 4), word(request, 5), word(request, 6), clientData, endedCommand,
                                          serverHandle, buffer, buffer}));
    }

    void SifCommandEndpoint::queue(uint32_t eeAddress, std::vector<uint8_t> reply)
    {
        if (reply.empty() || reply.size() > kRpcPacketBytes)
            return;
        m_replies.push_back({eeAddress, std::move(reply)});
    }
}
