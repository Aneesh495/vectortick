#pragma once
#include "query_result.hpp"
#include "vectortick/model/event.hpp"
#include "vectortick/query/ast.hpp"
#include "vectortick/jit/jit_compiler.hpp"
#include <map>

namespace vectortick {
enum class QueryEngine { Reference, Vector, Native };
using NativeCache = std::map<std::string,std::shared_ptr<jit::JitCompiler>>;
Result<QueryResult> execute_query(const std::vector<CanonicalEvent>& events, const query::QueryStmt* ast,
                                  QueryEngine engine, NativeCache* cache = nullptr);
}
