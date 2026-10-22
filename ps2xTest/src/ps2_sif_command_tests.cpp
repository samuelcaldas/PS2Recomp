#include "MiniTest.h"
#include "Kernel/Stubs/SifCommandEndpoint.h"
#include "Kernel/Stubs/SifLoadfileServer.h"
#include "Kernel/Stubs/SifCdvdServer.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

using ps2_sif::SifCommandEndpoint;

namespace
{
    constexpr uint32_t kCmdChangeAddress = 0x80000000u;
    constexpr uint32_t kCmdSetSreg = 0x80000001u;
    constexpr uint32_t kCmdInit = 0x80000002u;
    constexpr uint32_t kCmdRequestEnd = 0x80000008u;
    constexpr uint32_t kCmdBind = 0x80000009u;
    constexpr uint32_t kCmdCall = 0x8000000Au;
    constexpr uint32_t kCmdOtherData = 0x8000000Cu;
    constexpr uint32_t kEeReceiveBuffer = 0x004E4A40u;
    constexpr uint32_t kRpcBuffer = 0x00180000u;

    std::vector<uint8_t> packet(uint32_t commandId, uint32_t optional, std::initializer_list<uint32_t> body)
    {
        std::vector<uint32_t> words{0u, 0u, commandId, optional};
        words.insert(words.end(), body);
        std::vector<uint8_t> bytes(words.size() * 4u);
        std::memcpy(bytes.data(), words.data(), bytes.size());
        bytes[0] = static_cast<uint8_t>(bytes.size());
        return bytes;
    }

    uint32_t word(const std::vector<uint8_t> &bytes, uint32_t index)
    {
        uint32_t value = 0u;
        std::memcpy(&value, bytes.data() + index * 4u, sizeof(value));
        return value;
    }

    struct Recorder
    {
        std::vector<SifCommandEndpoint::RpcCall> calls;
        std::vector<std::array<uint32_t, 3>> copies;
        bool bindable = true;
        int reboots = 0;

        SifCommandEndpoint::Services services()
        {
            SifCommandEndpoint::Services result;
            result.canBind = [this](uint32_t) { return bindable; };
            result.call = [this](const SifCommandEndpoint::RpcCall &call) { calls.push_back(call); };
            result.copyIopToEe = [this](uint32_t ee, uint32_t iop, uint32_t size) { copies.push_back({ee, iop, size}); };
            result.rebootIop = [this]() { ++reboots; };
            return result;
        }
    };

    struct RuntimeEnv
    {
        std::vector<uint8_t> rdram;
        R5900Context ctx{};
        PS2Runtime runtime;

        RuntimeEnv() : rdram(PS2_RAM_SIZE, 0u)
        {
            ps2_stubs::resetSifState();
            std::memset(&ctx, 0, sizeof(ctx));
        }

        void setReg(int reg, uint32_t value)
        {
            ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
        }

        uint32_t result() const
        {
            return ::getRegU32(&ctx, 2);
        }

        uint32_t readGuest(uint32_t address) const
        {
            uint32_t value = 0u;
            std::memcpy(&value, rdram.data() + address, sizeof(value));
            return value;
        }

        // DMAs one packet from EE scratch memory to the IOP command buffer.
        void sendPacket(uint32_t iopCommandBuffer, const std::vector<uint8_t> &bytes)
        {
            constexpr uint32_t kDescriptor = 0x00020000u;
            constexpr uint32_t kSource = 0x00020100u;
            std::memcpy(rdram.data() + kSource, bytes.data(), bytes.size());
            const uint32_t descriptor[4] = {kSource, iopCommandBuffer, static_cast<uint32_t>(bytes.size()), 0u};
            std::memcpy(rdram.data() + kDescriptor, descriptor, sizeof(descriptor));
            setReg(4, kDescriptor);
            setReg(5, 1u);
            ps2_stubs::sceSifSetDma(rdram.data(), &ctx, &runtime);
        }
    };

    SifCommandEndpoint initializedEndpoint(Recorder &recorder)
    {
        SifCommandEndpoint endpoint;
        endpoint.setRpcBuffer(kRpcBuffer);
        endpoint.receive(packet(kCmdInit, 0u, {kEeReceiveBuffer | 0x20000000u}), recorder.services());
        return endpoint;
    }
}

