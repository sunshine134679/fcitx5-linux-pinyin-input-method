#include "modernime/pinyin/context_decoder.h"

#include <fcitx-utils/utf8.h>
#include <libime/core/userlanguagemodel.h>
#include <libime/pinyin/pinyincontext.h>
#include <libime/pinyin/pinyindecoder.h>
#include <libime/pinyin/pinyinime.h>
#include <libime/pinyin/pinyinmatchstate.h>
#include "modernime/pinyin/shuangpin_input.h"
#include "modernime/core/learning_snapshot.h"
#include "modernime/core/pinyin_match.h"
#include "modernime/core/common_characters.h"
#include "modernime/core/candidate_model.h"
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <limits>

namespace modernime::pinyin {
namespace {
// Request-local vocabulary plus the shared system/explicit-user dictionaries.
// Separate match states prevent LibIME's per-dictionary cache keys colliding.
class SentenceDictionary final : public libime::PinyinDictionary {
public:
    explicit SentenceDictionary(libime::PinyinContext &context)
        : base_(*context.ime()->dict()), localMatch_(&context) {}
private:
    void matchPrefixImpl(const libime::SegmentGraph &graph,
                         const libime::GraphMatchCallback &callback,
                         const std::unordered_set<const libime::SegmentGraphNode *> &ignore,
                         void *helper) const override {
        base_.matchPrefix(graph, callback, ignore, helper);
        libime::PinyinDictionary::matchPrefixImpl(graph, callback, ignore, &localMatch_);
    }
    const libime::PinyinDictionary &base_;
    mutable libime::PinyinMatchState localMatch_;
};

// Keep up to four plausible tokenizations at each character boundary. The
// previous greedy longest word can hide a useful suffix (e.g. a pronoun + 的).
// Only static LM states survive; no WordNode or surrounding-text view escapes.
std::vector<libime::State> contextStates(const libime::LanguageModel &model,
                                       std::string_view before,
                                       const std::vector<std::size_t> &ends,
                                       std::size_t start) {
    struct Hypothesis { libime::State state; double score; bool active; };
    constexpr std::size_t beamSize = 4;
    std::vector<std::vector<Hypothesis>> beams(ends.size());
    beams[start].push_back({model.nullState(), 0.0, false});
    const auto add = [&](std::size_t position, Hypothesis hypothesis) {
        auto &beam = beams[position];
        beam.push_back(std::move(hypothesis));
        std::stable_sort(beam.begin(), beam.end(), [](const auto &a, const auto &b) {
            return a.score > b.score;
        });
        if (beam.size() > beamSize) beam.resize(beamSize);
    };
    for (auto i = start; i + 1 < ends.size(); ++i) {
        const auto character = before.substr(ends[i], ends[i + 1] - ends[i]);
        if (character == " " || character == "\t") {
            for (const auto &hypothesis : beams[i]) add(i + 1, hypothesis);
            continue;
        }
        bool known = false;
        for (auto j = std::min(i + 8, ends.size() - 1); j > i; --j) {
            const auto word = before.substr(ends[i], ends[j] - ends[i]);
            const auto index = model.index(word);
            if (model.isUnknown(index, word)) continue;
            known = true;
            for (const auto &hypothesis : beams[i]) {
                libime::State out;
                const auto score = model.score(hypothesis.state, libime::WordNode(word, index), out);
                add(j, {out, hypothesis.score + score, true});
            }
        }
        if (!known && !beams[i].empty())
            add(i + 1, {model.nullState(), 0.0, false});
    }
    std::vector<libime::State> result;
    for (const auto &hypothesis : beams.back())
        if (hypothesis.active) result.push_back(hypothesis.state);
    return result;
}
} // namespace

ContextDecoder::ContextDecoder(libime::PinyinContext &context, std::string_view before,
                               const core::LearningSnapshot *learning, std::int64_t nowMs,
                               std::shared_ptr<const libime::StaticLanguageModelFile> contextModel)
    : context_(context), model_(context.ime()->model()->languageModelFile()),
      baseline_(model_.nullState()), state_(baseline_),
      evidenceModel_(contextModel ? contextModel : model_.languageModelFile()),
      learning_(learning), nowMs_(nowMs) {
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
    if (contextModel) evidenceStates_ = contextStates(evidenceModel_, before, ends, start);
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
    if (!contextModel && active_) evidenceStates_.push_back(state_);
}

double ContextDecoder::gain(const libime::SentenceResult &sentence) const {
    if (evidenceStates_.empty() || fcitx::utf8::length(sentence.toString()) < 2) return 0.0;
    const bool independentModel = evidenceModel_.languageModelFile() != model_.languageModelFile();
    if (sentence.sentence().empty()) return 0.0;
    const auto firstWord = sentence.sentence().front()->word();
    if (independentModel && evidenceModel_.isUnknown(evidenceModel_.index(firstWord), firstWord)) return 0.0;
    // Word indices belong to their model. Re-index primary-decoder nodes by
    // text before scoring in the independent static context model.
    const auto score = [&](libime::State state) {
        double total = 0.0;
        for (const auto *node : sentence.sentence()) {
            libime::State out;
            total += evidenceModel_.score(state,
                libime::WordNode(node->word(), evidenceModel_.index(node->word())), out);
            state = out;
            // The independent model supplies evidence across the context/word
            // boundary only. Internal fluency and tokenization remain owned by
            // the primary decoder; comparing whole-sentence likelihood ratios
            // across its tokenizations can reward an implausible split suffix.
            if (independentModel) break;
        }
        return total;
    };
    double contextual = -std::numeric_limits<double>::infinity();
    for (const auto &state : evidenceStates_) contextual = std::max(contextual, score(state));
    const auto difference = contextual - score(evidenceModel_.nullState());
    return std::isfinite(difference) ? std::clamp<double>(difference, -2.0, 2.0) : 0.0;
}

std::vector<core::CandidateScore> ContextDecoder::candidates() const {
    std::vector<core::CandidateScore> result;
    if (context_.userInput().size() > 128 || context_.candidates().empty() ||
        fcitx::utf8::length(context_.candidates().front().toString()) < 2) return result;
    auto &ime = *context_.ime();
    std::vector<std::string> readings;
    if (context_.useShuangpin()) {
        readings = ShuangpinInput(context_.userInput(), *ime.shuangpinProfile(), ime.fuzzyFlags()).recallInputs();
    } else {
        readings.push_back(core::PinyinMatchPolicy::segmentPinyin(context_.userInput()));
        for (std::size_t i = 0; i < std::min<std::size_t>(4, context_.candidates().size()); ++i)
            readings.push_back(context_.candidateFullPinyin(i));
    }
    SentenceDictionary dictionary(context_);
    std::unordered_map<std::string, double> learnedWords;
    if (learning_) for (const auto &word : learning_->recallSentenceWords(readings, nowMs_)) {
        std::vector<char> encoded;
        try { encoded = libime::PinyinEncoder::encodeFullPinyin(word.pinyin); }
        catch (const std::invalid_argument &) { continue; }
        if (encoded.size() / 2 != fcitx::utf8::length(word.phrase)) continue;
        bool known = false;
        float lexicalCost = 0.0F;
        ime.dict()->matchWords(encoded.data(), encoded.size(), [&](std::string_view, std::string_view text, float cost) {
            if (text == word.phrase) {
                lexicalCost = known ? std::max(lexicalCost, cost) : cost;
                known = true;
            }
            return true;
        });
        // Mature preferences also apply inside unseen sentences for known words.
        // Preserve their dictionary cost; add only the bounded, decayed evidence
        // in this request-local dictionary, without changing shared dictionaries.
        dictionary.addWord(1, word.pinyin, word.phrase,
                           lexicalCost + static_cast<float>(2.0 * std::clamp(word.learningBoost, 0.0, 3.0)));
        learnedWords.emplace(core::candidateOrderKey(word.phrase, word.pinyin), word.learningBoost);
    }
    if (!active_ && learnedWords.empty()) return result;
    auto graph = context_.useShuangpin()
        ? libime::PinyinEncoder::parseUserShuangpin(context_.userInput(), *ime.shuangpinProfile(), ime.fuzzyFlags())
        : libime::PinyinEncoder::parseUserPinyin(context_.userInput(), ime.fuzzyFlags());
    libime::Lattice lattice;
    libime::PinyinMatchState match(&context_);
    libime::PinyinDecoder decoder(&dictionary, &model_);
    if (!decoder.decode(lattice, graph, 16, state_, ime.maxDistance(), ime.minPath(),
                        ime.beamSize(), ime.frameSize(), &match)) return result;
    for (std::size_t i = 0; i < lattice.sentenceSize(); ++i) {
        const auto &sentence = lattice.sentence(i);
        const auto evidence = gain(sentence);
        if (sentence.sentence().empty()) continue;
        core::CandidateScore candidate;
        candidate.text = sentence.toString();
        candidate.source_index = i;
        candidate.language_context_bonus = evidence;
        std::string encoded;
        double componentEvidence = 0.0;
        bool rareCovered = true;
        for (const auto *node : sentence.sentence()) {
            const auto &part = static_cast<const libime::PinyinLatticeNode *>(node)->encodedPinyin();
            encoded += part;
            const auto pinyin = libime::PinyinEncoder::decodeFullPinyin(part);
            const auto found = learnedWords.find(core::candidateOrderKey(node->word(), pinyin));
            if (found != learnedWords.end())
                componentEvidence += std::min(found->second, 3.0) * static_cast<double>(part.size() / 2);
            else if (core::CommonCharacters::containsRare(node->word())) rareCovered = false;
        }
        if (evidence < 1.0 && componentEvidence == 0.0) continue;
        if (componentEvidence > 0.0) {
            candidate.decoder_score = sentence.score();
            candidate.learned_word_bonus = componentEvidence / static_cast<double>(encoded.size() / 2);
            candidate.learned_rare_coverage = rareCovered;
        }
        candidate.full_pinyin = libime::PinyinEncoder::decodeFullPinyin(encoded.data(), encoded.size());
        candidate.consumed_input_bytes = sentence.sentence().back()->to()->index();
        if (*candidate.consumed_input_bytes == context_.userInput().size())
            result.push_back(std::move(candidate));
    }
    return result;
}

} // namespace modernime::pinyin
