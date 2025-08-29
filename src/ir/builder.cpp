#include "vectortick/ir/builder.hpp"
#include "vectortick/query/type_checker.hpp"
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

ValueId Builder::lower_expression(const query::Expression* expr) {
    using query::TokenType;
    if (!expr || !function_) return 0;
    auto unary = [&](Opcode op, ValueId v, Type type) {
        auto out = function_->create_value(type);
        current_block_->append(std::make_unique<UnaryOp>(op, v, out, type)); return out;
    };
    switch (expr->type) {
        case query::ExprType::Literal:
            return create_const_u64(static_cast<const query::LiteralExpr*>(expr)->value);
        case query::ExprType::ColumnRef: {
            vts1::ColumnID id;
            auto col = static_cast<const query::ColumnRefExpr*>(expr);
            if (!query::TypeChecker::resolve_column(col->name, id) || function_->parameters().empty()) return 0;
            return create_load_column(static_cast<u32>(id), function_->parameters()[0], infer_type(expr));
        }
        case query::ExprType::UnaryOp: {
            auto un = static_cast<const query::UnaryOpExpr*>(expr);
            auto v = lower_expression(un->operand.get());
            if (un->op == TokenType::Bang) return create_not(v);
            if (un->op == TokenType::Minus) return unary(infer_type(un->operand.get()) == Type::I64 ? Opcode::NegI64 : Opcode::NegU64, v, Type::I64);
            return 0;
        }
        case query::ExprType::BinaryOp: {
            auto bin = static_cast<const query::BinaryOpExpr*>(expr);
            auto left = lower_expression(bin->left.get());
            if (bin->op == TokenType::And || bin->op == TokenType::Or) {
                auto rhs_block = create_block("logical_rhs");
                auto short_block = create_block("logical_short");
                auto merge = create_block("logical_merge");
                bool is_and = bin->op == TokenType::And;
                create_branch(left, is_and ? rhs_block : short_block, is_and ? short_block : rhs_block);
                set_insertion_point(rhs_block);
                auto right = create_not(create_not(lower_expression(bin->right.get())));
                auto rhs_end = current_block_; create_jump(merge);
                set_insertion_point(short_block);
                auto short_value = create_const_bool(!is_and); create_jump(merge);
                set_insertion_point(merge);
                auto result = function_->create_value(Type::I1);
                current_block_->append(std::make_unique<PhiOp>(std::vector<BasicBlock*>{rhs_end, short_block},
                    std::vector<ValueId>{right, short_value}, result, Type::I1)); return result;
            }
            auto right = lower_expression(bin->right.get());
            auto lt = infer_type(bin->left.get()), rt = infer_type(bin->right.get());
            bool ls = lt == Type::I64, rs = rt == Type::I64;
            bool comparison = bin->op == TokenType::Equal || bin->op == TokenType::NotEqual ||
                bin->op == TokenType::Less || bin->op == TokenType::LessEqual || bin->op == TokenType::Greater || bin->op == TokenType::GreaterEqual;
            auto compare = [&](ValueId l, ValueId r, Type t) {
                auto result = function_->create_value(Type::I1);
                current_block_->append(std::make_unique<CompareOp>(get_comparison_opcode(bin->op,t), l,r,result)); return result;
            };
            if (comparison) {
                if (ls == rs) return compare(left,right,ls ? Type::I64 : Type::U64);
                auto negative = create_lt(ls ? left : right, create_const_i64(0), Type::I64);
                bool neg_answer = bin->op == TokenType::NotEqual ||
                    (ls && (bin->op == TokenType::Less || bin->op == TokenType::LessEqual)) ||
                    (rs && (bin->op == TokenType::Greater || bin->op == TokenType::GreaterEqual));
                auto normal = compare(left,right,Type::U64);
                return create_select(negative,create_const_bool(neg_answer),normal,Type::I1);
            }
            bool bitwise = bin->op == TokenType::Amp || bin->op == TokenType::Pipe || bin->op == TokenType::Caret;
            if (bitwise) {
                if (bin->op == TokenType::Amp) return create_bitand(left,right);
                if (bin->op == TokenType::Pipe) return create_bitor(left,right);
                return create_bitxor(left,right);
            }
            Type type = ls || rs ? Type::I64 : Type::U64;
            if (type == Type::I64) {
                if (!ls) left = unary(Opcode::U64ToI64,left,Type::I64);
                if (!rs) right = unary(Opcode::U64ToI64,right,Type::I64);
            }
            switch (bin->op) {
                case TokenType::Plus:return create_add(left,right,type);
                case TokenType::Minus:return create_sub(left,right,type);
                case TokenType::Star:return create_mul(left,right,type);
                case TokenType::Slash:return create_div(left,right,type);
                case TokenType::Percent:return create_mod(left,right,type);
                default:return 0;
            }
        }
        case query::ExprType::FunctionCall: {
            auto call = static_cast<const query::FunctionCallExpr*>(expr);
            std::string name = call->name;
            for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (name == "count") return create_count(function_->parameters()[0]);
            if (call->args.size() != 1) return 0;
            auto v = lower_expression(call->args[0].get()); auto t = infer_type(call->args[0].get());
            if (name == "sum") return create_sum(v,t == Type::I64 ? Type::I64 : Type::U64);
            if (name == "min") return create_min(v,t);
            if (name == "max") return create_max(v,t);
            return 0;
        }
        default:return 0;
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
    query::TypeChecker checker;
    auto type = checker.check_expression(expr);
    if (!type.ok()) return Type::Void;
    switch (type.value()) {
        case vts1::ColumnType::I64:return Type::I64;
        case vts1::ColumnType::U32:return Type::U32;
        case vts1::ColumnType::U16:return Type::U16;
        case vts1::ColumnType::U8:return Type::U8;
        case vts1::ColumnType::Bool:return Type::I1;
        default:return Type::U64;
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
