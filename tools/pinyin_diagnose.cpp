#include "modernime/pinyin/candidate_diagnostics.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/pinyin/pinyin_shared_resources.h"
#include "modernime/core/pinyin_match.h"

#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyinencoder.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace modernime;

std::string json(std::string_view text) {
    std::ostringstream out;
    out << '"';
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
        else out << c;
    }
    out << '"';
    return out.str();
}

void number(std::ostream &out, double value) {
    if (std::isfinite(value)) out << value;
    else out << "null";
}

const char *sourceName(core::CandidateSource source) {
    switch (source) {
    case core::CandidateSource::Engine: return "engine";
    case core::CandidateSource::UserDictionary: return "user_dictionary";
    case core::CandidateSource::Learned: return "learned";
    case core::CandidateSource::Raw: return "raw";
    case core::CandidateSource::EnglishDefinition: return "english_definition";
    case core::CandidateSource::Command: return "command";
    case core::CandidateSource::None: return "none";
    }
    return "unknown";
}

const char *roleName(core::CommitCandidateRole role) {
    switch (role) {
    case core::CommitCandidateRole::Chinese: return "chinese";
    case core::CommitCandidateRole::UserDictionary: return "user_dictionary";
    case core::CommitCandidateRole::English: return "english";
    case core::CommitCandidateRole::Raw: return "raw";
    case core::CommitCandidateRole::Command: return "command";
    case core::CommitCandidateRole::Generated: return "generated";
    }
    return "unknown";
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "modernime-diagnose-XXXXXX").string();
        if (!::mkdtemp(pattern.data())) throw std::runtime_error("无法创建临时诊断目录");
        path = pattern;
    }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

std::optional<std::size_t> expectedIndex(const core::CandidatePage &page,
                                        std::string_view expected, std::string_view reading) {
    const auto canonical = core::PinyinMatchPolicy::canonical(reading);
    for (std::size_t i = 0; i < page.items.size(); ++i) {
        const auto &item = page.items[i];
        if (item.text == expected &&
            (reading.empty() || core::PinyinMatchPolicy::canonical(item.fullPinyin) == canonical) &&
            core::candidateIdentity(item, page).consumedInputBytes == page.rawInput.size()) return i;
    }
    return std::nullopt;
}

void rank(std::ostream &out, std::optional<std::size_t> index) {
    if (index) out << *index + 1;
    else out << "null";
}

struct Report {
    core::CandidatePage page;
    pinyin::PinyinDiagnostics diagnostics;
    double generationMs = 0;
};

Report query(const std::shared_ptr<pinyin::PinyinCandidateProvider::SharedResources> &shared,
             pinyin::PinyinProviderOptions options, const std::string &input,
             const std::string &before, const std::string &after) {
    pinyin::PinyinCandidateProvider provider(shared, options);
    provider.setDiagnosticsEnabled(true);
    provider.setContext(before, after);
    const auto start = std::chrono::steady_clock::now();
    if (!provider.append(input)) throw std::runtime_error("输入未被拼音引擎接受");
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return {provider.page(), *provider.diagnostics(), elapsed};
}

