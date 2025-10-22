#include "vectortick/ir/builder.hpp"
#include <cassert>

namespace vectortick {
namespace ir {

Function* Builder::create_function(const std::string& name) {
    owned_function_ = std::make_unique<Function>(name);
    function_ = owned_function_.get();
    next_temp_ = 1;
    current_block_ = function_->create_block("entry");
    return function_;
}

BasicBlock* Builder::create_block(const std::string& name) {
    assert(function_ && "No function created");
    return function_->create_block(name);
}

ValueId Builder::create_const_i64(i64 value) {
    ValueId result = function_->create_value(Type::I64, "const");
    auto instr = std::make_unique<ConstOp>(Constant::i64_const(value), result);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_const_u64(u64 value) {
    ValueId result = function_->create_value(Type::U64, "const");
    auto instr = std::make_unique<ConstOp>(Constant::u64_const(value), result);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_const_u32(u32 value) {
    ValueId result = function_->create_value(Type::U32, "const");
    auto instr = std::make_unique<ConstOp>(Constant::u32_const(value), result);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_const_bool(bool value) {
    ValueId result = function_->create_value(Type::I1, "const");
    auto instr = std::make_unique<ConstOp>(Constant::boolean(value), result);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_add(ValueId lhs, ValueId rhs, Type type) {
    ValueId result = function_->create_value(type, "add");
    Opcode op = (type == Type::I64) ? Opcode::AddI64 : Opcode::AddU64;
    auto instr = std::make_unique<BinaryOp>(op, lhs, rhs, result, type);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_sub(ValueId lhs, ValueId rhs, Type type) {
    ValueId result = function_->create_value(type, "sub");
    Opcode op = (type == Type::I64) ? Opcode::SubI64 : Opcode::SubU64;
    auto instr = std::make_unique<BinaryOp>(op, lhs, rhs, result, type);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_mul(ValueId lhs, ValueId rhs, Type type) {
    ValueId result = function_->create_value(type, "mul");
    Opcode op = (type == Type::I64) ? Opcode::MulI64 : Opcode::MulU64;
    auto instr = std::make_unique<BinaryOp>(op, lhs, rhs, result, type);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_div(ValueId lhs, ValueId rhs, Type type) {
    ValueId result = function_->create_value(type, "div");
    Opcode op = (type == Type::I64) ? Opcode::DivI64 : Opcode::DivU64;
    auto instr = std::make_unique<BinaryOp>(op, lhs, rhs, result, type);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_mod(ValueId lhs, ValueId rhs, Type type) {
    ValueId result = function_->create_value(type, "mod");
    Opcode op = (type == Type::I64) ? Opcode::ModI64 : Opcode::ModU64;
    auto instr = std::make_unique<BinaryOp>(op, lhs, rhs, result, type);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_neg(ValueId operand, Type type) {
    ValueId result = function_->create_value(type, "neg");
    auto instr = std::make_unique<UnaryOp>(Opcode::NegI64, operand, result, type);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_eq(ValueId lhs, ValueId rhs, Type type) {
    ValueId result = function_->create_value(Type::I1, "eq");
    Opcode op = (type == Type::I64) ? Opcode::EqI64 : Opcode::EqU64;
    auto instr = std::make_unique<CompareOp>(op, lhs, rhs, result);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_ne(ValueId lhs, ValueId rhs, Type type) {
    ValueId result = function_->create_value(Type::I1, "ne");
    Opcode op = (type == Type::I64) ? Opcode::NeI64 : Opcode::NeU64;
    auto instr = std::make_unique<CompareOp>(op, lhs, rhs, result);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_lt(ValueId lhs, ValueId rhs, Type type) {
    ValueId result = function_->create_value(Type::I1, "lt");
    Opcode op = (type == Type::I64) ? Opcode::LtI64 : Opcode::LtU64;
    auto instr = std::make_unique<CompareOp>(op, lhs, rhs, result);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_le(ValueId lhs, ValueId rhs, Type type) {
    ValueId result = function_->create_value(Type::I1, "le");
    Opcode op = (type == Type::I64) ? Opcode::LeI64 : Opcode::LeU64;
    auto instr = std::make_unique<CompareOp>(op, lhs, rhs, result);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_gt(ValueId lhs, ValueId rhs, Type type) {
    ValueId result = function_->create_value(Type::I1, "gt");
    Opcode op = (type == Type::I64) ? Opcode::GtI64 : Opcode::GtU64;
    auto instr = std::make_unique<CompareOp>(op, lhs, rhs, result);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_ge(ValueId lhs, ValueId rhs, Type type) {
    ValueId result = function_->create_value(Type::I1, "ge");
    Opcode op = (type == Type::I64) ? Opcode::GeI64 : Opcode::GeU64;
    auto instr = std::make_unique<CompareOp>(op, lhs, rhs, result);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_and(ValueId lhs, ValueId rhs) {
    ValueId result = function_->create_value(Type::I1, "and");
    auto instr = std::make_unique<BinaryOp>(Opcode::And, lhs, rhs, result, Type::I1);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_or(ValueId lhs, ValueId rhs) {
    ValueId result = function_->create_value(Type::I1, "or");
    auto instr = std::make_unique<BinaryOp>(Opcode::Or, lhs, rhs, result, Type::I1);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_not(ValueId operand) {
    ValueId result = function_->create_value(Type::I1, "not");
    auto instr = std::make_unique<UnaryOp>(Opcode::Not, operand, result, Type::I1);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_bitand(ValueId lhs, ValueId rhs) {
    ValueId result = function_->create_value(Type::U64, "bitand");
    auto instr = std::make_unique<BinaryOp>(Opcode::BitAnd, lhs, rhs, result, Type::U64);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_bitor(ValueId lhs, ValueId rhs) {
    ValueId result = function_->create_value(Type::U64, "bitor");
    auto instr = std::make_unique<BinaryOp>(Opcode::BitOr, lhs, rhs, result, Type::U64);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_bitxor(ValueId lhs, ValueId rhs) {
    ValueId result = function_->create_value(Type::U64, "bitxor");
    auto instr = std::make_unique<BinaryOp>(Opcode::BitXor, lhs, rhs, result, Type::U64);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_bitnot(ValueId operand) {
    ValueId result = function_->create_value(Type::U64, "bitnot");
    auto instr = std::make_unique<UnaryOp>(Opcode::BitNot, operand, result, Type::U64);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_select(ValueId cond, ValueId true_val, ValueId false_val, Type type) {
    ValueId result = function_->create_value(type, "select");
    auto instr = std::make_unique<SelectOp>(cond, true_val, false_val, result, type);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_load_column(u32 column_id, ValueId row_idx, Type type) {
    ValueId result = function_->create_value(type, "load");
    auto instr = std::make_unique<LoadColumnOp>(column_id, row_idx, result, type);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_count(ValueId input) {
    ValueId result = function_->create_value(Type::U64, "count");
    auto instr = std::make_unique<AggregateOp>(Opcode::Count, input, result, Type::U64);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_sum(ValueId input, Type type) {
    ValueId result = function_->create_value(type, "sum");
    Opcode op = (type == Type::I64) ? Opcode::SumI64 : Opcode::SumU64;
    auto instr = std::make_unique<AggregateOp>(op, input, result, type);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_min(ValueId input, Type type) {
    ValueId result = function_->create_value(type, "min");
    Opcode op = (type == Type::I64) ? Opcode::MinI64 : Opcode::MinU64;
    auto instr = std::make_unique<AggregateOp>(op, input, result, type);
    current_block_->append(std::move(instr));
    return result;
}

ValueId Builder::create_max(ValueId input, Type type) {
    ValueId result = function_->create_value(type, "max");
    Opcode op = (type == Type::I64) ? Opcode::MaxI64 : Opcode::MaxU64;
    auto instr = std::make_unique<AggregateOp>(op, input, result, type);
    current_block_->append(std::move(instr));
    return result;
}

void Builder::create_return(ValueId value) {
    auto instr = std::make_unique<ReturnOp>(value);
    current_block_->append(std::move(instr));
}

void Builder::create_jump(BasicBlock* target) {
    auto instr = std::make_unique<JumpOp>(target);
    current_block_->append(std::move(instr));
}

void Builder::create_branch(ValueId cond, BasicBlock* true_block, BasicBlock* false_block) {
    auto instr = std::make_unique<BranchOp>(cond, true_block, false_block);
    current_block_->append(std::move(instr));
}

namespace {
inline bool resolve_column_info(std::string_view name, u32& out_id, Type& out_type) noexcept {
    auto ieq = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::toupper(static_cast<unsigned char>(a[i])) !=
                std::toupper(static_cast<unsigned char>(b[i]))) return false;
        }
        return true;
    };
    
    if (ieq(name, "exchange_ts_ns") || ieq(name, "exchange_ts")) {
        out_id = 0; out_type = Type::U64; return true;
    } else if (ieq(name, "receive_ts_ns") || ieq(name, "receive_ts")) {
        out_id = 1; out_type = Type::U64; return true;
    } else if (ieq(name, "sequence") || ieq(name, "seq")) {
        out_id = 2; out_type = Type::U64; return true;
    } else if (ieq(name, "instrument_id") || ieq(name, "symbol") || ieq(name, "instrument")) {
        out_id = 3; out_type = Type::U32; return true;
    } else if (ieq(name, "event_type")) {
        out_id = 4; out_type = Type::U8; return true;
    } else if (ieq(name, "side")) {
        out_id = 5; out_type = Type::U8; return true;
    } else if (ieq(name, "flags")) {
        out_id = 6; out_type = Type::U16; return true;
    } else if (ieq(name, "price_ticks") || ieq(name, "price")) {
        out_id = 7; out_type = Type::I64; return true;
    } else if (ieq(name, "quantity") || ieq(name, "qty")) {
        out_id = 8; out_type = Type::U32; return true;
    } else if (ieq(name, "venue_id") || ieq(name, "venue")) {
        out_id = 9; out_type = Type::U16; return true;
    } else if (ieq(name, "source_id") || ieq(name, "source")) {
        out_id = 10; out_type = Type::U16; return true;
    } else if (ieq(name, "trade_or_order_id") || ieq(name, "trade_id") || ieq(name, "order_id")) {
        out_id = 11; out_type = Type::U64; return true;
    }
    return false;
}
} // namespace

ValueId Builder::lower_expression(const query::Expression* expr) {
    if (!expr) return 0;
    
    switch (expr->type) {
        case query::ExprType::Literal: {
            auto lit = static_cast<const query::LiteralExpr*>(expr);
            return create_const_u64(lit->value);
        }
        
        case query::ExprType::ColumnRef: {
            auto col = static_cast<const query::ColumnRefExpr*>(expr);
            u32 column_id = 0;
            Type col_type = Type::U64;
            if (!resolve_column_info(col->name, column_id, col_type)) {
                column_id = 0;
                col_type = Type::U64;
            }
            ValueId row_idx = function_->parameters()[0];  // First param is row index
            return create_load_column(column_id, row_idx, col_type);
        }
        
        case query::ExprType::BinaryOp: {
            auto binop = static_cast<const query::BinaryOpExpr*>(expr);
            ValueId lhs = lower_expression(binop->left.get());
            ValueId rhs = lower_expression(binop->right.get());
            Type lt = infer_type(binop->left.get());
            Type rt = infer_type(binop->right.get());
            Type type = (lt == Type::I64 || rt == Type::I64) ? Type::I64 : Type::U64;
            
            switch (binop->op) {
                case query::TokenType::Plus: return create_add(lhs, rhs, type);
                case query::TokenType::Minus: return create_sub(lhs, rhs, type);
                case query::TokenType::Star: return create_mul(lhs, rhs, type);
                case query::TokenType::Slash: return create_div(lhs, rhs, type);
                case query::TokenType::Percent: return create_mod(lhs, rhs, type);
                case query::TokenType::Equal: return create_eq(lhs, rhs, type);
                case query::TokenType::NotEqual: return create_ne(lhs, rhs, type);
                case query::TokenType::Less: return create_lt(lhs, rhs, type);
                case query::TokenType::LessEqual: return create_le(lhs, rhs, type);
                case query::TokenType::Greater: return create_gt(lhs, rhs, type);
                case query::TokenType::GreaterEqual: return create_ge(lhs, rhs, type);
                case query::TokenType::And: return create_and(lhs, rhs);
                case query::TokenType::Or: return create_or(lhs, rhs);
                case query::TokenType::Amp: return create_bitand(lhs, rhs);
                case query::TokenType::Pipe: return create_bitor(lhs, rhs);
                case query::TokenType::Caret: return create_bitxor(lhs, rhs);
                default: return 0;
            }
        }
        
        case query::ExprType::UnaryOp: {
            auto unop = static_cast<const query::UnaryOpExpr*>(expr);
            ValueId operand = lower_expression(unop->operand.get());
            
            switch (unop->op) {
                case query::TokenType::Minus: return create_neg(operand, Type::I64);
                case query::TokenType::Bang: return create_not(operand);
                default: return 0;
            }
        }

        case query::ExprType::FunctionCall: {
            auto fn = static_cast<const query::FunctionCallExpr*>(expr);
            std::string fname = fn->name;
            for (char& c : fname) c = std::tolower(static_cast<unsigned char>(c));
            if (fname == "count") {
                ValueId row_idx = function_->parameters()[0];
                return create_count(row_idx);
            }
            if (!fn->args.empty()) {
                ValueId arg = lower_expression(fn->args[0].get());
                Type atype = infer_type(fn->args[0].get());
                if (fname == "sum") return create_sum(arg, atype);
                if (fname == "min") return create_min(arg, atype);
                if (fname == "max") return create_max(arg, atype);
            }
            return 0;
        }
        
        default:
            return 0;
    }
}

std::unique_ptr<Function> Builder::build_from_query(const query::QueryStmt* stmt) {
    if (!stmt) return nullptr;
    
    // Create function
    owned_function_ = std::make_unique<Function>("query");
    function_ = owned_function_.get();
    
    // Create entry block
    current_block_ = function_->create_block("entry");
    
    // Add a row index parameter
    function_->add_parameter(function_->create_value(Type::U64, "row_idx"));
    
    // Check if query has aggregations (either in aggregations or in projections)
    const query::FunctionCallExpr* agg_call = nullptr;
    if (!stmt->aggregations.empty() && stmt->aggregations[0].second) {
        if (stmt->aggregations[0].second->type == query::ExprType::FunctionCall) {
            agg_call = static_cast<const query::FunctionCallExpr*>(stmt->aggregations[0].second.get());
        }
    } else {
        for (const auto& proj : stmt->projections) {
            if (proj.expr && proj.expr->type == query::ExprType::FunctionCall) {
                agg_call = static_cast<const query::FunctionCallExpr*>(proj.expr.get());
                break;
            }
        }
    }
    
    if (agg_call) {
        std::string fname = agg_call->name;
        for (char& c : fname) c = std::tolower(static_cast<unsigned char>(c));
        
        ValueId agg_result = 0;
        if (fname == "count") {
            ValueId row_idx = function_->parameters()[0];
            agg_result = create_count(row_idx);
        } else if (!agg_call->args.empty()) {
            ValueId arg = lower_expression(agg_call->args[0].get());
            Type atype = infer_type(agg_call->args[0].get());
            if (fname == "sum") agg_result = create_sum(arg, atype);
            else if (fname == "min") agg_result = create_min(arg, atype);
            else if (fname == "max") agg_result = create_max(arg, atype);
        }
        create_return(agg_result);
        return std::move(owned_function_);
    }
    
    // Lower WHERE clause if present
    if (stmt->where_expr) {
        ValueId cond = lower_expression(stmt->where_expr.get());
        create_return(cond);
    } else if (!stmt->projections.empty() && stmt->projections[0].expr) {
        ValueId proj_val = lower_expression(stmt->projections[0].expr.get());
        create_return(proj_val);
    } else {
        // Return constant 1 (include all rows)
        ValueId one = create_const_u64(1);
        create_return(one);
    }
    
    return std::move(owned_function_);
}

Type Builder::infer_type(const query::Expression* expr) const {
    if (!expr) return Type::U64;
    switch (expr->type) {
        case query::ExprType::Literal:
            return Type::U64;
        case query::ExprType::ColumnRef: {
            auto col = static_cast<const query::ColumnRefExpr*>(expr);
            u32 cid = 0;
            Type ctype = Type::U64;
            if (resolve_column_info(col->name, cid, ctype)) {
                return ctype;
            }
            return Type::U64;
        }
        case query::ExprType::BinaryOp: {
            auto binop = static_cast<const query::BinaryOpExpr*>(expr);
            switch (binop->op) {
                case query::TokenType::Equal:
                case query::TokenType::NotEqual:
                case query::TokenType::Less:
                case query::TokenType::LessEqual:
                case query::TokenType::Greater:
                case query::TokenType::GreaterEqual:
                case query::TokenType::And:
                case query::TokenType::Or:
                    return Type::I1;
                default: {
                    Type lt = infer_type(binop->left.get());
                    Type rt = infer_type(binop->right.get());
                    if (lt == Type::I64 || rt == Type::I64) return Type::I64;
                    return Type::U64;
                }
            }
        }
        case query::ExprType::UnaryOp: {
            auto unop = static_cast<const query::UnaryOpExpr*>(expr);
            if (unop->op == query::TokenType::Bang) return Type::I1;
            if (unop->op == query::TokenType::Minus) return Type::I64;
            return infer_type(unop->operand.get());
        }
        default:
            return Type::U64;
    }
}

Opcode Builder::get_comparison_opcode(query::TokenType op, Type type) const {
    bool is_i64 = (type == Type::I64);
    switch (op) {
        case query::TokenType::Equal: return is_i64 ? Opcode::EqI64 : Opcode::EqU64;
        case query::TokenType::NotEqual: return is_i64 ? Opcode::NeI64 : Opcode::NeU64;
        case query::TokenType::Less: return is_i64 ? Opcode::LtI64 : Opcode::LtU64;
        case query::TokenType::LessEqual: return is_i64 ? Opcode::LeI64 : Opcode::LeU64;
        case query::TokenType::Greater: return is_i64 ? Opcode::GtI64 : Opcode::GtU64;
        case query::TokenType::GreaterEqual: return is_i64 ? Opcode::GeI64 : Opcode::GeU64;
        default: return Opcode::EqU64;
    }
}

Opcode Builder::get_arithmetic_opcode(query::TokenType op, Type type) const {
    bool is_i64 = (type == Type::I64);
    switch (op) {
        case query::TokenType::Plus: return is_i64 ? Opcode::AddI64 : Opcode::AddU64;
        case query::TokenType::Minus: return is_i64 ? Opcode::SubI64 : Opcode::SubU64;
        case query::TokenType::Star: return is_i64 ? Opcode::MulI64 : Opcode::MulU64;
        case query::TokenType::Slash: return is_i64 ? Opcode::DivI64 : Opcode::DivU64;
        case query::TokenType::Percent: return is_i64 ? Opcode::ModI64 : Opcode::ModU64;
        default: return Opcode::AddU64;
    }
}

} // namespace ir
} // namespace vectortick
