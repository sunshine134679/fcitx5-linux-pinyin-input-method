#include "modernime/pinyin/context_decoder.h"

#include <fcitx-utils/utf8.h>
#include <libime/core/userlanguagemodel.h>
#include <libime/pinyin/pinyincontext.h>
#include <libime/pinyin/pinyindecoder.h>
#include <libime/pinyin/pinyinime.h>
#include <libime/pinyin/pinyinmatchstate.h>
#include <algorithm>
#include <cmath>

namespace modernime::pinyin {

ContextDecoder::ContextDecoder(libime::PinyinContext &context, std::string_view before)
    : context_(context), model_(context.ime()->model()->languageModelFile()),
      baseline_(model_.nullState()), state_(baseline_) {
    if (before.size() > 128) {
        before.remove_prefix(before.size() - 128);
        while (!before.empty() && (static_cast<unsigned char>(before.front()) & 0xc0U) == 0x80U)
            before.remove_prefix(1);
    }
    for (const std::string_view boundary : {"\n", "\r", "。", "！", "？", ".", "!", "?"}) {
        const auto end = before.rfind(boundary);
        if (end != std::string_view::npos) before.remove_prefix(end + boundary.size());
    }
    if (before.empty() || !fcitx::utf8::validate(before)) return;
    std::vector<std::size_t> ends{0};
    for (std::size_t i = 1; i <= before.size(); ++i)
        if (i == before.size() || (static_cast<unsigned char>(before[i]) & 0xc0U) != 0x80U)
            ends.push_back(i);
    // The host already bounds context; keep this public entry point bounded too.
    const auto start = ends.size() > 33 ? ends.size() - 33 : 0;
    const auto &model = model_;
    for (auto i = start; i + 1 < ends.size();) {
        const auto character = before.substr(ends[i], ends[i + 1] - ends[i]);
        if (character == " " || character == "\t") { ++i; continue; }
        std::size_t next = i;
        for (auto j = std::min(i + 8, ends.size() - 1); j > i; --j) {
            const auto word = before.substr(ends[i], ends[j] - ends[i]);
            const auto index = model.index(word);
            if (model.isUnknown(index, word)) continue;
            libime::State out;
            model.score(state_, libime::WordNode(word, index), out);
            state_ = out;
            next = j;
            active_ = true;
            break;
        }
        if (next == i) {
            // Unknown text and sentence boundaries end the usable suffix.
            state_ = baseline_;
            active_ = false;
            ++i;
        } else i = next;
    }
}

double ContextDecoder::gain(const libime::SentenceResult &sentence) const {
    if (!active_ || fcitx::utf8::length(sentence.toString()) < 2) return 0.0;
    // Score stable lattice nodes directly. UserLanguageModel states retain a
    // WordNode pointer; the separate static model shares only the model file
    // and neither borrows temporary word nodes nor adds another learning path.
    const auto score = [&](libime::State state) {
        double total = 0.0;
        for (const auto *node : sentence.sentence()) {
            libime::State out;
            total += model_.score(state, *node, out);
            state = out;
        }
        return total;
    };
    const auto difference = score(state_) - score(baseline_);
    return std::isfinite(difference) ? std::clamp<double>(difference, -2.0, 2.0) : 0.0;
}

std::vector<core::CandidateScore> ContextDecoder::candidates() const {
    std::vector<core::CandidateScore> result;
    if (!active_ || context_.userInput().size() > 128 || context_.candidates().empty() ||
        fcitx::utf8::length(context_.candidates().front().toString()) < 2) return result;
    auto &ime = *context_.ime();
    auto graph = context_.useShuangpin()
        ? libime::PinyinEncoder::parseUserShuangpin(context_.userInput(), *ime.shuangpinProfile(), ime.fuzzyFlags())
        : libime::PinyinEncoder::parseUserPinyin(context_.userInput(), ime.fuzzyFlags());
    libime::Lattice lattice;
    libime::PinyinMatchState match(&context_);
    libime::PinyinDecoder decoder(ime.dict(), &model_);
    if (!decoder.decode(lattice, graph, 16, state_, ime.maxDistance(), ime.minPath(),
                        ime.beamSize(), ime.frameSize(), &match)) return result;
    for (std::size_t i = 0; i < lattice.sentenceSize(); ++i) {
        const auto &sentence = lattice.sentence(i);
        const auto evidence = gain(sentence);
        // Only add new hypotheses when context supplies positive model evidence.
        if (evidence < 1.0 || sentence.sentence().empty()) continue;
        core::CandidateScore candidate;
        candidate.text = sentence.toString();
        candidate.source_index = i;
        candidate.language_context_bonus = evidence;
        std::string encoded;
        for (const auto *node : sentence.sentence())
            encoded += static_cast<const libime::PinyinLatticeNode *>(node)->encodedPinyin();
        candidate.full_pinyin = libime::PinyinEncoder::decodeFullPinyin(encoded.data(), encoded.size());
        candidate.consumed_input_bytes = sentence.sentence().back()->to()->index();
        if (*candidate.consumed_input_bytes == context_.userInput().size())
            result.push_back(std::move(candidate));
    }
    return result;
}

} // namespace modernime::pinyin
