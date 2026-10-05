#include "vectortick/execution/query_engine.hpp"
#include "vectortick/common/checked_math.hpp"
#include "vectortick/execution/reference_interpreter.hpp"
#include "vectortick/ir/builder.hpp"
#include "vectortick/jit/native_plan.hpp"
#include "vectortick/query/type_checker.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <limits>
#include <set>

namespace vectortick {
namespace {
using namespace ir;
using CT = vts1::ColumnType;
bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (usize i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}
std::string expression_key(const query::Expression *e) {
    if (!e)
        return "all";
    switch (e->type) {
    case query::ExprType::Literal:
        return "l" + std::to_string(static_cast<const query::LiteralExpr *>(e)->value);
    case query::ExprType::ColumnRef:
        return "c" + static_cast<const query::ColumnRefExpr *>(e)->name;
    case query::ExprType::UnaryOp: {
        auto n = static_cast<const query::UnaryOpExpr *>(e);
        return "u" + std::to_string(static_cast<int>(n->op)) + "(" +
               expression_key(n->operand.get()) + ")";
    }
    case query::ExprType::BinaryOp: {
        auto n = static_cast<const query::BinaryOpExpr *>(e);
        return "b" + std::to_string(static_cast<int>(n->op)) + "(" + expression_key(n->left.get()) +
               "," + expression_key(n->right.get()) + ")";
    }
    default:
        return "unsupported";
    }
}
u64 field(const CanonicalEvent &e, u32 id) {
    switch (id) {
    case 0:
        return e.exchange_ts_ns;
    case 1:
        return e.receive_ts_ns;
    case 2:
        return e.sequence;
    case 3:
        return e.instrument_id;
    case 4:
        return static_cast<u64>(e.event_type);
    case 5:
        return static_cast<u64>(e.side);
    case 6:
        return e.flags;
    case 7:
        return static_cast<u64>(e.price_ticks);
    case 8:
        return e.quantity;
    case 9:
        return e.venue_id;
    case 10:
        return e.source_id;
    case 11:
        return e.trade_or_order_id;
    default:
        return 0;
    }
}
// Per-lane checked primitives. The batch evaluator runs one IR instruction across
// its active lanes before moving to the next instruction, with CFG masks for
// short-circuiting. It does not call the reference interpreter.
Result<u64> lane(Opcode op, u64 a, u64 b, u64 c) {
    auto error = []() {
        return make_error<u64>(StatusCode::InternalError, "Batch arithmetic error");
    };
    auto si = [](u64 v) { return static_cast<i64>(v); };
    switch (op) {
    case Opcode::AddU64: {
        auto v = checked_add(a, b);
        return v ? Result<u64>(*v) : error();
    }
    case Opcode::SubU64: {
        auto v = checked_sub(a, b);
        return v ? Result<u64>(*v) : error();
    }
    case Opcode::MulU64: {
        auto v = checked_mul(a, b);
        return v ? Result<u64>(*v) : error();
    }
    case Opcode::AddI64: {
        auto v = checked_add(si(a), si(b));
        return v ? Result<u64>(static_cast<u64>(*v)) : error();
    }
    case Opcode::SubI64: {
        auto v = checked_sub(si(a), si(b));
        return v ? Result<u64>(static_cast<u64>(*v)) : error();
    }
    case Opcode::MulI64: {
        auto v = checked_mul(si(a), si(b));
        return v ? Result<u64>(static_cast<u64>(*v)) : error();
    }
    case Opcode::DivU64:
        if (!b)
            return error();
        return a / b;
    case Opcode::ModU64:
        if (!b)
            return error();
        return a % b;
    case Opcode::DivI64: {
        auto v = checked_div(si(a), si(b));
        return v ? Result<u64>(static_cast<u64>(*v)) : error();
    }
    case Opcode::ModI64:
        if (!b)
            return error();
        return a == (u64{1} << 63) && b == ~u64{0} ? 0 : static_cast<u64>(si(a) % si(b));
    case Opcode::NegI64:
        if (a == (u64{1} << 63))
            return error();
        return u64{0} - a;
    case Opcode::NegU64:
        if (a > (u64{1} << 63))
            return error();
        return u64{0} - a;
    case Opcode::U64ToI64:
        if (a > static_cast<u64>(INT64_MAX))
            return error();
        return a;
    case Opcode::EqU64:
    case Opcode::EqI64:
        return u64(a == b);
    case Opcode::NeU64:
    case Opcode::NeI64:
        return u64(a != b);
    case Opcode::LtU64:
        return u64(a < b);
    case Opcode::LeU64:
        return u64(a <= b);
    case Opcode::GtU64:
        return u64(a > b);
    case Opcode::GeU64:
        return u64(a >= b);
    case Opcode::LtI64:
        return u64(si(a) < si(b));
    case Opcode::LeI64:
        return u64(si(a) <= si(b));
    case Opcode::GtI64:
        return u64(si(a) > si(b));
    case Opcode::GeI64:
        return u64(si(a) >= si(b));
    case Opcode::And:
        return u64(a != 0 && b != 0);
    case Opcode::Or:
        return u64(a != 0 || b != 0);
    case Opcode::Not:
        return u64(a == 0);
    case Opcode::BitAnd:
        return a & b;
    case Opcode::BitOr:
        return a | b;
    case Opcode::BitXor:
        return a ^ b;
    case Opcode::BitNot:
        return ~a;
    case Opcode::Shl:
        if (b >= 64)
            return error();
        return a << b;
    case Opcode::Shr:
        if (b >= 64)
            return error();
        return a >> b;
    case Opcode::ExtendI32ToI64:
        return static_cast<u64>(static_cast<i64>(static_cast<i32>(a)));
    case Opcode::ExtendU32ToU64:
    case Opcode::TruncI64ToI32:
        return static_cast<u64>(static_cast<u32>(a));
    case Opcode::Select:
        return a ? b : c;
    default:
        return make_error<u64>(StatusCode::NotImplemented, "Unsupported batch opcode");
    }
}
Result<std::vector<u64>> batch_expression(const Function *fn,
                                          const std::vector<const CanonicalEvent *> &events) {
    auto plan = jit::plan_native(fn, 0);
    if (!plan.ok())
        return make_error<std::vector<u64>>(plan.status().code(), plan.status().message());
    std::vector<u64> out(events.size());
    for (usize start = 0; start < events.size(); start += 1024) {
        usize n = std::min(usize{1024}, events.size() - start);
        std::map<ValueId, std::vector<u64>> vals;
        for (auto p : fn->parameters())
            vals[p].resize(n);
        std::map<const BasicBlock *, std::vector<u8>> active;
        active[fn->entry_block()].assign(n, 1);
        std::vector<const BasicBlock *> previous(n, nullptr);
        for (auto block : plan.value().blocks) {
            auto &mask = active[block];
            mask.resize(n);
            for (const auto &ip : block->instructions()) {
                auto in = ip.get();
                auto op = in->opcode();
                if (op == Opcode::Nop)
                    continue;
                if (in->has_result())
                    vals[in->result()].resize(n);
                for (usize i = 0; i < n; ++i)
                    if (mask[i]) {
                        if (op == Opcode::Return) {
                            out[start + i] = in->num_operands() ? vals.at(in->operand(0))[i] : 0;
                            continue;
                        }
                        if (op == Opcode::Branch || op == Opcode::Jump) {
                            const BasicBlock *target;
                            if (op == Opcode::Jump)
                                target = static_cast<const JumpOp *>(in)->target();
                            else {
                                auto br = static_cast<const BranchOp *>(in);
                                target = vals.at(br->condition())[i] ? br->true_block()
                                                                     : br->false_block();
                            }
                            active[target].resize(n);
                            active[target][i] = 1;
                            previous[i] = block;
                            continue;
                        }
                        u64 value = 0;
                        if (op == Opcode::Phi) {
                            auto phi = static_cast<const PhiOp *>(in);
                            for (usize k = 0; k < phi->num_operands(); ++k)
                                if (phi->incoming_blocks()[k] == previous[i])
                                    value = vals.at(phi->operand(k))[i];
                        } else if (op >= Opcode::ConstI64 && op <= Opcode::ConstBool)
                            value = static_cast<const ConstOp *>(in)->constant().as_u64();
                        else if (op == Opcode::LoadColumn)
                            value = field(*events[start + i],
                                          static_cast<const LoadColumnOp *>(in)->column_id());
                        else {
                            auto v = lane(op, vals.at(in->operand(0))[i],
                                          in->num_operands() > 1 ? vals.at(in->operand(1))[i] : 0,
                                          in->num_operands() > 2 ? vals.at(in->operand(2))[i] : 0);
                            if (!v.ok())
                                return make_error<std::vector<u64>>(v.status().code(),
                                                                    v.status().message());
                            value = v.value();
                        }
                        vals[in->result()][i] = value;
                    }
            }
        }
    }
    return out;
}
struct ExpressionPlan {
    std::unique_ptr<Function> ir;
    std::shared_ptr<jit::JitCompiler> compiler;
};
struct Projection {
    std::string name;
    CT type;
    const query::Expression *expression;
    std::string aggregate;
    ExpressionPlan plan;
    std::vector<u64> values;
};
std::string text(u64 v, CT type) {
    return type == CT::I64 ? std::to_string(static_cast<i64>(v)) : std::to_string(v);
}
struct Accumulator {
    u64 count = 0, sum = 0, min = 0, max = 0;
    Status add(u64 v, const Projection &p) {
        bool is_signed = p.type == CT::I64;
        if (count == 0)
            min = max = v;
        else if (is_signed) {
            if (static_cast<i64>(v) < static_cast<i64>(min))
                min = v;
            if (static_cast<i64>(v) > static_cast<i64>(max))
                max = v;
        } else {
            min = std::min(min, v);
            max = std::max(max, v);
        }
        ++count;
        if (ieq(p.aggregate, "sum") || ieq(p.aggregate, "avg")) {
            if (is_signed) {
                auto s = checked_add(static_cast<i64>(sum), static_cast<i64>(v));
                if (!s)
                    return Status(StatusCode::InternalError, "Signed aggregate overflow");
                sum = static_cast<u64>(*s);
            } else {
                auto s = checked_add(sum, v);
                if (!s)
                    return Status(StatusCode::InternalError, "Unsigned aggregate overflow");
                sum = *s;
            }
        }
        return Status::OK();
    }
    u64 result(const Projection &p) const {
        if (ieq(p.aggregate, "count"))
            return count;
        if (ieq(p.aggregate, "sum"))
            return sum;
        if (ieq(p.aggregate, "min"))
            return count ? min : 0;
        if (ieq(p.aggregate, "max"))
            return count ? max : 0;
        if (ieq(p.aggregate, "avg"))
            return !count ? 0
                   : p.type == CT::I64
                       ? static_cast<u64>(static_cast<i64>(sum) / static_cast<i64>(count))
                       : sum / count;
        return 0;
    }
};
} // namespace
Result<QueryResult> execute_query(const std::vector<CanonicalEvent> &events,
                                  const query::QueryStmt *ast, QueryEngine engine,
                                  NativeCache *cache) {
    auto start = std::chrono::steady_clock::now();
    query::TypeChecker types;
    auto valid = types.check_query(ast);
    if (!valid.ok())
        return make_error<QueryResult>(valid.code(), valid.message());
    QueryResult result;
    result.rows_scanned = events.size();
    result.backend =
        engine == QueryEngine::Native
            ? (jit::detect_host_arch() == jit::TargetArch::X86_64 ? "x86_64" : "aarch64")
        : engine == QueryEngine::Vector ? "batch"
                                        : "reference";
    std::vector<std::unique_ptr<query::Expression>> synthetic;
    auto column = [&](const std::string &name) {
        synthetic.push_back(std::make_unique<query::ColumnRefExpr>(name, 0, 0));
        return synthetic.back().get();
    };
    auto compile = [&](const query::Expression *expr, ExpressionPlan &plan) -> Status {
        ir::Builder b;
        auto fn = b.create_function("query_expression");
        fn->add_parameter(fn->create_value(Type::U64, "context"));
        auto v = expr ? b.lower_expression(expr) : b.create_const_u64(1);
        b.create_return(v);
        plan.ir = b.take_function();
        auto native_plan = jit::plan_native(plan.ir.get(), 0);
        if (!native_plan.ok())
            return native_plan.status();
        if (engine == QueryEngine::Native) {
            std::string key = expression_key(expr);
            if (cache) {
                auto it = cache->find(key);
                if (it != cache->end()) {
                    plan.compiler = it->second;
                    return Status::OK();
                }
            }
            plan.compiler = std::make_shared<jit::JitCompiler>();
            auto compiled = plan.compiler->compile(plan.ir.get());
            if (!compiled.ok())
                return compiled.status();
            ++result.functions_compiled;
            if (cache) {
                if (cache->size() >= 128)
                    cache->erase(cache->begin());
                cache->emplace(std::move(key), plan.compiler);
            }
        }
        return Status::OK();
    };
    auto evaluate =
        [&](ExpressionPlan &plan,
            const std::vector<const CanonicalEvent *> &rows) -> Result<std::vector<u64>> {
        if (engine == QueryEngine::Vector)
            return batch_expression(plan.ir.get(), rows);
        std::vector<u64> out;
        out.reserve(rows.size());
        ReferenceInterpreter interpreter;
        for (auto e : rows) {
            Result<u64> v = make_error<u64>(StatusCode::InternalError, "Unexecuted expression");
            if (engine == QueryEngine::Native) {
                ++result.native_invocations;
                v = jit::invoke_native(plan.compiler->function(), e);
            } else
                v = interpreter.execute(plan.ir.get(), e);
            if (!v.ok())
                return make_error<std::vector<u64>>(v.status().code(), v.status().message());
            out.push_back(v.value());
        }
        return out;
    };
    std::vector<Projection> projections;
    auto add_projection = [&](const query::Expression *expr, const std::string &alias) -> Status {
        auto ty = types.check_expression(expr);
        if (!ty.ok())
            return ty.status();
        Projection p;
        p.type = ty.value();
        p.expression = expr;
        p.name = !alias.empty() ? alias
                 : expr->type == query::ExprType::ColumnRef
                     ? static_cast<const query::ColumnRefExpr *>(expr)->name
                 : expr->type == query::ExprType::FunctionCall
                     ? static_cast<const query::FunctionCallExpr *>(expr)->name + "()"
                     : "col";
        const query::Expression *eval = expr;
        if (expr->type == query::ExprType::FunctionCall) {
            auto call = static_cast<const query::FunctionCallExpr *>(expr);
            p.aggregate = call->name;
            eval = call->args.empty() ? nullptr : call->args[0].get();
        }
        auto st = compile(eval, p.plan);
        if (!st.ok())
            return st;
        result.column_names.push_back(p.name);
        result.column_types.push_back(p.type);
        projections.push_back(std::move(p));
        return Status::OK();
    };
    auto wildcard = [&]() -> Status {
        for (auto name : {"sequence", "instrument_id", "price_ticks", "quantity"}) {
            auto st = add_projection(column(name), "");
            if (!st.ok())
                return st;
        }
        return Status::OK();
    };
    if (ast->projections.empty() && ast->aggregations.empty()) {
        auto st = wildcard();
        if (!st.ok())
            return make_error<QueryResult>(st.code(), st.message());
    }
    for (const auto &p : ast->projections) {
        auto st = p.is_wildcard ? wildcard() : add_projection(p.expr.get(), p.alias);
        if (!st.ok())
            return make_error<QueryResult>(st.code(), st.message());
    }
    for (const auto &p : ast->aggregations) {
        auto st = add_projection(p.second.get(), p.first);
        if (!st.ok())
            return make_error<QueryResult>(st.code(), st.message());
    }
    bool aggregate = std::any_of(projections.begin(), projections.end(),
                                 [](const auto &p) { return !p.aggregate.empty(); });
    std::vector<ExpressionPlan> group_plans;
    std::vector<CT> group_types;
    for (const auto &name : ast->group_by_columns) {
        auto expr = column(name);
        auto ty = types.check_expression(expr);
        group_types.push_back(ty.value());
        ExpressionPlan p;
        auto st = compile(expr, p);
        if (!st.ok())
            return make_error<QueryResult>(st.code(), st.message());
        group_plans.push_back(std::move(p));
    }
    std::vector<usize> projection_groups(projections.size(), 0);
    if (aggregate || !group_plans.empty())
        for (usize i = 0; i < projections.size(); ++i)
            if (projections[i].aggregate.empty()) {
                bool found = false;
                if (projections[i].expression->type == query::ExprType::ColumnRef) {
                    vts1::ColumnID id;
                    if (!query::TypeChecker::resolve_column(
                            static_cast<const query::ColumnRefExpr *>(projections[i].expression)
                                ->name,
                            id))
                        return make_error<QueryResult>(StatusCode::InvalidArgument,
                                                       "Unknown group projection");
                    for (usize g = 0; g < ast->group_by_columns.size(); ++g) {
                        vts1::ColumnID gid;
                        if (!query::TypeChecker::resolve_column(ast->group_by_columns[g], gid))
                            return make_error<QueryResult>(StatusCode::InvalidArgument,
                                                           "Unknown group column");
                        if (id == gid) {
                            projection_groups[i] = g;
                            found = true;
                            break;
                        }
                    }
                }
                if (!found)
                    return make_error<QueryResult>(
                        StatusCode::InvalidArgument,
                        "Nonaggregate projection must be a GROUP BY key");
            }
    std::vector<std::pair<usize, bool>> orders;
    for (const auto &[name, ascending] : ast->order_by) {
        usize matches = 0, index = 0;
        for (usize i = 0; i < projections.size(); ++i)
            if (ieq(name, projections[i].name)) {
                ++matches;
                index = i;
            }
        if (matches != 1)
            return make_error<QueryResult>(StatusCode::InvalidArgument,
                                           "ORDER BY requires one projected column or alias");
        orders.emplace_back(index, ascending);
    }
    ExpressionPlan filter;
    auto st = compile(ast->where_expr.get(), filter);
    if (!st.ok())
        return make_error<QueryResult>(st.code(), st.message());
    std::vector<const CanonicalEvent *> all;
    all.reserve(events.size());
    for (const auto &e : events)
        all.push_back(&e);
    auto selected = evaluate(filter, all);
    if (!selected.ok())
        return make_error<QueryResult>(selected.status().code(), selected.status().message());
    std::vector<const CanonicalEvent *> matched;
    for (usize i = 0; i < all.size(); ++i)
        if (selected.value()[i])
            matched.push_back(all[i]);
    result.rows_matched = matched.size();
    result.rows_evaluated = events.size();
    for (auto &p : projections) {
        auto values = evaluate(p.plan, matched);
        if (!values.ok())
            return make_error<QueryResult>(values.status().code(), values.status().message());
        p.values = std::move(values.value());
    }
    std::vector<std::vector<u64>> group_values;
    for (auto &p : group_plans) {
        auto values = evaluate(p, matched);
        if (!values.ok())
            return make_error<QueryResult>(values.status().code(), values.status().message());
        group_values.push_back(std::move(values.value()));
    }
    if (aggregate || !group_plans.empty()) {
        std::map<std::vector<std::string>, std::vector<Accumulator>> groups;
        if (group_plans.empty())
            groups[{}].resize(projections.size());
        for (usize r = 0; r < matched.size(); ++r) {
            std::vector<std::string> key;
            for (usize g = 0; g < group_values.size(); ++g)
                key.push_back(text(group_values[g][r], group_types[g]));
            auto &acc = groups[key];
            acc.resize(projections.size());
            for (usize i = 0; i < projections.size(); ++i)
                if (!projections[i].aggregate.empty()) {
                    auto status = acc[i].add(projections[i].values[r], projections[i]);
                    if (!status.ok())
                        return make_error<QueryResult>(status.code(), status.message());
                }
        }
        for (const auto &[key, acc] : groups) {
            std::vector<std::string> row;
            for (usize i = 0; i < projections.size(); ++i) {
                row.push_back(projections[i].aggregate.empty()
                                  ? key[projection_groups[i]]
                                  : text(acc[i].result(projections[i]), projections[i].type));
            }
            result.rows.push_back(std::move(row));
        }
    } else
        for (usize r = 0; r < matched.size(); ++r) {
            std::vector<std::string> row;
            for (const auto &p : projections)
                row.push_back(text(p.values[r], p.type));
            result.rows.push_back(std::move(row));
        }
    std::stable_sort(result.rows.begin(), result.rows.end(), [&](const auto &a, const auto &b) {
        for (auto [i, ascending] : orders) {
            if (a[i] == b[i])
                continue;
            if (result.column_types[i] == CT::I64) {
                i64 x = 0, y = 0;
                std::from_chars(a[i].data(), a[i].data() + a[i].size(), x);
                std::from_chars(b[i].data(), b[i].data() + b[i].size(), y);
                return ascending ? x < y : x > y;
            }
            u64 x = 0, y = 0;
            std::from_chars(a[i].data(), a[i].data() + a[i].size(), x);
            std::from_chars(b[i].data(), b[i].data() + b[i].size(), y);
            return ascending ? x < y : x > y;
        }
        return false;
    });
    if (ast->limit && result.rows.size() > ast->limit)
        result.rows.resize(static_cast<usize>(ast->limit));
    result.execution_time_ns =
        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count());
    return result;
}
} // namespace vectortick
