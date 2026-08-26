#include "modernime/pinyin/pinyin_candidate_provider.h"

#include "modernime/pinyin/candidate_pipeline.h"

#include <libime/core/userlanguagemodel.h>
#include <libime/pinyin/pinyincontext.h>
#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyinime.h>

#include <filesystem>
#include <memory>
#include <utility>

namespace modernime::pinyin {

class PinyinCandidateProvider::Impl final {
public:
    explicit Impl(const PinyinDataPaths &paths) {
        auto dictionary = std::make_unique<libime::PinyinDictionary>();
        if (std::filesystem::is_regular_file(paths.dictionary)) {
            dictionary->load(0, paths.dictionary.c_str(),
                             libime::PinyinDictFormat::Binary);
        }

        std::unique_ptr<libime::UserLanguageModel> model;
        if (std::filesystem::is_regular_file(paths.languageModel)) {
            model = std::make_unique<libime::UserLanguageModel>(
                paths.languageModel.c_str());
        } else {
            model = std::make_unique<libime::UserLanguageModel>();
        }

        ime = std::make_unique<libime::PinyinIME>(std::move(dictionary),
                                                  std::move(model));
        ime->setNBest(32);
        context = std::make_unique<libime::PinyinContext>(ime.get());
        refresh();
    }

    bool append(std::string_view input) {
        if (input.empty() || input.find_first_not_of("abcdefghijklmnopqrstuvwxyz") !=
                                 std::string_view::npos) {
            return false;
        }
        if (!context->type(input)) {
            return false;
        }
        refresh();
        return true;
    }

    bool eraseLast() {
        if (!context->backspace()) {
            return false;
        }
        refresh();
        return true;
    }

    bool select(std::size_t index) {
        if (index >= page_.items.size()) {
            return false;
        }
        context->select(page_.items[index].sourceIndex);
        refresh();
        return true;
    }

    void reset() {
        context->clear();
        refresh();
    }

    const core::CandidatePage &page() const { return page_; }

private:
    void refresh() {
        page_.clear();
        page_.preedit = context->userInput();
        ++generation;
        page_.generation = generation;
        if (context->userInput().empty()) {
            return;
        }

        const auto result = buildCandidatePipeline(*context, *ime->dict());
        page_.items.reserve(result.order.size());
        for (const auto sourceIndex : result.order) {
            const auto &candidate = result.scored[sourceIndex];
            page_.items.push_back(
                {candidate.text, candidate.full_pinyin, candidate.source_index});
        }
    }

    std::unique_ptr<libime::PinyinIME> ime;
    std::unique_ptr<libime::PinyinContext> context;
    core::CandidatePage page_;
    std::uint64_t generation = 0;
};

PinyinCandidateProvider::PinyinCandidateProvider(PinyinDataPaths paths)
    : impl_(std::make_unique<Impl>(paths)) {}

PinyinCandidateProvider::~PinyinCandidateProvider() = default;

bool PinyinCandidateProvider::append(std::string_view input) {
    return impl_->append(input);
}

bool PinyinCandidateProvider::eraseLast() { return impl_->eraseLast(); }

bool PinyinCandidateProvider::select(std::size_t index) {
    return impl_->select(index);
}

void PinyinCandidateProvider::reset() { impl_->reset(); }

const core::CandidatePage &PinyinCandidateProvider::page() const {
    return impl_->page();
}

} // namespace modernime::pinyin
