#include "vectortick/query/type_checker.hpp"
#include <cctype>

namespace vectortick {
namespace query {

namespace {
inline bool ieq(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}
} // namespace

bool TypeChecker::resolve_column(std::string_view name, vts1::ColumnID &out_id) noexcept {
    if (ieq(name, "exchange_ts_ns") || ieq(name, "exchange_ts")) {
        out_id = vts1::ColumnID::ExchangeTsNs;
        return true;
    } else if (ieq(name, "receive_ts_ns") || ieq(name, "receive_ts")) {
        out_id = vts1::ColumnID::ReceiveTsNs;
        return true;
    } else if (ieq(name, "sequence") || ieq(name, "seq")) {
        out_id = vts1::ColumnID::Sequence;
        return true;
    } else if (ieq(name, "instrument_id") || ieq(name, "symbol") || ieq(name, "instrument")) {
        out_id = vts1::ColumnID::InstrumentId;
        return true;
    } else if (ieq(name, "event_type")) {
        out_id = vts1::ColumnID::EventType;
        return true;
    } else if (ieq(name, "side")) {
        out_id = vts1::ColumnID::Side;
        return true;
    } else if (ieq(name, "flags")) {
        out_id = vts1::ColumnID::Flags;
        return true;
    } else if (ieq(name, "price_ticks") || ieq(name, "price")) {
        out_id = vts1::ColumnID::PriceTicks;
        return true;
    } else if (ieq(name, "quantity") || ieq(name, "qty")) {
        out_id = vts1::ColumnID::Quantity;
        return true;
    } else if (ieq(name, "venue_id") || ieq(name, "venue")) {
        out_id = vts1::ColumnID::VenueId;
        return true;
    } else if (ieq(name, "source_id") || ieq(name, "source")) {
        out_id = vts1::ColumnID::SourceId;
        return true;
    } else if (ieq(name, "trade_or_order_id") || ieq(name, "trade_id") || ieq(name, "order_id")) {
        out_id = vts1::ColumnID::TradeOrOrderId;
        return true;
    }
    return false;
}

Result<vts1::ColumnType> TypeChecker::check_expression(const Expression *expr) const noexcept {
    if (!expr) {
        return make_error<vts1::ColumnType>(StatusCode::InvalidArgument, "Null expression");
    }

    switch (expr->type) {
    case ExprType::Literal:
        return vts1::ColumnType::U64;

    case ExprType::ColumnRef: {
        auto col = static_cast<const ColumnRefExpr *>(expr);
        vts1::ColumnID id;
        if (!resolve_column(col->name, id)) {
            return make_error<vts1::ColumnType>(StatusCode::InvalidArgument,
                                                "Unknown column reference: " + col->name);
        }
        return vts1::get_column_type(id);
    }

    case ExprType::BinaryOp: {
        auto binop = static_cast<const BinaryOpExpr *>(expr);
        auto left_res = check_expression(binop->left.get());
        if (!left_res.ok())
            return left_res;
        auto right_res = check_expression(binop->right.get());
        if (!right_res.ok())
            return right_res;

        switch (binop->op) {
        case TokenType::Equal:
        case TokenType::NotEqual:
        case TokenType::Less:
        case TokenType::LessEqual:
        case TokenType::Greater:
        case TokenType::GreaterEqual:
        case TokenType::And:
        case TokenType::Or:
            return vts1::ColumnType::Bool;

        case TokenType::Amp:
        case TokenType::Pipe:
        case TokenType::Caret:
            return vts1::ColumnType::U64;
        case TokenType::Plus:
        case TokenType::Minus:
        case TokenType::Star:
        case TokenType::Slash:
        case TokenType::Percent:
            if (left_res.value() == vts1::ColumnType::I64 ||
                right_res.value() == vts1::ColumnType::I64) {
                return vts1::ColumnType::I64;
            }
            return vts1::ColumnType::U64;
        default:
            return make_error<vts1::ColumnType>(StatusCode::NotImplemented,
                                                "Unsupported binary operator");
        }
    }

    case ExprType::UnaryOp: {
        auto unop = static_cast<const UnaryOpExpr *>(expr);
        auto operand_res = check_expression(unop->operand.get());
        if (!operand_res.ok())
            return operand_res;

        if (unop->op == TokenType::Bang) {
            return vts1::ColumnType::Bool;
        } else if (unop->op == TokenType::Minus) {
            return vts1::ColumnType::I64;
        }
        return make_error<vts1::ColumnType>(StatusCode::NotImplemented,
                                            "Unsupported unary operator");
    }

    case ExprType::FunctionCall: {
        auto fn = static_cast<const FunctionCallExpr *>(expr);
        bool count = ieq(fn->name, "count");
        bool sum = ieq(fn->name, "sum"), avg = ieq(fn->name, "avg");
        bool extrema = ieq(fn->name, "min") || ieq(fn->name, "max");
        if ((!count && !sum && !avg && !extrema) || fn->args.size() > 1 ||
            (!count && fn->args.size() != 1))
            return make_error<vts1::ColumnType>(StatusCode::InvalidArgument,
                                                "Invalid function or argument count");
        if (count && fn->args.empty())
            return vts1::ColumnType::U64;
        auto arg = check_expression(fn->args[0].get());
        if (!arg.ok())
            return arg;
        if (count)
            return vts1::ColumnType::U64;
        if (sum || avg)
            return arg.value() == vts1::ColumnType::I64 ? vts1::ColumnType::I64
                                                        : vts1::ColumnType::U64;
        return arg;
    }

    default:
        return make_error<vts1::ColumnType>(StatusCode::NotImplemented, "Unsupported expression");
    }
}

Status TypeChecker::check_query(const QueryStmt *stmt) const noexcept {
    if (!stmt)
        return Status(StatusCode::InvalidArgument, "Null query statement");
    if (!stmt->let_bindings.empty() || stmt->tumble_window)
        return Status(StatusCode::NotImplemented, "LET and TUMBLE are unsupported");
    if (!stmt->projections.empty() && !stmt->aggregations.empty())
        return Status(StatusCode::InvalidArgument, "SELECT and AGG cannot be combined");
    auto scalar = [&](auto &&self, const Expression *e) -> Status {
        if (!e)
            return Status(StatusCode::InvalidArgument, "Missing expression");
        if (e->type == ExprType::FunctionCall || e->type == ExprType::Select)
            return Status(StatusCode::NotImplemented,
                          "Nested aggregate or conditional AST is unsupported");
        if (e->type == ExprType::BinaryOp) {
            auto b = static_cast<const BinaryOpExpr *>(e);
            auto st = self(self, b->left.get());
            if (!st.ok())
                return st;
            return self(self, b->right.get());
        }
        if (e->type == ExprType::UnaryOp)
            return self(self, static_cast<const UnaryOpExpr *>(e)->operand.get());
        return Status::OK();
    };
    auto projection = [&](const Expression *e) -> Status {
        auto type = check_expression(e);
        if (!type.ok())
            return type.status();
        if (e->type == ExprType::FunctionCall) {
            auto fn = static_cast<const FunctionCallExpr *>(e);
            if (!fn->args.empty())
                return scalar(scalar, fn->args[0].get());
            return Status::OK();
        }
        return scalar(scalar, e);
    };
    for (const auto &p : stmt->projections)
        if (!p.is_wildcard) {
            auto st = projection(p.expr.get());
            if (!st.ok())
                return st;
        }
    for (const auto &a : stmt->aggregations) {
        auto st = projection(a.second.get());
        if (!st.ok())
            return st;
    }
    if (stmt->where_expr) {
        auto st = scalar(scalar, stmt->where_expr.get());
        if (!st.ok())
            return st;
        auto type = check_expression(stmt->where_expr.get());
        if (!type.ok())
            return type.status();
    }
    for (const auto &name : stmt->group_by_columns) {
        vts1::ColumnID id;
        if (!resolve_column(name, id))
            return Status(StatusCode::InvalidArgument, "Unknown GROUP BY column");
    }
    return Status::OK();
}

} // namespace query
} // namespace vectortick
