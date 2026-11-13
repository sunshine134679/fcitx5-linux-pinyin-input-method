#pragma once

#include <gio/gio.h>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::settings {

// Parsed on the worker; values handed to GTK are immutable copies.
struct BackendValue final {
    enum class Kind { Null, Boolean, Number, String, Array, Object };
    Kind kind = Kind::Null;
    std::string scalar;
    std::vector<BackendValue> items;
    std::map<std::string, BackendValue, std::less<>> members;
    const BackendValue &get(std::string_view key) const;
    std::string text(std::string_view key = {}) const;
    std::uint64_t number(std::string_view key = {}) const;
    bool boolean(std::string_view key = {}) const;
    const std::vector<BackendValue> &array(std::string_view key = {}) const;
};
struct BackendReply final {
    bool ok = false;
    bool uncertain = false; // Transport failed after a mutation may have started.
    std::string error;
    std::string message;
    BackendValue data;
};
BackendReply parseBackendReply(std::string_view json);

// All blocking DBus and parsing work runs in GTask workers, with a 30 s timeout.
// Every request probes capabilities against the current service owner, allowing
// reconnection after an input-method restart. Destruction drops all callbacks.
class BackendClient final {
public:
    using Callback = std::function<void(BackendReply)>;
    BackendClient();
    ~BackendClient();
    BackendClient(const BackendClient &) = delete;
    BackendClient &operator=(const BackendClient &) = delete;
    void execute(std::string action, std::vector<std::string> args, Callback callback);
private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace modernime::settings
