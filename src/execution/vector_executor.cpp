#include "vectortick/execution/vector_executor.hpp"
#include "vectortick/query/type_checker.hpp"
#include <chrono>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <charconv>
#include <cctype>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#elif (defined(__x86_64__) || defined(_M_X64)) && defined(__AVX2__)
#include <immintrin.h>
#endif

namespace vectortick {

// ---------------------------------------------------------------------------
// SimdDispatcher implementation
// ---------------------------------------------------------------------------

SimdArch SimdDispatcher::detect_host_arch() noexcept {
#if defined(__ARM_NEON)
    return SimdArch::NEON;
#elif defined(__x86_64__) || defined(_M_X64)
    #if defined(__has_builtin)
        #if __has_builtin(__builtin_cpu_supports)
            if (__builtin_cpu_supports("avx2")) {
                return SimdArch::AVX2;
            }
        #endif
    #endif
    return SimdArch::Generic;
#else
    return SimdArch::Generic;
#endif
}

const char* SimdDispatcher::arch_name(SimdArch arch) noexcept {
    switch (arch) {
        case SimdArch::NEON: return "AArch64 NEON";
        case SimdArch::AVX2: return "x86-64 AVX2";
        case SimdArch::Generic: return "Portable Scalar";
    }
    return "Unknown";
}

usize SimdDispatcher::filter_gt_i64(const i64* data, i64 threshold, const u16* in_sel, usize in_count, u16* out_sel) noexcept {
    usize out_count = 0;
#if defined(__ARM_NEON)
    if (in_sel == nullptr) {
        int64x2_t vthresh = vdupq_n_s64(threshold);
        usize i = 0;
        for (; i + 2 <= in_count; i += 2) {
            int64x2_t vval = vld1q_s64(data + i);
            uint64x2_t vcmp = vcgtq_s64(vval, vthresh);
            if (vgetq_lane_u64(vcmp, 0) != 0) out_sel[out_count++] = static_cast<u16>(i);
            if (vgetq_lane_u64(vcmp, 1) != 0) out_sel[out_count++] = static_cast<u16>(i + 1);
        }
        for (; i < in_count; ++i) {
            if (data[i] > threshold) out_sel[out_count++] = static_cast<u16>(i);
        }
        return out_count;
    }
#endif

    for (usize i = 0; i < in_count; ++i) {
        u16 idx = in_sel ? in_sel[i] : static_cast<u16>(i);
        if (data[idx] > threshold) {
            out_sel[out_count++] = idx;
        }
    }
    return out_count;
}

usize SimdDispatcher::filter_lt_i64(const i64* data, i64 threshold, const u16* in_sel, usize in_count, u16* out_sel) noexcept {
    usize out_count = 0;
#if defined(__ARM_NEON)
    if (in_sel == nullptr) {
        int64x2_t vthresh = vdupq_n_s64(threshold);
        usize i = 0;
        for (; i + 2 <= in_count; i += 2) {
            int64x2_t vval = vld1q_s64(data + i);
            uint64x2_t vcmp = vcltq_s64(vval, vthresh);
            if (vgetq_lane_u64(vcmp, 0) != 0) out_sel[out_count++] = static_cast<u16>(i);
            if (vgetq_lane_u64(vcmp, 1) != 0) out_sel[out_count++] = static_cast<u16>(i + 1);
        }
        for (; i < in_count; ++i) {
            if (data[i] < threshold) out_sel[out_count++] = static_cast<u16>(i);
        }
        return out_count;
    }
#endif

    for (usize i = 0; i < in_count; ++i) {
        u16 idx = in_sel ? in_sel[i] : static_cast<u16>(i);
        if (data[idx] < threshold) {
            out_sel[out_count++] = idx;
        }
    }
    return out_count;
}

usize SimdDispatcher::filter_eq_u32(const u32* data, u32 val, const u16* in_sel, usize in_count, u16* out_sel) noexcept {
    usize out_count = 0;
#if defined(__ARM_NEON)
    if (in_sel == nullptr) {
        uint32x4_t vval = vdupq_n_u32(val);
        usize i = 0;
        for (; i + 4 <= in_count; i += 4) {
            uint32x4_t d = vld1q_u32(data + i);
            uint32x4_t vcmp = vceqq_u32(d, vval);
            if (vgetq_lane_u32(vcmp, 0) != 0) out_sel[out_count++] = static_cast<u16>(i);
            if (vgetq_lane_u32(vcmp, 1) != 0) out_sel[out_count++] = static_cast<u16>(i + 1);
            if (vgetq_lane_u32(vcmp, 2) != 0) out_sel[out_count++] = static_cast<u16>(i + 2);
            if (vgetq_lane_u32(vcmp, 3) != 0) out_sel[out_count++] = static_cast<u16>(i + 3);
        }
        for (; i < in_count; ++i) {
            if (data[i] == val) out_sel[out_count++] = static_cast<u16>(i);
        }
        return out_count;
    }
#endif

    for (usize i = 0; i < in_count; ++i) {
        u16 idx = in_sel ? in_sel[i] : static_cast<u16>(i);
        if (data[idx] == val) {
            out_sel[out_count++] = idx;
        }
    }
    return out_count;
}

usize SimdDispatcher::filter_eq_u16(const u16* data, u16 val, const u16* in_sel, usize in_count, u16* out_sel) noexcept {
    usize out_count = 0;
    for (usize i = 0; i < in_count; ++i) {
        u16 idx = in_sel ? in_sel[i] : static_cast<u16>(i);
        if (data[idx] == val) {
            out_sel[out_count++] = idx;
        }
    }
    return out_count;
}

usize SimdDispatcher::filter_eq_u8(const u8* data, u8 val, const u16* in_sel, usize in_count, u16* out_sel) noexcept {
    usize out_count = 0;
    for (usize i = 0; i < in_count; ++i) {
        u16 idx = in_sel ? in_sel[i] : static_cast<u16>(i);
        if (data[idx] == val) {
            out_sel[out_count++] = idx;
        }
    }
    return out_count;
}

i64 SimdDispatcher::sum_i64(const i64* data, const u16* sel, usize count) noexcept {
    if (count == 0) return 0;
#if defined(__ARM_NEON)
    if (sel == nullptr) {
        int64x2_t vsum0 = vdupq_n_s64(0);
        int64x2_t vsum1 = vdupq_n_s64(0);
        usize i = 0;
        for (; i + 4 <= count; i += 4) {
            vsum0 = vaddq_s64(vsum0, vld1q_s64(data + i));
            vsum1 = vaddq_s64(vsum1, vld1q_s64(data + i + 2));
        }
        int64x2_t vsum = vaddq_s64(vsum0, vsum1);
        i64 total = vgetq_lane_s64(vsum, 0) + vgetq_lane_s64(vsum, 1);
        for (; i < count; ++i) total += data[i];
        return total;
    }
#endif

    i64 total = 0;
    for (usize i = 0; i < count; ++i) {
        u16 idx = sel ? sel[i] : static_cast<u16>(i);
        total += data[idx];
    }
    return total;
}

i64 SimdDispatcher::min_i64(const i64* data, const u16* sel, usize count) noexcept {
    if (count == 0) return 0;
    i64 res = sel ? data[sel[0]] : data[0];
    for (usize i = 1; i < count; ++i) {
        u16 idx = sel ? sel[i] : static_cast<u16>(i);
        if (data[idx] < res) res = data[idx];
    }
    return res;
}

i64 SimdDispatcher::max_i64(const i64* data, const u16* sel, usize count) noexcept {
    if (count == 0) return 0;
    i64 res = sel ? data[sel[0]] : data[0];
    for (usize i = 1; i < count; ++i) {
        u16 idx = sel ? sel[i] : static_cast<u16>(i);
        if (data[idx] > res) res = data[idx];
    }
    return res;
}

u64 SimdDispatcher::sum_u32(const u32* data, const u16* sel, usize count) noexcept {
    u64 total = 0;
    for (usize i = 0; i < count; ++i) {
        u16 idx = sel ? sel[i] : static_cast<u16>(i);
        total += data[idx];
    }
    return total;
}

u32 SimdDispatcher::min_u32(const u32* data, const u16* sel, usize count) noexcept {
    if (count == 0) return 0;
    u32 res = sel ? data[sel[0]] : data[0];
    for (usize i = 1; i < count; ++i) {
        u16 idx = sel ? sel[i] : static_cast<u16>(i);
        if (data[idx] < res) res = data[idx];
    }
    return res;
}

u32 SimdDispatcher::max_u32(const u32* data, const u16* sel, usize count) noexcept {
    if (count == 0) return 0;
    u32 res = sel ? data[sel[0]] : data[0];
    for (usize i = 1; i < count; ++i) {
        u16 idx = sel ? sel[i] : static_cast<u16>(i);
        if (data[idx] > res) res = data[idx];
    }
    return res;
}

// ---------------------------------------------------------------------------
// VectorExecutor implementation
// ---------------------------------------------------------------------------

VectorExecutor::VectorExecutor()
    : arch_(SimdDispatcher::detect_host_arch()) {}

VectorExecutor::VectorExecutor(SimdArch arch)
    : arch_(arch) {}

void VectorExecutor::load_batch(SegmentReader& reader, usize start_row, usize count, ColumnBatch& batch) {
    batch.size = count;
    (void)reader.read_column_u64(vts1::ColumnID::ExchangeTsNs, batch.exchange_ts_ns + start_row, count);
    (void)reader.read_column_u64(vts1::ColumnID::ReceiveTsNs, batch.receive_ts_ns + start_row, count);
    (void)reader.read_column_u64(vts1::ColumnID::Sequence, batch.sequence + start_row, count);
    (void)reader.read_column_u32(vts1::ColumnID::InstrumentId, batch.instrument_id + start_row, count);
    (void)reader.read_column_u8(vts1::ColumnID::EventType, batch.event_type + start_row, count);
    (void)reader.read_column_u8(vts1::ColumnID::Side, batch.side + start_row, count);
    (void)reader.read_column_u16(vts1::ColumnID::Flags, batch.flags + start_row, count);
    (void)reader.read_column_i64(vts1::ColumnID::PriceTicks, batch.price_ticks + start_row, count);
    (void)reader.read_column_u32(vts1::ColumnID::Quantity, batch.quantity + start_row, count);
    (void)reader.read_column_u16(vts1::ColumnID::VenueId, batch.venue_id + start_row, count);
    (void)reader.read_column_u16(vts1::ColumnID::SourceId, batch.source_id + start_row, count);
    (void)reader.read_column_u64(vts1::ColumnID::TradeOrOrderId, batch.trade_or_order_id + start_row, count);
}

void VectorExecutor::load_batch_from_events(const std::vector<CanonicalEvent>& events,
                                           usize start_row, usize count, ColumnBatch& batch) {
    batch.size = count;
    for (usize i = 0; i < count; ++i) {
        const auto& ev = events[start_row + i];
        batch.exchange_ts_ns[i] = ev.exchange_ts_ns;
        batch.receive_ts_ns[i] = ev.receive_ts_ns;
        batch.sequence[i] = ev.sequence;
        batch.instrument_id[i] = ev.instrument_id;
        batch.event_type[i] = static_cast<u8>(ev.event_type);
        batch.side[i] = static_cast<u8>(ev.side);
        batch.flags[i] = ev.flags;
        batch.price_ticks[i] = ev.price_ticks;
        batch.quantity[i] = ev.quantity;
        batch.venue_id[i] = ev.venue_id;
        batch.source_id[i] = ev.source_id;
        batch.trade_or_order_id[i] = ev.trade_or_order_id;
    }
}

void VectorExecutor::evaluate_filter(const ColumnBatch& batch,
                                    const query::Expression* filter_expr,
                                    const SelectionVector& in_sel,
                                    SelectionVector& out_sel) {
    if (!filter_expr || filter_expr->type != query::ExprType::BinaryOp) {
        out_sel = in_sel;
        return;
    }

    const auto* bin_expr = static_cast<const query::BinaryOpExpr*>(filter_expr);
    if (!bin_expr->left || !bin_expr->right) {
        out_sel = in_sel;
        return;
    }

    if (bin_expr->left->type != query::ExprType::ColumnRef ||
        bin_expr->right->type != query::ExprType::Literal) {
        out_sel = in_sel;
        return;
    }

    const auto* left_id = static_cast<const query::ColumnRefExpr*>(bin_expr->left.get());
    const auto* right_lit = static_cast<const query::LiteralExpr*>(bin_expr->right.get());

    const std::string& col = left_id->name;
    query::TokenType op = bin_expr->op;
    out_sel.count = 0;

    i64 lit_int = static_cast<i64>(right_lit->value);

    if (col == "price_ticks") {
        if (op == query::TokenType::Greater) {
            out_sel.count = SimdDispatcher::filter_gt_i64(batch.price_ticks, lit_int, in_sel.indices, in_sel.count, out_sel.indices);
        } else if (op == query::TokenType::Less) {
            out_sel.count = SimdDispatcher::filter_lt_i64(batch.price_ticks, lit_int, in_sel.indices, in_sel.count, out_sel.indices);
        } else {
            for (usize i = 0; i < in_sel.count; ++i) {
                u16 idx = in_sel.indices[i];
                bool match = false;
                if (op == query::TokenType::Equal) match = (batch.price_ticks[idx] == lit_int);
                else if (op == query::TokenType::NotEqual) match = (batch.price_ticks[idx] != lit_int);
                else if (op == query::TokenType::GreaterEqual) match = (batch.price_ticks[idx] >= lit_int);
                else if (op == query::TokenType::LessEqual) match = (batch.price_ticks[idx] <= lit_int);
                if (match) out_sel.indices[out_sel.count++] = idx;
            }
        }
    } else if (col == "instrument_id") {
        u32 target = static_cast<u32>(lit_int);
        if (op == query::TokenType::Equal) {
            out_sel.count = SimdDispatcher::filter_eq_u32(batch.instrument_id, target, in_sel.indices, in_sel.count, out_sel.indices);
        } else {
            for (usize i = 0; i < in_sel.count; ++i) {
                u16 idx = in_sel.indices[i];
                bool match = false;
                if (op == query::TokenType::NotEqual) match = (batch.instrument_id[idx] != target);
                else if (op == query::TokenType::Greater) match = (batch.instrument_id[idx] > target);
                else if (op == query::TokenType::Less) match = (batch.instrument_id[idx] < target);
                if (match) out_sel.indices[out_sel.count++] = idx;
            }
        }
    } else if (col == "quantity") {
        u32 target = static_cast<u32>(lit_int);
        for (usize i = 0; i < in_sel.count; ++i) {
            u16 idx = in_sel.indices[i];
            bool match = false;
            if (op == query::TokenType::Equal) match = (batch.quantity[idx] == target);
            else if (op == query::TokenType::NotEqual) match = (batch.quantity[idx] != target);
            else if (op == query::TokenType::Greater) match = (batch.quantity[idx] > target);
            else if (op == query::TokenType::GreaterEqual) match = (batch.quantity[idx] >= target);
            else if (op == query::TokenType::Less) match = (batch.quantity[idx] < target);
            else if (op == query::TokenType::LessEqual) match = (batch.quantity[idx] <= target);
            if (match) out_sel.indices[out_sel.count++] = idx;
        }
    } else if (col == "venue_id") {
        out_sel.count = SimdDispatcher::filter_eq_u16(batch.venue_id, static_cast<u16>(lit_int), in_sel.indices, in_sel.count, out_sel.indices);
    } else {
        out_sel = in_sel;
    }
}

Result<QueryResult> VectorExecutor::execute(SegmentReader& reader,
                                           const query::QueryStmt* query_ast) {
    if (!reader.is_open()) {
        return Result<QueryResult>(Status(StatusCode::InvalidArgument, "Segment reader not open"));
    }

    std::vector<CanonicalEvent> events;
    auto st = reader.read_all_events(events);
    if (!st.ok()) return Result<QueryResult>(st);

    return execute_events(events, query_ast);
}

Result<QueryResult> VectorExecutor::execute_events(const std::vector<CanonicalEvent>& events,
                                                  const query::QueryStmt* query_ast) {
    auto start_time = std::chrono::high_resolution_clock::now();

    QueryResult result;
    result.rows_scanned = events.size();

    // Check for aggregates
    bool has_aggregates = false;
    for (const auto& item : query_ast->projections) {
        if (item.expr && item.expr->type == query::ExprType::FunctionCall) {
            has_aggregates = true;
            break;
        }
    }

    query::TypeChecker type_checker;
    for (const auto& item : query_ast->projections) {
        if (item.is_wildcard) {
            result.column_names.push_back("sequence");
            result.column_types.push_back(vts1::ColumnType::U64);
            result.column_names.push_back("instrument_id");
            result.column_types.push_back(vts1::ColumnType::U32);
            result.column_names.push_back("price_ticks");
            result.column_types.push_back(vts1::ColumnType::I64);
            result.column_names.push_back("quantity");
            result.column_types.push_back(vts1::ColumnType::U32);
        } else if (!item.alias.empty()) {
            result.column_names.push_back(item.alias);
            auto ty = type_checker.check_expression(item.expr.get());
            result.column_types.push_back(ty.ok() ? ty.value() : vts1::ColumnType::U64);
        } else if (item.expr && item.expr->type == query::ExprType::ColumnRef) {
            const auto* cr = static_cast<const query::ColumnRefExpr*>(item.expr.get());
            result.column_names.push_back(cr->name);
            auto ty = type_checker.check_expression(item.expr.get());
            result.column_types.push_back(ty.ok() ? ty.value() : vts1::ColumnType::U64);
        } else if (item.expr && item.expr->type == query::ExprType::FunctionCall) {
            const auto* fc = static_cast<const query::FunctionCallExpr*>(item.expr.get());
            result.column_names.push_back(fc->name + "()");
            auto ty = type_checker.check_expression(item.expr.get());
            result.column_types.push_back(ty.ok() ? ty.value() : vts1::ColumnType::U64);
        } else {
            result.column_names.push_back("col");
            if (item.expr) {
                auto ty = type_checker.check_expression(item.expr.get());
                result.column_types.push_back(ty.ok() ? ty.value() : vts1::ColumnType::U64);
            } else {
                result.column_types.push_back(vts1::ColumnType::U64);
            }
        }
    }

    const query::Expression* filter_expr = query_ast->where_expr ? query_ast->where_expr.get() : nullptr;

    const usize total_rows = events.size();
    usize row_offset = 0;

    // Aggregation state
    u64 agg_count = 0;
    i64 agg_sum_price = 0;
    i64 agg_min_price = 0x7FFFFFFFFFFFFFFFLL;
    i64 agg_max_price = -0x7FFFFFFFFFFFFFFFLL;
    u64 agg_sum_qty = 0;
    u32 agg_min_qty = 0xFFFFFFFFU;
    u32 agg_max_qty = 0;

    ColumnBatch batch;
    SelectionVector in_sel;
    SelectionVector out_sel;

    while (row_offset < total_rows) {
        usize cur_batch_size = std::min<usize>(SelectionVector::MaxBatchSize, total_rows - row_offset);
        load_batch_from_events(events, row_offset, cur_batch_size, batch);

        in_sel.init_all(cur_batch_size);

        if (filter_expr) {
            evaluate_filter(batch, filter_expr, in_sel, out_sel);
        } else {
            out_sel = in_sel;
        }

        result.rows_matched += out_sel.count;

        if (has_aggregates) {
            agg_count += out_sel.count;
            if (out_sel.count > 0) {
                agg_sum_price += SimdDispatcher::sum_i64(batch.price_ticks, out_sel.indices, out_sel.count);
                i64 b_min_p = SimdDispatcher::min_i64(batch.price_ticks, out_sel.indices, out_sel.count);
                i64 b_max_p = SimdDispatcher::max_i64(batch.price_ticks, out_sel.indices, out_sel.count);
                if (b_min_p < agg_min_price) agg_min_price = b_min_p;
                if (b_max_p > agg_max_price) agg_max_price = b_max_p;

                agg_sum_qty += SimdDispatcher::sum_u32(batch.quantity, out_sel.indices, out_sel.count);
                u32 b_min_q = SimdDispatcher::min_u32(batch.quantity, out_sel.indices, out_sel.count);
                u32 b_max_q = SimdDispatcher::max_u32(batch.quantity, out_sel.indices, out_sel.count);
                if (b_min_q < agg_min_qty) agg_min_qty = b_min_q;
                if (b_max_q > agg_max_qty) agg_max_qty = b_max_q;
            }
        } else {
            // Collect rows for projections
            for (usize i = 0; i < out_sel.count; ++i) {
                u16 idx = out_sel.indices[i];
                std::vector<std::string> row;
                for (const auto& item : query_ast->projections) {
                    if (item.expr && item.expr->type == query::ExprType::ColumnRef) {
                        const auto* cr = static_cast<const query::ColumnRefExpr*>(item.expr.get());
                        if (cr->name == "instrument_id") row.push_back(std::to_string(batch.instrument_id[idx]));
                        else if (cr->name == "price_ticks") row.push_back(std::to_string(batch.price_ticks[idx]));
                        else if (cr->name == "quantity") row.push_back(std::to_string(batch.quantity[idx]));
                        else if (cr->name == "sequence") row.push_back(std::to_string(batch.sequence[idx]));
                        else if (cr->name == "exchange_ts_ns") row.push_back(std::to_string(batch.exchange_ts_ns[idx]));
                        else if (cr->name == "receive_ts_ns") row.push_back(std::to_string(batch.receive_ts_ns[idx]));
                        else if (cr->name == "venue_id") row.push_back(std::to_string(batch.venue_id[idx]));
                        else if (cr->name == "source_id") row.push_back(std::to_string(batch.source_id[idx]));
                        else if (cr->name == "trade_or_order_id") row.push_back(std::to_string(batch.trade_or_order_id[idx]));
                        else row.push_back("0");
                    } else if (item.is_wildcard) {
                        row.push_back(std::to_string(batch.sequence[idx]));
                        row.push_back(std::to_string(batch.instrument_id[idx]));
                        row.push_back(std::to_string(batch.price_ticks[idx]));
                        row.push_back(std::to_string(batch.quantity[idx]));
                    } else {
                        row.push_back("0");
                    }
                }
                result.rows.push_back(std::move(row));

                if (query_ast->order_by.empty() && query_ast->limit > 0 && result.rows.size() >= query_ast->limit) {
                    break;
                }
            }
            if (query_ast->order_by.empty() && query_ast->limit > 0 && result.rows.size() >= query_ast->limit) {
                break;
            }
        }

        row_offset += cur_batch_size;
    }

    if (has_aggregates) {
        std::vector<std::string> agg_row;
        for (const auto& item : query_ast->projections) {
            if (item.expr && item.expr->type == query::ExprType::FunctionCall) {
                const auto* call = static_cast<const query::FunctionCallExpr*>(item.expr.get());
                std::string fn = call->name;
                std::transform(fn.begin(), fn.end(), fn.begin(), ::toupper);
                std::string arg = "";
                if (!call->args.empty() && call->args[0]->type == query::ExprType::ColumnRef) {
                    const auto* aid = static_cast<const query::ColumnRefExpr*>(call->args[0].get());
                    arg = aid->name;
                }

                if (fn == "COUNT") {
                    agg_row.push_back(std::to_string(agg_count));
                } else if (fn == "SUM") {
                    if (arg == "quantity") agg_row.push_back(std::to_string(agg_sum_qty));
                    else agg_row.push_back(std::to_string(agg_sum_price));
                } else if (fn == "MIN") {
                    if (arg == "quantity") agg_row.push_back(std::to_string(agg_count > 0 ? agg_min_qty : 0));
                    else agg_row.push_back(std::to_string(agg_count > 0 ? agg_min_price : 0));
                } else if (fn == "MAX") {
                    if (arg == "quantity") agg_row.push_back(std::to_string(agg_count > 0 ? agg_max_qty : 0));
                    else agg_row.push_back(std::to_string(agg_count > 0 ? agg_max_price : 0));
                } else if (fn == "AVG") {
                    if (agg_count == 0) agg_row.push_back("0");
                    else if (arg == "quantity") agg_row.push_back(std::to_string(agg_sum_qty / agg_count));
                    else agg_row.push_back(std::to_string(agg_sum_price / agg_count));
                } else {
                    agg_row.push_back("0");
                }
            } else {
                agg_row.push_back("0");
            }
        }
        result.rows.push_back(std::move(agg_row));
    }
 
    if (!query_ast->order_by.empty() && !result.rows.empty()) {
        struct OrderCol {
            size_t col_idx = 0;
            bool ascending = true;
            bool is_signed = false;
        };
        std::vector<OrderCol> order_cols;

        auto ieq = [](std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (std::toupper(static_cast<unsigned char>(a[i])) !=
                    std::toupper(static_cast<unsigned char>(b[i]))) return false;
            }
            return true;
        };

        for (const auto& [col_name, ascending] : query_ast->order_by) {
            for (size_t i = 0; i < result.column_names.size(); ++i) {
                if (ieq(result.column_names[i], col_name)) {
                    bool is_signed = (i < result.column_types.size() &&
                                      result.column_types[i] == vts1::ColumnType::I64);
                    order_cols.push_back({i, ascending, is_signed});
                    break;
                }
            }
        }

        if (!order_cols.empty()) {
            std::stable_sort(result.rows.begin(), result.rows.end(), [&](const std::vector<std::string>& a, const std::vector<std::string>& b) {
                for (const auto& oc : order_cols) {
                    if (oc.col_idx >= a.size() || oc.col_idx >= b.size()) continue;
                    const auto& sa = a[oc.col_idx];
                    const auto& sb = b[oc.col_idx];
                    if (sa == sb) continue;

                    if (oc.is_signed) {
                        int64_t va = 0, vb = 0;
                        std::from_chars(sa.data(), sa.data() + sa.size(), va);
                        std::from_chars(sb.data(), sb.data() + sb.size(), vb);
                        return oc.ascending ? (va < vb) : (va > vb);
                    } else {
                        uint64_t va = 0, vb = 0;
                        std::from_chars(sa.data(), sa.data() + sa.size(), va);
                        std::from_chars(sb.data(), sb.data() + sb.size(), vb);
                        return oc.ascending ? (va < vb) : (va > vb);
                    }
                }
                return false;
            });
        }
    }

    if (query_ast->limit > 0 && result.rows.size() > query_ast->limit) {
        result.rows.resize(query_ast->limit);
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    result.execution_time_ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());

    return Result<QueryResult>(std::move(result));
}

} // namespace vectortick
