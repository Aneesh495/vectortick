#pragma once

#include "vectortick/common/types.hpp"
#include "vectortick/common/status.hpp"
#include "vectortick/common/result.hpp"
#include "vectortick/model/event.hpp"
#include "vectortick/storage/segment_reader.hpp"
#include "vectortick/query/ast.hpp"
#include "vectortick/execution/query_result.hpp"

#include <vector>

namespace vectortick {

class ReferenceExecutor {
public:
    ReferenceExecutor() = default;

    // Execute query against a segment file
    Result<QueryResult> execute(SegmentReader& reader,
                                const query::QueryStmt* query_ast);

    // Execute query against in-memory canonical events
    Result<QueryResult> execute_events(const std::vector<CanonicalEvent>& events,
                                       const query::QueryStmt* query_ast);
};

} // namespace vectortick
