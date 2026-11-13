#include "modernime/core/candidate_model.h"
#include "modernime/core/pinyin_match.h"
#include "modernime/fcitx5/engine.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace modernime;

std::size_t failures = 0;
std::size_t scenarios = 0;
std::size_t events = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "简拼仲裁失败：" << message << '\n';
    }
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() /
                        "modernime-abbreviation-XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr) {
            throw std::runtime_error("无法创建隔离简拼测试目录");
        }
        path = pattern;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

struct RecordingHost final : fcitx5::EngineHost {
    std::vector<std::string> commits;
    void publishPage(const core::CandidatePage &) override {}
    void commit(std::string_view text) override { commits.emplace_back(text); }
};

std::size_t indexOf(const core::CandidatePage &page, std::string_view text) {
    for (std::size_t i = 0; i < page.items.size(); ++i) {
        if (page.items[i].text == text) return i;
    }
    return page.items.size();
}

void type(fcitx5::ModernIMEController &controller, std::string_view input) {
    for (const char c : input) {
        ++events;
        check(controller.handle({fcitx5::KeyKind::Character, c, 0}),
              "控制器接受实际按键");
    }
}

void checkFirstCommit(fcitx5::ModernIMEController &controller,
                      RecordingHost &host, std::string_view input,
                      std::string_view expected, bool chinese,
                      bool digit = false) {
    ++scenarios;
    controller.reset();
    type(controller, input);
    const auto page = controller.page();
    if (page.items.empty()) {
        check(false, std::string(input) + " 没有候选");
        return;
    }
    const auto identity = core::candidateIdentity(page.items.front(), page);
    std::cout << "input=" << input << " first=" << identity.text
              << " target=" << expected << " source="
              << static_cast<int>(page.items.front().source)
              << " pinyin=" << identity.fullPinyin
              << " consumed=" << identity.consumedInputBytes << '\n';
    check(identity.text == expected,
          std::string(input) + " 首选应为 " + std::string(expected));
    check(identity.consumedInputBytes == input.size(),
          std::string(input) + " 完整消费原始输入");
    if (chinese) {
        check(identity.role == core::CommitCandidateRole::Chinese ||
                  identity.role == core::CommitCandidateRole::UserDictionary,
              std::string(input) + " 首选保持中文提交角色");
    } else {
        check(page.items.front().source == core::CandidateSource::Raw,
              std::string(input) + " 保持英文首选来源");
    }
    const auto previous = host.commits.size();
    ++events;
    check(controller.handle({digit ? fcitx5::KeyKind::Digit : fcitx5::KeyKind::Space,
                             0, digit ? '1' : char{0}}),
          "数字1或空格选择首项");
    check(host.commits.size() == previous + 1 && host.commits.back() == expected,
          std::string(input) + " 实际上屏与期望首选相同");
    check(controller.page().rawInput.empty(),
          std::string(input) + " 完整上屏后没有残余按键");
}

