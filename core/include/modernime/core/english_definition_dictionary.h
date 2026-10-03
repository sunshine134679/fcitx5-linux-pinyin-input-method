#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace modernime::core {

class EnglishDefinitionDictionary final {
public:
    EnglishDefinitionDictionary() noexcept;
    ~EnglishDefinitionDictionary();

    EnglishDefinitionDictionary(const EnglishDefinitionDictionary &) = delete;
    EnglishDefinitionDictionary &operator=(const EnglishDefinitionDictionary &) = delete;
    EnglishDefinitionDictionary(EnglishDefinitionDictionary &&) noexcept;
    EnglishDefinitionDictionary &operator=(EnglishDefinitionDictionary &&) noexcept;

    // 打开二进制词典文件进行只读 mmap 映射；失败返回 false
    bool load(const std::filesystem::path &path);

    // 关闭并解除 mmap
    void close() noexcept;

    // 是否已成功载入
    bool isLoaded() const noexcept;

    // 词条总数
    std::size_t entryCount() const noexcept;

    // 根据英文单词检索清洗后的中文释义，严格 O(log N) 二分查找
    // 大小写不敏感（内部自动转为小写），未命中返回空 string_view
    std::string_view lookup(std::string_view word) const noexcept;

    // 文本清洗与规范化函数（供测试与离线工具对齐逻辑）
    static std::string cleanDefinition(std::string_view rawTranslation,
                                      std::size_t maxChars = 12);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::core
