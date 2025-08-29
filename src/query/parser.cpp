#include "vectortick/query/parser.hpp"
#include <sstream>

namespace vectortick {
namespace query {

Token Parser::expect(TokenType type, const char* msg) noexcept {
    if (check(type)) {
        return advance();
    }
    
    Token tok = current_token();
    std::stringstream ss;
    ss << msg << ", got " << token_type_name(tok.type);
    set_error(ss.str().c_str(), tok.line, tok.column);
    return tok;
}

void Parser::set_error(const char* msg, usize line, usize col) noexcept {
    if (!has_error_) {
        has_error_ = true;
        error_msg_ = msg;
        error_line_ = line;
        error_column_ = col;
    }
}

Result<std::unique_ptr<QueryStmt>> Parser::parse_query() noexcept {
    Token tok = current_token();
    auto query = std::make_unique<QueryStmt>(tok.line, tok.column);
    
    // Support either starting with SELECT or starting with FROM
    if (match(TokenType::Select)) {
        // Parse projection list
        do {
            if (match(TokenType::Star)) {
                query->projections.emplace_back(nullptr, "*", true);
            } else {
                auto expr = parse_expression();
                if (!expr || has_error_) break;
                
                std::string alias;
                if (match(TokenType::As)) {
                    Token a = expect(TokenType::Identifier, "Expected alias after AS");
                    if (has_error_) break;
                    alias = std::string(a.text);
                } else if (check(TokenType::Identifier) && 
                           current_token().type != TokenType::From &&
                           current_token().type != TokenType::Where &&
                           current_token().type != TokenType::Group &&
                           current_token().type != TokenType::Order &&
                           current_token().type != TokenType::Limit) {
                    alias = std::string(advance().text);
                }
                
                query->projections.emplace_back(std::move(expr), alias, false);
            }
        } while (match(TokenType::Comma) && !has_error_);
        
        if (has_error_) {
            return make_error<std::unique_ptr<QueryStmt>>(StatusCode::ParserError, error_msg_);
        }
        
        // Optional FROM clause
        if (match(TokenType::From)) {
            Token table = expect(TokenType::Identifier, "Expected table name");
            if (has_error_) {
                return make_error<std::unique_ptr<QueryStmt>>(StatusCode::ParserError, error_msg_);
            }
            query->table_name = std::string(table.text);
        } else {
            query->table_name = "events";
        }
    } else if (match(TokenType::From)) {
        Token table = expect(TokenType::Identifier, "Expected table name");
        if (has_error_) {
            return make_error<std::unique_ptr<QueryStmt>>(StatusCode::ParserError, error_msg_);
        }
        query->table_name = std::string(table.text);
    } else {
        set_error("Expected SELECT or FROM", tok.line, tok.column);
        return make_error<std::unique_ptr<QueryStmt>>(StatusCode::ParserError, error_msg_);
    }
    
    // Optional clauses
    while (!check(TokenType::Eof) && !has_error_) {
        if (check(TokenType::Select)) {
            advance();
            do {
                if (match(TokenType::Star)) {
                    query->projections.emplace_back(nullptr, "*", true);
                } else {
                    auto expr = parse_expression();
                    if (!expr || has_error_) break;
                    
                    std::string alias;
                    if (match(TokenType::As)) {
                        Token a = expect(TokenType::Identifier, "Expected alias after AS");
                        if (has_error_) break;
                        alias = std::string(a.text);
                    }
                    query->projections.emplace_back(std::move(expr), alias, false);
                }
            } while (match(TokenType::Comma) && !has_error_);
        }
        else if (check(TokenType::Where)) {
            advance();
            query->where_expr = parse_expression();
            if (has_error_) break;
        }
        else if (check(TokenType::Let)) {
            advance();
            Token name = expect(TokenType::Identifier, "Expected variable name");
            if (has_error_) break;
            
            if (!match(TokenType::Equal)) {
                set_error("Expected '=' after variable name", current_token().line, current_token().column);
                break;
            }
            
            auto expr = parse_expression();
            if (has_error_) break;
            
            query->let_bindings.emplace_back(std::string(name.text), std::move(expr));
        }
        else if (check(TokenType::Group)) {
            advance();
            if (!match(TokenType::By)) {
                set_error("Expected BY after GROUP", current_token().line, current_token().column);
                break;
            }
            
            // Parse group by columns
            do {
                Token col = expect(TokenType::Identifier, "Expected column name");
                if (has_error_) break;
                query->group_by_columns.emplace_back(col.text);
            } while (match(TokenType::Comma) && !has_error_);
            
            // Optional TUMBLE window
            if (match(TokenType::Tumble)) {
                if (!match(TokenType::LParen)) {
                    set_error("Expected '(' after TUMBLE", current_token().line, current_token().column);
                    break;
                }
                
                query->tumble_window = parse_expression();
                if (has_error_) break;
                
                if (!match(TokenType::RParen)) {
                    set_error("Expected ')' after TUMBLE expression", current_token().line, current_token().column);
                    break;
                }
            }
        }
        else if (check(TokenType::Agg)) {
            advance();
            
            // Parse aggregations
            do {
                Token name;
                if (check(TokenType::Identifier) || check(TokenType::Count) || check(TokenType::Sum) ||
                    check(TokenType::Min) || check(TokenType::Max) || check(TokenType::Avg)) {
                    name = advance();
                } else {
                    set_error("Expected aggregation function", current_token().line, current_token().column);
                    break;
                }
                
                if (!match(TokenType::LParen)) {
                    set_error("Expected '(' after aggregation function", current_token().line, current_token().column);
                    break;
                }
                
                // Parse arguments (or empty for count())
                std::string agg_name(name.text);
                std::unique_ptr<Expression> arg;
                
                if (match(TokenType::Star)) {
                    // Wildcard count(*)
                } else if (!check(TokenType::RParen)) {
                    arg = parse_expression();
                    if (has_error_) break;
                }
                
                if (!match(TokenType::RParen)) {
                    set_error("Expected ')' after aggregation arguments", current_token().line, current_token().column);
                    break;
                }
                
                // Create function call expression
                auto func_call = std::make_unique<FunctionCallExpr>(agg_name, name.line, name.column);
                if (arg) {
                    func_call->args.push_back(std::move(arg));
                }
                
                query->aggregations.emplace_back(agg_name, std::move(func_call));
                
            } while (match(TokenType::Comma) && !has_error_);
        }
        else if (check(TokenType::Order)) {
            advance();
            if (!match(TokenType::By)) {
                set_error("Expected BY after ORDER", current_token().line, current_token().column);
                break;
            }
            
            // Parse order by columns
            do {
                Token col = expect(TokenType::Identifier, "Expected column name");
                if (has_error_) break;
                
                bool ascending = true;
                if (match(TokenType::Asc)) {
                    ascending = true;
                } else if (match(TokenType::Desc)) {
                    ascending = false;
                } else if (check(TokenType::Identifier)) {
                    Token dir = current_token();
                    if (dir.text == "ASC" || dir.text == "asc") {
                        advance();
                        ascending = true;
                    } else if (dir.text == "DESC" || dir.text == "desc") {
                        advance();
                        ascending = false;
                    }
                }
                
                query->order_by.emplace_back(std::string(col.text), ascending);
            } while (match(TokenType::Comma) && !has_error_);
        }
        else if (check(TokenType::Limit)) {
            advance();
            Token limit_tok = expect(TokenType::IntegerLiteral, "Expected limit value");
            if (has_error_) break;
            query->limit = limit_tok.int_value;
            if (query->limit == 0) set_error("LIMIT must be positive",limit_tok.line,limit_tok.column);
        }
        else {
            // Unknown clause
            set_error("Unexpected token in query", current_token().line, current_token().column);
            break;
        }
    }
    
    if (has_error_) {
        return make_error<std::unique_ptr<QueryStmt>>(StatusCode::ParserError, error_msg_);
    }
    
    return query;
}

std::unique_ptr<Expression> Parser::parse_expression() noexcept {
    return parse_or_expr();
}

std::unique_ptr<Expression> Parser::parse_or_expr() noexcept {
    auto left = parse_and_expr();
    if (!left || has_error_) return left;
    
    while (match(TokenType::Or)) {
        usize line = current_token().line;
        usize col = current_token().column;
        auto right = parse_and_expr();
        if (!right || has_error_) return nullptr;
        
        left = std::make_unique<BinaryOpExpr>(TokenType::Or, std::move(left), std::move(right), line, col);
    }
    
    return left;
}

std::unique_ptr<Expression> Parser::parse_and_expr() noexcept {
    auto left = parse_bitwise_or_expr();
    if (!left || has_error_) return left;
    
    while (match(TokenType::And)) {
        usize line = current_token().line;
        usize col = current_token().column;
        auto right = parse_bitwise_or_expr();
        if (!right || has_error_) return nullptr;
        
        left = std::make_unique<BinaryOpExpr>(TokenType::And, std::move(left), std::move(right), line, col);
    }
    
    return left;
}

std::unique_ptr<Expression> Parser::parse_bitwise_or_expr() noexcept {
    auto left = parse_bitwise_xor_expr();
    if (!left || has_error_) return left;
    
    while (match(TokenType::Pipe)) {
        usize line = current_token().line;
        usize col = current_token().column;
        auto right = parse_bitwise_xor_expr();
        if (!right || has_error_) return nullptr;
        
        left = std::make_unique<BinaryOpExpr>(TokenType::Pipe, std::move(left), std::move(right), line, col);
    }
    
    return left;
}

std::unique_ptr<Expression> Parser::parse_bitwise_xor_expr() noexcept {
    auto left = parse_bitwise_and_expr();
    if (!left || has_error_) return left;
    
    while (match(TokenType::Caret)) {
        usize line = current_token().line;
        usize col = current_token().column;
        auto right = parse_bitwise_and_expr();
        if (!right || has_error_) return nullptr;
        
        left = std::make_unique<BinaryOpExpr>(TokenType::Caret, std::move(left), std::move(right), line, col);
    }
    
    return left;
}

std::unique_ptr<Expression> Parser::parse_bitwise_and_expr() noexcept {
    auto left = parse_equality_expr();
    if (!left || has_error_) return left;
    
    while (match(TokenType::Amp)) {
        usize line = current_token().line;
        usize col = current_token().column;
        auto right = parse_equality_expr();
        if (!right || has_error_) return nullptr;
        
        left = std::make_unique<BinaryOpExpr>(TokenType::Amp, std::move(left), std::move(right), line, col);
    }
    
    return left;
}

std::unique_ptr<Expression> Parser::parse_equality_expr() noexcept {
    auto left = parse_comparison_expr();
    if (!left || has_error_) return left;
    
    while (check(TokenType::Equal) || check(TokenType::NotEqual)) {
        Token op = advance();
        auto right = parse_comparison_expr();
        if (!right || has_error_) return nullptr;
        
        left = std::make_unique<BinaryOpExpr>(op.type, std::move(left), std::move(right), op.line, op.column);
    }
    
    return left;
}

std::unique_ptr<Expression> Parser::parse_comparison_expr() noexcept {
    auto left = parse_additive_expr();
    if (!left || has_error_) return left;
    
    while (check(TokenType::Less) || check(TokenType::LessEqual) ||
           check(TokenType::Greater) || check(TokenType::GreaterEqual)) {
        Token op = advance();
        auto right = parse_additive_expr();
        if (!right || has_error_) return nullptr;
        
        left = std::make_unique<BinaryOpExpr>(op.type, std::move(left), std::move(right), op.line, op.column);
    }
    
    return left;
}

std::unique_ptr<Expression> Parser::parse_additive_expr() noexcept {
    auto left = parse_multiplicative_expr();
    if (!left || has_error_) return left;
    
    while (check(TokenType::Plus) || check(TokenType::Minus)) {
        Token op = advance();
        auto right = parse_multiplicative_expr();
        if (!right || has_error_) return nullptr;
        
        left = std::make_unique<BinaryOpExpr>(op.type, std::move(left), std::move(right), op.line, op.column);
    }
    
    return left;
}

std::unique_ptr<Expression> Parser::parse_multiplicative_expr() noexcept {
    auto left = parse_unary_expr();
    if (!left || has_error_) return left;
    
    while (check(TokenType::Star) || check(TokenType::Slash) || check(TokenType::Percent)) {
        Token op = advance();
        auto right = parse_unary_expr();
        if (!right || has_error_) return nullptr;
        
        left = std::make_unique<BinaryOpExpr>(op.type, std::move(left), std::move(right), op.line, op.column);
    }
    
    return left;
}

std::unique_ptr<Expression> Parser::parse_unary_expr() noexcept {
    if (check(TokenType::Minus) || check(TokenType::Bang)) {
        Token op = advance();
        auto operand = parse_unary_expr();
        if (!operand || has_error_) return nullptr;
        
        return std::make_unique<UnaryOpExpr>(op.type, std::move(operand), op.line, op.column);
    }
    
    return parse_primary_expr();
}

std::unique_ptr<Expression> Parser::parse_primary_expr() noexcept {
    Token tok = current_token();
    
    // Integer literal
    if (match(TokenType::IntegerLiteral)) {
        return std::make_unique<LiteralExpr>(tok.int_value, tok.line, tok.column);
    }
    
    // Boolean literals
    if (match(TokenType::True)) {
        return std::make_unique<LiteralExpr>(1, tok.line, tok.column);
    }
    if (match(TokenType::False)) {
        return std::make_unique<LiteralExpr>(0, tok.line, tok.column);
    }
    
    // Parenthesized expression
    if (match(TokenType::LParen)) {
        auto expr = parse_expression();
        if (!expr || has_error_) return nullptr;
        
        if (!match(TokenType::RParen)) {
            set_error("Expected ')' after expression", current_token().line, current_token().column);
            return nullptr;
        }
        
        return expr;
    }
    
    // Keywords used as function calls or identifiers (count, sum, min, max, avg)
    if (check(TokenType::Count) || check(TokenType::Sum) || check(TokenType::Min) ||
        check(TokenType::Max) || check(TokenType::Avg)) {
        Token name = advance();
        if (check(TokenType::LParen)) {
            return parse_function_call(std::string(name.text), name.line, name.column);
        }
        return std::make_unique<ColumnRefExpr>(std::string(name.text), name.line, name.column);
    }
    
    // Identifier (column ref or function call)
    if (check(TokenType::Identifier)) {
        Token name = advance();
        
        // Check for function call
        if (check(TokenType::LParen)) {
            return parse_function_call(std::string(name.text), name.line, name.column);
        }
        
        // Column reference
        return std::make_unique<ColumnRefExpr>(std::string(name.text), name.line, name.column);
    }
    
    set_error("Expected expression", tok.line, tok.column);
    return nullptr;
}

std::unique_ptr<Expression> Parser::parse_function_call(const std::string& name, usize line, usize col) noexcept {
    advance();  // consume '('
    
    auto call = std::make_unique<FunctionCallExpr>(name, line, col);
    
    // Parse arguments (or empty / star for count(*))
    if (match(TokenType::Star)) {
        // count(*)
    } else if (!check(TokenType::RParen)) {
        do {
            auto arg = parse_expression();
            if (!arg || has_error_) return nullptr;
            call->args.push_back(std::move(arg));
        } while (match(TokenType::Comma) && !has_error_);
    }
    
    if (!match(TokenType::RParen)) {
        set_error("Expected ')' after function arguments", current_token().line, current_token().column);
        return nullptr;
    }
    
    return call;
}

} // namespace query
} // namespace vectortick