void writeReport(std::ostream &out, const Report &report,
                 std::string_view expected, std::string_view reading) {
    const auto &page = report.page;
    const auto &trace = report.diagnostics;
    out << "{\"generation_ms_with_diagnostics\":" << report.generationMs
        << ",\"timestamp_ms\":" << trace.timestampMs
        << ",\"segmented_input\":" << json(trace.segmentedInput)
        << ",\"preedit\":" << json(page.preedit)
        << ",\"native_offset\":" << trace.nativeOffset
        << ",\"has_more\":" << (page.hasMoreCandidates ? "true" : "false")
        << ",\"expected_rank\":";
    rank(out, expectedIndex(page, expected, reading));
    out << ",\"candidates\":[";
    for (std::size_t i = 0; i < page.items.size(); ++i) {
        const auto &item = page.items[i];
        const auto identity = core::candidateIdentity(item, page);
        if (i) out << ',';
        out << "{\"rank\":" << i + 1 << ",\"text\":" << json(item.text)
            << ",\"pinyin\":" << json(item.fullPinyin)
            << ",\"source\":" << json(sourceName(item.source))
            << ",\"role\":" << json(roleName(identity.role))
            << ",\"consumed_bytes\":" << identity.consumedInputBytes << '}';
    }
    out << "],\"ranked_evidence\":[";
    for (std::size_t i = 0; i < trace.order.size(); ++i) {
        const auto &score = trace.scored.at(trace.order[i]);
        if (i) out << ',';
        out << "{\"rank_before_composition\":" << i + 1 << ",\"text\":" << json(score.text)
            << ",\"pinyin\":" << json(score.full_pinyin)
            << ",\"native_rank\":" << score.source_index + 1
            << ",\"match_priority\":" << score.match_priority
            << ",\"lexical\":" << (score.is_lexical ? "true" : "false")
            << ",\"decoder_score\":";
        if (score.decoder_score) number(out, *score.decoder_score); else out << "null";
        for (const auto &[key, value] : std::initializer_list<std::pair<const char *, double>>{
                 {"decoder_bonus", score.decoder_bonus}, {"dictionary_bonus", score.dictionary_bonus},
                 {"learning_boost", score.learning_boost}, {"context_bonus", score.context_bonus},
                 {"language_context_bonus", score.language_context_bonus},
                 {"learned_word_bonus", score.learned_word_bonus}, {"rare_penalty", score.rare_penalty()},
                 {"native_rank_scale", score.native_rank_scale},
                 {"unigram_anchor", score.unigram_anchor},
                 {"unigram_prominence", score.unigram_prominence},
                 {"unigram_comparison_offset", score.unigram_comparison_offset},
                 {"base_prior", score.base_prior()}, {"final_cost", score.final_score()}}) {
            out << ',' << json(key) << ':'; number(out, value);
        }
        out << '}';
    }
    // Stable names follow the existing composition enums; indices refer to the stage at that time.
    static constexpr const char *stages[] = {"fuzzy", "granularity", "variant", "macro", "english_raw",
        "command", "definition", "suppression", "deduplication", "ambiguity", "pin"};
    static constexpr const char *reasons[] = {"initial", "fuzzy_rank", "variant_visible", "macro_position",
        "english_preferred", "english_normal", "clipboard_trigger", "suppressed", "duplicate",
        "reading_annotation", "remainder_annotation", "explicit_pin"};
    out << "],\"composition_moves\":[";
    for (std::size_t i = 0; i < trace.moves.size(); ++i) {
        const auto &move = trace.moves[i];
        if (i) out << ',';
        out << "{\"stage\":" << json(stages[static_cast<std::size_t>(move.stage)])
            << ",\"reason\":" << json(reasons[static_cast<std::size_t>(move.reason)])
            << ",\"source\":" << json(sourceName(move.source)) << ",\"index\":" << move.index << '}';
    }
    out << "]}";
}
} // namespace

