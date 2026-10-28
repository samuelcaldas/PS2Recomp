#include "Common.h"
#include "SIF.h"
#include "SifCommandEndpoint.h"
#include "SifLoadfileServer.h"
#include "SifCdvdServer.h"
#include "../Syscalls/RPC.h"
#include "../../ps2_iop_transport.h"
#include "runtime/ps2_address.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ps2_stubs
{
    void sceSifCmdIntrHdlr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSifCmdIntrHdlr", rdram, ctx, runtime);
    }

    void sceSifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifLoadModule(rdram, ctx, runtime);
    }

    void sceSifSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t srcAddr = getRegU32(ctx, 7); // $a3
        const uint32_t dstAddr = readStackU32(rdram, ctx, 16);
        const uint32_t size = readStackU32(rdram, ctx, 20);
        if (size != 0u && srcAddr != 0u && dstAddr != 0u)
        {
            std::vector<uint8_t> payload(size);
            bool valid = runtime != nullptr;
            for (uint32_t i = 0; i < size; ++i)
            {
                const uint8_t *src = getConstMemPtr(rdram, srcAddr + i);
                if (!src)
                {
                    valid = false;
                    break;
                }
                payload[i] = *src;
            }
            if (!valid || !runtime->writeIopMemory(dstAddr, payload.data(), payload.size()))
            {
                setReturnS32(ctx, 0);
                return;
            }
        }

        setReturnS32(ctx, 1);
    }

    namespace
    {
        struct Ps2SifDmaTransfer
        {
            uint32_t src = 0;
            uint32_t dest = 0;
            int32_t size = 0;
            int32_t attr = 0;
        };
        static_assert(sizeof(Ps2SifDmaTransfer) == 16u, "Unexpected SIF DMA descriptor size");

        std::mutex g_sifDmaTransferMutex;
        uint32_t g_nextSifDmaTransferId = 1u;
        std::mutex g_sifCmdStateMutex;
        std::unordered_map<uint32_t, uint32_t> g_sifRegs;
        std::unordered_map<uint32_t, uint32_t> g_sifSregs;
        struct SifCmdHandler
        {
            uint32_t function = 0u;
            uint32_t argument = 0u;
        };

        std::unordered_map<uint32_t, SifCmdHandler> g_sifCmdHandlers;
        uint32_t g_sifCmdBuffer = 0u;
        uint32_t g_sifSysCmdBuffer = 0u;
        bool g_sifCmdInitialized = false;
        uint32_t g_sifGetRegLogCount = 0u;
        uint32_t g_sifSetRegLogCount = 0u;

        // IOP side of the packet protocol used by guest-code sifcmd/sifrpc libraries. Its
        // buffers live in IOP RAM, which only exists once a runtime is attached.
        ps2_sif::SifCommandEndpoint g_sifEndpoint;
        ps2_sif::SifCdvdServer g_sifCdvdServer;
        uint32_t g_iopCommandBuffer = 0u;
        uint32_t g_iopRpcBuffer = 0u;
        // Set by a RESET packet; the reboot completes when the EE next polls SMFLAG.
        bool g_iopRebootPending = false;
        bool g_sifDeliveryScheduled = false;
        struct RpcDiagnosticRecord
        {
            uint64_t sequence = 0u;
            uint32_t client = 0u;
            uint32_t packet = 0u;
            uint32_t rpcId = 0u;
            std::array<uint32_t, 14> request{};
        };
        uint64_t g_rpcDiagnosticSequence = 0u;
        std::unordered_map<uint64_t, std::deque<RpcDiagnosticRecord>> g_rpcDiagnostics;

        uint64_t rpcDiagnosticKey(uint32_t client, uint32_t rpcId)
        {
            return (static_cast<uint64_t>(client) << 32u) | rpcId;
        }

        constexpr uint32_t kSifRegBootStatus = 0x4u;
        constexpr uint32_t kSifRegMainAddr = 0x80000000u;
        constexpr uint32_t kSifRegSubAddr = 0x80000001u;
        constexpr uint32_t kSifRegMsCom = 0x80000002u;
        constexpr uint32_t kSifBootReadyMask = 0x00020000u;
        // SMFLAG after an IOP boot: command library ready (CMDINIT) and boot finished (BOOTEND).
        constexpr uint32_t kSifRebootedStatus = 0x00060000u;
        // SIF_REG_SUBADDR: where the IOP sifcmd module receives EE command packets.
        constexpr uint32_t kSifRegIopCommandBuffer = 0x2u;
        constexpr uint32_t kIopCommandBufferSize = 0x100u;
        constexpr uint32_t kIopRpcBufferSize = 0x20000u;
        constexpr uint32_t kDmacCauseSif0 = 5u;
        // IOP turnaround before a reply reaches the EE (~55 us). Without it, a thread that
        // polls an IOP server wakes instantly and starves lower-priority threads.
        constexpr uint64_t kSifReplyLatencyCycles = 0x4000u;

        void seedDefaultSifRegsLocked()
        {
            g_sifRegs.clear();
            g_sifSregs.clear();
            g_sifCmdHandlers.clear();
            g_sifCmdBuffer = 0u;
            g_sifSysCmdBuffer = 0u;
            g_sifCmdInitialized = false;
            g_sifGetRegLogCount = 0u;
            g_sifSetRegLogCount = 0u;

            g_sifRegs[kSifRegBootStatus] = kSifBootReadyMask;
            g_sifRegs[kSifRegMainAddr] = 0u;
            g_sifRegs[kSifRegSubAddr] = 0u;
            g_sifRegs[kSifRegMsCom] = 0u;
            g_sifEndpoint.reset();
            g_sifCdvdServer = {};
            g_iopCommandBuffer = 0u;
            g_iopRpcBuffer = 0u;
            g_iopRebootPending = false;
            g_sifDeliveryScheduled = false;
            g_rpcDiagnostics.clear();
            g_rpcDiagnosticSequence = 0u;
        }

        bool shouldTraceSifReg(uint32_t reg)
        {
            switch (reg)
            {
            case 0x2u:
            case 0x4u:
            case 0x80000000u:
            case 0x80000001u:
            case 0x80000002u:
                return true;
            default:
                return false;
            }
        }

        struct SifStateInitializer
        {
            SifStateInitializer()
            {
                std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                seedDefaultSifRegsLocked();
            }
        } g_sifStateInitializer;

        uint32_t allocateSifDmaTransferId()
        {
            std::lock_guard<std::mutex> lock(g_sifDmaTransferMutex);
            uint32_t id = g_nextSifDmaTransferId++;
            if (id == 0u)
            {
                id = g_nextSifDmaTransferId++;
            }
            return id;
        }

        bool isCopyableGuestAddress(uint32_t addr)
        {
            if (Ps2AddressInRange(addr, PS2_SCRATCHPAD_BASE, PS2_SCRATCHPAD_SIZE))
            {
                return true;
            }

            if (addr < PS2_EE_UNCACHED_RAM_MIRROR_BASE)
            {
                return true;
            }

            if (Ps2IsUncachedRamMirrorAddress(addr))
            {
                return true;
            }

            if (Ps2IsKseg01Address(addr))
            {
                return true;
            }

            return false;
        }

        bool canAccessEeRange(const uint8_t *rdram, uint32_t address, uint32_t sizeBytes)
        {
            if (!rdram)
            {
                return false;
            }
            if (sizeBytes == 0u)
            {
                return true;
            }
            if (sizeBytes - 1u > std::numeric_limits<uint32_t>::max() - address)
            {
                return false;
            }
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                const uint32_t byteAddress = address + i;
                if (!isCopyableGuestAddress(byteAddress) || getConstMemPtr(rdram, byteAddress) == nullptr)
                {
                    return false;
                }
            }
            return true;
        }

        bool readEeRange(const uint8_t *rdram, uint32_t address, void *destination, uint32_t sizeBytes)
        {
            if ((!destination && sizeBytes != 0u) || !canAccessEeRange(rdram, address, sizeBytes))
                return false;
            auto *bytes = static_cast<uint8_t *>(destination);
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                const uint8_t *source = getConstMemPtr(rdram, address + i);
                if (!source)
                    return false;
                bytes[i] = *source;
            }
            return true;
        }

        bool writeEeRange(uint8_t *rdram, uint32_t address, const void *source, uint32_t sizeBytes)
        {
            if ((!source && sizeBytes != 0u) || !canAccessEeRange(rdram, address, sizeBytes))
                return false;
            ps2TraceGuestRangeWrite(rdram, address, sizeBytes, "SIF IOP-to-EE DMA", nullptr);
            const auto *bytes = static_cast<const uint8_t *>(source);
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                uint8_t *destination = getMemPtr(rdram, address + i);
                if (!destination)
                    return false;
                *destination = bytes[i];
            }
            return true;
        }

        // Allocates the IOP command and RPC buffers on first use and publishes the command
        // buffer in SIF_REG_SUBADDR, as the IOP sifcmd module does at boot.
        void ensureIopBuffersLocked(PS2Runtime *runtime)
        {
            if (g_iopCommandBuffer != 0u || !runtime)
                return;
            g_iopCommandBuffer = runtime->allocateIopMemory(kIopCommandBufferSize, 64u);
            g_iopRpcBuffer = runtime->allocateIopMemory(kIopRpcBufferSize, 64u);
            g_sifRegs[kSifRegIopCommandBuffer] = g_iopCommandBuffer;
            g_sifEndpoint.setRpcBuffer(g_iopRpcBuffer);
        }

        void serveLoadfile(uint8_t *rdram, PS2Runtime *runtime, const ps2_sif::SifCommandEndpoint::RpcCall &call)
        {
            std::vector<uint8_t> send(call.sendSize);
            if (!readEeRange(rdram, call.sendAddress, send.data(), call.sendSize))
                send.clear();
            ps2_sif::SifLoadfileServer::Modules modules;
            modules.load = [runtime](const std::string &path, const std::vector<uint8_t> &arguments)
            {
                const auto loaded = runtime->loadIopModule(path, arguments.empty() ? nullptr : arguments.data(),
                                                           static_cast<uint32_t>(arguments.size()));
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "[sif:loadfile] path=" << path << " handled=" << loaded.handled
                              << " id=" << loaded.moduleId << std::endl;
                });
                return loaded.handled && loaded.moduleId > 0 ? loaded.moduleId : -1;
            };
            const std::vector<uint32_t> result = ps2_sif::SifLoadfileServer::invoke(call.function, send, modules);
            const uint32_t size = std::min<uint32_t>(call.receiveSize, static_cast<uint32_t>(result.size() * 4u));
            (void)writeEeRange(rdram, call.receiveAddress, result.data(), size);
        }

        // Size of the mounted disc: the image file, or the extracted tree under the CD root.
        uint64_t virtualDiscBytes()
        {
            static std::filesystem::path measuredPath;
            static uint64_t measuredBytes = 0u;
            const std::filesystem::path image = getCdImagePath();
            const std::filesystem::path source = image.empty() ? getCdRootPath() : image;
            if (source == measuredPath)
                return measuredBytes;
            std::error_code error;
            uint64_t total = 0u;
            if (std::filesystem::is_regular_file(source, error))
                total = std::filesystem::file_size(source, error);
            for (auto it = std::filesystem::recursive_directory_iterator(source, error);
                 !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
            {
                if (it->is_regular_file(error))
                    total += it->file_size(error);
            }
            measuredPath = source;
            measuredBytes = total;
            return total;
        }

        ps2_sif::SifCdvdServer::Disc virtualDisc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ps2_sif::SifCdvdServer::Disc disc;
            disc.searchFile = [rdram, ctx, runtime](uint32_t fileAddress, uint32_t pathAddress)
            {
                R5900Context scratch = *ctx;
                SET_GPR_U32(&scratch, 4, fileAddress);
                SET_GPR_U32(&scratch, 5, pathAddress);
                ps2_stubs::sceCdSearchFile(rdram, &scratch, runtime);
                return static_cast<int32_t>(getRegU32(&scratch, 2));
            };
            disc.readToEe = [rdram](uint32_t lbn, uint32_t count, uint32_t eeDestination)
            {
                const uint64_t bytes = std::min<uint64_t>(static_cast<uint64_t>(count) * kCdSectorSize,
                                                          PS2_RAM_SIZE - (eeDestination & PS2_RAM_MASK));
                ps2TraceGuestRangeWrite(rdram, eeDestination, static_cast<uint32_t>(bytes), "SIF CDVD read", nullptr);
                const bool ok = readCdSectors(lbn, count, rdram + (eeDestination & PS2_RAM_MASK), static_cast<size_t>(bytes));
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "[sif:cdread] lbn=0x" << std::hex << lbn << " count=0x" << count << " dst=0x"
                              << eeDestination << " bytes=0x" << bytes << std::dec << " ok=" << ok << std::endl;
                });
                return ok;
            };
            disc.diskType = []()
            {
                return ps2_sif::SifCdvdServer::diskTypeForSize(virtualDiscBytes());
            };
            disc.readToIop = [runtime](uint32_t lbn, uint32_t count, uint32_t iopDestination)
            {
                std::vector<uint8_t> data(static_cast<size_t>(count) * kCdSectorSize);
                return readCdSectors(lbn, count, data.data(), data.size()) &&
                       runtime->writeIopMemory(iopDestination, data.data(), data.size());
            };
            return disc;
        }

        void serveCdvd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const ps2_sif::SifCommandEndpoint::RpcCall &call)
        {
            ps2_sif::SifCdvdServer::Request request;
            request.sid = call.sid;
            request.function = call.function;
            request.sendAddress = call.sendAddress;
            request.send.resize(call.sendSize);
            if (!readEeRange(rdram, call.sendAddress, request.send.data(), call.sendSize))
                request.send.clear();
            const std::vector<uint32_t> result = g_sifCdvdServer.invoke(request, virtualDisc(rdram, ctx, runtime));
            PS2_IF_AGRESSIVE_LOGS({
                std::cerr << "[sif:cdvd] sid=0x" << std::hex << call.sid << " fn=0x" << call.function
                          << " send=0x" << call.sendSize << " result=0x" << (result.empty() ? 0u : result[0])
                          << std::dec << std::endl;
            });
            const uint32_t size = std::min<uint32_t>(call.receiveSize, static_cast<uint32_t>(result.size() * 4u));
            (void)writeEeRange(rdram, call.receiveAddress, result.data(), size);
            // On real PS2, async NCmds complete via a CDVD DMA interrupt that:
            //   1. Fills a slot in D_4F1400 (the EE-side NCmd completion ring, 4 × 0x38-byte entries):
            //      slot[0x00] = 1 (occupied), slot[0x04] = 0 (pending), slot[0x14] = fn_code
            //   2. Signals the EE sema at D_4ED300+0x48 (= 0x4ED348).
            // Thread 7 (func_0012B648) wakes, reads the slot via func_0012BE70, dispatches the
            // command, and eventually unblocks the loading thread (sema 27).
            // The HLE is synchronous. We replicate both steps here.
            if (ps2_sif::SifCdvdServer::isAsyncNCommand(call.function) && runtime)
            {
                // Step 1: write a completion slot into D_4F1400 (EE RAM).
                // Scan for a free slot (slot[0] == 0). Slot stride = 0x38, 4 slots.
                constexpr uint32_t kNcmdRingBase = 0x4F1400u;
                constexpr uint32_t kSlotStride   = 0x38u;
                constexpr uint32_t kSlotCount    = 4u;
                uint32_t slotAddr = 0u;
                for (uint32_t i = 0u; i < kSlotCount; ++i)
                {
                    const uint32_t addr = kNcmdRingBase + i * kSlotStride;
                    uint32_t busy = 0u;
                    (void)readEeRange(rdram, addr, &busy, sizeof(busy));
                    if (busy == 0u) { slotAddr = addr; break; }
                }
                if (slotAddr)
                {
                    // slot[+0x00] = 1 (busy/occupied), slot[+0x04] = 0 (not claimed),
                    // slot[+0x14] = fn_code (thread 7 reads this to dispatch the command).
                    const uint32_t one  = 1u;
                    const uint32_t zero = 0u;
                    const uint32_t fn   = call.function;
                    (void)writeEeRange(rdram, slotAddr + 0x00u, &one,  sizeof(one));
                    (void)writeEeRange(rdram, slotAddr + 0x04u, &zero, sizeof(zero));
                    (void)writeEeRange(rdram, slotAddr + 0x14u, &fn,   sizeof(fn));
                    PS2_IF_AGRESSIVE_LOGS({
                        std::cerr << "[sif:cdvd] NCmd slot at 0x" << std::hex << slotAddr
                                  << " fn=0x" << call.function << std::dec << '\n';
                    });
                }
                else
                {
                    std::cerr << "[sif:cdvd] WARNING: all NCmd slots busy, skipping slot write for fn=0x"
                              << std::hex << call.function << std::dec << '\n';
                }
                // Step 2: signal the EE NCmd completion sema (D_4ED348).
                // If sema not yet created (CDVD init races with GetToc), defer and drain later.
                uint32_t ncmd_sema = 0u;
                (void)readEeRange(rdram, 0x4ED348u, &ncmd_sema, sizeof(ncmd_sema));
                if (static_cast<int32_t>(ncmd_sema) > 0)
                {
                    const uint32_t pending = ps2_sif::SifCdvdServer::s_pendingNcmdSignals.exchange(0u, std::memory_order_relaxed);
                    const uint32_t total = pending + 1u;
                    PS2_IF_AGRESSIVE_LOGS({
                        std::cerr << "[sif:cdvd] async NCmd fn=0x" << std::hex << call.function
                                  << " signaling ncmd_sema=" << std::dec << ncmd_sema
                                  << " x" << total << " (pending=" << pending << ")" << std::endl;
                    });
                    for (uint32_t i = 0u; i < total; ++i)
                        (void)runtime->eeScheduler().signalSemaphore(static_cast<int>(ncmd_sema), true);
                }
                else
                {
                    ps2_sif::SifCdvdServer::s_pendingNcmdSignals.fetch_add(1u, std::memory_order_relaxed);
                    PS2_IF_AGRESSIVE_LOGS({
                        std::cerr << "[sif:cdvd] async NCmd fn=0x" << std::hex << call.function
                                  << " deferred (sema not created yet), pending="
                                  << std::dec << ps2_sif::SifCdvdServer::s_pendingNcmdSignals.load() << std::endl;
                    });
                }
            }
        }

        void callIopRpc(uint8_t *rdram,
                        R5900Context *ctx,
                        PS2Runtime *runtime,
                        const ps2_sif::SifCommandEndpoint::RpcCall &call)
        {
            if (call.sid == ps2_sif::SifLoadfileServer::kSid)
            {
                serveLoadfile(rdram, runtime, call);
                return;
            }
            if (ps2_sif::SifCdvdServer::serves(call.sid))
            {
                serveCdvd(rdram, ctx, runtime, call);
                return;
            }
            ps2x::iop::RpcRequest request{};
            request.clientAddress = call.clientAddress;
            request.sid = call.sid;
            request.function = call.function;
            request.mode = call.mode;
            request.send = {call.sendAddress, call.sendSize};
            request.receive = {call.receiveAddress, call.receiveSize};
            const ps2x::iop::RpcResult result = PS2IopTransport::handleRpc(runtime, rdram, ctx, request);
            PS2_IF_AGRESSIVE_LOGS({
                std::cerr << "[sif:call] sid=0x" << std::hex << call.sid << " fn=0x" << call.function
                          << " send=0x" << call.sendAddress << "+0x" << call.sendSize
                          << " recv=0x" << call.receiveAddress << "+0x" << call.receiveSize
                          << " handled=" << std::dec << result.handled;
                uint32_t words[4] = {};
                if (readEeRange(rdram, call.sendAddress, words, std::min<uint32_t>(16u, call.sendSize)))
                    std::cerr << std::hex << " sendw=" << words[0] << "," << words[1] << "," << words[2] << "," << words[3];
                std::memset(words, 0, sizeof(words));
                const uint32_t replyAddress = result.resultAddress != 0u ? result.resultAddress : call.receiveAddress;
                if (readEeRange(rdram, replyAddress, words, std::min<uint32_t>(16u, call.receiveSize)))
                    std::cerr << " recvw=" << words[0] << "," << words[1] << "," << words[2] << "," << words[3];
                std::cerr << std::dec << std::endl;
            });
            if (!result.handled || call.receiveSize == 0u || result.resultAddress == 0u ||
                result.resultAddress == call.receiveAddress)
                return;
            std::vector<uint8_t> data(call.receiveSize);
            if (readEeRange(rdram, result.resultAddress, data.data(), call.receiveSize))
                (void)writeEeRange(rdram, call.receiveAddress, data.data(), call.receiveSize);
        }

        ps2_sif::SifCommandEndpoint::Services endpointServices(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ps2_sif::SifCommandEndpoint::Services services;
            services.canBind = [runtime](uint32_t sid)
            {
                const bool bound = sid == ps2_sif::SifLoadfileServer::kSid || ps2_sif::SifCdvdServer::serves(sid) ||
                                   PS2IopTransport::canBindRpc(runtime, sid);
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "[sif:bind] sid=0x" << std::hex << sid << std::dec << " bound=" << bound << std::endl;
                });
                return bound;
            };
            services.call = [rdram, ctx, runtime](const ps2_sif::SifCommandEndpoint::RpcCall &call)
            {
                callIopRpc(rdram, ctx, runtime, call);
            };
            services.rebootIop = []()
            {
                std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                g_iopRebootPending = true;
            };
            services.copyIopToEe = [rdram, runtime](uint32_t eeDestination, uint32_t iopSource, uint32_t size)
            {
                std::vector<uint8_t> data(size);
                if (runtime && runtime->readIopMemory(iopSource, data.data(), size))
                    (void)writeEeRange(rdram, eeDestination, data.data(), size);
            };
            return services;
        }

        bool scheduleSifReply(uint8_t *rdram, PS2Runtime *runtime);

        // Writes the next IOP reply into the EE receive buffer and raises the SIF0 DMAC
        // interrupt. Returns false when no reply was ready. Runs on the executor thread.
        bool deliverSifReply(uint8_t *rdram, PS2Runtime *runtime)
        {
            if (!runtime)
                return false;
            const std::optional<ps2_sif::SifCommandEndpoint::Reply> reply = g_sifEndpoint.nextReply();
            if (!reply)
                return false;
            const uint32_t size = static_cast<uint32_t>(reply->bytes.size());
            PS2_IF_AGRESSIVE_LOGS({
                uint32_t replyWords[16] = {};
                uint32_t destinationBefore[4] = {};
                std::memcpy(replyWords, reply->bytes.data(), std::min<size_t>(sizeof(replyWords), reply->bytes.size()));
                for (uint32_t index = 0u; index < 4u; ++index)
                {
                    const auto *word = reinterpret_cast<const uint32_t *>(getMemPtr(rdram, reply->eeAddress + index * 4u));
                    if (word)
                        destinationBefore[index] = *word;
                }
                std::cerr << "[sif:reply] cid=0x" << std::hex << replyWords[2] << " to=0x" << reply->eeAddress
                          << " size=0x" << size << " words=";
                for (size_t index = 0u; index < std::min<size_t>(16u, reply->bytes.size() / sizeof(uint32_t)); ++index)
                    std::cerr << (index == 0u ? "" : ",") << "0x" << replyWords[index];
                std::cerr << " dst-before=" << destinationBefore[0] << "," << destinationBefore[1]
                          << "," << destinationBefore[2] << "," << destinationBefore[3] << std::dec << std::endl;
            });
            if (reply->eeAddress >= PS2_RAM_SIZE || size > PS2_RAM_SIZE - reply->eeAddress ||
                !writeEeRange(rdram, reply->eeAddress, reply->bytes.data(), size))
            {
                std::cerr << "[sif:reply] failed destination=0x" << std::hex << reply->eeAddress
                          << std::dec << " size=" << size << std::endl;
                g_sifEndpoint.acknowledge();
                if (g_sifEndpoint.hasDeliverableReply())
                    (void)scheduleSifReply(rdram, runtime);
                return false;
            }
            PS2_IF_AGRESSIVE_LOGS({
                uint32_t destinationAfter[4] = {};
                for (uint32_t index = 0u; index < 4u; ++index)
                {
                    const auto *word = reinterpret_cast<const uint32_t *>(getMemPtr(rdram, reply->eeAddress + index * 4u));
                    if (word)
                        destinationAfter[index] = *word;
                }
                std::cerr << "[sif:reply-written] to=0x" << std::hex << reply->eeAddress
                          << " words=" << destinationAfter[0] << "," << destinationAfter[1] << ","
                          << destinationAfter[2] << "," << destinationAfter[3] << std::dec << std::endl;
            });
            bool diagnosticDispatched = false;
            PS2_IF_AGRESSIVE_LOGS((diagnosticDispatched = [&]() {
                uint32_t words[16] = {};
                if (size >= sizeof(uint32_t) * 9u)
                {
                    std::memcpy(words, reply->bytes.data(), std::min<size_t>(sizeof(words), reply->bytes.size()));
                    if (words[2] == 0x80000008u && words[8] == 0x8000000Au)
                    {
                        const uint32_t client = words[7];
                        const uint32_t rpcId = words[6];
                        const uint32_t packet = words[5] & 0x01FFFFFFu;
                        auto found = g_rpcDiagnostics.find(rpcDiagnosticKey(client, rpcId));
                        if (found != g_rpcDiagnostics.end() && !found->second.empty())
                        {
                            RpcDiagnosticRecord record = found->second.front();
                            found->second.pop_front();
                            auto snapshot = [rdram, record, words]()
                            {
                                uint32_t clientWords[8] = {};
                                if (record.client != 0u && readEeRange(rdram, record.client, clientWords, sizeof(clientWords)))
                                    std::cerr << "[sif:rpc-client-before] seq=" << record.sequence << " client=0x" << std::hex << record.client
                                              << " packet=0x" << record.packet << " rpcid=0x" << record.rpcId << " hdr="
                                              << clientWords[0] << "," << clientWords[1] << "," << clientWords[2] << "," << clientWords[3]
                                              << " req=" << record.request[0] << "," << record.request[4] << "," << record.request[5]
                                              << " reply=" << words[0] << "," << words[4] << "," << words[5] << std::dec << std::endl;
                                else
                                    std::cerr << "[sif:rpc-client-before] seq=" << record.sequence << " client=0x" << std::hex << record.client << " unreadable" << std::dec << std::endl;
                            };
                            snapshot();
                            runtime->eeScheduler().dispatchIrq(true, kDmacCauseSif0, [rdram, record]()
                            {
                                uint32_t clientWords[8] = {};
                                if (record.client != 0u && readEeRange(rdram, record.client, clientWords, sizeof(clientWords)))
                                    std::cerr << "[sif:rpc-client-after-handler] seq=" << record.sequence << " client=0x" << std::hex << record.client
                                              << " packet=0x" << record.packet << " rpcid=0x" << record.rpcId << " hdr="
                                              << clientWords[0] << "," << clientWords[1] << "," << clientWords[2] << "," << clientWords[3]
                                              << " handler-complete=1" << std::dec << std::endl;
                                else
                                    std::cerr << "[sif:rpc-client-after-handler] seq=" << record.sequence << " client=0x" << std::hex << record.client << " unreadable handler-complete=1" << std::dec << std::endl;
                            });
                            return true;
                        }
                    }
                }
                return false;
            }()));
            if (!diagnosticDispatched)
                ps2_syscalls::dispatchDmacHandlersForCause(rdram, runtime, kDmacCauseSif0);
            return true;
        }

        // Delivers the next reply after the IOP latency when guest code is running; direct
        // stub calls (no EE time) deliver at once. Returns true if a reply was or will be sent.
        bool scheduleSifReply(uint8_t *rdram, PS2Runtime *runtime)
        {
            if (!runtime || !runtime->eeScheduler().isExecutingGuest())
                return deliverSifReply(rdram, runtime);
            if (g_sifDeliveryScheduled)
                return true;
            if (!g_sifEndpoint.hasDeliverableReply())
                return false;
            g_sifDeliveryScheduled = true;
            runtime->eeScheduler().scheduleHostCallback(kSifReplyLatencyCycles, [rdram, runtime]()
            {
                g_sifDeliveryScheduled = false;
                (void)deliverSifReply(rdram, runtime);
            });
            return true;
        }

        // Hands EE packets DMAed into the IOP command buffer to the endpoint and remembers
        // the source of RPC arguments DMAed into the RPC buffer.
        void forwardToEndpoint(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const Ps2SifDmaTransfer &xfer)
        {
            uint32_t commandBuffer = 0u;
            uint32_t rpcBuffer = 0u;
            {
                std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                ensureIopBuffersLocked(runtime);
                commandBuffer = g_iopCommandBuffer;
                rpcBuffer = g_iopRpcBuffer;
            }
            if (rpcBuffer != 0u && xfer.dest == rpcBuffer)
            {
                g_sifEndpoint.noteRpcData(xfer.src);
                return;
            }
            if (commandBuffer == 0u || xfer.dest != commandBuffer)
                return;
            std::vector<uint8_t> packet(static_cast<uint32_t>(xfer.size));
            if (!readEeRange(rdram, xfer.src, packet.data(), static_cast<uint32_t>(packet.size())))
                return;
            PS2_IF_AGRESSIVE_LOGS({
                uint32_t words[14] = {};
                std::memcpy(words, packet.data(), std::min<size_t>(sizeof(words), packet.size()));
                std::cerr << "[sif:packet] cid=0x" << std::hex << words[2] << " opt=0x" << words[3]
                          << " size=0x" << packet.size() << " words=";
                for (size_t index = 0u; index < std::min<size_t>(14u, packet.size() / sizeof(uint32_t)); ++index)
                    std::cerr << (index == 0u ? "" : ",") << "0x" << words[index];
                std::cerr << std::dec << std::endl;
            });
            if (packet.size() >= 56u)
            {
                uint32_t words[14] = {};
                std::memcpy(words, packet.data(), sizeof(words));
                if (words[2] == 0x8000000Au)
                {
                    RpcDiagnosticRecord record{};
                    record.sequence = ++g_rpcDiagnosticSequence;
                    record.client = words[7];
                    record.packet = words[5] & 0x01FFFFFFu;
                    record.rpcId = words[6];
                    std::copy(std::begin(words), std::end(words), record.request.begin());
                    auto &records = g_rpcDiagnostics[rpcDiagnosticKey(record.client, record.rpcId)];
                    if (records.size() == 16u)
                        records.pop_front();
                    records.push_back(record);
                }
            }
            g_sifEndpoint.receive(packet, endpointServices(rdram, ctx, runtime));
        }
    }

    void resetSifState()
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        seedDefaultSifRegsLocked();
    }

    bool dispatchSifCommand(uint8_t *rdram,
                            PS2Runtime *runtime,
                            uint32_t commandId,
                            const void *packet,
                            size_t packetSize) noexcept
    {
        if (!rdram || !runtime || !packet || packetSize < 16u || packetSize > 112u)
            return false;

        SifCmdHandler registered{};
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            const auto handler = g_sifCmdHandlers.find(commandId);
            if (handler == g_sifCmdHandlers.end() || handler->second.function == 0u)
                return false;
            registered = handler->second;
        }

        if (!runtime->hasFunction(registered.function))
            return false;

        const uint32_t packetAddress = runtime->guestMalloc(static_cast<uint32_t>(packetSize), 16u);
        if (packetAddress == 0u)
            return false;

        uint8_t *const first = getMemPtr(rdram, packetAddress);
        uint8_t *const last = getMemPtr(rdram, packetAddress + static_cast<uint32_t>(packetSize - 1u));
        if (!first || !last || last < first || static_cast<size_t>(last - first) != packetSize - 1u)
        {
            runtime->guestFree(packetAddress);
            return false;
        }

        ps2TraceGuestRangeWrite(rdram, packetAddress, static_cast<uint32_t>(packetSize), "SIF command packet", nullptr);
        std::memcpy(first, packet, packetSize);

        try
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::SifCommand;
            invocation.tag = commandId;
            invocation.context = runtime->cpu();
            invocation.context.pc = registered.function;
            SET_GPR_U32(&invocation.context, 4, packetAddress);
            SET_GPR_U32(&invocation.context, 5, registered.argument);
            SET_GPR_U32(&invocation.context, 6, 0u);
            SET_GPR_U32(&invocation.context, 7, 0u);
            SET_GPR_U32(&invocation.context, 29, 0u);
            SET_GPR_U32(&invocation.context, 31, 0u);
            invocation.onComplete = [runtime, packetAddress](const R5900Context &, R5900Context &)
            {
                runtime->guestFree(packetAddress);
            };
            runtime->eeScheduler().queueInvocation(std::move(invocation));
            return true;
        }
        catch (...)
        {
            runtime->guestFree(packetAddress);
            return false;
        }
    }

    void sceSifAddCmdHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cid = getRegU32(ctx, 4);
        const uint32_t handler = getRegU32(ctx, 5);
        const uint32_t argument = getRegU32(ctx, 6);
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdHandlers[cid] = SifCmdHandler{handler, argument};
        setReturnS32(ctx, 0);
    }

    void sceSifAllocIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t reqSize = getRegU32(ctx, 4);
        setReturnU32(ctx, runtime ? runtime->allocateIopMemory(reqSize, 64u) : 0u);
    }

    void sceSifAllocSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t size = getRegU32(ctx, 5);
        setReturnU32(ctx, runtime ? runtime->allocateIopMemory(size, 64u) : 0u);
    }

    void sceSifBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifBindRpc(rdram, ctx, runtime);
    }

    void sceSifCheckStatRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifCheckStatRpc(rdram, ctx, runtime);
    }

    void sceSifDmaStat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        (void)getRegU32(ctx, 4); // trid

        // Transfers are applied immediately by sceSifSetDma in this runtime.
        setReturnS32(ctx, -1);
    }

    void sceSifExecRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifExitCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        seedDefaultSifRegsLocked();
        setReturnS32(ctx, 0);
    }

    void sceSifExitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifFreeIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t addr = getRegU32(ctx, 4);
        setReturnS32(ctx, runtime && runtime->freeIopMemory(addr) ? 0 : -1);
    }

    void sceSifFreeSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t addr = getRegU32(ctx, 4);
        setReturnS32(ctx, runtime && runtime->freeIopMemory(addr) ? 0 : -1);
    }

    void sceSifGetDataTable(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        setReturnU32(ctx, g_sifCmdBuffer);
    }

    void sceSifGetIopAddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, getRegU32(ctx, 4));
    }

    void sceSifGetNextRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifGetOtherData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t rdAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const uint32_t dstAddr = getRegU32(ctx, 6);
        const int32_t sizeSigned = static_cast<int32_t>(getRegU32(ctx, 7));

        if (sizeSigned <= 0)
        {
            setReturnS32(ctx, 0);
            return;
        }

        const uint32_t size = static_cast<uint32_t>(sizeSigned);
        if (size > PS2_RAM_SIZE)
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                std::cerr << "sceSifGetOtherData rejected oversized transfer size=0x"
                          << std::hex << size << std::dec << std::endl;
                ++warnCount;
            }
            setReturnS32(ctx, -1);
            return;
        }

        if (runtime)
        {
            PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                ps2x::iop::SifTransferKind::GetOtherData,
                                                                ps2x::iop::SifTransferPhase::BeforeCopy,
                                                                srcAddr,
                                                                dstAddr,
                                                                size,
                                                            });
        }

        std::vector<uint8_t> payload(size);
        if (!runtime || !runtime->isIopMemoryRange(srcAddr, size) ||
            !canAccessEeRange(rdram, dstAddr, size) ||
            !runtime->readIopMemory(srcAddr, payload.data(), payload.size()) ||
            !writeEeRange(rdram, dstAddr, payload.data(), size))
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "sceSifGetOtherData copy failed src=0x" << std::hex << srcAddr
                              << " dst=0x" << dstAddr
                              << " size=0x" << size
                              << std::dec << std::endl;
                });
                ++warnCount;
            }
            setReturnS32(ctx, -1);
            return;
        }

        // SifRpcReceiveData_t keeps src/dest/size at offsets 0x10/0x14/0x18.
        if (uint8_t *rd = getMemPtr(rdram, rdAddr))
        {
            std::memcpy(rd + 0x10u, &srcAddr, sizeof(srcAddr));
            std::memcpy(rd + 0x14u, &dstAddr, sizeof(dstAddr));
            std::memcpy(rd + 0x18u, &size, sizeof(size));
        }

        if (runtime)
        {
            PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                ps2x::iop::SifTransferKind::GetOtherData,
                                                                ps2x::iop::SifTransferPhase::AfterCopy,
                                                                srcAddr,
                                                                dstAddr,
                                                                size,
                                                            });
        }

        setReturnS32(ctx, 0);
    }

    void sceSifGetReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        uint32_t value = 0u;
        bool shouldLog = false;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            if (reg == kSifRegIopCommandBuffer)
                ensureIopBuffersLocked(runtime);
            if (reg == kSifRegBootStatus && g_iopRebootPending)
            {
                g_iopRebootPending = false;
                g_sifRegs[kSifRegBootStatus] |= kSifRebootedStatus;
            }
            auto it = g_sifRegs.find(reg);
            if (it != g_sifRegs.end())
            {
                value = it->second;
            }
            shouldLog = shouldTraceSifReg(reg) && g_sifGetRegLogCount < 128u;
            if (shouldLog)
            {
                ++g_sifGetRegLogCount;
            }
        }
        if (shouldLog)
        {
            PS2_IF_AGRESSIVE_LOGS({
                auto flags = std::cerr.flags();
                std::cerr << "[sceSifGetReg] reg=0x" << std::hex << reg
                          << " value=0x" << value
                          << " pc=0x" << (ctx ? ctx->pc : 0u)
                          << " ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                          << std::dec << std::endl;
                std::cerr.flags(flags);
            });
        }
        setReturnU32(ctx, value);
    }

    void sceSifGetSreg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        uint32_t value = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifSregs.find(reg);
            if (it != g_sifSregs.end())
            {
                value = it->second;
            }
        }
        setReturnU32(ctx, value);
    }

    void sceSifInitCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdInitialized = true;
        setReturnS32(ctx, 0);
    }

    void sceSifInitIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // The physical IOP allocator is initialized by IopSubsystem::reset().
        setReturnS32(ctx, 0);
    }

    void sceSifInitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifInitRpc(rdram, ctx, runtime);
    }

    void sceSifIsAliveIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSifLoadElf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadElf(rdram, ctx, runtime);
    }

    void sceSifLoadElfPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadElfPart(rdram, ctx, runtime);
    }

    void sceSifLoadFileReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifLoadIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifLoadModuleBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadModuleBuffer(rdram, ctx, runtime);
    }

    void sceSifRebootIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PS2IopTransport::reset(runtime);
        setReturnS32(ctx, 1);
    }

    void sceSifRegisterRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRegisterRpc(rdram, ctx, runtime);
    }

    void sceSifRemoveCmdHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cid = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdHandlers.erase(cid);
        setReturnS32(ctx, 0);
    }

    void sceSifRemoveRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRemoveRpc(rdram, ctx, runtime);
    }

    void sceSifRemoveRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRemoveRpcQueue(rdram, ctx, runtime);
    }

    void sceSifResetIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PS2IopTransport::reset(runtime);
        setReturnS32(ctx, 1);
    }

    void sceSifRpcLoop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifSetCmdBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newBuffer = getRegU32(ctx, 4);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            prev = g_sifCmdBuffer;
            g_sifCmdBuffer = newBuffer;
        }
        setReturnU32(ctx, prev);
    }

    void isceSifSetDChain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sceSifSetDChain(rdram, ctx, runtime);
    }

    void isceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sceSifSetDma(rdram, ctx, runtime);
    }

    void sceSifSetDChain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // The EE SIF0 handler re-arms the channel after copying the received packet out,
        // so the next queued IOP reply can be delivered.
        g_sifEndpoint.acknowledge();
        (void)scheduleSifReply(rdram, runtime);
        setReturnS32(ctx, 0);
    }

    void sceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dmatAddr = getRegU32(ctx, 4);
        const uint32_t count = getRegU32(ctx, 5);

        const uint32_t listAddr = getRegU32(ctx, 4);
        PS2_IF_AGRESSIVE_LOGS({
            std::cerr << "[sceSifSetDma:CALL] pc=0x" << std::hex << ctx->pc
                      << " ra=0x" << getRegU32(ctx, 31)
                      << " list=0x" << listAddr
                      << " count=" << std::dec << count
                      << std::endl;

            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t desc = listAddr + i * 16;
                const uint32_t src = READ32(desc + 0);
                const uint32_t dst = READ32(desc + 4);
                const uint32_t size = READ32(desc + 8);
                const uint32_t attr = READ32(desc + 12);

                std::cerr << "[sceSifSetDma:DESC] i=" << i
                          << " src=0x" << std::hex << src
                          << " dst=0x" << dst
                          << " size=0x" << size
                          << " attr=0x" << attr
                          << " pc=0x" << ctx->pc
                          << " ra=0x" << getRegU32(ctx, 31)
                          << std::dec << std::endl;
            }
        });

        if (!dmatAddr || count == 0u || count > 32u)
        {
            setReturnS32(ctx, 0);
            return;
        }

        std::array<Ps2SifDmaTransfer, 32u> pending{};
        uint32_t pendingCount = 0u;
        bool ok = true;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t entryAddr = dmatAddr + (i * static_cast<uint32_t>(sizeof(Ps2SifDmaTransfer)));
            const uint8_t *entry = getConstMemPtr(rdram, entryAddr);
            if (!entry)
            {
                ok = false;
                break;
            }

            Ps2SifDmaTransfer xfer{};
            std::memcpy(&xfer, entry, sizeof(xfer));
            if (xfer.size <= 0)
            {
                continue;
            }

            const uint32_t sizeBytes = static_cast<uint32_t>(xfer.size);
            if (sizeBytes > PS2_RAM_SIZE)
            {
                ok = false;
                break;
            }
            if (!runtime || !canAccessEeRange(rdram, xfer.src, sizeBytes) || !runtime->isIopMemoryRange(xfer.dest, sizeBytes))
            {
                ok = false;
                break;
            }

            pending[pendingCount++] = xfer;
        }

        if (ok)
        {
            for (uint32_t i = 0; i < pendingCount; ++i)
            {
                const Ps2SifDmaTransfer &xfer = pending[i];
                if (runtime)
                {
                    PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                        ps2x::iop::SifTransferKind::SetDma,
                                                                        ps2x::iop::SifTransferPhase::BeforeCopy,
                                                                        xfer.src,
                                                                        xfer.dest,
                                                                        static_cast<uint32_t>(xfer.size),
                                                                    });
                }
                const uint32_t sizeBytes = static_cast<uint32_t>(xfer.size);
                std::vector<uint8_t> payload(sizeBytes);
                if (!readEeRange(rdram, xfer.src, payload.data(), sizeBytes) || !runtime->writeIopMemory(xfer.dest, payload.data(), payload.size()))
                {
                    ok = false;
                    break;
                }
                if (runtime)
                {
                    PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                        ps2x::iop::SifTransferKind::SetDma,
                                                                        ps2x::iop::SifTransferPhase::AfterCopy,
                                                                        xfer.src,
                                                                        xfer.dest,
                                                                        static_cast<uint32_t>(xfer.size),
                                                                    });
                }
            }
        }

        if (!ok)
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "sceSifSetDma failed dmat=0x" << std::hex << dmatAddr
                              << " count=0x" << count
                              << std::dec << std::endl;
                });
                ++warnCount;
            }
            setReturnS32(ctx, 0);
            return;
        }

        for (uint32_t i = 0; i < pendingCount; ++i)
        {
            forwardToEndpoint(rdram, ctx, runtime, pending[i]);
        }
        if (!scheduleSifReply(rdram, runtime))
        {
            ps2_syscalls::dispatchDmacHandlersForCause(rdram, runtime, kDmacCauseSif0);
        }

        setReturnS32(ctx, static_cast<int32_t>(allocateSifDmaTransferId()));
    }

    void sceSifSetIopAddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, getRegU32(ctx, 5));
    }

    void sceSifSetReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        const uint32_t value = getRegU32(ctx, 5);
        uint32_t prev = 0u;
        bool shouldLog = false;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifRegs.find(reg);
            if (it != g_sifRegs.end())
            {
                prev = it->second;
            }
            g_sifRegs[reg] = value;
            shouldLog = shouldTraceSifReg(reg) && g_sifSetRegLogCount < 128u;
            if (shouldLog)
            {
                ++g_sifSetRegLogCount;
            }
        }
        if (shouldLog)
        {
            PS2_IF_AGRESSIVE_LOGS({
                auto flags = std::cerr.flags();
                std::cerr << "[sceSifSetReg] reg=0x" << std::hex << reg
                          << " prev=0x" << prev
                          << " value=0x" << value
                          << " pc=0x" << (ctx ? ctx->pc : 0u)
                          << " ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                          << std::dec << std::endl;
                std::cerr.flags(flags);
            });
        }
        setReturnU32(ctx, prev);
    }

    void sceSifSetRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifSetRpcQueue(rdram, ctx, runtime);
    }

    void sceSifSetSreg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        const uint32_t value = getRegU32(ctx, 5);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifSregs.find(reg);
            if (it != g_sifSregs.end())
            {
                prev = it->second;
            }
            g_sifSregs[reg] = value;
        }
        setReturnU32(ctx, prev);
    }

    void sceSifSetSysCmdBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newBuffer = getRegU32(ctx, 4);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            prev = g_sifSysCmdBuffer;
            g_sifSysCmdBuffer = newBuffer;
        }
        setReturnU32(ctx, prev);
    }

    void sceSifStopDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifSyncIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSifWriteBackDCache(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }
}
