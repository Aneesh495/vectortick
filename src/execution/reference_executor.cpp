#include "vectortick/execution/reference_executor.hpp"
#include "vectortick/execution/query_engine.hpp"
namespace vectortick {
Result<QueryResult> ReferenceExecutor::execute(SegmentReader& reader,const query::QueryStmt* ast) {
    if (!reader.is_open()) return make_error<QueryResult>(StatusCode::InvalidArgument,"Segment reader not open");
    std::vector<CanonicalEvent> events;
    auto st=reader.read_all_events(events);
    if (!st.ok()) return make_error<QueryResult>(st.code(),st.message());
    return execute_events(events,ast);
}
Result<QueryResult> ReferenceExecutor::execute_events(const std::vector<CanonicalEvent>& events,const query::QueryStmt* ast) {
    return execute_query(events,ast,QueryEngine::Reference);
}
}
