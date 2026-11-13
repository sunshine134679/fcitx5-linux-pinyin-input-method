#include "modernime/core/user_data_bundle.h"
#include "modernime/core/learning_store.h"
#include "modernime/core/learning_writer.h"
#include "modernime/core/settings.h"
#include "modernime/core/clipboard_history.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <utility>

namespace modernime::core {
namespace {
using Component = UserDataComponent;
constexpr std::array components{Component::Settings, Component::UserDictionary,
    Component::QuickPhrases, Component::Learning, Component::ClipboardHistory,
    Component::ClipboardFavorites};
constexpr std::array<std::string_view, 6> names{"settings", "dictionary", "quick-phrases",
    "learning", "clipboard-history", "clipboard-favorites"};
constexpr std::array<std::string_view, 6> filenames{"settings.conf", "dictionary.txt", "quick-phrases.mb",
    "learning.bin", "clipboard-history.bin", "clipboard-favorites.bin"};
constexpr std::size_t maxFileBytes = 256 * 1024 * 1024;
constexpr std::string_view learningMagic = "ModernIME Learning Entries\n1\n";
constexpr std::string_view manifestMagic = "ModernIME User Data Bundle\t1\nchecksum\tfnv1a64\n";
std::atomic<unsigned long> counter{0};

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }
[[noreturn]] void ioFail(std::string_view operation) {
    fail(std::string(operation) + ": " + std::strerror(errno));
}
struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(Fd &&other) noexcept : value(std::exchange(other.value, -1)) {}
    Fd &operator=(Fd &&other) noexcept {
        if (this != &other) { if (value >= 0) ::close(value); value = std::exchange(other.value, -1); }
        return *this;
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};
std::filesystem::path absolutePath(const std::filesystem::path &path) {
    if (path.empty()) fail("数据路径为空");
    for (const auto &part : path) if (part == "..") fail("数据路径不能包含上级目录");
    return std::filesystem::absolute(path).lexically_normal();
}
// Resolve all ancestors through descriptors; subsequent I/O never follows
// paths through an attacker-replaced ancestor or leaf symlink.
Fd openDirectory(const std::filesystem::path &path, bool create = false, bool missingOk = false) {
    const auto absolute = absolutePath(path);
    Fd current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (current.value < 0) ioFail("打开根目录");
    for (const auto &part : absolute.relative_path()) {
        const auto name = part.string();
        if (name.empty() || name == ".") continue;
        int next = ::openat(current.value, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT && create) {
            if (::mkdirat(current.value, name.c_str(), 0700) != 0 && errno != EEXIST) ioFail("创建数据目录");
            next = ::openat(current.value, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next < 0 && errno == ENOENT && missingOk) return Fd{};
        if (next < 0) ioFail("打开数据目录（不允许符号链接）");
        current = Fd(next);
    }
    return current;
}
std::string fingerprint(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : bytes) { hash ^= c; hash *= 1099511628211ULL; }
    std::ostringstream out; out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}
std::optional<std::string> readAt(int directory, std::string_view name, std::size_t limit = maxFileBytes) {
    Fd input(::openat(directory, std::string(name).c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (input.value < 0 && errno == ENOENT) return std::nullopt;
    if (input.value < 0) ioFail("读取数据文件");
    struct stat before{};
    if (::fstat(input.value, &before) != 0) ioFail("读取数据文件属性");
    if (!S_ISREG(before.st_mode) || before.st_size < 0 || static_cast<std::uintmax_t>(before.st_size) > limit)
        fail("数据文件类型或大小不支持");
    std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
    std::size_t position = 0;
    while (position < bytes.size()) {
        const auto result = ::read(input.value, bytes.data() + position, bytes.size() - position);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) fail("数据文件读取不完整");
        position += static_cast<std::size_t>(result);
    }
    char tail = 0;
    if (::read(input.value, &tail, 1) != 0) fail("读取过程中数据文件大小发生变化");
    struct stat after{};
    if (::fstat(input.value, &after) != 0 || before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
        fail("读取过程中数据文件发生变化");
    return bytes;
}
void writeAt(int directory, std::string_view name, std::string_view bytes) {
    Fd output(::openat(directory, std::string(name).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (output.value < 0) ioFail("创建数据文件");
    try {
        while (!bytes.empty()) {
            auto size = ::write(output.value, bytes.data(), bytes.size());
            if (size < 0 && errno == EINTR) continue;
            if (size <= 0) ioFail("写入数据文件");
            bytes.remove_prefix(static_cast<std::size_t>(size));
        }
        if (::fsync(output.value) != 0) ioFail("同步数据文件");
    } catch (...) {
        ::unlinkat(directory, std::string(name).c_str(), 0);
        throw;
    }
}
std::string uniqueName(std::string_view suffix) {
    return ".modernime-bundle-" + std::to_string(::getpid()) + "-" +
        std::to_string(counter.fetch_add(1)) + std::string(suffix);
}
const std::filesystem::path &pathFor(const UserDataPaths &paths, Component component) {
    switch (component) {
    case Component::Settings: return paths.settings;
    case Component::UserDictionary: return paths.userDictionary;
    case Component::QuickPhrases: return paths.quickPhrases;
    case Component::ClipboardHistory: return paths.clipboardHistory;
    case Component::ClipboardFavorites: return paths.clipboardFavorites;
    case Component::Learning: break;
    }
    fail("学习路径由现有Writer管理");
}
void checkDistinctPaths(const UserDataPaths &paths, const std::filesystem::path &bundle) {
    const auto root = absolutePath(bundle);
    std::set<std::filesystem::path> found;
    for (auto component : components) {
        if (component == Component::Learning) continue;
        const auto &path = pathFor(paths, component);
        if (path.empty()) continue;
        auto absolute = absolutePath(path);
        auto relative = absolute.lexically_relative(root);
        if (absolute == root || (!relative.empty() && *relative.begin() != ".."))
            fail("数据包不能包含或覆盖正在使用的数据路径");
        if (!found.insert(absolute).second) fail("不同数据项不能共用文件路径");
    }
}
Fd lockFile(int directory, std::string_view leaf, Component component) {
    const auto name = component == Component::ClipboardHistory || component == Component::ClipboardFavorites
        ? "." + std::string(leaf) + ".lock" : std::string(leaf) + ".lock";
    Fd lock(::openat(directory, name.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (lock.value < 0) ioFail("打开数据写入锁");
    struct stat status{};
    if (::fstat(lock.value, &status) != 0 || !S_ISREG(status.st_mode)) fail("数据锁不是普通文件");
    if (::flock(lock.value, LOCK_EX | LOCK_NB) != 0) ioFail("数据项正在被其他程序修改");
    return lock;
}
bool validUtf8(std::string_view value) {
    for (std::size_t i = 0; i < value.size();) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c < 0x80) { if (c == 0) return false; ++i; continue; }
        std::size_t count = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        if (!count || count >= value.size()-i) return false;
        const auto second = static_cast<unsigned char>(value[i+1]);
        if ((c==0xe0 && second<0xa0) || (c==0xed && second>=0xa0) ||
            (c==0xf0 && second<0x90) || (c==0xf4 && second>0x8f)) return false;
        for (std::size_t j=1; j<=count; ++j)
            if ((static_cast<unsigned char>(value[i+j]) & 0xc0) != 0x80) return false;
        i += count+1;
    }
    return true;
}
void appendNumber(std::string &out, std::uint64_t number) {
    for (int i=0; i<8; ++i) out.push_back(static_cast<char>(number >> (i*8)));
}
std::uint64_t takeNumber(std::string_view &bytes) {
    if (bytes.size()<8) fail("数据包字段不完整");
    std::uint64_t result=0;
    for (int i=0; i<8; ++i) result |= std::uint64_t(static_cast<unsigned char>(bytes[static_cast<std::size_t>(i)])) << (i*8);
    bytes.remove_prefix(8); return result;
}
void appendString(std::string &out, std::string_view value) { appendNumber(out,value.size()); out += value; }
std::string takeString(std::string_view &bytes) {
    auto count=takeNumber(bytes);
    if (count>bytes.size() || count>ClipboardHistory::kMaxEntryBytes) fail("数据包字符串长度无效");
    std::string result(bytes.substr(0,static_cast<std::size_t>(count))); bytes.remove_prefix(static_cast<std::size_t>(count));
    if (!validUtf8(result)) fail("数据包字符串不是有效UTF-8");
    return result;
}
auto identity(const LearningEntry &entry) { return std::tie(entry.phrase,entry.pinyin,entry.contextBefore,entry.contextAfter); }
std::string encodeLearning(const LearningStoreState &state) {
    auto entries=state.snapshot->entries();
    std::sort(entries.begin(),entries.end(),[](const auto &a,const auto &b){ return identity(a)<identity(b); });
    std::string out(learningMagic); appendNumber(out,static_cast<std::uint64_t>(state.resetRevision)); appendNumber(out,entries.size());
    for (const auto &entry:entries) {
        appendString(out,entry.phrase); appendString(out,entry.pinyin);
        appendString(out,entry.contextBefore); appendString(out,entry.contextAfter);
        appendNumber(out,static_cast<std::uint64_t>(entry.frequency));
        appendNumber(out,static_cast<std::uint64_t>(entry.lastSelectedMs));
        appendNumber(out,static_cast<std::uint64_t>(entry.negativeFeedback)); appendNumber(out,entry.suppressed ? 1 : 0);
    }
    return out;
}
std::vector<LearningEntry> decodeLearning(std::string_view bytes) {
    if (!bytes.starts_with(learningMagic)) fail("学习备份版本或格式不支持");
    bytes.remove_prefix(learningMagic.size());
    if (takeNumber(bytes)>static_cast<std::uint64_t>(INT64_MAX)) fail("学习清空世代无效");
    auto count=takeNumber(bytes);
    if (count>kMaxLearningEntries || count>bytes.size()/64) fail("学习备份条目数无效");
    std::vector<LearningEntry> entries; entries.reserve(static_cast<std::size_t>(count));
    std::set<std::tuple<std::string,std::string,std::string,std::string>> seen;
    const auto signedNumber=[&] { const auto value=takeNumber(bytes); if(value>static_cast<std::uint64_t>(INT64_MAX)) fail("学习字段数值无效"); return static_cast<std::int64_t>(value); };
    for (std::uint64_t i=0;i<count;++i) {
        LearningEntry entry;
        entry.phrase=takeString(bytes); entry.pinyin=takeString(bytes);
        entry.contextBefore=takeString(bytes); entry.contextAfter=takeString(bytes);
        entry.frequency=signedNumber(); entry.lastSelectedMs=signedNumber(); entry.negativeFeedback=signedNumber();
        auto suppressed=takeNumber(bytes); if(suppressed>1) fail("学习屏蔽标记无效"); entry.suppressed=suppressed!=0;
        if(entry.phrase.empty() || entry.pinyin.empty() || !seen.emplace(identity(entry)).second) fail("学习条目身份无效或重复");
        entries.push_back(std::move(entry));
    }
    if(!bytes.empty()) fail("学习备份含额外字段");
    return entries;
}
std::size_t validateData(Component component,std::string_view bytes) {
    if(component==Component::Learning) return decodeLearning(bytes).size();
    if(component==Component::ClipboardHistory || component==Component::ClipboardFavorites) {
        constexpr std::string_view magic="ModernIME Clipboard History\n";
        if(!bytes.starts_with(magic)) fail("剪贴板备份格式无效");
        bytes.remove_prefix(magic.size()); auto count=takeNumber(bytes);
        if(count>ClipboardHistory::kMaxEntries) fail("剪贴板备份条目数无效");
        for(std::uint64_t i=0;i<count;++i) {
            auto size=takeNumber(bytes);
            if(!size || size>ClipboardHistory::kMaxEntryBytes || size>bytes.size()) fail("剪贴板备份条目不完整");
            bytes.remove_prefix(static_cast<std::size_t>(size));
        }
        if(!bytes.empty()) fail("剪贴板备份含额外字段");
        return static_cast<std::size_t>(count);
    }
    if(!validUtf8(bytes)) fail("文本备份不是有效UTF-8");
    if (component == Component::Settings) {
        // Reuse the authoritative settings parser without materializing a
        // second disk copy or re-resolving a changing package file.
        Fd memory(::memfd_create("modernime-bundle-settings", MFD_CLOEXEC));
        if (memory.value < 0) ioFail("验证设置备份");
        auto remaining = bytes;
        while (!remaining.empty()) {
            const auto written = ::write(memory.value, remaining.data(), remaining.size());
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) ioFail("验证设置备份内容");
            remaining.remove_prefix(static_cast<std::size_t>(written));
        }
        const auto loaded = SettingsStore::load("/proc/self/fd/" + std::to_string(memory.value));
        if (!loaded.isSuccess()) fail("无法解析设置备份");
        for (const auto &diagnostic : loaded.diagnostics)
            if (diagnostic.find("unknown key '") == std::string::npos) fail("设置备份含无效字段：" + diagnostic);
        if (!validateSettings(loaded.settings).valid) fail("设置备份值无效");
    }
    std::istringstream input{std::string(bytes)}; std::string line; std::size_t count=0;
    while(std::getline(input,line)) {
        if(!line.empty() && line.back()=='\r') line.pop_back();
        if(line.empty() || line.front()=='#') continue;
        if(component==Component::Settings) {
            const auto first=line.find_first_not_of(" \t");
            if(first==std::string::npos || line[first]=='#') continue;
        } else if(component==Component::UserDictionary) {
            auto first=line.find('\t'), second=first==std::string::npos ? first : line.find('\t',first+1);
            if(first==std::string::npos || first==0 || second==std::string::npos || second==first+1 || line.find('\t',second+1)!=std::string::npos) fail("个人词典备份需要三列");
            auto key=std::string_view(line).substr(0,first); bool letter=false, separator=false;
            for(char c:key) {
                if((c>='a'&&c<='z') || (c>='A'&&c<='Z')) {letter=true;separator=false;}
                else if(c=='\''&&letter&&!separator) separator=true;
                else fail("个人词典拼音格式无效");
            }
            if(separator) fail("个人词典拼音格式无效");
            try {
                const auto text=line.substr(second+1); std::size_t consumed=0;
                const auto weight=std::stof(text,&consumed);
                if(consumed!=text.size() || !std::isfinite(weight) || weight<0) fail("个人词典权重无效");
            } catch(const std::exception &) { fail("个人词典权重无效"); }
        } else {
            auto split=line.find_first_of(" \t"); auto start=split==std::string::npos ? split : line.find_first_not_of(" \t",split);
            if(split==std::string::npos || split==0 || split>30 || start==std::string::npos ||
                !std::all_of(line.begin(),line.begin()+static_cast<std::ptrdiff_t>(split),[](char c){return c>='a'&&c<='z';})) fail("快捷短语备份格式无效");
            auto text=std::string_view(line).substr(start);
            if(text.front()=='"') { if(text.size()<2 || text.back()!='"') fail("快捷短语引号不完整"); text=text.substr(1,text.size()-2); }
            if(text.empty()) fail("快捷短语正文为空");
            for(std::size_t i=0;i<text.size();++i) {
                const auto c=static_cast<unsigned char>(text[i]);
                if(c<0x20 || c==0x7f) fail("快捷短语正文含控制字符");
                if(c=='\\') { if(++i==text.size() || (text[i]!='\\' && text[i]!='"')) fail("快捷短语转义格式无效"); }
            }
        }
        ++count;
    }
    return count;
}
struct Data {
    bool included=false;
    std::optional<std::string> bytes;
    std::size_t entries=0;
};
using Package=std::array<Data,6>;
Data readCurrent(const UserDataPaths &paths,LearningWriter *writer,Component component) {
    Data data; data.included=true;
    if(component==Component::Learning) {
        if(!writer) fail("学习数据操作需要现有LearningWriter");
        auto state=writer->exportConsistentState();
        if(!state || !state->snapshot) fail("无法导出一致的学习数据");
        data.bytes=encodeLearning(*state);
    } else {
        const auto path=absolutePath(pathFor(paths,component));
        auto directory=openDirectory(path.parent_path(),false,true);
        if(directory.value<0) return data;
        auto lock=lockFile(directory.value,path.filename().string(),component);
        data.bytes=readAt(directory.value,path.filename().string());
    }
    if(data.bytes) data.entries=validateData(component,*data.bytes);
    return data;
}
std::string manifestFor(const Package &package) {
    std::ostringstream out; out<<manifestMagic;
    for(std::size_t i=0;i<package.size();++i) {
        const auto &data=package[i]; out<<names[i]<<'\t'<<(!data.included?"excluded":!data.bytes?"missing":"present")<<'\t';
        out<<filenames[i]<<'\t'<<(data.bytes?data.bytes->size():0)<<'\t'<<data.entries<<'\t'<<(data.bytes?fingerprint(*data.bytes):"-")<<'\n';
    }
    return out.str();
}
std::uint64_t parseNumber(std::string_view text) {
    std::uint64_t value=0; auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
    if(text.empty() || parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size()) fail("数据包清单数值格式无效");
    return value;
}
std::pair<Package,std::string> loadPackage(const std::filesystem::path &path) {
    auto directory=openDirectory(path);
    auto manifest=readAt(directory.value,"manifest.txt",16384);
    if(!manifest || !manifest->starts_with(manifestMagic)) fail("数据包清单缺失或版本不支持");
    std::istringstream input(manifest->substr(manifestMagic.size())); std::string line; Package package;
    for(std::size_t i=0;i<package.size();++i) {
        if(!std::getline(input,line)) fail("数据包清单不完整");
        std::array<std::string,6> fields; std::istringstream row(line);
        for(auto &field:fields) if(!std::getline(row,field,'\t')) fail("数据包清单列数无效");
        std::string extra;
        if(std::count(line.begin(),line.end(),'\t')!=5 || std::getline(row,extra,'\t') || fields[0]!=names[i] || fields[2]!=filenames[i]) fail("数据包清单名称无效");
        auto &data=package[i]; data.included=fields[1]!="excluded";
        const auto size=parseNumber(fields[3]), count=parseNumber(fields[4]);
        if(fields[1]!="present" && fields[1]!="missing" && fields[1]!="excluded") fail("数据包状态无效");
        data.bytes=readAt(directory.value,filenames[i]);
        if(fields[1]=="present") {
            if(!data.bytes || data.bytes->size()!=size || fingerprint(*data.bytes)!=fields[5]) fail("数据包校验失败");
            data.entries=validateData(components[i],*data.bytes);
            if(data.entries!=count) fail("数据包条目数量不一致");
        } else if(data.bytes || size!=0 || count!=0 || fields[5]!="-") fail("数据包排除项包含意外数据");
    }
    if(std::getline(input,line)) fail("数据包清单含额外项目");
    return {std::move(package),fingerprint(*manifest)};
}
void checkPreview(const UserDataPreview &expected,const UserDataPreview &current,UserDataSelection selection) {
    if(expected.version!=1 || expected.bundleFingerprint!=current.bundleFingerprint || expected.differences.size()!=6) fail("数据包与预览不一致，请重新预览");
    for(std::size_t i=0;i<components.size();++i) {
        const auto &before=expected.differences[i]; const auto &after=current.differences[i];
        if(before.component!=components[i]) fail("恢复预览项目无效");
        if(selection.includes(components[i]) && after.included && after.present &&
            (before.currentPresent!=after.currentPresent || before.currentFingerprint!=after.currentFingerprint)) fail("数据在预览后发生变化，请重新预览");
    }
}
}

std::string_view userDataComponentName(Component component) {
    auto index=static_cast<std::size_t>(component); return index<names.size()?names[index]:std::string_view{};
}
bool UserDataSelection::includes(Component component) const {
    switch(component) {
    case Component::Settings:return settings;
    case Component::UserDictionary:return userDictionary;
    case Component::QuickPhrases:return quickPhrases;
    case Component::Learning:return learning;
    case Component::ClipboardHistory:return clipboardHistory;
    case Component::ClipboardFavorites:return clipboardFavorites;
    }
    return false;
}
UserDataBundle::UserDataBundle(UserDataPaths paths,LearningWriter *learning)
    :paths_(std::move(paths)),learning_(learning) {}
bool UserDataBundle::exportTo(const std::filesystem::path &directory,UserDataSelection selection,std::string *error) const {
    if(error) error->clear();
    try {
        checkDistinctPaths(paths_,directory);
        Package package;
        for(std::size_t i=0;i<components.size();++i) if(selection.includes(components[i])) package[i]=readCurrent(paths_,learning_,components[i]);
        const auto absolute=absolutePath(directory); auto parent=openDirectory(absolute.parent_path(),true);
        const auto leaf=absolute.filename().string();
        if(::mkdirat(parent.value,leaf.c_str(),0700)!=0) ioFail("创建新数据包目录（目标必须不存在）");
        Fd output(::openat(parent.value,leaf.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
        if (output.value < 0) ioFail("打开新数据包目录");
        try {
            for(std::size_t i=0;i<package.size();++i) if(package[i].bytes) writeAt(output.value,filenames[i],*package[i].bytes);
            writeAt(output.value,"manifest.txt",manifestFor(package));
            if(::fsync(output.value)!=0 || ::fsync(parent.value)!=0) ioFail("同步数据包目录");
        } catch(...) {
            for(auto file:filenames) ::unlinkat(output.value,std::string(file).c_str(),0);
            ::unlinkat(output.value,"manifest.txt",0); ::unlinkat(parent.value,leaf.c_str(),AT_REMOVEDIR); throw;
        }
        return true;
    } catch(const std::exception &exception) { if(error) *error=exception.what(); return false; }
}
std::optional<UserDataPreview> UserDataBundle::preview(const std::filesystem::path &directory,std::string *error) const {
    if(error) error->clear();
    try {
        checkDistinctPaths(paths_,directory);
        auto [package,manifestHash]=loadPackage(directory); UserDataPreview result; result.bundleFingerprint=manifestHash;
        for(std::size_t i=0;i<components.size();++i) {
            const auto &data=package[i]; UserDataDifference difference; difference.component=components[i];
            difference.included=data.included; difference.present=data.bytes.has_value(); difference.bundleEntries=data.entries;
            if(data.bytes) difference.bundleFingerprint=fingerprint(*data.bytes);
            // Never read excluded private components as a side effect of preview.
            if(data.included && data.bytes) {
                auto current=readCurrent(paths_,learning_,components[i]);
                difference.currentPresent=current.bytes.has_value(); difference.currentEntries=current.entries;
                if(current.bytes) difference.currentFingerprint=fingerprint(*current.bytes);
                difference.changed=!current.bytes || difference.currentFingerprint!=difference.bundleFingerprint;
            }
            result.differences.push_back(std::move(difference));
        }
        return result;
    } catch(const std::exception &exception) { if(error) *error=exception.what(); return std::nullopt; }
}
bool UserDataBundle::restore(const std::filesystem::path &directory,UserDataSelection selection,
                             const UserDataPreview &expected,std::string *error) const {
    if(error) error->clear();
    struct Staged {
        Fd directory,lock; std::string leaf,temporary,rollback;
        std::filesystem::path path;
        bool existed=false,applied=false,temporaryCreated=false,rollbackCreated=false;
    };
    std::vector<Staged> staged;
    try {
        std::string previewError; auto current=preview(directory,&previewError);
        if(!current) fail(previewError);
        checkPreview(expected,*current,selection);
        auto [package,manifestHash]=loadPackage(directory);
        if(manifestHash!=expected.bundleFingerprint) fail("预览后数据包发生变化");
        staged.reserve(5);
        for(std::size_t i=0;i<components.size();++i) {
            auto component=components[i]; const auto &data=package[i];
            if(component==Component::Learning || !selection.includes(component) || !data.included || !data.bytes) continue;
            const auto path=absolutePath(pathFor(paths_,component));
            Staged stage; stage.path=path;
            stage.directory=openDirectory(path.parent_path(),true); stage.leaf=path.filename().string();
            stage.lock=lockFile(stage.directory.value,stage.leaf,component);
            auto before=readAt(stage.directory.value,stage.leaf);
            const auto &baseline=expected.differences[i];
            if(before.has_value()!=baseline.currentPresent || (before && fingerprint(*before)!=baseline.currentFingerprint)) fail("数据在预览后发生变化，请重新预览");
            stage.existed=before.has_value(); stage.temporary=uniqueName(".new");
            stage.rollback=uniqueName("-"+stage.leaf+".rollback");
            staged.push_back(std::move(stage)); auto &saved=staged.back();
            writeAt(saved.directory.value,saved.temporary,*data.bytes);
            saved.temporaryCreated=true;
            if(before) {
                writeAt(saved.directory.value,saved.rollback,*before);
                saved.rollbackCreated=true;
            }
            if(::fsync(saved.directory.value)!=0) ioFail("同步恢复暂存目录");
        }
        std::optional<LearningStoreState> learningBaseline;
        const auto &learning=package[3];
        if(selection.learning && learning.included && learning.bytes) {
            if(!learning_) fail("学习恢复需要现有LearningWriter");
            learningBaseline=learning_->exportConsistentState();
            if(!learningBaseline || !learningBaseline->snapshot ||
               fingerprint(encodeLearning(*learningBaseline))!=expected.differences[3].currentFingerprint)
                fail("学习数据在预览后发生变化，请重新预览");
        }
        for(auto &stage:staged) {
            if(::renameat(stage.directory.value,stage.temporary.c_str(),stage.directory.value,stage.leaf.c_str())!=0) ioFail("替换恢复文件");
            stage.applied=true;
            if(::fsync(stage.directory.value)!=0) ioFail("同步恢复文件目录");
        }
        if(selection.learning && learning.included && learning.bytes) {
            if(!learning_->restoreTrustedEntries(decodeLearning(*learning.bytes),*learningBaseline))
                fail("学习恢复失败或数据已变化，现有学习库未替换，请重新预览");
        }
        for(auto &stage:staged) if(stage.rollbackCreated) ::unlinkat(stage.directory.value,stage.rollback.c_str(),0);
        return true;
    } catch(const std::exception &exception) {
        std::string message=exception.what();
        for(auto iterator=staged.rbegin();iterator!=staged.rend();++iterator) {
            auto &stage=*iterator; bool rollbackOk=true;
            if(stage.applied) {
                rollbackOk=stage.existed ? ::renameat(stage.directory.value,stage.rollback.c_str(),stage.directory.value,stage.leaf.c_str())==0
                                        : ::unlinkat(stage.directory.value,stage.leaf.c_str(),0)==0;
                if(rollbackOk) ::fsync(stage.directory.value);
            }
            if(stage.temporaryCreated) ::unlinkat(stage.directory.value,stage.temporary.c_str(),0);
            if(rollbackOk && stage.rollbackCreated) ::unlinkat(stage.directory.value,stage.rollback.c_str(),0);
            else message+="；文件回滚失败，保留恢复副本："+(stage.path.parent_path()/stage.rollback).string();
        }
        if(error) *error=message;
        return false;
    }
}
} // namespace modernime::core
