#include "../test_framework.hpp"
#include "vectortick/execution/reference_interpreter.hpp"
#include "vectortick/ir/builder.hpp"
#include "vectortick/model/event.hpp"

using namespace vectortick;
using namespace vectortick::ir;

VT_TEST(reference_executor_tests, scalar_arithmetic_100_plus_200) {
    Builder builder;
    Function* fn = builder.create_function("add_100_200");
    ValueId c1 = builder.create_const_u64(100);
    ValueId c2 = builder.create_const_u64(200);
    ValueId sum = builder.create_add(c1, c2, Type::U64);
    builder.create_return(sum);
    
    CanonicalEvent evt = event::make_empty_event();
    ReferenceInterpreter interp;
    auto res = interp.execute(fn, &evt);
    VT_ASSERT(res.ok());
    VT_ASSERT_EQ(res.value(), 300ULL);
}

VT_TEST(reference_executor_tests, signed_price_comparison) {
    Builder builder;
    Function* fn = builder.create_function("check_price");
    ValueId param = fn->create_value(Type::U64, "row");
    fn->add_parameter(param);
    ValueId price = builder.create_load_column(7, param, Type::I64); // col 7 = price_ticks
    ValueId threshold = builder.create_const_i64(-500);
    ValueId cond = builder.create_gt(price, threshold, Type::I64);
    builder.create_return(cond);
    
    ReferenceInterpreter interp;
    
    // -100 > -500 is true
    CanonicalEvent evt1 = event::make_empty_event();
    evt1.price_ticks = -100;
    auto res1 = interp.execute(fn, &evt1);
    VT_ASSERT(res1.ok());
    VT_ASSERT_EQ(res1.value(), 1ULL);
    
    // -1000 > -500 is false
    CanonicalEvent evt2 = event::make_empty_event();
    evt2.price_ticks = -1000;
    auto res2 = interp.execute(fn, &evt2);
    VT_ASSERT(res2.ok());
    VT_ASSERT_EQ(res2.value(), 0ULL);
}

VT_TEST(reference_executor_tests, branch_and_jump_execution) {
    Builder builder;
    Function* fn = builder.create_function("branch_exec");
    BasicBlock* entry = fn->entry_block();
    BasicBlock* then_b = builder.create_block("then");
    BasicBlock* else_b = builder.create_block("else");
    BasicBlock* merge_b = builder.create_block("merge");
    
    builder.set_insertion_point(entry);
    ValueId cond = builder.create_const_bool(false);
    builder.create_branch(cond, then_b, else_b);
    
    builder.set_insertion_point(then_b);
    builder.create_jump(merge_b);
    
    builder.set_insertion_point(else_b);
    builder.create_jump(merge_b);
    
    builder.set_insertion_point(merge_b);
    ValueId ret_val = builder.create_const_u64(999);
    builder.create_return(ret_val);
    
    CanonicalEvent evt = event::make_empty_event();
    ReferenceInterpreter interp;
    auto res = interp.execute(fn, &evt);
    VT_ASSERT(res.ok());
    VT_ASSERT_EQ(res.value(), 999ULL);
}

VT_TEST(reference_executor_tests, error_on_missing_ssa_value) {
    Builder builder;
    Function* fn = builder.create_function("missing_ssa");
    // Return an undefined ValueId 9999
    builder.create_return(9999);
    
    CanonicalEvent evt = event::make_empty_event();
    ReferenceInterpreter interp;
    auto res = interp.execute(fn, &evt);
    VT_ASSERT(!res.ok());
    VT_ASSERT(res.status().code() == StatusCode::InternalError);
}

VT_TEST(reference_executor_tests, cross_row_aggregates) {
    std::vector<CanonicalEvent> events(5);
    for (size_t i = 0; i < 5; ++i) {
        events[i] = event::make_empty_event();
        events[i].quantity = static_cast<u32>((i + 1) * 10); // 10, 20, 30, 40, 50
    }
    
    // SUM(quantity)
    Builder builder;
    Function* sum_fn = builder.create_function("sum_qty");
    ValueId param = sum_fn->create_value(Type::U64, "row");
    sum_fn->add_parameter(param);
    ValueId qty = builder.create_load_column(8, param, Type::U32); // col 8 = quantity
    ValueId sum = builder.create_sum(qty, Type::U64);
    builder.create_return(sum);
    
    ReferenceInterpreter interp;
    auto res = interp.execute_aggregate(sum_fn, events);
    VT_ASSERT(res.ok());
    VT_ASSERT_EQ(res.value(), 150ULL); // 10+20+30+40+50 = 150
}

#include "vectortick/execution/reference_executor.hpp"
#include "vectortick/query/parser.hpp"
#include "../oracle/query_oracle.hpp"

VT_TEST(reference_executor_tests, query_plan_validation) {
    std::vector<CanonicalEvent> events(10);
    for (size_t i = 0; i < 10; ++i) events[i] = event::make_empty_event();

    query::Parser p("SELECT invalid_column_xyz WHERE price_ticks > 0");
    auto q = p.parse_query();
    VT_ASSERT(q.ok());

    ReferenceExecutor ref;
    auto res = ref.execute_events(events, q.value().get());
    VT_ASSERT(!res.ok());
}

