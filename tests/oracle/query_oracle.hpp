#pragma once

#include "vectortick/common/types.hpp"
#include "vectortick/common/status.hpp"
#include "vectortick/common/result.hpp"
#include "vectortick/model/event.hpp"
#include "vectortick/query/ast.hpp"
#include "vectortick/execution/query_result.hpp"

#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <cctype>
#include <limits>
#include <charconv>

namespace vectortick {
namespace test {

// Completely independent AST Oracle that evaluates QueryStmt directly
// against canonical events using standard C++ structures.
// Does NOT use IR, ReferenceInterpreter, or VectorExecutor.
class QueryOracle {
public:
    struct OracleValue {
        bool is_signed = false;
        int64_t i_val = 0;
        uint64_t u_val = 0;

        static OracleValue make_signed(int64_t v) {
            OracleValue o;
            o.is_signed = true;
            o.i_val = v;
            o.u_val = static_cast<uint64_t>(v);
            return o;
        }

        static OracleValue make_unsigned(uint64_t v) {
            OracleValue o;
            o.is_signed = false;
            o.i_val = static_cast<int64_t>(v);
            o.u_val = v;
            return o;
        }

        [[nodiscard]] bool is_true() const noexcept {
            return is_signed ? (i_val != 0) : (u_val != 0);
        }

        [[nodiscard]] std::string to_string() const {
            return is_signed ? std::to_string(i_val) : std::to_string(u_val);
        }
    };

    static bool ieq(std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::toupper(static_cast<unsigned char>(a[i])) !=
                std::toupper(static_cast<unsigned char>(b[i]))) return false;
        }
        return true;
    }

    static OracleValue get_column_value(const CanonicalEvent& ev, std::string_view col) {
        if (ieq(col, "exchange_ts_ns") || ieq(col, "exchange_ts")) return OracleValue::make_unsigned(ev.exchange_ts_ns);
        if (ieq(col, "receive_ts_ns") || ieq(col, "receive_ts")) return OracleValue::make_unsigned(ev.receive_ts_ns);
        if (ieq(col, "sequence") || ieq(col, "seq")) return OracleValue::make_unsigned(ev.sequence);
        if (ieq(col, "instrument_id") || ieq(col, "instrument") || ieq(col, "symbol")) return OracleValue::make_unsigned(ev.instrument_id);
        if (ieq(col, "event_type")) return OracleValue::make_unsigned(static_cast<uint64_t>(ev.event_type));
        if (ieq(col, "side")) return OracleValue::make_unsigned(static_cast<uint64_t>(ev.side));
        if (ieq(col, "flags")) return OracleValue::make_unsigned(ev.flags);
        if (ieq(col, "price_ticks") || ieq(col, "price")) return OracleValue::make_signed(ev.price_ticks);
        if (ieq(col, "quantity") || ieq(col, "qty")) return OracleValue::make_unsigned(ev.quantity);
        if (ieq(col, "venue_id") || ieq(col, "venue")) return OracleValue::make_unsigned(ev.venue_id);
        if (ieq(col, "source_id") || ieq(col, "source")) return OracleValue::make_unsigned(ev.source_id);
        if (ieq(col, "trade_or_order_id") || ieq(col, "trade_id") || ieq(col, "order_id")) return OracleValue::make_unsigned(ev.trade_or_order_id);
        return OracleValue::make_unsigned(0);
    }

    static vts1::ColumnType get_column_type(std::string_view col) {
        if (ieq(col, "price_ticks") || ieq(col, "price")) return vts1::ColumnType::I64;
        if (ieq(col, "instrument_id") || ieq(col, "quantity")) return vts1::ColumnType::U32;
        if (ieq(col, "event_type") || ieq(col, "side")) return vts1::ColumnType::U8;
        if (ieq(col, "flags") || ieq(col, "venue_id") || ieq(col, "source_id")) return vts1::ColumnType::U16;
        return vts1::ColumnType::U64;
    }

