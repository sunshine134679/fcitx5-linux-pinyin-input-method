#include <fcitx/candidatelist.h>

#include <cstdlib>
#include <iostream>

namespace {

class TestCandidate final : public fcitx::CandidateWord {
public:
    explicit TestCandidate(const char *text)
        : CandidateWord(fcitx::Text(text)) {}

    void select(fcitx::InputContext *) const override {}
};

void assertTrue(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "fcitx5 candidate list test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    fcitx::CommonCandidateList candidates;
    candidates.setPageSize(9);
    candidates.setLayoutHint(fcitx::CandidateLayoutHint::Horizontal);
    candidates.setCursorIncludeUnselected(true);
    candidates.append<TestCandidate>("你好");
    candidates.append<TestCandidate>("您好");

    // Fcitx5 requires the global cursor to be assigned after candidates exist.
    candidates.setGlobalCursorIndex(0);

    assertTrue(candidates.size() == 2, "two candidates are retained");
    assertTrue(candidates.globalCursorIndex() == 0,
               "global cursor points at the first candidate");
    assertTrue(candidates.cursorIndex() == 0,
               "cursor index is valid after publication");

    fcitx::CommonCandidateList paged;
    paged.setPageSize(9);
    for (int index = 0; index < 20; ++index) {
        paged.append<TestCandidate>("candidate");
    }
    paged.setGlobalCursorIndex(10);
    paged.setPage(1);
    assertTrue(paged.totalSize() == 20, "all candidates are retained");
    assertTrue(paged.totalPages() == 3, "candidate list exposes three pages");
    assertTrue(paged.currentPage() == 1,
               "global cursor selects the second page");
    assertTrue(paged.cursorIndex() == 1,
               "cursor index is relative to the current page");
    return EXIT_SUCCESS;
}
