#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

namespace ps2_sif
{
    // IOP side of the SIF command protocol, for games whose EE sifcmd/sifrpc libraries run as
    // recompiled guest code instead of going through the HLE sceSif* stubs. The EE DMAs command
    // packets into the IOP command buffer; the IOP answers with packets written to the EE receive
    // buffer, one at a time, each raising a SIF0 DMAC interrupt. The EE handler re-arms the channel
    // with SetDChain, which acknowledges the packet. Modeled on CSIF in Play! (Source/ee/SIF.cpp,
    // BSD 2-Clause, Jean-Philip Desjardins).
    class SifCommandEndpoint
    {
    public:
        struct RpcCall
        {
            uint32_t sid = 0u;
            uint32_t function = 0u;
            uint32_t clientAddress = 0u;
            uint32_t sendAddress = 0u;
            uint32_t sendSize = 0u;
            uint32_t receiveAddress = 0u;
            uint32_t receiveSize = 0u;
            uint32_t mode = 0u;
        };

        struct Services
        {
            std::function<bool(uint32_t sid)> canBind;
            std::function<void(const RpcCall &call)> call;
            std::function<void(uint32_t eeDestination, uint32_t iopSource, uint32_t size)> copyIopToEe;
            std::function<void()> rebootIop;
        };

        struct Reply
        {
            uint32_t eeAddress = 0u;
            std::vector<uint8_t> bytes;
        };

        void reset();

        // IOP buffer that bound clients DMA their call arguments into.
        void setRpcBuffer(uint32_t iopAddress);

        // Records the EE source of the arguments DMAed into the RPC buffer for the next CALL.
        void noteRpcData(uint32_t eeSource);

        // Handles one packet the EE DMAed into the IOP command buffer.
        void receive(const std::vector<uint8_t> &packet, const Services &services);

        // Next reply to write to the EE, or nothing while the previous one is unacknowledged.
        [[nodiscard]] std::optional<Reply> nextReply();

        // True when nextReply() would return a reply.
        [[nodiscard]] bool hasDeliverableReply() const;

        // The EE handler consumed the delivered reply (SetDChain).
        void acknowledge();

    private:
        void receiveInit(const std::vector<uint8_t> &packet);
        void receiveBind(const std::vector<uint8_t> &packet, const Services &services);
        void receiveCall(const std::vector<uint8_t> &packet, const Services &services);
        void receiveOtherData(const std::vector<uint8_t> &packet, const Services &services);
        void queueRequestEnd(const std::vector<uint8_t> &request, uint32_t clientData, uint32_t endedCommand,
                             uint32_t serverHandle);
        void queue(uint32_t eeAddress, std::vector<uint8_t> reply);

        uint32_t m_eeReceiveAddress = 0u;
        uint32_t m_rpcBuffer = 0u;
        uint32_t m_rpcDataSource = 0u;
        bool m_awaitingAcknowledge = false;
        std::deque<Reply> m_replies;
    };
}
