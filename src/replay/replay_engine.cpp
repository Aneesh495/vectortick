#include "vectortick/replay/replay_engine.hpp"

#include <chrono>
#include <thread>
#include <algorithm>

namespace vectortick {

ReplayEngine::ReplayEngine(ReplayOptions options)
    : options_(options) {
    reset_stats();
}

void ReplayEngine::reset_stats() {
    stats_ = ReplayStatistics{};
    last_sequence_ = 0;
    last_receive_ts_ = 0;
    first_event_ = true;
}

void ReplayEngine::process_event(const CanonicalEvent& ev, EventCallback callback) {
    if (first_event_) {
        stats_.min_sequence = ev.sequence;
        stats_.max_sequence = ev.sequence;
        last_sequence_ = ev.sequence;
        last_receive_ts_ = ev.receive_ts_ns;
        first_event_ = false;
    } else {
        if (ev.sequence < stats_.min_sequence) stats_.min_sequence = ev.sequence;
        if (ev.sequence > stats_.max_sequence) stats_.max_sequence = ev.sequence;

        if (options_.verify_sequence) {
            if (ev.sequence <= last_sequence_) {
                stats_.out_of_order_events++;
            } else if (ev.sequence > last_sequence_ && ev.sequence - last_sequence_ > 1) {
                stats_.sequence_breaks++;
            }
        }

        if (options_.preserve_relative_timing && ev.receive_ts_ns > last_receive_ts_) {
            u64 delta_ns = ev.receive_ts_ns - last_receive_ts_;
            if (options_.time_dilation > 0.0) {
                delta_ns = static_cast<u64>(static_cast<double>(delta_ns) / options_.time_dilation);
            }
            if (delta_ns > 0 && delta_ns < 10'000'000'000ULL) { // Max 10s sleep per event
                std::this_thread::sleep_for(std::chrono::nanoseconds(delta_ns));
            }
        }

        last_sequence_ = ev.sequence;
        last_receive_ts_ = ev.receive_ts_ns;
    }

    stats_.total_events++;

    if (callback) {
        callback(ev);
    }
}

Status ReplayEngine::replay_events(const std::vector<CanonicalEvent>& events,
                                   EventCallback callback) {
    if (events.empty()) {
        return Status::OK();
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    const u64 initial_count = stats_.total_events;
    const u64 rate = options_.rate_limit_events_per_sec;
    const u64 batch_size = rate > 0 ? std::max<u64>(1, rate / 100) : 1000;
    u64 batch_count = 0;

    for (const auto& ev : events) {
        process_event(ev, callback);
        batch_count++;

        if (rate > 0 && batch_count >= batch_size) {
            auto now = std::chrono::high_resolution_clock::now();
            double elapsed_sec = std::chrono::duration<double>(now - start_time).count();
            double expected_sec = static_cast<double>(stats_.total_events - initial_count) / static_cast<double>(rate);

            if (expected_sec > elapsed_sec) {
                auto sleep_duration = std::chrono::duration<double>(expected_sec - elapsed_sec);
                std::this_thread::sleep_for(sleep_duration);
            }
            batch_count = 0;
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    stats_.elapsed_wall_ns += static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());

    double elapsed_sec = static_cast<double>(stats_.elapsed_wall_ns) / 1e9;
    if (elapsed_sec > 0.0) {
        stats_.throughput_events_per_sec = static_cast<double>(stats_.total_events) / elapsed_sec;
    }

    return Status::OK();
}

Status ReplayEngine::replay_segment(const std::string& segment_path,
                                    EventCallback callback) {
    SegmentReader reader;
    auto status = reader.open(segment_path);
    if (!status.ok()) return status;

    std::vector<CanonicalEvent> events;
    status = reader.read_all_events(events);
    if (!status.ok()) return status;

    return replay_events(events, callback);
}

Status ReplayEngine::replay_catalog(const Catalog& catalog,
                                    EventCallback callback) {
    auto segments = catalog.active_segments();
    std::sort(segments.begin(), segments.end(), [](const SegmentMeta& a, const SegmentMeta& b) {
        if (a.min_seq != b.min_seq) return a.min_seq < b.min_seq;
        return a.min_exchange_ts < b.min_exchange_ts;
    });

    for (const auto& seg : segments) {
        std::string path = catalog.root_dir() + "/" + seg.filename;
        auto st = replay_segment(path, callback);
        if (!st.ok()) return st;
    }

    return Status::OK();
}

} // namespace vectortick
