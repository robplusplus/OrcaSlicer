#pragma once

#include <string>
#include <memory>
#include <map>
#include <mutex>
#include <functional>
#include <type_traits>
#include "nlohmann/json.hpp"
#include <atomic>
#include <chrono>
#include <unordered_map>

namespace Slic3r {
namespace Utils {


class ActionItemBase {
    public: 
    virtual ~ ActionItemBase() = default;
    virtual std::string run(std::string parms) = 0;
};

class SerializeUsingNlohmannJson{};

// Helper for static_assert in dependent contexts
template <typename T>
struct always_false : std::false_type {};

template <class T, class SerializationType = SerializeUsingNlohmannJson>
T from_string(const std::string & parms){
    if constexpr (std::is_same<SerializationType, SerializeUsingNlohmannJson>::value) {
        nlohmann::json j = nlohmann::json::parse(parms);
        return j.get<T>();
    } else {
        static_assert(always_false<SerializationType>::value, "Unsupported SerializationType");
    }
}

template <class T, class SerializationType = SerializeUsingNlohmannJson>
std::string to_string(const T & result){
    if constexpr (std::is_same<SerializationType, SerializeUsingNlohmannJson>::value) {
        nlohmann::json j = result;
        return j.dump();
    } else {
        static_assert(always_false<SerializationType>::value, "Unsupported SerializationType");
    }
}

template <class Args = void, 
    class Return = void, 
    class ArgsSerializationType = SerializeUsingNlohmannJson, 
    class ReturnSerializationType = SerializeUsingNlohmannJson>
class ActionItem : public ActionItemBase {
public:
    using Callback = std::function<Return(Args)>;
    explicit ActionItem(Callback fn) : action_fn(std::move(fn)) {}

    std::string run(std::string parms) override {
        Args args = from_string<Args, ArgsSerializationType>(parms);
        Return result = action_fn(args);
        return to_string<Return, ReturnSerializationType>(result);
    }

    Return execute(const Args& args){
        return action_fn(args);
    }

private:
    Callback action_fn;
};


class ActionRegister {
public:
    ActionRegister() = default;
    ~ActionRegister() = default;
    template <class Args = void, class Return = void, 
        class ArgsSerializationType = SerializeUsingNlohmannJson, 
        class ReturnSerializationType = SerializeUsingNlohmannJson>
    std::shared_ptr<ActionItem<Args, Return, ArgsSerializationType, ReturnSerializationType>> register_action(std::string name, std::function<Return(Args)> callback) {
        std::lock_guard<std::mutex> lock(mutex);

        auto action = std::make_shared<ActionItem<Args, Return, ArgsSerializationType, ReturnSerializationType>>(std::move(callback));
        actions[std::move(name)] = action;
        return action;
    }

    std::shared_ptr<ActionItemBase> get_action(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = actions.find(name);
        if (it != actions.end()) {
            return it->second;
        }
        return nullptr;
    }

private:
    std::map<std::string, std::shared_ptr<ActionItemBase>> actions;
    std::mutex mutex;
};


// Simple in-process task tracking for action executions
struct ActionTaskSnapshot {
    std::string id;
    std::string action;
    std::string state;     // queued | running | scheduled | submitted | completed | failed
    std::string message;   // last message
    int         progress{-1};
    std::string result;    // optional: last known result JSON (as string)
    std::uint64_t started_ms{0};
    std::uint64_t updated_ms{0};
    // Collected in-memory logs for this task (most-recent-first or append order)
    std::vector<std::string> logs;
};

class ActionTaskRegistry {
public:
    static ActionTaskRegistry& instance()
    {
        static ActionTaskRegistry reg;
        return reg;
    }

    std::string new_task(const std::string& action)
    {
        auto id = next_id();
        auto nowms = now_ms();
        std::lock_guard<std::mutex> lock(mtx);
        ActionTaskSnapshot t;
        t.id = id;
        t.action = action;
        t.state = "queued";
        t.started_ms = nowms;
        t.updated_ms = nowms;
        tasks[id] = std::move(t);
        return id;
    }

    void update(const std::string& id, const std::string& state, const std::string& message = std::string(), int progress = -1)
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = tasks.find(id);
        if (it == tasks.end()) return;
        if (!state.empty()) it->second.state = state;
        it->second.updated_ms = now_ms();
        if (!message.empty()) it->second.message = message;
        if (progress >= 0) it->second.progress = progress;
    }

    void append_log(const std::string& id, const std::string& msg)
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = tasks.find(id);
        if (it == tasks.end()) return;
        it->second.logs.emplace_back(msg);
        // keep only the last N logs to avoid unbounded growth
        if (it->second.logs.size() > max_logs_per_task_) {
            const auto trim = it->second.logs.size() - max_logs_per_task_;
            it->second.logs.erase(it->second.logs.begin(), it->second.logs.begin() + static_cast<long long>(trim));
        }
        it->second.updated_ms = now_ms();
    }