int main(int argc, char **argv) {
    try {
        std::map<std::string, std::string> args{{"--input", ""}, {"--expected", ""}, {"--pinyin", ""},
            {"--before", ""}, {"--after", ""}, {"--scheme", "full"}, {"--learn", "0"},
            {"--dictionary", pinyin::defaultDictionaryPath()}, {"--model", pinyin::defaultLanguageModelPath()},
            {"--context-model", MODERNIME_CONTEXT_MODEL_BUILD_BINARY},
            {"--extension", MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY},
            {"--hotwords", MODERNIME_PINYIN_HOTWORDS_BUILD_BINARY}};
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--help") {
                std::cout << "用法：modernime_pinyin_diagnose --input 拼音 [--expected 词语 --pinyin 完整拼音]\n"
                             "  [--before 前文 --after 后文 --scheme full|xiaohe|ziranma --learn 重复选择次数]\n"
                             "  [--dictionary 词典 --model 模型 --extension 扩展词典 --hotwords 热词词典]\n"
                             "  [--context-model 独立语境模型；无效路径回退系统模型]\n"
                             "输出JSON；使用临时词典/学习库，最多显示当前首批候选，不读取个人数据。\n";
                return 0;
            }
            if (!args.contains(key) || i + 1 == argc) throw std::runtime_error("未知参数或缺少参数值：" + key);
            args[key] = argv[++i];
        }
        if (args["--input"].empty()) throw std::runtime_error("请提供 --input");
        std::size_t parsed = 0;
        const int repetitions = std::stoi(args["--learn"], &parsed);
        if (parsed != args["--learn"].size() || repetitions < 0 || repetitions > 100)
            throw std::runtime_error("--learn 需要0至100的整数");
        pinyin::PinyinProviderOptions options;
        if (args["--scheme"] == "xiaohe") options.pinyinScheme = core::PinyinScheme::Xiaohe;
        else if (args["--scheme"] == "ziranma") options.pinyinScheme = core::PinyinScheme::Ziranma;
        else if (args["--scheme"] != "full") throw std::runtime_error("未知拼音方案");
        TemporaryDirectory temporary;
        pinyin::PinyinDataPaths paths;
        paths.dictionary = args["--dictionary"];
        paths.languageModel = args["--model"];
        paths.contextLanguageModel = args["--context-model"];
        paths.extensionDictionary = args["--extension"];
        paths.hotwordDictionary = args["--hotwords"];
        paths.userDictionary = (temporary.path / "user.dict").string();
        paths.learningStore = (temporary.path / "learning.sqlite3").string();
        auto shared = pinyin::PinyinCandidateProvider::createSharedResources(paths, options);
        const auto &input = args["--input"], &expected = args["--expected"], &reading = args["--pinyin"];
        bool dictionaryContains = false;
        if (!expected.empty() && !reading.empty()) {
            const auto encoded = libime::PinyinEncoder::encodeFullPinyin(reading);
            shared->ime->dict()->matchWords(encoded.data(), encoded.size(),
                [&](std::string_view, std::string_view text, float) {
                    dictionaryContains = dictionaryContains || text == expected;
                    return !dictionaryContains;
                });
        }
        const auto baseline = query(shared, options, input, args["--before"], args["--after"]);
        auto withoutContext = options;
        withoutContext.contextLearningEnabled = false;
        const auto neutral = query(shared, withoutContext, input, "", "");
        for (int i = 0; i < repetitions; ++i) {
            pinyin::PinyinCandidateProvider learner(shared, options);
            learner.setContext(args["--before"], args["--after"]);
            learner.append(input);
            const auto index = expectedIndex(learner.page(), expected, reading);
            if (!index || !learner.select(*index)) throw std::runtime_error("期望完整候选不在首批，无法执行重复选择");
            if (!learner.flush()) throw std::runtime_error("临时学习库保存失败");
        }
        const auto learned = repetitions ? std::optional(query(shared, options, input, args["--before"], args["--after"]))
                                         : std::nullopt;
        std::cout << std::setprecision(9) << "{\"schema\":1,\"version\":" << json(MODERNIME_DIAG_VERSION)
            << ",\"build_type\":" << json(MODERNIME_DIAG_BUILD_TYPE) << ",\"compiler\":" << json(__VERSION__)
            << ",\"input\":" << json(input) << ",\"expected\":" << json(expected)
            << ",\"expected_pinyin\":" << json(reading) << ",\"scheme\":" << json(args["--scheme"])
            << ",\"before\":" << json(args["--before"]) << ",\"after\":" << json(args["--after"])
            << ",\"dictionary_contains_expected\":";
        if (reading.empty() || expected.empty()) std::cout << "null";
        else std::cout << (dictionaryContains ? "true" : "false");
        std::cout << ",\"resource_paths\":{\"dictionary\":" << json(paths.dictionary)
            << (shared->contextLanguageModelPath.empty() ? "" : ",\"context_model\":" + json(shared->contextLanguageModelPath.string()))
            << ",\"model\":" << json(paths.languageModel) << ",\"extension\":" << json(paths.extensionDictionary)
            << ",\"hotwords\":" << json(paths.hotwordDictionary) << "},\"without_context_rank\":";
        rank(std::cout, expectedIndex(neutral.page, expected, reading));
        std::cout << ",\"learning_selections\":" << repetitions << ",\"baseline\":";
        writeReport(std::cout, baseline, expected, reading);
        std::cout << ",\"after_learning\":";
        if (learned) writeReport(std::cout, *learned, expected, reading); else std::cout << "null";
        std::cout << "}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "候选诊断失败：" << error.what() << '\n';
        return 1;
    }
}