    static Result<OracleValue> eval_expr(const query::Expression* expr, const CanonicalEvent& ev) {
        if (!expr) return OracleValue::make_unsigned(1);

        switch (expr->type) {
            case query::ExprType::Literal: {
                const auto* lit = static_cast<const query::LiteralExpr*>(expr);
                return OracleValue::make_unsigned(lit->value);
            }
            case query::ExprType::ColumnRef: {
                const auto* cr = static_cast<const query::ColumnRefExpr*>(expr);
                return get_column_value(ev, cr->name);
            }
            case query::ExprType::UnaryOp: {
                const auto* un = static_cast<const query::UnaryOpExpr*>(expr);
                auto val_res = eval_expr(un->operand.get(), ev);
                if (!val_res.ok()) return val_res;
                const auto& val = val_res.value();
                if (un->op == query::TokenType::Bang) {
                    return OracleValue::make_unsigned(val.is_true() ? 0 : 1);
                } else if (un->op == query::TokenType::Minus) {
                    int64_t s = val.is_signed ? val.i_val : static_cast<int64_t>(val.u_val);
                    if (s == std::numeric_limits<int64_t>::min()) {
                        return make_error<OracleValue>(StatusCode::InternalError, "Negation overflow");
                    }
                    return OracleValue::make_signed(-s);
                }
                return val_res;
            }
            case query::ExprType::BinaryOp: {
                const auto* bin = static_cast<const query::BinaryOpExpr*>(expr);
                auto l_res = eval_expr(bin->left.get(), ev);
                if (!l_res.ok()) return l_res;
                auto r_res = eval_expr(bin->right.get(), ev);
                if (!r_res.ok()) return r_res;

                const auto& l = l_res.value();
                const auto& r = r_res.value();

                if (bin->op == query::TokenType::And) {
                    return OracleValue::make_unsigned((l.is_true() && r.is_true()) ? 1 : 0);
                }
                if (bin->op == query::TokenType::Or) {
                    return OracleValue::make_unsigned((l.is_true() || r.is_true()) ? 1 : 0);
                }

                bool is_signed_op = (l.is_signed || r.is_signed);
                if (is_signed_op) {
                    int64_t lv = l.is_signed ? l.i_val : static_cast<int64_t>(l.u_val);
                    int64_t rv = r.is_signed ? r.i_val : static_cast<int64_t>(r.u_val);

                    switch (bin->op) {
                        case query::TokenType::Equal: return OracleValue::make_unsigned(lv == rv ? 1 : 0);
                        case query::TokenType::NotEqual: return OracleValue::make_unsigned(lv != rv ? 1 : 0);
                        case query::TokenType::Less: return OracleValue::make_unsigned(lv < rv ? 1 : 0);
                        case query::TokenType::LessEqual: return OracleValue::make_unsigned(lv <= rv ? 1 : 0);
                        case query::TokenType::Greater: return OracleValue::make_unsigned(lv > rv ? 1 : 0);
                        case query::TokenType::GreaterEqual: return OracleValue::make_unsigned(lv >= rv ? 1 : 0);
                        case query::TokenType::Plus:
                            if ((rv > 0 && lv > std::numeric_limits<int64_t>::max() - rv) ||
                                (rv < 0 && lv < std::numeric_limits<int64_t>::min() - rv)) {
                                return make_error<OracleValue>(StatusCode::InternalError, "Addition overflow");
                            }
                            return OracleValue::make_signed(lv + rv);
                        case query::TokenType::Minus:
                            if ((rv < 0 && lv > std::numeric_limits<int64_t>::max() + rv) ||
                                (rv > 0 && lv < std::numeric_limits<int64_t>::min() + rv)) {
                                return make_error<OracleValue>(StatusCode::InternalError, "Subtraction overflow");
                            }
                            return OracleValue::make_signed(lv - rv);
                        case query::TokenType::Star:
                            if (lv != 0 && rv != 0) {
                                if (lv > 0 && (rv > std::numeric_limits<int64_t>::max() / lv || rv < std::numeric_limits<int64_t>::min() / lv))
                                    return make_error<OracleValue>(StatusCode::InternalError, "Multiplication overflow");
                                if (lv < 0 && (rv > 0 ? lv < std::numeric_limits<int64_t>::min() / rv : lv < std::numeric_limits<int64_t>::max() / rv))
                                    return make_error<OracleValue>(StatusCode::InternalError, "Multiplication overflow");
                            }
                            return OracleValue::make_signed(lv * rv);
                        case query::TokenType::Slash:
                            if (rv == 0) return make_error<OracleValue>(StatusCode::InternalError, "Division by zero");
                            if (lv == std::numeric_limits<int64_t>::min() && rv == -1)
                                return make_error<OracleValue>(StatusCode::InternalError, "Division overflow");
                            return OracleValue::make_signed(lv / rv);
                        case query::TokenType::Percent:
                            if (rv == 0) return make_error<OracleValue>(StatusCode::InternalError, "Modulo by zero");
                            return OracleValue::make_signed(lv % rv);
                        default:
                            return make_error<OracleValue>(StatusCode::NotImplemented, "Unsupported binary operator");
                    }
                } else {
                    uint64_t lv = l.u_val;
                    uint64_t rv = r.u_val;
                    switch (bin->op) {
                        case query::TokenType::Equal: return OracleValue::make_unsigned(lv == rv ? 1 : 0);
                        case query::TokenType::NotEqual: return OracleValue::make_unsigned(lv != rv ? 1 : 0);
                        case query::TokenType::Less: return OracleValue::make_unsigned(lv < rv ? 1 : 0);
                        case query::TokenType::LessEqual: return OracleValue::make_unsigned(lv <= rv ? 1 : 0);
                        case query::TokenType::Greater: return OracleValue::make_unsigned(lv > rv ? 1 : 0);
                        case query::TokenType::GreaterEqual: return OracleValue::make_unsigned(lv >= rv ? 1 : 0);
                        case query::TokenType::Plus:
                            if (lv > std::numeric_limits<uint64_t>::max() - rv)
                                return make_error<OracleValue>(StatusCode::InternalError, "Unsigned add overflow");
                            return OracleValue::make_unsigned(lv + rv);
                        case query::TokenType::Minus:
                            if (lv < rv)
                                return make_error<OracleValue>(StatusCode::InternalError, "Unsigned sub underflow");
                            return OracleValue::make_unsigned(lv - rv);
                        case query::TokenType::Star:
                            if (lv > 0 && rv > std::numeric_limits<uint64_t>::max() / lv)
                                return make_error<OracleValue>(StatusCode::InternalError, "Unsigned mul overflow");
                            return OracleValue::make_unsigned(lv * rv);
                        case query::TokenType::Slash:
                            if (rv == 0) return make_error<OracleValue>(StatusCode::InternalError, "Division by zero");
                            return OracleValue::make_unsigned(lv / rv);
                        case query::TokenType::Percent:
                            if (rv == 0) return make_error<OracleValue>(StatusCode::InternalError, "Modulo by zero");
                            return OracleValue::make_unsigned(lv % rv);
                        default:
                            return make_error<OracleValue>(StatusCode::NotImplemented, "Unsupported binary operator");
                    }
                }
            }
            default:
                return OracleValue::make_unsigned(0);
        }
    }