    void set_result(const std::string& id, const std::string& result_json)
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = tasks.find(id);
        if (it == tasks.end()) return;
        it->second.result = result_json;
        it->second.updated_ms = now_ms();
    }

    bool get(const std::string& id, ActionTaskSnapshot& out)
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = tasks.find(id);
        if (it == tasks.end()) return false;
        out = it->second;
        return true;
    }

    // Convenience JSON
    static nlohmann::json to_json(const ActionTaskSnapshot& t)
    {
        nlohmann::json j;
        j["id"] = t.id;
        j["action"] = t.action;
        j["state"] = t.state;
        j["message"] = t.message;
        if (t.progress >= 0) j["progress"] = t.progress; else j["progress"] = nullptr;
        if (!t.result.empty()) j["result"] = nlohmann::json::parse(t.result, nullptr, false, true);
        j["started_ms"] = t.started_ms;
        j["updated_ms"] = t.updated_ms;
        j["logs"] = t.logs; // simple array of strings
        return j;
    }

    // Task-context helpers
    void set_current_task_id(const std::string& id) { current_task_id() = id; }
    std::string get_current_task_id() const { return current_task_id(); }

    // Wrapper to run a registered action as a task: returns {result_json, task_id}
    std::pair<std::string, std::string> run_as_task(ActionItemBase* action, const std::string& action_name, const std::string& parms)
    {
        const std::string id = new_task(action_name);
        set_current_task_id(id);
        update(id, "running", "action invoked");
        std::string result;
        try {
            result = action->run(parms);
            set_result(id, result);
            // Try infer immediate state from result.status/message
            try {
                auto jr = nlohmann::json::parse(result);
                std::string st = jr.value("status", std::string());
                std::string msg = jr.value("message", std::string());
                if (st == "ok" && msg.find("scheduled") != std::string::npos) update(id, "scheduled", msg);
                else if (st == "ok") update(id, "success", msg);
                else update(id, "failed", msg);
            } catch (...) {
                update(id, "success");
            }
        } catch (const std::exception& ex) {
            nlohmann::json er; er["status"] = "error"; er["message"] = std::string("exception: ") + ex.what();
            result = er.dump();
            update(id, "failed", ex.what());
        } catch (...) {
            nlohmann::json er; er["status"] = "error"; er["message"] = "unknown exception";
            result = er.dump();
            update(id, "failed", "unknown exception");
        }
        // Clear current task id
        set_current_task_id("");
        return {result, id};
    }

    // Static helpers for actions to update status
    static void task_update(const std::string& id, const std::string& state, const std::string& message = std::string(), int progress = -1)
    { instance().update(id, state, message, progress); }
    static void task_set_result(const std::string& id, const std::string& result_json)
    { instance().set_result(id, result_json); }
    static void task_append_log(const std::string& id, const std::string& msg)
    { instance().append_log(id, msg); }
    static std::string current_id() { return instance().get_current_task_id(); }

private:
    ActionTaskRegistry() = default;
    ~ActionTaskRegistry() = default;

    static std::string& current_task_id()
    {
        thread_local std::string tid;
        return tid;
    }

    static std::uint64_t now_ms()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    std::string next_id()
    {
        auto n = ++counter;
        // Simple monotonic id; good enough for in-process tracking
        return std::to_string(n);
    }

    std::unordered_map<std::string, ActionTaskSnapshot> tasks;
    std::mutex mtx;
    std::atomic<std::uint64_t> counter{0};
    const std::size_t max_logs_per_task_{2000};
};

// Convenience free functions for callers and actions
inline std::string start_task_for_action_and_run(ActionRegister& reg, const std::string& action_name, const std::string& parms, std::string& out_result)
{
    auto act = reg.get_action(action_name);
    if (!act) {
        nlohmann::json er; er["status"] = "error"; er["message"] = std::string("unknown action: ") + action_name; out_result = er.dump();
        return std::string();
    }
    auto pair = ActionTaskRegistry::instance().run_as_task(act.get(), action_name, parms);
    out_result = pair.first;
    return pair.second;
}

inline std::string current_action_task_id()
{ return ActionTaskRegistry::current_id(); }

inline void update_action_task(const std::string& id, const std::string& state, const std::string& message = std::string(), int progress = -1)
{ ActionTaskRegistry::task_update(id, state, message, progress); }

inline void append_action_task_log(const std::string& id, const std::string& message)
{ ActionTaskRegistry::task_append_log(id, message); }

inline void append_current_action_task_log(const std::string& message)
{ ActionTaskRegistry::task_append_log(current_action_task_id(), message); }

inline bool get_action_task_snapshot(const std::string& id, nlohmann::json& out)
{
    ActionTaskSnapshot snaps;
    if (!ActionTaskRegistry::instance().get(id, snaps)) return false;
    out = ActionTaskRegistry::to_json(snaps);
    return true;
}

// No test/demo code in headers to avoid ODR/link issues

}}
