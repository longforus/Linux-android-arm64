#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <vector>
#include <stdexcept>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include "../../driver/debug_wire.h"

namespace lsdebug
{
using Json = nlohmann::json;
inline std::string Hex(uint64_t value)
{
    char buf[16];
    auto out = std::to_chars(buf, buf + sizeof(buf), value, 16);
    return "0x" + std::string(buf, out.ptr);
}
// Identifiers, sequence numbers and nanoseconds are strings to avoid JS's 53-bit limit.
inline Json EventJson(const ls_debug_event &event)
{
    Json frames = Json::array();
    for (unsigned i = 0; i < std::min(event.frame_count, LS_DEBUG_FRAMES); ++i)
        frames.push_back({{"fp", Hex(event.frames[i].fp)}, {"return_address", Hex(event.frames[i].lr)}});
    return {{"seq", std::to_string(event.seq)}, {"timestamp_ns", std::to_string(event.timestamp_ns)},
        {"process_start_ns", std::to_string(event.process_start_ns)}, {"pid", event.tgid}, {"tid", event.tid},
        {"kind", event.kind == 2 ? "step" : "breakpoint"}, {"pc", Hex(event.pc)}, {"lr", Hex(event.lr)},
        {"sp", Hex(event.sp)}, {"fp", Hex(event.fp)}, {"frames", frames},
        {"unwind", {{"method", "captured_fp_chain"}, {"reason", event.frame_reason},
            {"complete", false}, {"raw_return_addresses", true}}}};
}
inline Json MessageJson(const ls_debug_message &message, uint64_t after = 0)
{
    Json events = Json::array();
    for (unsigned i = 0; i < std::min(message.count, LS_DEBUG_BATCH); ++i) events.push_back(EventJson(message.events[i]));
    const uint64_t lost = message.oldest > after && message.oldest - after > 1 ? message.oldest - after - 1 : 0;
    return {{"stream", std::to_string(message.stream)}, {"active", message.active != 0},
        {"captured", std::to_string(message.captured)}, {"ring_overwritten", std::to_string(message.dropped)},
        {"oldest_seq", std::to_string(message.oldest)}, {"newest_seq", std::to_string(message.newest)},
        {"lost_before_page", std::to_string(lost)}, {"reason", message.reason}, {"events", events},
        {"next_after", std::to_string(message.count ? message.events[message.count - 1].seq : after)}};
}

template <typename Backend> class DebugService
{
    Backend &driver_;
    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic_bool stop_{false}, busy_{false};
    int directory_ = -1, output_ = -1;
    std::string session_;
    Json state_ = Json::object();
    uint64_t stream_ = 0, cursor_ = 0, lost_ = 0, saved_ = 0, bytes_ = 0;
    static constexpr uint64_t FileBudget = 8 * 1024 * 1024;
    static constexpr size_t SessionBudget = 32;

    static bool validId(const std::string &id)
    {
        return !id.empty() && id.size() <= 96 && std::all_of(id.begin(), id.end(), [](unsigned char c)
        { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '-'; });
    }
    static void require(int status)
    {
        if (status) throw std::runtime_error("lsdriver debug status=" + std::to_string(status));
    }
    static bool writeAll(int fd, const std::string &line)
    {
        size_t offset = 0;
        while (offset < line.size())
        {
            const ssize_t count = ::write(fd, line.data() + offset, line.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
            offset += size_t(count);
        }
        return true;
    }
    bool append(const Json &record)
    {
        const std::string line = record.dump() + "\n";
        // Reserve space for the final status, even when the event budget is full.
        if (bytes_ + line.size() > FileBudget || !writeAll(output_, line)) return false;
        bytes_ += line.size();
        return true;
    }
    std::vector<std::string> files() const
    {
        if (directory_ < 0) throw std::runtime_error("call trace.configure with a private writable directory first");
        // A new directory description avoids sharing readdir's offset with the root fd.
        int scan = openat(directory_, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (scan < 0) throw std::runtime_error("cannot scan trace directory");
        DIR *dir = fdopendir(scan);
        if (!dir) { close(scan); throw std::runtime_error("cannot scan trace directory"); }
        std::vector<std::string> result;
        while (auto *entry = readdir(dir))
        {
            std::string name(entry->d_name);
            if (name.size() > 6 && name.substr(name.size() - 6) == ".jsonl" && validId(name.substr(0, name.size() - 6)))
            {
                result.push_back(name.substr(0, name.size() - 6));
                if (result.size() > SessionBudget) break;
            }
        }
        closedir(dir);
        std::sort(result.begin(), result.end());
        return result;
    }
    template <typename Visitor> void visit(const std::string &id, Visitor visitor) const
    {
        if (directory_ < 0 || !validId(id)) throw std::invalid_argument("invalid session id or trace directory");
        const int fd = openat(directory_, (id + ".jsonl").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) throw std::runtime_error("session not found");
        struct stat st{};
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > off_t(FileBudget + 8192))
        { close(fd); throw std::runtime_error("invalid trace file"); }
        FILE *file = fdopen(fd, "r");
        if (!file) { close(fd); throw std::runtime_error("cannot read trace file"); }
        // Bounded line buffer; never let a malformed file allocate unbounded memory.
        char line[8192];
        bool header = false;
        while (fgets(line, sizeof(line), file))
        {
            const size_t length = std::strlen(line);
            if (!length || line[length - 1] != '\n') break; // interrupted tail is not evidence
            auto record = Json::parse(line, line + length, nullptr, false);
            if (record.is_discarded() || !record.is_object()) { fclose(file); throw std::runtime_error("corrupt trace record"); }
            if (!header)
            {
                if (record.value("type", "") != "header" || record.value("format", 0) != 1 ||
                    !record.contains("session") || !record["session"].is_object())
                { fclose(file); throw std::runtime_error("bad trace file header"); }
                header = true;
            }
            if (!visitor(record)) break;
        }
        const bool failed = ferror(file);
        fclose(file);
        if (failed) throw std::runtime_error("trace read failed");
        if (!header) throw std::runtime_error("missing trace file header");
    }
    void pump(uint64_t durationMs) noexcept
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
        std::string reason = "stopped", error;
        try
        {
            while (true)
            {
                ls_debug_message message{};
                message.command = LS_DEBUG_READ;
                message.stream = stream_;
                message.after = cursor_;
                message.count = LS_DEBUG_BATCH;
                require(driver_.DebugCall(message));
                bool budget = false;
                {
                    std::lock_guard lock(mutex_);
                    if (message.oldest > cursor_ && message.oldest - cursor_ > 1) lost_ += message.oldest - cursor_ - 1;
                    for (unsigned i = 0; i < message.count; ++i)
                    {
                        if (!append({{"type", "event"}, {"event", EventJson(message.events[i])}}))
                        { budget = true; break; }
                        cursor_ = message.events[i].seq;
                        ++saved_;
                    }
                    state_["captured"] = std::to_string(message.captured);
                    state_["saved_through_seq"] = std::to_string(cursor_);
                    state_["saved_events"] = std::to_string(saved_);
                    state_["lost"] = std::to_string(lost_);
                    state_["ring_overwritten"] = std::to_string(message.dropped);
                    state_["kernel_reason"] = message.reason;
                }
                if (budget) { reason = "storage_limit_or_write_error"; break; }
                if (!message.active && cursor_ >= message.newest)
                { reason = message.reason == LS_DEBUG_REASON_EXIT ? "target_exit" : "step_limit"; break; }
                if (stop_.load() || std::chrono::steady_clock::now() >= deadline)
                {
                    // Stop first, then drain the final immutable ring on the next iteration.
                    ls_debug_message end{}; end.command = LS_DEBUG_TRACE_STOP; end.stream = stream_;
                    require(driver_.DebugCall(end));
                    reason = stop_.load() ? "stopped" : "duration_limit";
                    stop_.store(false);
                    // Drain without waiting; stopping disarms all target threads.
                    for (;;)
                    {
                        ls_debug_message tail{}; tail.command = LS_DEBUG_READ; tail.stream = stream_;
                        tail.after = cursor_; tail.count = LS_DEBUG_BATCH;
                        require(driver_.DebugCall(tail));
                        std::lock_guard lock(mutex_);
                        if (tail.oldest > cursor_ && tail.oldest - cursor_ > 1) lost_ += tail.oldest - cursor_ - 1;
                        for (unsigned i = 0; i < tail.count; ++i)
                        {
                            if (!append({{"type", "event"}, {"event", EventJson(tail.events[i])}}))
                                throw std::runtime_error("trace storage limit or write failure");
                            cursor_ = tail.events[i].seq;
                            ++saved_;
                        }
                        state_["captured"] = std::to_string(tail.captured);
                        state_["saved_through_seq"] = std::to_string(cursor_);
                    state_["saved_events"] = std::to_string(saved_);
                        state_["lost"] = std::to_string(lost_);
                        state_["ring_overwritten"] = std::to_string(tail.dropped);
                        state_["kernel_reason"] = tail.reason;
                        if (!tail.count || cursor_ >= tail.newest) break;
                    }
                    break;
                }
                if (message.count < LS_DEBUG_BATCH) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        catch (const std::exception &ex) { error = ex.what(); reason = "error"; }
        catch (...) { error = "unknown trace error"; reason = "error"; }
        ls_debug_message end{}; end.command = LS_DEBUG_TRACE_STOP; end.stream = stream_;
        const int stopStatus = driver_.DebugCall(end);
        ls_debug_message final{}; final.command = LS_DEBUG_STATUS; final.stream = stream_;
        const int finalStatus = driver_.DebugCall(final);
        std::lock_guard lock(mutex_);
        if (!finalStatus)
        {
            state_["captured"] = std::to_string(final.captured);
            state_["lost"] = std::to_string(final.captured >= saved_ ? final.captured - saved_ : lost_);
            state_["ring_overwritten"] = std::to_string(final.dropped);
            state_["kernel_reason"] = final.reason;
        }
        state_["active"] = false;
        state_["reason"] = reason;
        state_["error"] = error;
        state_["stop_status"] = stopStatus;
        state_["cleanup_pending"] = stopStatus != 0 && stopStatus != -ESTALE;
        state_["size_bytes"] = bytes_;
        // Footer has its own fixed reserve. Its failure is visible in live status.
        const std::string footer = Json{{"type", "footer"}, {"session", state_}}.dump() + "\n";
        if (!writeAll(output_, footer) || fsync(output_) != 0) state_["error"] = "final trace flush failed";
        close(output_); output_ = -1;
        busy_.store(stopStatus != 0 && stopStatus != -ESTALE);
    }

public:
    explicit DebugService(Backend &driver) : driver_(driver) {}
    ~DebugService() { stop(); if (directory_ >= 0) close(directory_); }
    bool busy() const { return busy_.load(); }
    Json capabilities()
    {
        ls_debug_message message{}; message.command = LS_DEBUG_INFO;
        const int status = driver_.DebugCall(message);
        Json result{{"backend", "lsdriver"}, {"protocol_version", 1}, {"debug_abi", status ? 0 : message.abi},
            {"debug_status", status}, {"read_integrity", true}, {"legacy_shared_layout", true},
            {"events", !status && bool(message.flags & LS_DEBUG_CAP_EVENTS)},
            {"captured_fp_chain", !status && bool(message.flags & LS_DEBUG_CAP_FP)},
            {"step_trace_prerequisites", !status && bool(message.flags & LS_DEBUG_CAP_STEP_TRACE)},
            {"trace_runtime_verified", false}, {"trace_scope", "main_thread"}, {"etm_trace", false},
            {"trace_file_budget", FileBudget}, {"trace_session_budget", SessionBudget}};
        if (!status)
        {
            result["kernel_version"] = std::string(message.version, strnlen(message.version, sizeof(message.version)));
            result["kernel_page_size"] = message.page_size;
            result["event_ring_size"] = LS_DEBUG_RING;
            result["event_page_size"] = LS_DEBUG_BATCH;
        }
        return result;
    }
    Json configure(const std::string &path)
    {
        std::lock_guard lock(mutex_);
        if (busy()) throw std::runtime_error("trace is active");
        if (path.empty() || path[0] != '/') throw std::invalid_argument("trace directory must be absolute");
        if (mkdir(path.c_str(), 0700) && errno != EEXIST) throw std::runtime_error("cannot create private trace directory");
        int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        struct stat st{};
        if (fd < 0) throw std::runtime_error("cannot open trace directory");
        if (fstat(fd, &st) || st.st_uid != geteuid() || (st.st_mode & 0077))
        { close(fd); throw std::runtime_error("trace directory must be owned by this uid and mode 0700"); }
        if (directory_ >= 0) close(directory_);
        directory_ = fd;
        return {{"configured", true}, {"directory", path}};
    }
    Json start(int pid, uint64_t pc, uint64_t steps, uint64_t durationMs)
    {
        if (busy()) throw std::runtime_error("trace is active");
        if (worker_.joinable()) worker_.join();
        std::lock_guard lock(mutex_);
        if (!steps || steps > 1000000 || !durationMs || durationMs > 60000)
            throw std::invalid_argument("max_steps=1..1000000, duration_ms=1..60000");
        if (files().size() >= SessionBudget) throw std::runtime_error("trace session budget reached; archive files before starting another trace");
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        session_ = "trace-" + std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
        output_ = openat(directory_, (session_ + ".jsonl").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (output_ < 0) throw std::runtime_error("cannot create trace file");
        cursor_ = lost_ = saved_ = bytes_ = 0;
        ls_debug_message message{}; message.command = LS_DEBUG_TRACE_START;
        message.tgid = pid; message.tid = pid; message.start_pc = pc; message.max_steps = steps;
        const int status = driver_.DebugCall(message);
        if (status)
        { close(output_); output_ = -1; unlinkat(directory_, (session_ + ".jsonl").c_str(), 0); require(status); }
        stream_ = message.stream;
        state_ = {{"session_id", session_}, {"stream", std::to_string(stream_)}, {"pid", pid}, {"tid", pid},
            {"start_pc", Hex(pc)}, {"max_steps", steps}, {"duration_ms", durationMs}, {"active", true},
            {"process_start_ns", std::to_string(message.process_start_ns)}, {"captured", "0"}, {"lost", "0"}, {"reason", "running"}};
        try
        {
            if (!append({{"type", "header"}, {"format", 1}, {"session", state_}})) throw std::runtime_error("cannot write trace header");
            stop_.store(false); busy_.store(true);
            worker_ = std::thread([this, durationMs] { pump(durationMs); });
        }
        catch (...)
        {
            ls_debug_message end{}; end.command = LS_DEBUG_TRACE_STOP; end.stream = stream_; driver_.DebugCall(end);
            busy_.store(false); close(output_); output_ = -1;
            state_["active"] = false; state_["reason"] = "start_failed";
            unlinkat(directory_, (session_ + ".jsonl").c_str(), 0);
            throw;
        }
        return state_;
    }
    Json status() const { std::lock_guard lock(mutex_); return state_; }
    void stop() noexcept
    {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
        if (busy_.load())
        {
            ls_debug_message end{}; end.command = LS_DEBUG_TRACE_STOP; end.stream = stream_;
            const int status = driver_.DebugCall(end);
            std::lock_guard lock(mutex_);
            state_["stop_status"] = status;
            state_["cleanup_pending"] = status != 0 && status != -ESTALE;
            busy_.store(status != 0 && status != -ESTALE);
        }
    }
    Json list() const
    {
        std::lock_guard lock(mutex_);
        Json result = Json::array();
        for (const auto &id : files())
        {
            Json metadata;
            bool finalized = false;
            try
            {
                visit(id, [&](const Json &record)
                {
                    if (record.value("type", "") == "header") metadata = record.at("session");
                    if (record.value("type", "") == "footer") { metadata = record.at("session"); finalized = true; }
                    return true;
                });
                if (id == session_) metadata = state_;
                else if (!finalized && metadata.is_object()) { metadata["active"] = false; metadata["reason"] = "interrupted"; }
                result.push_back({{"session_id", id}, {"metadata", metadata}});
            }
            catch (const std::exception &ex) { result.push_back({{"session_id", id}, {"error", ex.what()}}); }
        }
        return result;
    }
    Json read(const std::string &id, uint64_t after, unsigned count) const
    {
        if (!count || count > 500) throw std::invalid_argument("count=1..500");
        std::lock_guard lock(mutex_);
        Json result = Json::array();
        uint64_t next = after;
        visit(id, [&](const Json &record)
        {
            if (record.value("type", "") != "event") return true;
            const auto &event = record.at("event");
            const uint64_t seq = std::stoull(event.at("seq").template get<std::string>());
            if (seq > after) { result.push_back(event); next = seq; }
            return result.size() < count;
        });
        return {{"session_id", id}, {"events", result}, {"next_after", std::to_string(next)}};
    }
};
}
