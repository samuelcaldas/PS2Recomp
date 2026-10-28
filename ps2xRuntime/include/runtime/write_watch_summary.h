#pragma once
// PS2X_WRITE_WATCH_SUMMARY=<hits>: instead of one line per watched write, count writes by
// guest pc (and $ra) and print the busiest writers every <hits> writes.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

class WriteWatchSummary
{
public:
    static WriteWatchSummary &instance()
    {
        static WriteWatchSummary summary;
        return summary;
    }

    bool enabled() const { return m_interval != 0u; }

    void record(uint32_t pc, uint32_t ra, uint32_t address, uint32_t size)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Writer &writer = m_writers[{pc, ra}];
        writer.bytes += size;
        writer.low = std::min(writer.low, address);
        writer.high = std::max(writer.high, address + size);
        if (++m_hits % m_interval == 0u)
            flush();
    }

private:
    struct Writer
    {
        uint64_t bytes = 0;
        uint32_t low = 0xFFFFFFFFu;
        uint32_t high = 0;
    };

    WriteWatchSummary()
    {
        const char *text = std::getenv("PS2X_WRITE_WATCH_SUMMARY");
        m_interval = text != nullptr ? static_cast<uint64_t>(std::strtoull(text, nullptr, 0)) : 0u;
    }

    void flush()
    {
        std::vector<std::pair<std::pair<uint32_t, uint32_t>, Writer>> writers(m_writers.begin(), m_writers.end());
        std::sort(writers.begin(), writers.end(),
                  [](const auto &a, const auto &b) { return a.second.bytes > b.second.bytes; });
        std::cerr << "[write-watch-summary] hits=" << m_hits << std::hex;
        for (size_t i = 0; i < writers.size() && i < 16u; ++i)
            std::cerr << " pc=0x" << writers[i].first.first << "/ra=0x" << writers[i].first.second << ":"
                      << std::dec << writers[i].second.bytes << "B[0x" << std::hex << writers[i].second.low
                      << "-0x" << writers[i].second.high << ")";
        std::cerr << std::dec << std::endl;
        m_writers.clear();
    }

    uint64_t m_interval = 0;
    uint64_t m_hits = 0;
    std::mutex m_mutex;
    std::map<std::pair<uint32_t, uint32_t>, Writer> m_writers;
};