    static Result<QueryResult> evaluate(const std::vector<CanonicalEvent>& events,
                                        const query::QueryStmt* query_ast) {
        if (!query_ast) {
            return make_error<QueryResult>(StatusCode::InvalidArgument, "Null query statement");
        }

        QueryResult result;
        result.rows_scanned = events.size();

        // Effective projections
        struct ProjectionView {
            const query::Expression* expr = nullptr;
            std::string alias;
            bool is_wildcard = false;
        };
        std::vector<ProjectionView> eff_projections;
        if (!query_ast->projections.empty()) {
            for (const auto& item : query_ast->projections) {
                eff_projections.push_back({item.expr.get(), item.alias, item.is_wildcard});
            }
        } else if (!query_ast->aggregations.empty()) {
            for (const auto& agg : query_ast->aggregations) {
                eff_projections.push_back({agg.second.get(), agg.first, false});
            }
        }

        bool has_aggregates = false;
        for (const auto& item : eff_projections) {
            if (item.expr && item.expr->type == query::ExprType::FunctionCall) {
                has_aggregates = true;
                break;
            }
        }

        // Output column names and types
        for (const auto& item : eff_projections) {
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
                result.column_types.push_back(infer_type(item.expr));
            } else if (item.expr && item.expr->type == query::ExprType::ColumnRef) {
                const auto* cr = static_cast<const query::ColumnRefExpr*>(item.expr);
                result.column_names.push_back(cr->name);
                result.column_types.push_back(get_column_type(cr->name));
            } else if (item.expr && item.expr->type == query::ExprType::FunctionCall) {
                const auto* fc = static_cast<const query::FunctionCallExpr*>(item.expr);
                result.column_names.push_back(fc->name + "()");
                result.column_types.push_back(infer_type(item.expr));
            } else {
                result.column_names.push_back("col");
                result.column_types.push_back(infer_type(item.expr));
            }
        }

