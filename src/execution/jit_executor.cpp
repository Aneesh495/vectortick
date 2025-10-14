#include "vectortick/execution/jit_executor.hpp"
#include "vectortick/query/type_checker.hpp"
#include "vectortick/ir/builder.hpp"
#include "vectortick/jit/jit_compiler.hpp"
#include "vectortick/common/checked_math.hpp"

#include <chrono>
#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <memory>
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

struct JitFieldAccumulator {
    std::string fn_name;
    std::unique_ptr<jit::JitCompiler> arg_compiler;
    jit::JitFunction arg_fn = nullptr;
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

        if (!arg_fn && !arg_expr) return Status::OK();

        uint64_t raw_val = 0;
        if (arg_fn) {
            raw_val = arg_fn(&ev);
        }

        if (is_signed) {
            int64_t val = static_cast<int64_t>(raw_val);
            auto s_res = checked_add<int64_t>(sum_i64, val);
            if (!s_res) return Status(StatusCode::InternalError, "Signed aggregate sum overflow");
            sum_i64 = *s_res;
            if (val < min_i64) min_i64 = val;
            if (val > max_i64) max_i64 = val;
        } else {
            uint64_t val = raw_val;
            auto s_res = checked_add<uint64_t>(sum_u64, val);
            if (!s_res) return Status(StatusCode::InternalError, "Unsigned aggregate sum overflow");
            sum_u64 = *s_res;
            if (val < min_u64) min_u64 = val;
            if (val > max_u64) max_u64 = val;
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

Result<QueryResult> JitExecutor::execute(SegmentReader& reader,
                                         const query::QueryStmt* query_ast) {
    if (!reader.is_open()) {
        return make_error<QueryResult>(StatusCode::InvalidArgument, "Segment reader not open");
    }

    std::vector<CanonicalEvent> events;
    auto st = reader.read_all_events(events);
    if (!st.ok()) return make_error<QueryResult>(st.code(), st.message());

    return execute_events(events, query_ast);
}

Result<QueryResult> JitExecutor::execute_events(const std::vector<CanonicalEvent>& events,
                                                const query::QueryStmt* query_ast) {
    if (!query_ast) {
        return make_error<QueryResult>(StatusCode::InvalidArgument, "Null query AST");
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    QueryResult result;
    result.rows_scanned = events.size();

    query::TypeChecker type_checker;
    auto tc_st = type_checker.check_query(query_ast);
    if (!tc_st.ok()) {
        return make_error<QueryResult>(tc_st.code(), tc_st.message());
    }

    // 1. Resolve effective projections & result columns
    struct EffectiveProjection {
        std::string alias;
        const query::Expression* expr = nullptr;
        bool is_wildcard = false;
        vts1::ColumnType col_type = vts1::ColumnType::U64;
    };
    std::vector<EffectiveProjection> effective_projections;

    bool has_aggregates = false;

    if (query_ast->projections.empty()) {
        effective_projections.push_back({"", nullptr, true, vts1::ColumnType::U64});
        result.column_names.push_back("sequence");
        result.column_types.push_back(vts1::ColumnType::U64);
        result.column_names.push_back("instrument_id");
        result.column_types.push_back(vts1::ColumnType::U32);
        result.column_names.push_back("price_ticks");
        result.column_types.push_back(vts1::ColumnType::I64);
        result.column_names.push_back("quantity");
        result.column_types.push_back(vts1::ColumnType::U32);
    } else {
        for (const auto& item : query_ast->projections) {
            if (item.is_wildcard) {
                effective_projections.push_back({"", nullptr, true, vts1::ColumnType::U64});
                result.column_names.push_back("sequence");
                result.column_types.push_back(vts1::ColumnType::U64);
                result.column_names.push_back("instrument_id");
                result.column_types.push_back(vts1::ColumnType::U32);
                result.column_names.push_back("price_ticks");
                result.column_types.push_back(vts1::ColumnType::I64);
                result.column_names.push_back("quantity");
                result.column_types.push_back(vts1::ColumnType::U32);
            } else {
                vts1::ColumnType ctype = vts1::ColumnType::U64;
                if (item.expr) {
                    auto ty = type_checker.check_expression(item.expr.get());
                    if (ty.ok()) ctype = ty.value();
                }

                std::string col_name;
                if (!item.alias.empty()) {
                    col_name = item.alias;
                } else if (item.expr && item.expr->type == query::ExprType::ColumnRef) {
                    col_name = static_cast<const query::ColumnRefExpr*>(item.expr.get())->name;
                } else if (item.expr && item.expr->type == query::ExprType::FunctionCall) {
                    col_name = static_cast<const query::FunctionCallExpr*>(item.expr.get())->name + "()";
                } else {
                    col_name = "col";
                }

                if (item.expr && item.expr->type == query::ExprType::FunctionCall) {
                    has_aggregates = true;
                }

                result.column_names.push_back(col_name);
                result.column_types.push_back(ctype);
                effective_projections.push_back({col_name, item.expr.get(), false, ctype});
            }
        }
    }

    if (!query_ast->aggregations.empty()) {
        has_aggregates = true;
    }

    // 2. JIT compile filter expression if present
    std::unique_ptr<jit::JitCompiler> filter_compiler;
    jit::JitFunction filter_fn = nullptr;

    if (query_ast->where_expr) {
        ir::Builder filter_builder;
        filter_builder.create_function("jit_filter");
        filter_builder.function()->add_parameter(filter_builder.function()->create_value(ir::Type::U64, "context"));
        ir::ValueId cond = filter_builder.lower_expression(query_ast->where_expr.get());
        filter_builder.create_return(cond);

        filter_compiler = std::make_unique<jit::JitCompiler>();
        auto comp_res = filter_compiler->compile(filter_builder.function());
        if (!comp_res.ok()) {
            return make_error<QueryResult>(comp_res.status().code(), comp_res.status().message());
        }
        filter_fn = comp_res.value();
    }

    // 3. JIT compile scalar projections (if no aggregates)
    struct CompiledProj {
        std::unique_ptr<jit::JitCompiler> compiler;
        jit::JitFunction fn = nullptr;
        bool is_signed = false;
        bool is_wildcard = false;
    };
    std::vector<CompiledProj> compiled_projections;

    if (!has_aggregates && query_ast->group_by_columns.empty()) {
        for (size_t i = 0; i < effective_projections.size(); ++i) {
            const auto& ep = effective_projections[i];
            CompiledProj cp;
            cp.is_signed = (ep.col_type == vts1::ColumnType::I64);
            cp.is_wildcard = ep.is_wildcard;

            if (!ep.is_wildcard && ep.expr) {
                ir::Builder p_builder;
                p_builder.create_function("jit_proj_" + std::to_string(i));
                p_builder.function()->add_parameter(p_builder.function()->create_value(ir::Type::U64, "context"));
                ir::ValueId val = p_builder.lower_expression(ep.expr);
                p_builder.create_return(val);

                cp.compiler = std::make_unique<jit::JitCompiler>();
                auto c_res = cp.compiler->compile(p_builder.function());
                if (!c_res.ok()) {
                    return make_error<QueryResult>(c_res.status().code(), c_res.status().message());
                }
                cp.fn = c_res.value();
            }
            compiled_projections.push_back(std::move(cp));
        }
    }

    // 4. Execution logic
    if (!query_ast->group_by_columns.empty()) {
        // Group By Execution
        struct GroupState {
            std::vector<std::string> group_keys;
            std::vector<JitFieldAccumulator> accumulators;
        };
        std::map<std::string, GroupState> groups;

        // Compile aggregate arguments
        for (const auto& ev : events) {
            if (filter_fn && filter_fn(&ev) == 0) continue;
            result.rows_matched++;

            // Build group key
            std::string group_key_str;
            std::vector<std::string> group_keys;
            for (const auto& gcol : query_ast->group_by_columns) {
                query::ColumnRefExpr cr(gcol, 0, 0);
                ir::Builder gb_builder;
                gb_builder.create_function("jit_gb");
                gb_builder.function()->add_parameter(gb_builder.function()->create_value(ir::Type::U64, "context"));
                ir::ValueId gv = gb_builder.lower_expression(&cr);
                gb_builder.create_return(gv);

                jit::JitCompiler gcomp;
                auto gc_res = gcomp.compile(gb_builder.function());
                std::string val_str = "0";
                if (gc_res.ok()) {
                    val_str = std::to_string(gc_res.value()(&ev));
                }
                if (!group_key_str.empty()) group_key_str += "|";
                group_key_str += val_str;
                group_keys.push_back(val_str);
            }

            auto it = groups.find(group_key_str);
            if (it == groups.end()) {
                GroupState state;
                state.group_keys = std::move(group_keys);
                for (const auto& item : effective_projections) {
                    if (item.expr && item.expr->type == query::ExprType::FunctionCall) {
                        const auto* fc = static_cast<const query::FunctionCallExpr*>(item.expr);
                        JitFieldAccumulator acc;
                        acc.fn_name = fc->name;
                        std::transform(acc.fn_name.begin(), acc.fn_name.end(), acc.fn_name.begin(), ::toupper);
                        if (!fc->args.empty()) {
                            acc.arg_expr = fc->args[0].get();
                            auto ty = type_checker.check_expression(acc.arg_expr);
                            acc.is_signed = (ty.ok() && ty.value() == vts1::ColumnType::I64);

                            ir::Builder arg_b;
                            arg_b.create_function("jit_arg");
                            arg_b.function()->add_parameter(arg_b.function()->create_value(ir::Type::U64, "context"));
                            ir::ValueId av = arg_b.lower_expression(acc.arg_expr);
                            arg_b.create_return(av);

                            acc.arg_compiler = std::make_unique<jit::JitCompiler>();
                            auto ac_res = acc.arg_compiler->compile(arg_b.function());
                            if (ac_res.ok()) {
                                acc.arg_fn = ac_res.value();
                            }
                        }
                        state.accumulators.push_back(std::move(acc));
                    }
                }
                it = groups.emplace(group_key_str, std::move(state)).first;
            }

            for (auto& acc : it->second.accumulators) {
                auto up_st = acc.update(ev);
                if (!up_st.ok()) return make_error<QueryResult>(up_st.code(), up_st.message());
            }
        }

        for (const auto& [key, state] : groups) {
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
        // Scalar Aggregations
        std::vector<JitFieldAccumulator> accumulators;
        for (const auto& item : effective_projections) {
            if (item.expr && item.expr->type == query::ExprType::FunctionCall) {
                const auto* fc = static_cast<const query::FunctionCallExpr*>(item.expr);
                JitFieldAccumulator acc;
                acc.fn_name = fc->name;
                std::transform(acc.fn_name.begin(), acc.fn_name.end(), acc.fn_name.begin(), ::toupper);
                if (!fc->args.empty()) {
                    acc.arg_expr = fc->args[0].get();
                    auto ty = type_checker.check_expression(acc.arg_expr);
                    acc.is_signed = (ty.ok() && ty.value() == vts1::ColumnType::I64);

                    ir::Builder arg_b;
                    arg_b.create_function("jit_arg");
                    arg_b.function()->add_parameter(arg_b.function()->create_value(ir::Type::U64, "context"));
                    ir::ValueId av = arg_b.lower_expression(acc.arg_expr);
                    arg_b.create_return(av);

                    acc.arg_compiler = std::make_unique<jit::JitCompiler>();
                    auto ac_res = acc.arg_compiler->compile(arg_b.function());
                    if (ac_res.ok()) {
                        acc.arg_fn = ac_res.value();
                    }
                }
                accumulators.push_back(std::move(acc));
            }
        }

        for (const auto& ev : events) {
            if (filter_fn && filter_fn(&ev) == 0) continue;
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
        // Plain Projection Query
        for (const auto& ev : events) {
            if (filter_fn && filter_fn(&ev) == 0) continue;
            result.rows_matched++;

            std::vector<std::string> row;
            for (size_t i = 0; i < compiled_projections.size(); ++i) {
                const auto& cp = compiled_projections[i];
                if (cp.is_wildcard) {
                    row.push_back(std::to_string(ev.sequence));
                    row.push_back(std::to_string(ev.instrument_id));
                    row.push_back(std::to_string(ev.price_ticks));
                    row.push_back(std::to_string(ev.quantity));
                } else if (cp.fn) {
                    uint64_t raw_val = cp.fn(&ev);
                    if (cp.is_signed) {
                        row.push_back(std::to_string(static_cast<int64_t>(raw_val)));
                    } else {
                        row.push_back(std::to_string(raw_val));
                    }
                } else {
                    row.push_back("0");
                }
            }

            if (query_ast->limit == 0 || result.rows.size() < query_ast->limit || !query_ast->order_by.empty()) {
                result.rows.push_back(std::move(row));
            }
        }
    }

    // 5. ORDER BY handling
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

                    if (oc.is_signed) {
                        int64_t va = 0, vb = 0;
                        std::from_chars(sa.data(), sa.data() + sa.size(), va);
                        std::from_chars(sb.data(), sb.data() + sb.size(), vb);
                        if (va != vb) {
                            return oc.ascending ? (va < vb) : (va > vb);
                        }
                    } else {
                        uint64_t va = 0, vb = 0;
                        std::from_chars(sa.data(), sa.data() + sa.size(), va);
                        std::from_chars(sb.data(), sb.data() + sb.size(), vb);
                        if (va != vb) {
                            return oc.ascending ? (va < vb) : (va > vb);
                        }
                    }
                }
                return false;
            });
        }
    }

    // 6. LIMIT handling
    if (query_ast->limit > 0 && result.rows.size() > query_ast->limit) {
        result.rows.resize(query_ast->limit);
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    result.execution_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();

    return result;
}

} // namespace vectortick
