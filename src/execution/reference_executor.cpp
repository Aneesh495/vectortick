#include "vectortick/execution/reference_executor.hpp"
#include "vectortick/query/type_checker.hpp"
#include "vectortick/common/checked_math.hpp"

#include <chrono>
#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <charconv>

namespace vectortick {

namespace {

inline bool ieq(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

struct EvalValue {
    bool is_signed = false;
    int64_t i_val = 0;
    uint64_t u_val = 0;

    static EvalValue make_signed(int64_t v) {
        EvalValue val;
        val.is_signed = true;
        val.i_val = v;
        val.u_val = static_cast<uint64_t>(v);
        return val;
    }

    static EvalValue make_unsigned(uint64_t v) {
        EvalValue val;
        val.is_signed = false;
        val.i_val = static_cast<int64_t>(v);
        val.u_val = v;
        return val;
    }

    [[nodiscard]] bool is_true() const noexcept {
        return is_signed ? (i_val != 0) : (u_val != 0);
    }

    [[nodiscard]] std::string to_string() const {
        return is_signed ? std::to_string(i_val) : std::to_string(u_val);
    }
};

EvalValue get_event_column_value(const CanonicalEvent& ev, vts1::ColumnID id) {
    switch (id) {
        case vts1::ColumnID::ExchangeTsNs:
            return EvalValue::make_unsigned(ev.exchange_ts_ns);
        case vts1::ColumnID::ReceiveTsNs:
            return EvalValue::make_unsigned(ev.receive_ts_ns);
        case vts1::ColumnID::Sequence:
            return EvalValue::make_unsigned(ev.sequence);
        case vts1::ColumnID::InstrumentId:
            return EvalValue::make_unsigned(ev.instrument_id);
        case vts1::ColumnID::EventType:
            return EvalValue::make_unsigned(static_cast<uint64_t>(ev.event_type));
        case vts1::ColumnID::Side:
            return EvalValue::make_unsigned(static_cast<uint64_t>(ev.side));
        case vts1::ColumnID::Flags:
            return EvalValue::make_unsigned(ev.flags);
        case vts1::ColumnID::PriceTicks:
            return EvalValue::make_signed(ev.price_ticks);
        case vts1::ColumnID::Quantity:
            return EvalValue::make_unsigned(ev.quantity);
        case vts1::ColumnID::VenueId:
            return EvalValue::make_unsigned(ev.venue_id);
        case vts1::ColumnID::SourceId:
            return EvalValue::make_unsigned(ev.source_id);
        case vts1::ColumnID::TradeOrOrderId:
            return EvalValue::make_unsigned(ev.trade_or_order_id);
        default:
            return EvalValue::make_unsigned(0);
    }
}

Result<EvalValue> evaluate_expression(const query::Expression* expr, const CanonicalEvent& ev) {
    if (!expr) {
        return EvalValue::make_unsigned(1);
    }

    switch (expr->type) {
        case query::ExprType::Literal: {
            const auto* lit = static_cast<const query::LiteralExpr*>(expr);
            return EvalValue::make_unsigned(lit->value);
        }

        case query::ExprType::ColumnRef: {
            const auto* cr = static_cast<const query::ColumnRefExpr*>(expr);
            vts1::ColumnID id;
            if (!query::TypeChecker::resolve_column(cr->name, id)) {
                return make_error<EvalValue>(StatusCode::InvalidArgument, "Unknown column: " + cr->name);
            }
            return get_event_column_value(ev, id);
        }

        case query::ExprType::UnaryOp: {
            const auto* un = static_cast<const query::UnaryOpExpr*>(expr);
            auto op_res = evaluate_expression(un->operand.get(), ev);
            if (!op_res.ok()) return op_res;
            const auto& val = op_res.value();

            if (un->op == query::TokenType::Bang) {
                return EvalValue::make_unsigned(val.is_true() ? 0 : 1);
            } else if (un->op == query::TokenType::Minus) {
                int64_t s = val.is_signed ? val.i_val : static_cast<int64_t>(val.u_val);
                auto res = checked_sub<int64_t>(0, s);
                if (!res) {
                    return make_error<EvalValue>(StatusCode::InternalError, "Integer overflow on negation");
                }
                return EvalValue::make_signed(*res);
            }
            return op_res;
        }

        case query::ExprType::BinaryOp: {
            const auto* bin = static_cast<const query::BinaryOpExpr*>(expr);
            auto left_res = evaluate_expression(bin->left.get(), ev);
            if (!left_res.ok()) return left_res;
            auto right_res = evaluate_expression(bin->right.get(), ev);
            if (!right_res.ok()) return right_res;

            const auto& l = left_res.value();
            const auto& r = right_res.value();

            // Logical operators
            if (bin->op == query::TokenType::And) {
                return EvalValue::make_unsigned((l.is_true() && r.is_true()) ? 1 : 0);
            } else if (bin->op == query::TokenType::Or) {
                return EvalValue::make_unsigned((l.is_true() || r.is_true()) ? 1 : 0);
            }

            // Comparison operators
            bool is_signed_cmp = (l.is_signed || r.is_signed);
            switch (bin->op) {
                case query::TokenType::Equal: {
                    bool eq = is_signed_cmp ? (l.i_val == r.i_val) : (l.u_val == r.u_val);
                    return EvalValue::make_unsigned(eq ? 1 : 0);
                }
                case query::TokenType::NotEqual: {
                    bool ne = is_signed_cmp ? (l.i_val != r.i_val) : (l.u_val != r.u_val);
                    return EvalValue::make_unsigned(ne ? 1 : 0);
                }
                case query::TokenType::Less: {
                    bool lt = is_signed_cmp ? (l.i_val < r.i_val) : (l.u_val < r.u_val);
                    return EvalValue::make_unsigned(lt ? 1 : 0);
                }
                case query::TokenType::LessEqual: {
                    bool le = is_signed_cmp ? (l.i_val <= r.i_val) : (l.u_val <= r.u_val);
                    return EvalValue::make_unsigned(le ? 1 : 0);
                }
                case query::TokenType::Greater: {
                    bool gt = is_signed_cmp ? (l.i_val > r.i_val) : (l.u_val > r.u_val);
                    return EvalValue::make_unsigned(gt ? 1 : 0);
                }
                case query::TokenType::GreaterEqual: {
                    bool ge = is_signed_cmp ? (l.i_val >= r.i_val) : (l.u_val >= r.u_val);
                    return EvalValue::make_unsigned(ge ? 1 : 0);
                }
                default:
                    break;
            }

            // Arithmetic operators
            if (is_signed_cmp) {
                int64_t lv = l.is_signed ? l.i_val : static_cast<int64_t>(l.u_val);
                int64_t rv = r.is_signed ? r.i_val : static_cast<int64_t>(r.u_val);

                switch (bin->op) {
                    case query::TokenType::Plus: {
                        auto res = checked_add<int64_t>(lv, rv);
                        if (!res) return make_error<EvalValue>(StatusCode::InternalError, "Signed addition overflow");
                        return EvalValue::make_signed(*res);
                    }
                    case query::TokenType::Minus: {
                        auto res = checked_sub<int64_t>(lv, rv);
                        if (!res) return make_error<EvalValue>(StatusCode::InternalError, "Signed subtraction overflow");
                        return EvalValue::make_signed(*res);
                    }
                    case query::TokenType::Star: {
                        auto res = checked_mul<int64_t>(lv, rv);
                        if (!res) return make_error<EvalValue>(StatusCode::InternalError, "Signed multiplication overflow");
                        return EvalValue::make_signed(*res);
                    }
                    case query::TokenType::Slash: {
                        if (rv == 0) return make_error<EvalValue>(StatusCode::InternalError, "Division by zero");
                        auto res = checked_div<int64_t>(lv, rv);
                        if (!res) return make_error<EvalValue>(StatusCode::InternalError, "Signed division overflow");
                        return EvalValue::make_signed(*res);
                    }
                    case query::TokenType::Percent: {
                        if (rv == 0) return make_error<EvalValue>(StatusCode::InternalError, "Modulo by zero");
                        if (lv == std::numeric_limits<int64_t>::min() && rv == -1) return EvalValue::make_signed(0);
                        return EvalValue::make_signed(lv % rv);
                    }
                    default:
                        return make_error<EvalValue>(StatusCode::NotImplemented, "Binary operator not supported");
                }
            } else {
                uint64_t lv = l.u_val;
                uint64_t rv = r.u_val;

                switch (bin->op) {
                    case query::TokenType::Plus: {
                        auto res = checked_add<uint64_t>(lv, rv);
                        if (!res) return make_error<EvalValue>(StatusCode::InternalError, "Unsigned addition overflow");
                        return EvalValue::make_unsigned(*res);
                    }
                    case query::TokenType::Minus: {
                        auto res = checked_sub<uint64_t>(lv, rv);
                        if (!res) return make_error<EvalValue>(StatusCode::InternalError, "Unsigned subtraction underflow");
                        return EvalValue::make_unsigned(*res);
                    }
                    case query::TokenType::Star: {
                        auto res = checked_mul<uint64_t>(lv, rv);
                        if (!res) return make_error<EvalValue>(StatusCode::InternalError, "Unsigned multiplication overflow");
                        return EvalValue::make_unsigned(*res);
                    }
                    case query::TokenType::Slash: {
                        if (rv == 0) return make_error<EvalValue>(StatusCode::InternalError, "Division by zero");
                        auto res = checked_div<uint64_t>(lv, rv);
                        if (!res) return make_error<EvalValue>(StatusCode::InternalError, "Unsigned division error");
                        return EvalValue::make_unsigned(*res);
                    }
                    case query::TokenType::Percent: {
                        if (rv == 0) return make_error<EvalValue>(StatusCode::InternalError, "Modulo by zero");
                        return EvalValue::make_unsigned(lv % rv);
                    }
                    default:
                        return make_error<EvalValue>(StatusCode::NotImplemented, "Binary operator not supported");
                }
            }
        }

        default:
            return EvalValue::make_unsigned(0);
    }
}

struct FieldAccumulator {
    std::string fn_name;
    const query::Expression* arg_expr = nullptr;
    bool is_signed = false;

    u64 count = 0;
    int64_t sum_i64 = 0;
    uint64_t sum_u64 = 0;
    int64_t min_i64 = std::numeric_limits<int64_t>::max();
    int64_t max_i64 = std::numeric_limits<int64_t>::min();
    uint64_t min_u64 = std::numeric_limits<uint64_t>::max();
    uint64_t max_u64 = 0;

    Status update(const CanonicalEvent& ev) {
        count++;
        if (ieq(fn_name, "COUNT")) {
            return Status::OK();
        }

        if (!arg_expr) return Status::OK();

        auto val_res = evaluate_expression(arg_expr, ev);
        if (!val_res.ok()) return val_res.status();
        const auto& val = val_res.value();

        if (is_signed) {
            auto s_res = checked_add<int64_t>(sum_i64, val.i_val);
            if (!s_res) return Status(StatusCode::InternalError, "Signed aggregate sum overflow");
            sum_i64 = *s_res;
            if (val.i_val < min_i64) min_i64 = val.i_val;
            if (val.i_val > max_i64) max_i64 = val.i_val;
        } else {
            auto s_res = checked_add<uint64_t>(sum_u64, val.u_val);
            if (!s_res) return Status(StatusCode::InternalError, "Unsigned aggregate sum overflow");
            sum_u64 = *s_res;
            if (val.u_val < min_u64) min_u64 = val.u_val;
            if (val.u_val > max_u64) max_u64 = val.u_val;
        }
        return Status::OK();
    }

    [[nodiscard]] std::string compute_result() const {
        if (ieq(fn_name, "COUNT")) {
            return std::to_string(count);
        } else if (ieq(fn_name, "SUM")) {
            return is_signed ? std::to_string(sum_i64) : std::to_string(sum_u64);
        } else if (ieq(fn_name, "MIN")) {
            if (count == 0) return "0";
            return is_signed ? std::to_string(min_i64) : std::to_string(min_u64);
        } else if (ieq(fn_name, "MAX")) {
            if (count == 0) return "0";
            return is_signed ? std::to_string(max_i64) : std::to_string(max_u64);
        } else if (ieq(fn_name, "AVG")) {
            if (count == 0) return "0";
            if (is_signed) {
                return std::to_string(sum_i64 / static_cast<int64_t>(count));
            } else {
                return std::to_string(sum_u64 / count);
            }
        }
        return "0";
    }
};

} // namespace

Result<QueryResult> ReferenceExecutor::execute(SegmentReader& reader,
                                              const query::QueryStmt* query_ast) {
    if (!reader.is_open()) {
        return make_error<QueryResult>(StatusCode::InvalidArgument, "Segment reader not open");
    }

    std::vector<CanonicalEvent> events;
    auto st = reader.read_all_events(events);
    if (!st.ok()) return make_error<QueryResult>(st.code(), st.message());

    return execute_events(events, query_ast);
}

Result<QueryResult> ReferenceExecutor::execute_events(const std::vector<CanonicalEvent>& events,
                                                     const query::QueryStmt* query_ast) {
    if (!query_ast) {
        return make_error<QueryResult>(StatusCode::InvalidArgument, "Null query statement");
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    // 1. Validate query plan
    query::TypeChecker type_checker;
    auto check_st = type_checker.check_query(query_ast);
    if (!check_st.ok()) {
        return make_error<QueryResult>(check_st.code(), check_st.message());
    }

    QueryResult result;
    result.rows_scanned = events.size();

    // Determine effective projections (supporting legacy AGG clause)
    struct ProjectionView {
        const query::Expression* expr = nullptr;
        std::string alias;
        bool is_wildcard = false;
    };
    std::vector<ProjectionView> effective_projections;
    if (!query_ast->projections.empty()) {
        for (const auto& item : query_ast->projections) {
            effective_projections.push_back({item.expr.get(), item.alias, item.is_wildcard});
        }
    } else if (!query_ast->aggregations.empty()) {
        for (const auto& agg : query_ast->aggregations) {
            effective_projections.push_back({agg.second.get(), agg.first, false});
        }
    }

    // Determine if query has aggregates
    bool has_aggregates = false;
    for (const auto& item : effective_projections) {
        if (item.expr && item.expr->type == query::ExprType::FunctionCall) {
            has_aggregates = true;
            break;
        }
    }

    // Resolve column names and types for result
    for (const auto& item : effective_projections) {
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
            auto ty = type_checker.check_expression(item.expr);
            result.column_types.push_back(ty.ok() ? ty.value() : vts1::ColumnType::U64);
        } else if (item.expr && item.expr->type == query::ExprType::ColumnRef) {
            const auto* cr = static_cast<const query::ColumnRefExpr*>(item.expr);
            result.column_names.push_back(cr->name);
            auto ty = type_checker.check_expression(item.expr);
            result.column_types.push_back(ty.ok() ? ty.value() : vts1::ColumnType::U64);
        } else if (item.expr && item.expr->type == query::ExprType::FunctionCall) {
            const auto* fc = static_cast<const query::FunctionCallExpr*>(item.expr);
            result.column_names.push_back(fc->name + "()");
            auto ty = type_checker.check_expression(item.expr);
            result.column_types.push_back(ty.ok() ? ty.value() : vts1::ColumnType::U64);
        } else {
            result.column_names.push_back("col");
            if (item.expr) {
                auto ty = type_checker.check_expression(item.expr);
                result.column_types.push_back(ty.ok() ? ty.value() : vts1::ColumnType::U64);
            } else {
                result.column_types.push_back(vts1::ColumnType::U64);
            }
        }
    }

    bool has_group_by = !query_ast->group_by_columns.empty();

    if (has_group_by) {
        // Group by execution
        // Map: group key vector of string -> accumulator states
        struct GroupState {
            std::vector<std::string> group_keys;
            std::vector<FieldAccumulator> accumulators;
        };
        std::map<std::vector<std::string>, GroupState> groups;

        for (const auto& ev : events) {
            if (query_ast->where_expr) {
                auto match_res = evaluate_expression(query_ast->where_expr.get(), ev);
                if (!match_res.ok()) return make_error<QueryResult>(match_res.status().code(), match_res.status().message());
                if (!match_res.value().is_true()) continue;
            }

            result.rows_matched++;

            // Extract group keys
            std::vector<std::string> key_vals;
            for (const auto& gcol : query_ast->group_by_columns) {
                vts1::ColumnID cid;
                if (query::TypeChecker::resolve_column(gcol, cid)) {
                    key_vals.push_back(get_event_column_value(ev, cid).to_string());
                } else {
                    key_vals.push_back("0");
                }
            }

            auto it = groups.find(key_vals);
            if (it == groups.end()) {
                GroupState state;
                state.group_keys = key_vals;
                for (const auto& item : effective_projections) {
                    if (item.expr && item.expr->type == query::ExprType::FunctionCall) {
                        const auto* fc = static_cast<const query::FunctionCallExpr*>(item.expr);
                        FieldAccumulator acc;
                        acc.fn_name = fc->name;
                        std::transform(acc.fn_name.begin(), acc.fn_name.end(), acc.fn_name.begin(), ::toupper);
                        if (!fc->args.empty()) {
                            acc.arg_expr = fc->args[0].get();
                            auto ty = type_checker.check_expression(acc.arg_expr);
                            acc.is_signed = (ty.ok() && ty.value() == vts1::ColumnType::I64);
                        }
                        state.accumulators.push_back(std::move(acc));
                    }
                }
                it = groups.emplace(key_vals, std::move(state)).first;
            }

            for (auto& acc : it->second.accumulators) {
                auto up_st = acc.update(ev);
                if (!up_st.ok()) return make_error<QueryResult>(up_st.code(), up_st.message());
            }
        }

        // Convert groups to rows
        for (const auto& [k, state] : groups) {
            std::vector<std::string> row;
            size_t acc_idx = 0;
            for (const auto& item : effective_projections) {
                if (item.expr && item.expr->type == query::ExprType::FunctionCall) {
                    if (acc_idx < state.accumulators.size()) {
                        row.push_back(state.accumulators[acc_idx++].compute_result());
                    } else {
                        row.push_back("0");
                    }
                } else if (item.expr && item.expr->type == query::ExprType::ColumnRef) {
                    const auto* cr = static_cast<const query::ColumnRefExpr*>(item.expr);
                    // Find in group_by_columns
                    bool found = false;
                    for (size_t gi = 0; gi < query_ast->group_by_columns.size(); ++gi) {
                        if (ieq(query_ast->group_by_columns[gi], cr->name)) {
                            row.push_back(state.group_keys[gi]);
                            found = true;
                            break;
                        }
                    }
                    if (!found) row.push_back("0");
                } else {
                    row.push_back("0");
                }
            }
            result.rows.push_back(std::move(row));
        }

    } else if (has_aggregates) {
        // Scalar aggregation (no group by)
        std::vector<FieldAccumulator> accumulators;
        for (const auto& item : effective_projections) {
            if (item.expr && item.expr->type == query::ExprType::FunctionCall) {
                const auto* fc = static_cast<const query::FunctionCallExpr*>(item.expr);
                FieldAccumulator acc;
                acc.fn_name = fc->name;
                std::transform(acc.fn_name.begin(), acc.fn_name.end(), acc.fn_name.begin(), ::toupper);
                if (!fc->args.empty()) {
                    acc.arg_expr = fc->args[0].get();
                    auto ty = type_checker.check_expression(acc.arg_expr);
                    acc.is_signed = (ty.ok() && ty.value() == vts1::ColumnType::I64);
                }
                accumulators.push_back(std::move(acc));
            }
        }

        for (const auto& ev : events) {
            if (query_ast->where_expr) {
                auto match_res = evaluate_expression(query_ast->where_expr.get(), ev);
                if (!match_res.ok()) return make_error<QueryResult>(match_res.status().code(), match_res.status().message());
                if (!match_res.value().is_true()) continue;
            }

            result.rows_matched++;

            for (auto& acc : accumulators) {
                auto up_st = acc.update(ev);
                if (!up_st.ok()) return make_error<QueryResult>(up_st.code(), up_st.message());
            }
        }

        std::vector<std::string> agg_row;
        size_t acc_idx = 0;
        for (const auto& item : effective_projections) {
            if (item.expr && item.expr->type == query::ExprType::FunctionCall) {
                if (acc_idx < accumulators.size()) {
                    agg_row.push_back(accumulators[acc_idx++].compute_result());
                } else {
                    agg_row.push_back("0");
                }
            } else {
                agg_row.push_back("0");
            }
        }
        result.rows.push_back(std::move(agg_row));

    } else {
        // Plain projection query
        for (const auto& ev : events) {
            if (query_ast->where_expr) {
                auto match_res = evaluate_expression(query_ast->where_expr.get(), ev);
                if (!match_res.ok()) return make_error<QueryResult>(match_res.status().code(), match_res.status().message());
                if (!match_res.value().is_true()) continue;
            }

            result.rows_matched++;

            std::vector<std::string> row;
            for (const auto& item : effective_projections) {
                if (item.is_wildcard) {
                    row.push_back(std::to_string(ev.sequence));
                    row.push_back(std::to_string(ev.instrument_id));
                    row.push_back(std::to_string(ev.price_ticks));
                    row.push_back(std::to_string(ev.quantity));
                } else if (item.expr) {
                    auto val_res = evaluate_expression(item.expr, ev);
                    if (!val_res.ok()) return make_error<QueryResult>(val_res.status().code(), val_res.status().message());
                    row.push_back(val_res.value().to_string());
                } else {
                    row.push_back("0");
                }
            }
            if (query_ast->limit == 0 || result.rows.size() < query_ast->limit || !query_ast->order_by.empty()) {
                result.rows.push_back(std::move(row));
            }
        }
    }

    // 2. ORDER BY handling
    if (!query_ast->order_by.empty() && !result.rows.empty()) {
        struct OrderCol {
            size_t col_idx = 0;
            bool ascending = true;
            bool is_signed = false;
        };
        std::vector<OrderCol> order_cols;

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

    // 3. LIMIT handling
    if (query_ast->limit > 0 && result.rows.size() > query_ast->limit) {
        result.rows.resize(query_ast->limit);
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    result.execution_time_ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());

    return Result<QueryResult>(std::move(result));
}

} // namespace vectortick