VT_TEST(reference_executor_tests, query_projections_and_filters) {
    std::vector<CanonicalEvent> events;
    for (size_t i = 0; i < 100; ++i) {
        CanonicalEvent ev = event::make_empty_event();
        ev.sequence = i + 1;
        ev.instrument_id = (i % 2 == 0) ? 1001 : 1002;
        ev.price_ticks = 10000 + static_cast<i64>(i * 100);
        ev.quantity = static_cast<u32>(10 + i);
        events.push_back(ev);
    }

    query::Parser p("SELECT instrument_id, price_ticks, quantity WHERE price_ticks > 15000 LIMIT 5");
    auto q = p.parse_query();
    VT_ASSERT(q.ok());

    ReferenceExecutor ref;
    auto res = ref.execute_events(events, q.value().get());
    VT_ASSERT(res.ok());

    const auto& qr = res.value();
    VT_ASSERT_EQ(qr.rows_scanned, 100ULL);
    VT_ASSERT_EQ(qr.rows_matched, 49ULL);
    VT_ASSERT_EQ(qr.rows.size(), 5ULL);
    VT_ASSERT_EQ(qr.column_types.size(), 3ULL);
    VT_ASSERT(qr.column_types[0] == vts1::ColumnType::U32);
    VT_ASSERT(qr.column_types[1] == vts1::ColumnType::I64);
    VT_ASSERT(qr.column_types[2] == vts1::ColumnType::U32);
}

VT_TEST(reference_executor_tests, query_aggregates_and_group_by) {
    std::vector<CanonicalEvent> events;
    for (size_t i = 0; i < 100; ++i) {
        CanonicalEvent ev = event::make_empty_event();
        ev.sequence = i + 1;
        ev.instrument_id = (i % 2 == 0) ? 1001 : 1002;
        ev.price_ticks = 10000 + static_cast<i64>(i);
        ev.quantity = 10;
        events.push_back(ev);
    }

    query::Parser p("SELECT instrument_id, COUNT(*), SUM(quantity) GROUP BY instrument_id ORDER BY instrument_id ASC");
    auto q = p.parse_query();
    VT_ASSERT(q.ok());

    ReferenceExecutor ref;
    auto res = ref.execute_events(events, q.value().get());
    VT_ASSERT(res.ok());

    const auto& qr = res.value();
    VT_ASSERT_EQ(qr.rows.size(), 2ULL);
    VT_ASSERT_EQ(qr.rows[0][0], "1001");
    VT_ASSERT_EQ(qr.rows[0][1], "50");  // count
    VT_ASSERT_EQ(qr.rows[0][2], "500"); // sum(qty) = 50 * 10
    VT_ASSERT_EQ(qr.rows[1][0], "1002");
    VT_ASSERT_EQ(qr.rows[1][1], "50");
    VT_ASSERT_EQ(qr.rows[1][2], "500");
}

VT_TEST(reference_executor_tests, checked_math_division_by_zero) {
    std::vector<CanonicalEvent> events(5);
    for (size_t i = 0; i < 5; ++i) {
        events[i] = event::make_empty_event();
        events[i].price_ticks = 100;
    }

    query::Parser p("SELECT instrument_id WHERE price_ticks / 0 > 1");
    auto q = p.parse_query();
    VT_ASSERT(q.ok());

    ReferenceExecutor ref;
    auto res = ref.execute_events(events, q.value().get());
    VT_ASSERT(!res.ok());
    VT_ASSERT(res.status().code() == StatusCode::InternalError);
}

VT_TEST(reference_executor_tests, oracle_differential_crosscheck) {
    std::vector<CanonicalEvent> events;
    for (size_t i = 0; i < 200; ++i) {
        CanonicalEvent ev = event::make_empty_event();
        ev.sequence = i + 1;
        ev.instrument_id = (i % 3 == 0) ? 100 : ((i % 3 == 1) ? 200 : 300);
        ev.event_type = EventType::Trade;
        ev.side = Side::Bid;
        ev.price_ticks = 5000 + static_cast<i64>(i * 25);
        ev.quantity = static_cast<u32>(1 + (i % 10));
        events.push_back(ev);
    }

    const char* queries[] = {
        "SELECT instrument_id, price_ticks, quantity WHERE price_ticks > 6000 LIMIT 20",
        "SELECT COUNT(*), SUM(quantity), MIN(price_ticks), MAX(price_ticks) WHERE instrument_id = 200",
        "SELECT instrument_id, price_ticks ORDER BY price_ticks DESC LIMIT 15",
        "SELECT instrument_id, COUNT(*), SUM(quantity) GROUP BY instrument_id ORDER BY instrument_id ASC",
        "SELECT * WHERE price_ticks >= 7000 AND quantity > 5 LIMIT 10"
    };

    ReferenceExecutor ref;

    for (const char* sql : queries) {
        query::Parser p(sql);
        auto q = p.parse_query();
        VT_ASSERT(q.ok());

        auto ref_res = ref.execute_events(events, q.value().get());
        VT_ASSERT(ref_res.ok());

        auto oracle_res = test::QueryOracle::evaluate(events, q.value().get());
        VT_ASSERT(oracle_res.ok());

        const auto& r = ref_res.value();
        const auto& o = oracle_res.value();

        VT_ASSERT_EQ(r.rows_scanned, o.rows_scanned);
        VT_ASSERT_EQ(r.rows_matched, o.rows_matched);
        VT_ASSERT_EQ(r.column_names.size(), o.column_names.size());
        for (size_t c = 0; c < r.column_names.size(); ++c) {
            VT_ASSERT_EQ(r.column_names[c], o.column_names[c]);
            VT_ASSERT(r.column_types[c] == o.column_types[c]);
        }
        VT_ASSERT_EQ(r.rows.size(), o.rows.size());
        for (size_t row_i = 0; row_i < r.rows.size(); ++row_i) {
            VT_ASSERT_EQ(r.rows[row_i].size(), o.rows[row_i].size());
            for (size_t col_i = 0; col_i < r.rows[row_i].size(); ++col_i) {
                VT_ASSERT_EQ(r.rows[row_i][col_i], o.rows[row_i][col_i]);
            }
        }
    }
}
