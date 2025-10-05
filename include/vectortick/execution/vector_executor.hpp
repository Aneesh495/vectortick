#pragma once

#include "vectortick/common/types.hpp"
#include "vectortick/common/status.hpp"
#include "vectortick/common/result.hpp"
#include "vectortick/model/event.hpp"
#include "vectortick/storage/segment_reader.hpp"
#include "vectortick/query/ast.hpp"
#include "vectortick/execution/query_result.hpp"

#include <vector>
#include <string>
#include <memory>

namespace vectortick {

struct SelectionVector {
    static constexpr usize MaxBatchSize = 1024;
    u16 indices[MaxBatchSize];
    usize count{0};

    void init_all(usize n) {
        count = std::min(n, MaxBatchSize);
        for (usize i = 0; i < count; ++i) {
            indices[i] = static_cast<u16>(i);
        }
    }
};

struct ColumnBatch {
    usize size{0};
    u64 exchange_ts_ns[SelectionVector::MaxBatchSize];
    u64 receive_ts_ns[SelectionVector::MaxBatchSize];
    u64 sequence[SelectionVector::MaxBatchSize];
    u32 instrument_id[SelectionVector::MaxBatchSize];
    u8 event_type[SelectionVector::MaxBatchSize];
    u8 side[SelectionVector::MaxBatchSize];
    u16 flags[SelectionVector::MaxBatchSize];
    i64 price_ticks[SelectionVector::MaxBatchSize];
    u32 quantity[SelectionVector::MaxBatchSize];
    u16 venue_id[SelectionVector::MaxBatchSize];
    u16 source_id[SelectionVector::MaxBatchSize];
    u64 trade_or_order_id[SelectionVector::MaxBatchSize];
};

enum class SimdArch {
    Generic,
    NEON,
    AVX2
};

class SimdDispatcher {
public:
    static SimdArch detect_host_arch() noexcept;
    static const char* arch_name(SimdArch arch) noexcept;

    // SIMD-accelerated / portable primitive kernels
    static usize filter_gt_i64(const i64* data, i64 threshold, const u16* in_sel, usize in_count, u16* out_sel) noexcept;
    static usize filter_lt_i64(const i64* data, i64 threshold, const u16* in_sel, usize in_count, u16* out_sel) noexcept;
    static usize filter_eq_u32(const u32* data, u32 val, const u16* in_sel, usize in_count, u16* out_sel) noexcept;
    static usize filter_eq_u16(const u16* data, u16 val, const u16* in_sel, usize in_count, u16* out_sel) noexcept;
    static usize filter_eq_u8(const u8* data, u8 val, const u16* in_sel, usize in_count, u16* out_sel) noexcept;

    static i64 sum_i64(const i64* data, const u16* sel, usize count) noexcept;
    static i64 min_i64(const i64* data, const u16* sel, usize count) noexcept;
    static i64 max_i64(const i64* data, const u16* sel, usize count) noexcept;

    static u64 sum_u32(const u32* data, const u16* sel, usize count) noexcept;
    static u32 min_u32(const u32* data, const u16* sel, usize count) noexcept;
    static u32 max_u32(const u32* data, const u16* sel, usize count) noexcept;
};

class VectorExecutor {
public:
    VectorExecutor();
    explicit VectorExecutor(SimdArch arch);

    SimdArch arch() const noexcept { return arch_; }

    // Execute query against a segment reader
    Result<QueryResult> execute(SegmentReader& reader,
                                const query::QueryStmt* query_ast);

    // Execute query against in-memory events
    Result<QueryResult> execute_events(const std::vector<CanonicalEvent>& events,
                                       const query::QueryStmt* query_ast);

    // Batch columnar filter evaluation
    void evaluate_filter(const ColumnBatch& batch,
                         const query::Expression* filter_expr,
                         const SelectionVector& in_sel,
                         SelectionVector& out_sel);

private:
    void load_batch(SegmentReader& reader, usize start_row, usize count, ColumnBatch& batch);
    void load_batch_from_events(const std::vector<CanonicalEvent>& events, usize start_row, usize count, ColumnBatch& batch);

    SimdArch arch_;
};

} // namespace vectortick