        bool has_group_by = !query_ast->group_by_columns.empty();

        if (has_group_by) {
            struct OracleGroupAgg {
                std::vector<std::string> keys;
                u64 count = 0;
                struct Acc {
                    std::string fn;
                    const query::Expression* arg = nullptr;
                    bool is_signed = false;
                    int64_t sum_i64 = 0;
                    uint64_t sum_u64 = 0;
                    int64_t min_i64 = std::numeric_limits<int64_t>::max();
                    int64_t max_i64 = std::numeric_limits<int64_t>::min();
                    uint64_t min_u64 = std::numeric_limits<uint64_t>::max();
                    uint64_t max_u64 = 0;
                };
                std::vector<Acc> accs;
            };

            std::map<std::vector<std::string>, OracleGroupAgg> group_map;

            for (const auto& ev : events) {
                if (query_ast->where_expr) {
                    auto w_res = eval_expr(query_ast->where_expr.get(), ev);
                    if (!w_res.ok()) return make_error<QueryResult>(w_res.status().code(), w_res.status().message());
                    if (!w_res.value().is_true()) continue;
                }

                result.rows_matched++;

                std::vector<std::string> key_vals;
                for (const auto& gcol : query_ast->group_by_columns) {
                    key_vals.push_back(get_column_value(ev, gcol).to_string());
                }

                auto it = group_map.find(key_vals);
                if (it == group_map.end()) {
                    OracleGroupAgg oga;
                    oga.keys = key_vals;
                    for (const auto& item : eff_projections) {
                        if (item.expr && item.expr->type == query::ExprType::FunctionCall) {
                            const auto* fc = static_cast<const query::FunctionCallExpr*>(item.expr);
                            OracleGroupAgg::Acc acc;
                            acc.fn = fc->name;
                            std::transform(acc.fn.begin(), acc.fn.end(), acc.fn.begin(), ::toupper);
                            if (!fc->args.empty()) {
                                acc.arg = fc->args[0].get();
                                acc.is_signed = (infer_type(acc.arg) == vts1::ColumnType::I64);
                            }
                            oga.accs.push_back(std::move(acc));
                        }
                    }
                    it = group_map.emplace(key_vals, std::move(oga)).first;
                }

                it->second.count++;
                for (auto& acc : it->second.accs) {
                    if (ieq(acc.fn, "COUNT")) continue;
                    if (!acc.arg) continue;
                    auto v_res = eval_expr(acc.arg, ev);
                    if (!v_res.ok()) return make_error<QueryResult>(v_res.status().code(), v_res.status().message());
                    const auto& v = v_res.value();
                    if (acc.is_signed) {
                        acc.sum_i64 += v.i_val;
                        if (v.i_val < acc.min_i64) acc.min_i64 = v.i_val;
                        if (v.i_val > acc.max_i64) acc.max_i64 = v.i_val;
                    } else {
                        acc.sum_u64 += v.u_val;
                        if (v.u_val < acc.min_u64) acc.min_u64 = v.u_val;
                        if (v.u_val > acc.max_u64) acc.max_u64 = v.u_val;
                    }
                }
            }

            for (const auto& [k, oga] : group_map) {
                std::vector<std::string> row;
                size_t acc_idx = 0;
                for (const auto& item : eff_projections) {
                    if (item.expr && item.expr->type == query::ExprType::FunctionCall) {
                        const auto& acc = oga.accs[acc_idx++];
                        if (ieq(acc.fn, "COUNT")) row.push_back(std::to_string(oga.count));
                        else if (ieq(acc.fn, "SUM")) row.push_back(acc.is_signed ? std::to_string(acc.sum_i64) : std::to_string(acc.sum_u64));
                        else if (ieq(acc.fn, "MIN")) {
                            if (oga.count == 0) row.push_back("0");
                            else row.push_back(acc.is_signed ? std::to_string(acc.min_i64) : std::to_string(acc.min_u64));
                        } else if (ieq(acc.fn, "MAX")) {
                            if (oga.count == 0) row.push_back("0");
                            else row.push_back(acc.is_signed ? std::to_string(acc.max_i64) : std::to_string(acc.max_u64));
                        } else if (ieq(acc.fn, "AVG")) {
                            if (oga.count == 0) row.push_back("0");
                            else row.push_back(acc.is_signed ? std::to_string(acc.sum_i64 / static_cast<int64_t>(oga.count)) : std::to_string(acc.sum_u64 / oga.count));
                        } else row.push_back("0");
                    } else if (item.expr && item.expr->type == query::ExprType::ColumnRef) {
                        const auto* cr = static_cast<const query::ColumnRefExpr*>(item.expr);
                        bool found = false;
                        for (size_t gi = 0; gi < query_ast->group_by_columns.size(); ++gi) {
                            if (ieq(query_ast->group_by_columns[gi], cr->name)) {
                                row.push_back(oga.keys[gi]);
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
            u64 agg_count = 0;
            struct AggItem {
                std::string fn;
                const query::Expression* arg = nullptr;
                bool is_signed = false;
                int64_t sum_i64 = 0;
                uint64_t sum_u64 = 0;
                int64_t min_i64 = std::numeric_limits<int64_t>::max();
                int64_t max_i64 = std::numeric_limits<int64_t>::min();
                uint64_t min_u64 = std::numeric_limits<uint64_t>::max();
                uint64_t max_u64 = 0;
            };
            std::vector<AggItem> agg_items;

            for (const auto& item : eff_projections) {
                if (item.expr && item.expr->type == query::ExprType::FunctionCall) {
                    const auto* fc = static_cast<const query::FunctionCallExpr*>(item.expr);
                    AggItem ai;
                    ai.fn = fc->name;
                    std::transform(ai.fn.begin(), ai.fn.end(), ai.fn.begin(), ::toupper);
                    if (!fc->args.empty()) {
                        ai.arg = fc->args[0].get();
                        ai.is_signed = (infer_type(ai.arg) == vts1::ColumnType::I64);
                    }
                    agg_items.push_back(std::move(ai));
                }
            }

            for (const auto& ev : events) {
                if (query_ast->where_expr) {
                    auto w_res = eval_expr(query_ast->where_expr.get(), ev);
                    if (!w_res.ok()) return make_error<QueryResult>(w_res.status().code(), w_res.status().message());
                    if (!w_res.value().is_true()) continue;
                }

                result.rows_matched++;
                agg_count++;

                for (auto& ai : agg_items) {
                    if (ieq(ai.fn, "COUNT")) continue;
                    if (!ai.arg) continue;
                    auto v_res = eval_expr(ai.arg, ev);
                    if (!v_res.ok()) return make_error<QueryResult>(v_res.status().code(), v_res.status().message());
                    const auto& v = v_res.value();
                    if (ai.is_signed) {
                        ai.sum_i64 += v.i_val;
                        if (v.i_val < ai.min_i64) ai.min_i64 = v.i_val;
                        if (v.i_val > ai.max_i64) ai.max_i64 = v.i_val;
                    } else {
                        ai.sum_u64 += v.u_val;
                        if (v.u_val < ai.min_u64) ai.min_u64 = v.u_val;
                        if (v.u_val > ai.max_u64) ai.max_u64 = v.u_val;
                    }
                }
            }

            std::vector<std::string> agg_row;
            size_t idx = 0;
            for (const auto& item : eff_projections) {
                if (item.expr && item.expr->type == query::ExprType::FunctionCall) {
                    const auto& ai = agg_items[idx++];
                    if (ieq(ai.fn, "COUNT")) agg_row.push_back(std::to_string(agg_count));
                    else if (ieq(ai.fn, "SUM")) agg_row.push_back(ai.is_signed ? std::to_string(ai.sum_i64) : std::to_string(ai.sum_u64));
                    else if (ieq(ai.fn, "MIN")) {
                        if (agg_count == 0) agg_row.push_back("0");
                        else agg_row.push_back(ai.is_signed ? std::to_string(ai.min_i64) : std::to_string(ai.min_u64));
                    } else if (ieq(ai.fn, "MAX")) {
                        if (agg_count == 0) agg_row.push_back("0");
                        else agg_row.push_back(ai.is_signed ? std::to_string(ai.max_i64) : std::to_string(ai.max_u64));
                    } else if (ieq(ai.fn, "AVG")) {
                        if (agg_count == 0) agg_row.push_back("0");
                        else agg_row.push_back(ai.is_signed ? std::to_string(ai.sum_i64 / static_cast<int64_t>(agg_count)) : std::to_string(ai.sum_u64 / agg_count));
                    } else agg_row.push_back("0");
                } else {
                    agg_row.push_back("0");
                }
            }
            result.rows.push_back(std::move(agg_row));

        } else {
            for (const auto& ev : events) {
                if (query_ast->where_expr) {
                    auto w_res = eval_expr(query_ast->where_expr.get(), ev);
                    if (!w_res.ok()) return make_error<QueryResult>(w_res.status().code(), w_res.status().message());
                    if (!w_res.value().is_true()) continue;
                }

                result.rows_matched++;

                std::vector<std::string> row;
                for (const auto& item : eff_projections) {
                    if (item.is_wildcard) {
                        row.push_back(std::to_string(ev.sequence));
                        row.push_back(std::to_string(ev.instrument_id));
                        row.push_back(std::to_string(ev.price_ticks));
                        row.push_back(std::to_string(ev.quantity));
                    } else if (item.expr) {
                        auto v_res = eval_expr(item.expr, ev);
                        if (!v_res.ok()) return make_error<QueryResult>(v_res.status().code(), v_res.status().message());
                        row.push_back(v_res.value().to_string());
                    } else {
                        row.push_back("0");
                    }
                }
                if (query_ast->limit == 0 || result.rows.size() < query_ast->limit || !query_ast->order_by.empty()) {
                    result.rows.push_back(std::move(row));
                }
            }
        }

        // ORDER BY
        if (!query_ast->order_by.empty() && !result.rows.empty()) {
            struct OrdCol {
                size_t col_idx = 0;
                bool asc = true;
                bool is_signed = false;
            };
            std::vector<OrdCol> ords;
            for (const auto& [col_name, asc] : query_ast->order_by) {
                for (size_t i = 0; i < result.column_names.size(); ++i) {
                    if (ieq(result.column_names[i], col_name)) {
                        bool is_signed = (i < result.column_types.size() &&
                                          result.column_types[i] == vts1::ColumnType::I64);
                        ords.push_back({i, asc, is_signed});
                        break;
                    }
                }
            }

            if (!ords.empty()) {
                std::stable_sort(result.rows.begin(), result.rows.end(), [&](const std::vector<std::string>& a, const std::vector<std::string>& b) {
                    for (const auto& oc : ords) {
                        if (oc.col_idx >= a.size() || oc.col_idx >= b.size()) continue;
                        const auto& sa = a[oc.col_idx];
                        const auto& sb = b[oc.col_idx];
                        if (sa == sb) continue;
                        if (oc.is_signed) {
                            int64_t va = 0, vb = 0;
                            std::from_chars(sa.data(), sa.data() + sa.size(), va);
                            std::from_chars(sb.data(), sb.data() + sb.size(), vb);
                            return oc.asc ? (va < vb) : (va > vb);
                        } else {
                            uint64_t va = 0, vb = 0;
                            std::from_chars(sa.data(), sa.data() + sa.size(), va);
                            std::from_chars(sb.data(), sb.data() + sb.size(), vb);
                            return oc.asc ? (va < vb) : (va > vb);
                        }
                    }
                    return false;
                });
            }
        }

        // LIMIT
        if (query_ast->limit > 0 && result.rows.size() > query_ast->limit) {
            result.rows.resize(query_ast->limit);
        }

        return Result<QueryResult>(std::move(result));
    }

private:
    static vts1::ColumnType infer_type(const query::Expression* expr) {
        if (!expr) return vts1::ColumnType::U64;
        if (expr->type == query::ExprType::ColumnRef) {
            return get_column_type(static_cast<const query::ColumnRefExpr*>(expr)->name);
        }
        if (expr->type == query::ExprType::FunctionCall) {
            const auto* fc = static_cast<const query::FunctionCallExpr*>(expr);
            if (ieq(fc->name, "COUNT")) return vts1::ColumnType::U64;
            if (!fc->args.empty()) {
                return infer_type(fc->args[0].get());
            }
            return vts1::ColumnType::U64;
        }
        return vts1::ColumnType::U64;
    }
};

} // namespace test
} // namespace vectortick
