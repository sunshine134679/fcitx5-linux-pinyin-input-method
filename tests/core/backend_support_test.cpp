#include "modernime/core/candidate_filter.h"
#include "modernime/core/input_compatibility.h"
#include <cassert>
#include <iostream>

int main() {
    using namespace modernime::core;
    CandidateFilterIndex index;
    assert(index.available());
    auto stroke = index.predicate("s:sz");
    assert(stroke);
    CandidateItem shi; shi.text = "时"; shi.fullPinyin = "shi"; shi.consumedInputBytes = 3;
    CandidateItem shen; shen.text = "试";
    assert((*stroke)(shi));
    assert(!(*stroke)(shen));
    auto component = index.predicate("c:mumu");
    assert(component);
    CandidateItem lin; lin.text = "林";
    assert((*component)(lin));
    assert(!(*component)(shi));
    auto second = index.predicate("s:sz", 1);
    CandidateItem word; word.text = "临时";
    assert(second && (*second)(word));
    assert(!index.predicate("s:xyz"));
    assert(!index.predicate(std::string(65, 'h')));
    InputCompatibilityTracker tracker;
    assert(!tracker.report("app").observed);
    InputCompatibilityFacts facts;
    facts.application = "editor"; facts.frontend = "dbus";
    facts.display = "wayland:wayland-0"; facts.focused = true;
    facts.modernimeSelected = true; facts.observedAtMs = 1;
    tracker.observe(facts);
    tracker.recordKey("editor", true, 120);
    auto report = tracker.report("editor");
    assert(report.observed && report.transport == "wayland");
    assert(report.toolkit == "unknown" && report.sandbox == "unknown");
    assert(report.handledKeys == 1 && report.maximumDispatchUs == 120);
    assert(!report.suggestions.empty());
    facts.sensitive = true; facts.surroundingText = true; tracker.observe(facts);
    report = tracker.report();
    assert(report.facts.sensitive && !report.facts.surroundingText);
    assert(report.handledKeys == 1);
    for (int i = 0; i < 40; ++i) {
        facts.application = std::to_string(i); facts.observedAtMs = 2 + i;
        tracker.observe(facts);
    }
    assert(tracker.applications().size() == 32);
    std::cout << "candidate filters and observed application diagnostics passed\n";
}