void naturalDictionaryCases(const std::filesystem::path &directory) {
    pinyin::PinyinDataPaths paths;
    paths.learningStore = (directory / "empty.sqlite3").string();
    paths.userDictionary = (directory / "empty-user.dict").string();
    paths.dictionaryPacks = (directory / "empty-packs").string();
    pinyin::PinyinProviderOptions options;
    options.learningEnabled = false;
    options.contextLearningEnabled = false;
    pinyin::PinyinCandidateProvider provider(paths, options);
    RecordingHost host;
    fcitx5::ControllerOptions controllerOptions;
    controllerOptions.learningEnabled = false;
    controllerOptions.contextLearningEnabled = false;
    fcitx5::ModernIMEController controller(host, &provider, controllerOptions);
    struct Case { std::string_view input; std::string_view expected; };
    const Case chineseCases[]{
        {"aly", "阿里云"}, {"els", "俄罗斯"}, {"elb", "俄联邦"},
        {"alq", "阿联酋"}, {"elsfk", "俄罗斯方块"}, {"ab", "阿宝"},
        {"al", "阿里"}, {"nh", "你好"}, {"sm", "什么"}, {"zdl", "战斗力"},
        {"aliy", "阿里云"}, {"aoyh", "奥运会"}, {"ouly", "欧莱雅"},
        {"eluos", "俄罗斯"}, {"alad", "阿拉丁"}, {"erl", "而来"},
        {"ouz", "欧洲"}, {"jianp", "键盘"}, {"a'l'y", "阿里云"},
        {"e'l's", "俄罗斯"},
    };
    bool digit = false;
    for (const auto &item : chineseCases) {
        checkFirstCommit(controller, host, item.input, item.expected, true, digit);
        digit = !digit;
    }
    const Case englishCases[]{
        {"app", "app"}, {"doc", "document"}, {"compu", "compute"},
        {"apple", "apple"}, {"see", "see"}, {"fee", "fee"}, {"bee", "bee"},
        {"add", "add"}, {"all", "all"}, {"else", "else"},
        {"error", "error"}, {"exit", "exit"}, {"open", "open"}, {"one", "one"},
        {"syst", "system"}, {"sync", "sync"}, {"pyth", "python"}, {"crypt", "crypt"},
    };
    for (const auto &item : englishCases) {
        checkFirstCommit(controller, host, item.input, item.expected, false, digit);
        digit = !digit;
    }

    ++scenarios;
    controller.reset();
    type(controller, "pyt");
    auto page = controller.page();
    check(!page.items.empty() && core::candidateIdentity(page.items.front(), page).role ==
                                    core::CommitCandidateRole::Chinese,
          "pyt保留中文首选");
    const auto python = indexOf(page, "python");
    check(python < 3, "pyt的python保留既有前三席位");
    if (python < page.items.size()) {
        check(core::candidateIdentity(page.items[python], page).consumedInputBytes == 3,
              "python只消费pyt的3字节");
        ++events;
        check(controller.select(python) && host.commits.back() == "python" &&
                  controller.page().rawInput.empty(), "pyt选择python实际上屏");
    }

    ++scenarios;
    controller.reset();
    type(controller, "aly");
    const auto ali = indexOf(controller.page(), "阿里");
    if (ali < controller.page().items.size()) {
        check(controller.page().items[ali].consumedInputBytes == 2,
              "aly的阿里只消费al");
        ++events;
        check(controller.select(ali) && host.commits.back() == "阿里" &&
                  controller.page().rawInput == "y", "简拼部分选择保留剩余y");
    } else check(false, "aly仍能选择部分候选阿里");

    ++scenarios;
    controller.reset();
    ++events;
    check(controller.handle({fcitx5::KeyKind::ToggleTemporaryEnglish, 0, 0}),
          "进入明确临时英文模式");
    type(controller, "aly");
    ++events;
    const auto previousCommits = host.commits.size();
    check(controller.handle({fcitx5::KeyKind::Space, 0, 0}) &&
              host.commits.size() == previousCommits + 2 &&
              host.commits[previousCommits] == "aly" && host.commits.back() == " " &&
              controller.page().rawInput.empty(),
          "明确英文模式保持原文aly上屏");
}

void learnedVowelCases(const std::filesystem::path &directory) {
    pinyin::PinyinDataPaths paths;
    paths.learningStore = (directory / "learned.sqlite3").string();
    paths.userDictionary = (directory / "learned-user.dict").string();
    paths.dictionaryPacks = (directory / "learned-packs").string();
    pinyin::PinyinProviderOptions options;
    options.contextLearningEnabled = false;
    pinyin::PinyinCandidateProvider provider(paths, options);
    RecordingHost host;
    fcitx5::ControllerOptions controllerOptions;
    controllerOptions.contextLearningEnabled = false;
    fcitx5::ModernIMEController controller(host, &provider, controllerOptions);
    struct Case { std::string_view input; std::string_view expected; std::string_view full; };
    const Case cases[]{
        {"aly", "阿里云", "aliyun"}, {"els", "俄罗斯", "eluosi"},
        {"elb", "俄联邦", "elianbang"}, {"oly", "欧莱雅", "oulaiya"},
        {"olb", "欧罗巴", "ouluoba"}, {"ozb", "欧洲杯", "ouzhoubei"},
        {"opk", "欧佩克", "oupeike"},
    };
    for (const auto &item : cases) {
        for (int i = 0; i < 3; ++i) {
            controller.reset();
            type(controller, item.full);
            const auto index = indexOf(controller.page(), item.expected);
            check(index < controller.page().items.size(), "全拼可选择学习夹具词");
            if (index < controller.page().items.size()) {
                ++events;
                check(controller.select(index) && host.commits.back() == item.expected,
                      "通过实际控制器选择建立隔离学习事实");
            }
        }
        check(provider.flush(), "学习夹具落盘成功");
        checkFirstCommit(controller, host, item.input, item.expected, true);
    }
    check(provider.flush(), "简拼选择学习落盘成功");

    auto reopenedShared = pinyin::PinyinCandidateProvider::createSharedResources(paths, options);
    pinyin::PinyinCandidateProvider reopened(reopenedShared, options);
    RecordingHost reopenedHost;
    fcitx5::ModernIMEController reopenedController(reopenedHost, &reopened, controllerOptions);
    for (const auto &item : cases) {
        checkFirstCommit(reopenedController, reopenedHost, item.input, item.expected, true, true);
    }
}
} // namespace

int main() {
    try {
        for (int round = 1; round <= 3; ++round) {
            TemporaryDirectory directory;
            std::cout << "独立隔离轮次=" << round << '\n';
            naturalDictionaryCases(directory.path);
            learnedVowelCases(directory.path);
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "scenarios=" << scenarios << " events=" << events
              << " failures=" << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
