#include "stub_registry.hpp"

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

struct Entry {
    StubKind          kind;
    std::string       symbol;
    std::string       note;
    std::atomic<uint64_t> hits{0};
    std::atomic<bool>     logged_first{false};
};

// Entries live for the process lifetime; the wrapped lambdas capture raw
// pointers into g_entries. unique_ptr in a deque gives stable addresses with
// automatic cleanup at shutdown (no ASan-visible leak).
std::mutex                                 g_mu;
std::deque<std::unique_ptr<Entry>>         g_entries;
std::unordered_map<std::string, Entry*>    g_by_symbol;

const char* kind_name(StubKind k) {
    return k == StubKind::Noop ? "noop" : "placeholder";
}

} // namespace

NativeFunc wrap_stub(StubKind kind,
                     std::string symbol,
                     std::string note,
                     NativeFunc impl) {
    Entry* e;
    {
        std::lock_guard lk(g_mu);
        auto it = g_by_symbol.find(symbol);
        if (it != g_by_symbol.end()) {
            e = it->second;
        } else {
            auto up = std::make_unique<Entry>();
            up->kind   = kind;
            up->symbol = symbol;
            up->note   = std::move(note);
            e = up.get();
            g_entries.push_back(std::move(up));
            g_by_symbol.emplace(std::move(symbol), e);
        }
    }
    return [e, impl = std::move(impl)](VM& v, Frame& f, std::span<Slot> args) {
        if (!e->logged_first.exchange(true)) {
            std::fprintf(stderr, "[stub] first call: %s  (%s — %s)\n",
                         e->symbol.c_str(), kind_name(e->kind),
                         e->note.c_str());
        }
        e->hits.fetch_add(1, std::memory_order_relaxed);
        impl(v, f, args);
    };
}

void dump_stub_report() {
    std::lock_guard lk(g_mu);
    if (g_entries.empty()) return;

    std::vector<Entry*> hit, unhit;
    for (auto& up : g_entries) {
        Entry* e = up.get();
        if (e->hits.load() > 0) hit.push_back(e);
        else unhit.push_back(e);
    }
    std::sort(hit.begin(), hit.end(), [](Entry* a, Entry* b) {
        return a->hits.load() > b->hits.load();
    });

    std::fprintf(stderr, "\n=== Stub hit report ===\n");
    if (!hit.empty()) {
        std::fprintf(stderr, "Called this session:\n");
        for (auto* e : hit) {
            std::fprintf(stderr, "  [%8llu] %-11s %s  (%s)\n",
                         (unsigned long long)e->hits.load(),
                         kind_name(e->kind),
                         e->symbol.c_str(),
                         e->note.c_str());
        }
    }
    if (!unhit.empty()) {
        std::fprintf(stderr, "Registered but never called (%zu):\n", unhit.size());
        for (auto* e : unhit) {
            std::fprintf(stderr, "  %-11s %s\n",
                         kind_name(e->kind), e->symbol.c_str());
        }
    }
}
