#include "modernime/core/clipboard_observation.h"

#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {
using modernime::core::ClipboardHistory;
using modernime::core::ClipboardObservationBatch;
using modernime::core::ClipboardObservationTracker;
using Values = std::vector<std::string>;
void require(bool ok, std::string_view message) {
    if (!ok) { std::cerr << "clipboard observation failed: " << message << '\n'; std::exit(1); }
}
ClipboardHistory history(const Values &values) {
    ClipboardHistory h;
    for (auto it = values.rbegin(); it != values.rend(); ++it) h.observe(*it);
    return h;
}
void expect(const Values &base, const Values &samples, const Values &visible, const Values &pending) {
    auto h = history(base);
    ClipboardHistory p;
    ClipboardObservationBatch batch;
    for (const auto &v : samples) batch.observe(v);
    batch.apply(h, p);
    require(h.entries() == visible, "literal visible order");
    require(p.entries() == pending, "literal pending eligibility and order");
}
}
int main() {
    // A same-top first observation is not pending; subsequent real transitions
    // retain their eligibility even after the predecessors are compacted away.
    expect({"A", "B"}, {"B", "A", "B"}, {"B", "A"}, {"B", "A"});
    expect({"A", "B"}, {"A", "A"}, {"A", "B"}, {});
    expect({}, {"A"}, {"A"}, {"A"});
    expect({"C", "B", "A"}, {"A", "B", "A", "C"}, {"C", "A", "B"}, {"C", "A", "B"});

    ClipboardObservationTracker tracker;
    tracker.setBaseline("excluded");
    require(!tracker.accept("excluded"), "baseline is not a copy");
    require(tracker.accept("A") && !tracker.accept("A"), "same observation filtered once");
    tracker.setBaseline("B");
    require(tracker.accept("A"), "new interval has independent observation identity");
    require(!tracker.accept("") && tracker.accept("A"), "empty observation permits later recopy");
    std::string maxEntry(ClipboardHistory::kMaxEntryBytes, 'x');
    require(tracker.accept(maxEntry), "exactly 1MiB is accepted");
    require(!tracker.accept(maxEntry + "x") && tracker.accept(maxEntry), "oversize is not retained but changes identity");
    tracker.setBaseline(maxEntry + "x");
    require(tracker.accept(maxEntry), "oversize baseline does not swallow valid copy");

    ClipboardObservationBatch full;
    full.observe("predecessor");
    Values expected;
    for (int i = 0; i < 90; ++i) {
        full.observe(std::to_string(i));
        require(full.retainedSize() <= ClipboardHistory::kMaxEntries + 1, "batch remains bounded during slow I/O");
    }
    for (int i = 89; i >= 60; --i) expected.push_back(std::to_string(i));
    auto h = history({"60"});
    ClipboardHistory p;
    full.apply(h, p);
    require(h.entries() == expected && p.entries() == expected,
            "oldest retained transition remains pending even when equal to original top");
    ClipboardObservationBatch rejected;
    rejected.observe(""); rejected.observe(maxEntry + "x");
    require(rejected.empty(), "invalid values do not occupy batch capacity");

    // Independent reference is the pre-batching per-observation algorithm.
    // Mutations dropping first, treating tail like first, or losing a prefix
    // during composition change the final pending set, even if visible matches.
    std::mt19937 rng(20261108);
    for (unsigned trial = 0; trial < 10000; ++trial) {
        ClipboardHistory reference, referencePending;
        for (unsigned i = 0; i < 40; ++i) {
            reference.observe(std::to_string(rng() % 50));
            referencePending.observe(std::to_string(rng() % 50));
        }
        auto actual = reference, actualPending = referencePending;
        ClipboardObservationBatch combined, part;
        for (unsigned i = 0; i < 200; ++i) {
            const auto v = std::to_string(rng() % 50);
            if (reference.observe(v)) referencePending.observe(v);
            part.observe(v);
            if (rng() % 11 == 0) { combined.append(part); part = {}; }
        }
        combined.append(part);
        combined.apply(actual, actualPending);
        require(actual.entries() == reference.entries(), "randomized composed visible order");
        require(actualPending.entries() == referencePending.entries(), "randomized composed pending order");
        require(combined.retainedSize() <= 31, "composed batch stays bounded");
    }
    std::cout << "clipboard observation: literal boundaries and 10000 composed sequences passed\n";
}
