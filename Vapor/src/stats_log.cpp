#include "Vapor/stats_log.hpp"

#include <fmt/core.h>

namespace Vapor {

void StatLine::appendKV(const char* key, const std::string& value) {
    if (!buffer_.empty()) buffer_ += ' ';
    buffer_ += key;
    buffer_ += '=';
    buffer_ += value;
}

void StatLine::add(const char* key, double value) {
    appendKV(key, fmt::format("{:.3g}", value));
}

void StatLine::add(const char* key, const char* value) {
    appendKV(key, value);
}

StatsLog& StatsLog::get() {
    static StatsLog instance;
    return instance;
}

StatsLog::~StatsLog() {
    if (file_) std::fclose(file_);
}

void StatsLog::addSource(const char* tag, std::function<void(StatLine&)> fill, Mode mode) {
    sources_.push_back(Source{ tag, std::move(fill), mode, {} });
}

void StatsLog::ensureFileOpen() {
    if (!file_ && !filePath_.empty()) {
        file_ = std::fopen(filePath_.c_str(), "w");  // truncate at first emit
    }
}

void StatsLog::tick(std::uint64_t frame) {
    if (!enabled_ || sources_.empty()) return;

    // Periodic sources fire on the interval; OnChange sources are polled every
    // frame so a transition is never missed.
    const bool periodicDue = (interval_ == 0) || (frame % interval_ == 0);
    bool opened = false;

    for (auto& s : sources_) {
        if (s.mode == Mode::Periodic && !periodicDue) continue;

        StatLine line;
        s.fill(line);
        if (line.empty()) continue;

        if (s.mode == Mode::OnChange) {
            if (line.str() == s.last) continue;  // unchanged: stay quiet
            s.last = line.str();
        }

        const std::string out = fmt::format("[{}] f={} {}\n", s.tag, frame, line.str());
        std::fputs(out.c_str(), stderr);
        if (!opened) { ensureFileOpen(); opened = true; }
        if (file_) { std::fputs(out.c_str(), file_); std::fflush(file_); }
    }
}

} // namespace Vapor