void register_ps2_sif_command_tests()
{
    MiniTest::Case("PS2SifCommand", [](TestCase &tc)
    {
        tc.Run("INIT with opt 0 records the EE receive buffer and sends nothing", [](TestCase &t)
        {
            Recorder recorder;
            SifCommandEndpoint endpoint = initializedEndpoint(recorder);
            t.IsFalse(endpoint.nextReply().has_value(), "INIT opt 0 has no reply");
        });

        tc.Run("INIT with opt 1 answers SET_SREG 0 = 1 at the EE receive buffer", [](TestCase &t)
        {
            Recorder recorder;
            SifCommandEndpoint endpoint = initializedEndpoint(recorder);
            endpoint.receive(packet(kCmdInit, 1u, {}), recorder.services());
            auto reply = endpoint.nextReply();
            t.IsTrue(reply.has_value(), "INIT opt 1 queues a reply");
            if (!reply)
                return;
            t.Equals(reply->eeAddress, kEeReceiveBuffer, "reply goes to the masked EE receive buffer");
            t.Equals(reply->bytes.size(), size_t{24u}, "SET_SREG packet is 24 bytes");
            t.Equals(word(reply->bytes, 0) & 0xFFu, 24u, "header carries the packet size");
            t.Equals(word(reply->bytes, 2), kCmdSetSreg, "reply command is SET_SREG");
            t.Equals(word(reply->bytes, 4), 0u, "register index is RPCINIT (0)");
            t.Equals(word(reply->bytes, 5), 1u, "register value is 1");
        });

        tc.Run("CHANGE_SADDR from the EE replaces the receive buffer", [](TestCase &t)
        {
            Recorder recorder;
            SifCommandEndpoint endpoint = initializedEndpoint(recorder);
            endpoint.receive(packet(kCmdChangeAddress, 0u, {0x00400000u}), recorder.services());
            endpoint.receive(packet(kCmdInit, 1u, {}), recorder.services());
            auto reply = endpoint.nextReply();
            t.IsTrue(reply && reply->eeAddress == 0x00400000u, "reply uses the changed buffer");
        });

        tc.Run("BIND of a served SID answers RPC_END with a server handle and the RPC buffer", [](TestCase &t)
        {
            Recorder recorder;
            SifCommandEndpoint endpoint = initializedEndpoint(recorder);
            endpoint.receive(packet(kCmdBind, 0u, {7u, 0x1000u, 3u, 0x2000u, 0x80000400u}), recorder.services());
            auto reply = endpoint.nextReply();
            t.IsTrue(reply.has_value(), "BIND queues a reply");
            if (!reply)
                return;
            t.Equals(reply->bytes.size(), size_t{48u}, "RPC_END packet is 48 bytes");
            t.Equals(word(reply->bytes, 2), kCmdRequestEnd, "reply command is RPC_END");
            t.Equals(word(reply->bytes, 4), 7u, "record id echoes the request");
            t.Equals(word(reply->bytes, 5), 0x1000u, "packet address echoes the request");
            t.Equals(word(reply->bytes, 6), 3u, "rpc id echoes the request");
            t.Equals(word(reply->bytes, 7), 0x2000u, "client data address echoes the request");
            t.Equals(word(reply->bytes, 8), kCmdBind, "ended command is BIND");
            t.IsTrue(word(reply->bytes, 9) != 0u, "server handle is nonzero when bound");
            t.Equals(word(reply->bytes, 10), kRpcBuffer, "server buffer is the IOP RPC buffer");
        });

        tc.Run("BIND of an unserved SID answers with a zero server handle", [](TestCase &t)
        {
            Recorder recorder;
            recorder.bindable = false;
            SifCommandEndpoint endpoint = initializedEndpoint(recorder);
            endpoint.receive(packet(kCmdBind, 0u, {7u, 0x1000u, 3u, 0x2000u, 0x80000211u}), recorder.services());
            auto reply = endpoint.nextReply();
            t.IsTrue(reply && word(reply->bytes, 9) == 0u, "unbound server handle is zero so the EE retries");
        });

        tc.Run("CALL routes the bound SID with the DMAed send data and answers RPC_END", [](TestCase &t)
        {
            Recorder recorder;
            SifCommandEndpoint endpoint = initializedEndpoint(recorder);
            endpoint.receive(packet(kCmdBind, 0u, {1u, 0x1000u, 3u, 0x2000u, 0x80000400u}), recorder.services());
            auto bound = endpoint.nextReply();
            endpoint.acknowledge();
            const uint32_t handle = bound ? word(bound->bytes, 9) : 0u;
            endpoint.noteRpcData(0x00300000u);
            endpoint.receive(packet(kCmdCall, 0u, {2u, 0x1100u, 4u, 0x2000u, 5u, 64u, 0x20310000u, 32u, 1u, handle}),
                             recorder.services());
            t.Equals(recorder.calls.size(), size_t{1u}, "server is called once");
            if (recorder.calls.empty())
                return;
            const auto &call = recorder.calls.front();
            t.Equals(call.sid, 0x80000400u, "SID is recovered from the server handle");
            t.Equals(call.function, 5u, "rpc number is passed through");
            t.Equals(call.sendAddress, 0x00300000u, "send data comes from the DMAed EE source");
            t.Equals(call.sendSize, 64u, "send size is passed through");
            t.Equals(call.receiveAddress, 0x00310000u, "receive buffer is masked to EE RAM");
            t.Equals(call.receiveSize, 32u, "receive size is passed through");
            auto reply = endpoint.nextReply();
            t.IsTrue(reply && word(reply->bytes, 8) == kCmdCall, "CALL is answered with RPC_END for CALL");
        });

        tc.Run("CALL rmode 0 DMA-completes the caller packet without RPC_END", [](TestCase &t)
        {
            Recorder recorder;
            SifCommandEndpoint endpoint = initializedEndpoint(recorder);
            endpoint.noteRpcData(0x00300000u);
            constexpr uint32_t packetAddress = 0x20402000u;
            endpoint.receive(packet(kCmdCall, 0u, {2u, packetAddress, 4u, 0x2000u, 5u, 64u,
                                                   0x20310000u, 32u, 0u, 0xACACACACu}),
                             recorder.services());
            auto reply = endpoint.nextReply();
            t.IsTrue(reply.has_value(), "rmode 0 completion DMA is queued");
            if (!reply)
                return;
            t.Equals(reply->eeAddress, packetAddress & 0x01FFFFFFu, "completion DMA targets the CALL packet");
            t.Equals(reply->bytes.size(), size_t{64u}, "completion DMA writes the 64-byte RPC packet slot");
            t.IsTrue(std::all_of(reply->bytes.begin(), reply->bytes.end(), [](uint8_t byte) { return byte == 0u; }),
                     "cleared rpc_id and rec_id mark the caller packet complete");
            t.Equals(recorder.calls.size(), size_t{1u}, "rmode 0 still invokes the server");
            endpoint.acknowledge();
            endpoint.receive(packet(kCmdCall, 0u, {3u, packetAddress, 5u, 0x2000u, 5u, 64u,
                                                   0x20310000u, 32u, 0u, 0xACACACACu}),
                             recorder.services());
            t.IsTrue(endpoint.nextReply().has_value(), "the same CALL packet slot can be reused after DMA completion");
            t.Equals(recorder.calls.size(), size_t{2u}, "the reused call reaches the server");
        });

        tc.Run("OTHERDATA copies IOP memory to the EE and answers RPC_END", [](TestCase &t)
        {
            Recorder recorder;
            SifCommandEndpoint endpoint = initializedEndpoint(recorder);
            endpoint.receive(packet(kCmdOtherData, 0u, {1u, 0x1000u, 2u, 0x2400u, 0x8000u, 0x20500000u, 128u}),
                             recorder.services());
            t.Equals(recorder.copies.size(), size_t{1u}, "one copy is made");
            if (!recorder.copies.empty())
            {
                t.Equals(recorder.copies.front()[0], 0x00500000u, "EE destination is masked");
                t.Equals(recorder.copies.front()[1], 0x8000u, "IOP source is passed through");
                t.Equals(recorder.copies.front()[2], 128u, "size is passed through");
            }
            auto reply = endpoint.nextReply();
            t.IsTrue(reply && word(reply->bytes, 7) == 0x2400u && word(reply->bytes, 8) == kCmdOtherData,
                     "reply carries the receive data address and OTHERDATA");
        });

        tc.Run("replies are delivered one at a time until the EE acknowledges", [](TestCase &t)
        {
            Recorder recorder;
            SifCommandEndpoint endpoint = initializedEndpoint(recorder);
            endpoint.receive(packet(kCmdInit, 1u, {}), recorder.services());
            endpoint.receive(packet(kCmdInit, 1u, {}), recorder.services());
            t.IsTrue(endpoint.hasDeliverableReply(), "a queued reply is deliverable");
            t.IsTrue(endpoint.nextReply().has_value(), "first reply is delivered");
            t.IsFalse(endpoint.hasDeliverableReply(), "nothing is deliverable before the acknowledgement");
            t.IsFalse(endpoint.nextReply().has_value(), "second reply waits for the acknowledgement");
            endpoint.acknowledge();
            t.IsTrue(endpoint.nextReply().has_value(), "second reply follows the acknowledgement");
        });

        tc.Run("replies wait until the EE publishes its receive buffer", [](TestCase &t)
        {
            Recorder recorder;
            SifCommandEndpoint endpoint;
            endpoint.receive(packet(kCmdInit, 1u, {}), recorder.services());
            t.IsFalse(endpoint.nextReply().has_value(), "no receive buffer, no delivery");
            endpoint.receive(packet(kCmdInit, 0u, {kEeReceiveBuffer}), recorder.services());
            t.IsTrue(endpoint.nextReply().has_value(), "queued reply is delivered once the buffer is known");
        });

        tc.Run("reset drops queued replies and the receive buffer", [](TestCase &t)
        {
            Recorder recorder;
            SifCommandEndpoint endpoint = initializedEndpoint(recorder);
            endpoint.receive(packet(kCmdInit, 1u, {}), recorder.services());
            endpoint.reset();
            t.IsFalse(endpoint.nextReply().has_value(), "nothing is delivered after reset");
        });

        tc.Run("sceSifSetDma delivers the SET_SREG answer to an INIT handshake into EE RAM", [](TestCase &t)
        {
            RuntimeEnv env;
            env.setReg(4, 0x2u);
            ps2_stubs::sceSifGetReg(env.rdram.data(), &env.ctx, &env.runtime);
            const uint32_t commandBuffer = env.result();
            t.IsTrue(commandBuffer != 0u, "SIF_REG_SUBADDR publishes an IOP command buffer");

            env.sendPacket(commandBuffer, packet(kCmdInit, 0u, {kEeReceiveBuffer}));
            env.sendPacket(commandBuffer, packet(kCmdInit, 1u, {}));
            t.Equals(env.readGuest(kEeReceiveBuffer) & 0xFFu, 24u, "SET_SREG size is written at the EE receive buffer");
            t.Equals(env.readGuest(kEeReceiveBuffer + 8u), kCmdSetSreg, "SET_SREG command id is written");
            t.Equals(env.readGuest(kEeReceiveBuffer + 20u), 1u, "RPCINIT value is written");

            env.setReg(4, 0x2u);
            ps2_stubs::resetSifState();
            ps2_stubs::sceSifGetReg(env.rdram.data(), &env.ctx, &env.runtime);
            t.IsTrue(env.result() != 0u, "the command buffer is republished after a reset");
        });

        tc.Run("failed reply DMA does not block a later valid reply", [](TestCase &t)
        {
            RuntimeEnv env;
            env.setReg(4, 0x2u);
            ps2_stubs::sceSifGetReg(env.rdram.data(), &env.ctx, &env.runtime);
            const uint32_t commandBuffer = env.result();
            constexpr uint32_t invalidReceiveBuffer = PS2_RAM_SIZE - 8u;
            env.sendPacket(commandBuffer, packet(kCmdInit, 0u, {invalidReceiveBuffer}));
            env.sendPacket(commandBuffer, packet(kCmdInit, 1u, {}));
            env.sendPacket(commandBuffer, packet(kCmdInit, 0u, {kEeReceiveBuffer}));
            env.sendPacket(commandBuffer, packet(kCmdInit, 1u, {}));
            t.Equals(env.readGuest(kEeReceiveBuffer + 8u), kCmdSetSreg,
                     "a later reply is written after the out-of-range DMA is rejected");
        });

        tc.Run("RESET reboots the IOP and drops pending replies", [](TestCase &t)
        {
            Recorder recorder;
            SifCommandEndpoint endpoint = initializedEndpoint(recorder);
            endpoint.receive(packet(kCmdInit, 1u, {}), recorder.services());
            endpoint.receive(packet(kCmdInit, 1u, {}), recorder.services());
            t.IsTrue(endpoint.nextReply().has_value(), "a reply is in flight before the reset");
            endpoint.receive(packet(0x80000003u, 0u, {0u, 0u}), recorder.services());
            t.Equals(recorder.reboots, 1, "the IOP is rebooted once");
            t.IsFalse(endpoint.nextReply().has_value(), "queued replies are dropped");
            endpoint.receive(packet(kCmdInit, 1u, {}), recorder.services());
            t.IsTrue(endpoint.nextReply().has_value(), "the in-flight acknowledgement is not awaited after a reset");
        });

        tc.Run("SMFLAG reports boot end and command init once a reset IOP is polled", [](TestCase &t)
        {
            RuntimeEnv env;
            env.setReg(4, 0x2u);
            ps2_stubs::sceSifGetReg(env.rdram.data(), &env.ctx, &env.runtime);
            const uint32_t commandBuffer = env.result();
            env.sendPacket(commandBuffer, packet(0x80000003u, 0u, {0u, 0u}));
            env.setReg(4, 0x4u);
            env.setReg(5, 0x20000u);
            ps2_stubs::sceSifSetReg(env.rdram.data(), &env.ctx, &env.runtime);
            env.setReg(4, 0x4u);
            ps2_stubs::sceSifGetReg(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(env.result() & 0x60000u, 0x60000u, "BOOTEND and CMDINIT are set after the reboot");
        });

        tc.Run("short or unknown packets are ignored", [](TestCase &t)
        {
            Recorder recorder;
            SifCommandEndpoint endpoint = initializedEndpoint(recorder);
            endpoint.receive(std::vector<uint8_t>(8u, 0u), recorder.services());
            endpoint.receive(packet(0x80000004u, 0u, {}), recorder.services());
            endpoint.receive(packet(kCmdCall, 0u, {}), recorder.services());
            t.IsFalse(endpoint.nextReply().has_value(), "nothing is answered");
            t.IsTrue(recorder.calls.empty(), "no server is called for a truncated CALL");
        });
    });
}

void register_ps2_async_stack_tests()
{
    MiniTest::Case("PS2AsyncStacks", [](TestCase &tc)
    {
#if !defined(_DEBUG) && !defined(RUNTIME_DECI2CALL)
        tc.Run("Deci2Open fails when no DECI2 host is attached", [](TestCase &t)
        {
            RuntimeEnv env;
            env.setReg(4, 1u);
            env.setReg(5, 0x00020000u);
            env.setReg(2, 7u);
            ps2_syscalls::Deci2Call(env.rdram.data(), &env.ctx, &env.runtime);
            t.IsTrue(static_cast<int32_t>(env.result()) < 0, "open reports an error instead of leaving v0 untouched");
        });
#endif

        tc.Run("callback stacks are carved below the main thread stack", [](TestCase &t)
        {
            PS2Runtime runtime;
            runtime.keepAsyncCallbackStacksBelow(0x01FE0000u);
            const uint32_t top = runtime.reserveAsyncCallbackStack(0x4000u, 16u);
            t.IsTrue(top != 0u && top <= 0x01FE0000u, "the first callback stack ends below the main stack base");
        });

        tc.Run("a boundary above the current top changes nothing", [](TestCase &t)
        {
            PS2Runtime runtime;
            runtime.keepAsyncCallbackStacksBelow(0x01FE0000u);
            runtime.keepAsyncCallbackStacksBelow(0x01FF0000u);
            const uint32_t top = runtime.reserveAsyncCallbackStack(0x4000u, 16u);
            t.IsTrue(top != 0u && top <= 0x01FE0000u, "the lower boundary still applies");
        });
    });
}

void register_ps2_sif_loadfile_tests()
{
    MiniTest::Case("PS2SifLoadfile", [](TestCase &tc)
    {
        using ps2_sif::SifLoadfileServer;

        tc.Run("LOAD_MODULE passes path and arguments and returns the module id", [](TestCase &t)
        {
            std::vector<uint8_t> send(512u, 0u);
            const uint32_t argumentSize = 4u;
            std::memcpy(send.data(), &argumentSize, sizeof(argumentSize));
            const char path[] = "cdrom0:\\MODULES\\PADMAN.IRX;1";
            std::memcpy(send.data() + 8u, path, sizeof(path));
            std::memcpy(send.data() + 8u + 252u, "a=1", 4u);
            std::string loadedPath;
            std::vector<uint8_t> loadedArguments;
            SifLoadfileServer::Modules modules;
            modules.load = [&](const std::string &p, const std::vector<uint8_t> &a)
            {
                loadedPath = p;
                loadedArguments = a;
                return 7;
            };
            const auto result = SifLoadfileServer::invoke(0u, send, modules);
            t.Equals(loadedPath, std::string(path), "path is read from offset 8");
            t.Equals(loadedArguments.size(), size_t{4u}, "argument size comes from word 0");
            t.IsTrue(result.size() == 2u && result[0] == 7u && result[1] == 0u, "module id is returned");
        });

        tc.Run("LOAD_MODULE failure returns a negative id", [](TestCase &t)
        {
            std::vector<uint8_t> send(512u, 0u);
            SifLoadfileServer::Modules modules;
            modules.load = [](const std::string &, const std::vector<uint8_t> &) { return -1; };
            const auto result = SifLoadfileServer::invoke(0u, send, modules);
            t.IsTrue(!result.empty() && static_cast<int32_t>(result[0]) < 0, "failure is negative");
        });

        tc.Run("INITIALIZE returns the loadcore version marker", [](TestCase &t)
        {
            const auto result = SifLoadfileServer::invoke(0xFFu, {}, {});
            t.IsTrue(result.size() == 1u && result[0] == 0x2E2E2E2Eu, "version marker is '....'");
        });

        tc.Run("truncated LOAD_MODULE requests fail without loading", [](TestCase &t)
        {
            bool called = false;
            SifLoadfileServer::Modules modules;
            modules.load = [&](const std::string &, const std::vector<uint8_t> &) { called = true; return 1; };
            const auto result = SifLoadfileServer::invoke(0u, std::vector<uint8_t>(16u, 0u), modules);
            t.IsFalse(called, "nothing is loaded");
            t.IsTrue(!result.empty() && static_cast<int32_t>(result[0]) < 0, "failure is negative");
        });
    });
}

void register_ps2_sif_cdvd_tests()
{
    MiniTest::Case("PS2SifCdvd", [](TestCase &tc)
    {
        using ps2_sif::SifCdvdServer;

        struct DiscLog
        {
            std::vector<std::array<uint32_t, 3>> eeReads;
            std::vector<std::array<uint32_t, 3>> iopReads;
            std::vector<std::array<uint32_t, 2>> searches;

            SifCdvdServer::Disc disc()
            {
                SifCdvdServer::Disc result;
                result.searchFile = [this](uint32_t file, uint32_t path) { searches.push_back({file, path}); return 1; };
                result.readToEe = [this](uint32_t lbn, uint32_t count, uint32_t dst) { eeReads.push_back({lbn, count, dst}); return true; };
                result.readToIop = [this](uint32_t lbn, uint32_t count, uint32_t dst) { iopReads.push_back({lbn, count, dst}); return true; };
                return result;
            }
        };

        auto words = [](std::initializer_list<uint32_t> values)
        {
            std::vector<uint8_t> bytes(values.size() * 4u);
            std::memcpy(bytes.data(), std::data(values), bytes.size());
            return bytes;
        };

        tc.Run("serves the cdvdfsv SIDs only", [](TestCase &t)
        {
            t.IsTrue(SifCdvdServer::serves(0x80000592u), "init server");
            t.IsTrue(SifCdvdServer::serves(0x80000597u), "search server");
            t.IsTrue(SifCdvdServer::serves(0x8000059Cu), "disk-ready server");
            t.IsFalse(SifCdvdServer::serves(0x80000400u), "mcserv is not cdvdfsv");
        });

        tc.Run("Init reports the server ready", [](TestCase &t)
        {
            SifCdvdServer server;
            DiscLog log;
            const auto result = server.invoke({0x80000592u, 0u, 0u, std::vector<uint8_t>(4u, 0u)}, log.disc());
            t.IsTrue(result.size() == 4u && result[3] == 0xFFu, "word 3 is 0xFF");
        });

        tc.Run("disc type follows the disc size: PS2 CD up to CD capacity, PS2 DVD above", [](TestCase &t)
        {
            t.Equals(SifCdvdServer::diskTypeForSize(460ull << 20), 0x12u, "a 460 MiB disc is a PS2 CD");
            t.Equals(SifCdvdServer::diskTypeForSize(4ull << 30), 0x14u, "a 4 GiB disc is a PS2 DVD");
        });

        tc.Run("GetDiskType reports the disc's type", [](TestCase &t)
        {
            SifCdvdServer server;
            SifCdvdServer::Disc disc;
            disc.diskType = []() { return 0x12u; };
            t.IsTrue(server.invoke({0x80000593u, 0x03u, 0u, {}}, disc) == std::vector<uint32_t>{0x12u}, "CD type is passed through");
            t.IsTrue(server.invoke({0x80000593u, 0x03u, 0u, {}}, SifCdvdServer::Disc{}) == std::vector<uint32_t>{0x14u},
                     "without a disc description the type defaults to PS2 DVD");
        });

        tc.Run("DiskReady answers ready", [&](TestCase &t)
        {
            SifCdvdServer server;
            DiscLog log;
            t.IsTrue(server.invoke({0x8000059Cu, 0u, 0u, words({0u})}, log.disc()) == std::vector<uint32_t>{2u}, "2 = ready");
            t.IsTrue(server.invoke({0x80000595u, 0x0Eu, 0u, {}}, log.disc()) == std::vector<uint32_t>{2u}, "NDiskReady = ready");
        });

        tc.Run("SearchFile picks the path offset from the request size", [](TestCase &t)
        {
            SifCdvdServer server;
            DiscLog log;
            server.invoke({0x80000597u, 0u, 0x1000u, std::vector<uint8_t>(0x128u, 0u)}, log.disc());
            server.invoke({0x80000597u, 0u, 0x2000u, std::vector<uint8_t>(0x124u, 0u)}, log.disc());
            t.Equals(log.searches.size(), size_t{2u}, "two searches");
            if (log.searches.size() == 2u)
            {
                t.IsTrue(log.searches[0][0] == 0x1000u && log.searches[0][1] == 0x1024u, "0x128 request: path at 0x24");
                t.IsTrue(log.searches[1][0] == 0x2000u && log.searches[1][1] == 0x2020u, "0x124 request: path at 0x20");
            }
        });

        tc.Run("Read and ReadIopMem read sectors to the EE and IOP", [&](TestCase &t)
        {
            SifCdvdServer server;
            DiscLog log;
            server.invoke({0x80000595u, 0x01u, 0u, words({100u, 3u, 0x20400000u, 0u})}, log.disc());
            server.invoke({0x80000595u, 0x0Du, 0u, words({200u, 2u, 0x00080000u, 0u})}, log.disc());
            t.IsTrue(log.eeReads.size() == 1u && log.eeReads[0] == std::array<uint32_t, 3>{100u, 3u, 0x00400000u},
                     "EE read is masked to RAM");
            t.IsTrue(log.iopReads.size() == 1u && log.iopReads[0] == std::array<uint32_t, 3>{200u, 2u, 0x00080000u},
                     "IOP read keeps the IOP address");
        });

        tc.Run("stream start, read, and seek track the stream position", [&](TestCase &t)
        {
            SifCdvdServer server;
            DiscLog log;
            server.invoke({0x80000595u, 0x09u, 0u, words({50u, 0u, 0u, 1u, 0u})}, log.disc());
            const auto read = server.invoke({0x80000595u, 0x09u, 0u, words({0u, 4u, 0x500000u, 2u, 0u})}, log.disc());
            server.invoke({0x80000595u, 0x09u, 0u, words({0u, 2u, 0x600000u, 2u, 0u})}, log.disc());
            t.IsTrue(read == std::vector<uint32_t>{4u}, "stream read returns the sector count");
            t.IsTrue(log.eeReads.size() == 2u && log.eeReads[0][0] == 50u && log.eeReads[1][0] == 54u,
                     "stream reads continue from the previous position");
        });

        tc.Run("ReadChain reads every tuple until the terminator", [](TestCase &t)
        {
            SifCdvdServer server;
            DiscLog log;
            std::vector<uint32_t> chain{10u, 1u, 0x100000u, 20u, 2u, 0x200000u, ~0u, ~0u, ~0u};
            std::vector<uint8_t> send(chain.size() * 4u);
            std::memcpy(send.data(), chain.data(), send.size());
            server.invoke({0x80000595u, 0x0Fu, 0u, send}, log.disc());
            t.Equals(log.eeReads.size(), size_t{2u}, "two tuples are read");
        });
    });
}
