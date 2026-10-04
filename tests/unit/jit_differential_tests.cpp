#include "../test_framework.hpp"
#include "vectortick/jit/jit_compiler.hpp"
#include "vectortick/jit/code_generator.hpp"
#include "vectortick/execution/reference_interpreter.hpp"
#include "vectortick/execution/jit_executor.hpp"
#include "vectortick/execution/reference_executor.hpp"
#include "vectortick/query/parser.hpp"
#include "../oracle/query_oracle.hpp"
#include "vectortick/ir/builder.hpp"

using namespace vectortick;
using namespace vectortick::test;

VT_TEST(jit_differential_tests, host_jit_arithmetic_100_plus_200) {
    ir::Builder builder;
    ir::Function* func = builder.create_function("add_const");
    VT_ASSERT(func != nullptr);

    ir::ValueId c1 = builder.create_const_u64(100);
    ir::ValueId c2 = builder.create_const_u64(200);
    ir::ValueId sum = builder.create_add(c1, c2, ir::Type::U64);
    builder.create_return(sum);

    // 1. Reference interpreter
    CanonicalEvent evt = event::make_empty_event();
    ReferenceInterpreter interp;
    auto interp_res = interp.execute(func, &evt);
    VT_ASSERT(interp_res.ok());
    VT_ASSERT_EQ(interp_res.value(), 300ULL);

    // 2. JIT compiler on Host architecture
    auto jit_res = jit::jit_execute(func, &evt);
    VT_ASSERT(jit_res.ok());
    VT_ASSERT_EQ(jit_res.value(), 300ULL);

    // 3. Differential equivalence
    VT_ASSERT_EQ(jit_res.value(), interp_res.value());

    // 4. Both code generators succeed
    auto x86_gen = jit::create_generator(jit::TargetArch::X86_64);
    VT_ASSERT(x86_gen != nullptr);
    auto x86_res = x86_gen->generate(func);
    VT_ASSERT(x86_res.ok());
    VT_ASSERT(x86_res.value().size() > 0);

    auto a64_gen = jit::create_generator(jit::TargetArch::AArch64);
    VT_ASSERT(a64_gen != nullptr);
    auto a64_res = a64_gen->generate(func);
    VT_ASSERT(a64_res.ok());
    VT_ASSERT(a64_res.value().size() > 0);
}

VT_TEST(jit_differential_tests, host_jit_all_twelve_columns) {
    CanonicalEvent ev{};
    ev.exchange_ts_ns = 1234567890123ULL;
    ev.receive_ts_ns  = 2345678901234ULL;
    ev.sequence       = 987654321ULL;
    ev.instrument_id  = 42U;
    ev.event_type     = EventType::Trade;
    ev.side           = Side::Ask;
    ev.flags          = 0x1234;
    ev.price_ticks    = 555000LL;
    ev.quantity       = 750U;
    ev.venue_id       = 33;
    ev.source_id      = 88;
    ev.trade_or_order_id = 999888777666ULL;

    const u64 expected_values[12] = {
        1234567890123ULL,
        2345678901234ULL,
        987654321ULL,
        42ULL,
        static_cast<u64>(EventType::Trade),
        static_cast<u64>(Side::Ask),
        0x1234ULL,
        555000ULL,
        750ULL,
        33ULL,
        88ULL,
        999888777666ULL
    };

    const ir::Type column_types[12] = {
        ir::Type::U64, ir::Type::U64, ir::Type::U64, ir::Type::U32,
        ir::Type::U8,  ir::Type::U8,  ir::Type::U16, ir::Type::I64,
        ir::Type::U32, ir::Type::U16, ir::Type::U16, ir::Type::U64
    };

    ReferenceInterpreter interp;

    for (u32 col = 0; col < 12; ++col) {
        ir::Builder builder;
        ir::Function* func = builder.create_function("load_col");
        VT_ASSERT(func != nullptr);
        func->add_parameter(func->create_value(ir::Type::U64, "ctx"));

        ir::ValueId val = builder.create_load_column(col, func->parameters()[0], column_types[col]);
        builder.create_return(val);

        auto interp_res = interp.execute(func, &ev);
        VT_ASSERT(interp_res.ok());
        VT_ASSERT_EQ(interp_res.value(), expected_values[col]);

        auto jit_res = jit::jit_execute(func, &ev);
        VT_ASSERT(jit_res.ok());
        VT_ASSERT_EQ(jit_res.value(), expected_values[col]);

        // Cross-generator verification
        auto x86_gen = jit::create_generator(jit::TargetArch::X86_64);
        VT_ASSERT(x86_gen->generate(func).ok());
        auto a64_gen = jit::create_generator(jit::TargetArch::AArch64);
        VT_ASSERT(a64_gen->generate(func).ok());
    }
}

VT_TEST(jit_differential_tests, host_jit_signed_and_unsigned_comparisons) {
    CanonicalEvent ev{};
    ev.price_ticks = -500LL;

    // Test 1: Signed comparison: price (-500) < 100 should be true (1)
    {
        ir::Builder builder;
        ir::Function* func = builder.create_function("cmp_signed_lt");
        func->add_parameter(func->create_value(ir::Type::U64, "ctx"));
        ir::ValueId price = builder.create_load_column(7, func->parameters()[0], ir::Type::I64);
        ir::ValueId thresh = builder.create_const_i64(100);
        ir::ValueId cmp = builder.create_lt(price, thresh, ir::Type::I64);
        builder.create_return(cmp);

        ReferenceInterpreter interp;
        auto interp_res = interp.execute(func, &ev);
        VT_ASSERT(interp_res.ok());
        VT_ASSERT_EQ(interp_res.value(), 1ULL);

        auto jit_res = jit::jit_execute(func, &ev);
        VT_ASSERT(jit_res.ok());
        VT_ASSERT_EQ(jit_res.value(), 1ULL);

        VT_ASSERT(jit::create_generator(jit::TargetArch::X86_64)->generate(func).ok());
        VT_ASSERT(jit::create_generator(jit::TargetArch::AArch64)->generate(func).ok());
    }

    // Test 2: Unsigned comparison: price (reinterpreted as huge unsigned ~1.8e19) < 100 should be false (0)
    {
        ir::Builder builder;
        ir::Function* func = builder.create_function("cmp_unsigned_lt");
        func->add_parameter(func->create_value(ir::Type::U64, "ctx"));
        ir::ValueId price = builder.create_load_column(7, func->parameters()[0], ir::Type::U64);
        ir::ValueId thresh = builder.create_const_u64(100);
        ir::ValueId cmp = builder.create_lt(price, thresh, ir::Type::U64);
        builder.create_return(cmp);

        ReferenceInterpreter interp;
        auto interp_res = interp.execute(func, &ev);
        VT_ASSERT(interp_res.ok());
        VT_ASSERT_EQ(interp_res.value(), 0ULL);

        auto jit_res = jit::jit_execute(func, &ev);
        VT_ASSERT(jit_res.ok());
        VT_ASSERT_EQ(jit_res.value(), 0ULL);
    }

    // Test 3: Signed greater-than: price (-500) > 100 should be false (0)
    {
        ir::Builder builder;
        ir::Function* func = builder.create_function("cmp_signed_gt");
        func->add_parameter(func->create_value(ir::Type::U64, "ctx"));
        ir::ValueId price = builder.create_load_column(7, func->parameters()[0], ir::Type::I64);
        ir::ValueId thresh = builder.create_const_i64(100);
        ir::ValueId cmp = builder.create_gt(price, thresh, ir::Type::I64);
        builder.create_return(cmp);

        ReferenceInterpreter interp;
        VT_ASSERT_EQ(interp.execute(func, &ev).value(), 0ULL);
        VT_ASSERT_EQ(jit::jit_execute(func, &ev).value(), 0ULL);
    }

    // Test 4: Signed equality & inequality
    {
        ir::Builder builder;
        ir::Function* func = builder.create_function("cmp_signed_eq");
        func->add_parameter(func->create_value(ir::Type::U64, "ctx"));
        ir::ValueId price = builder.create_load_column(7, func->parameters()[0], ir::Type::I64);
        ir::ValueId val_neg500 = builder.create_const_i64(-500);
        ir::ValueId cmp = builder.create_eq(price, val_neg500, ir::Type::I64);
        builder.create_return(cmp);

        ReferenceInterpreter interp;
        VT_ASSERT_EQ(interp.execute(func, &ev).value(), 1ULL);
        VT_ASSERT_EQ(jit::jit_execute(func, &ev).value(), 1ULL);
    }
}

VT_TEST(jit_differential_tests, host_jit_division_modulo_and_zero_safety) {
    CanonicalEvent ev = event::make_empty_event();
    ReferenceInterpreter interp;

    // Normal unsigned div & mod: 100 / 7 = 14, 100 % 7 = 2
    {
        ir::Builder builder;
        ir::Function* func = builder.create_function("div_u64_normal");
        ir::ValueId a = builder.create_const_u64(100);
        ir::ValueId b = builder.create_const_u64(7);
        ir::ValueId q = builder.create_div(a, b, ir::Type::U64);
        builder.create_return(q);

        VT_ASSERT_EQ(interp.execute(func, &ev).value(), 14ULL);
        VT_ASSERT_EQ(jit::jit_execute(func, &ev).value(), 14ULL);
    }
    {
        ir::Builder builder;
        ir::Function* func = builder.create_function("mod_u64_normal");
        ir::ValueId a = builder.create_const_u64(100);
        ir::ValueId b = builder.create_const_u64(7);
        ir::ValueId r = builder.create_mod(a, b, ir::Type::U64);
        builder.create_return(r);

        VT_ASSERT_EQ(interp.execute(func, &ev).value(), 2ULL);
        VT_ASSERT_EQ(jit::jit_execute(func, &ev).value(), 2ULL);
    }

    // Zero divisor: 100 / 0 = 0, 100 % 0 = 0 (graceful zero, no hardware exception)
    {
        ir::Builder builder;
        ir::Function* func = builder.create_function("div_u64_zero");
        ir::ValueId a = builder.create_const_u64(100);
        ir::ValueId b = builder.create_const_u64(0);
        ir::ValueId q = builder.create_div(a, b, ir::Type::U64);
        builder.create_return(q);

        auto jit_res = jit::jit_execute(func, &ev);
        VT_ASSERT(jit_res.ok());
        VT_ASSERT_EQ(jit_res.value(), 0ULL);
    }
    {
        ir::Builder builder;
        ir::Function* func = builder.create_function("mod_u64_zero");
        ir::ValueId a = builder.create_const_u64(100);
        ir::ValueId b = builder.create_const_u64(0);
        ir::ValueId r = builder.create_mod(a, b, ir::Type::U64);
        builder.create_return(r);

        auto jit_res = jit::jit_execute(func, &ev);
        VT_ASSERT(jit_res.ok());
        VT_ASSERT_EQ(jit_res.value(), 0ULL);
    }

    // Signed div & mod: -100 / 7 = -14, -100 % 7 = -2
    {
        ir::Builder builder;
        ir::Function* func = builder.create_function("div_i64_neg");
        ir::ValueId a = builder.create_const_i64(-100);
        ir::ValueId b = builder.create_const_i64(7);
        ir::ValueId q = builder.create_div(a, b, ir::Type::I64);
        builder.create_return(q);

        auto interp_res = interp.execute(func, &ev);
        VT_ASSERT_EQ(static_cast<i64>(interp_res.value()), -14LL);
        auto jit_res = jit::jit_execute(func, &ev);
        VT_ASSERT_EQ(static_cast<i64>(jit_res.value()), -14LL);
    }
    {
        ir::Builder builder;
        ir::Function* func = builder.create_function("mod_i64_neg");
        ir::ValueId a = builder.create_const_i64(-100);
        ir::ValueId b = builder.create_const_i64(7);
        ir::ValueId r = builder.create_mod(a, b, ir::Type::I64);
        builder.create_return(r);

        auto interp_res = interp.execute(func, &ev);
        VT_ASSERT_EQ(static_cast<i64>(interp_res.value()), -2LL);
        auto jit_res = jit::jit_execute(func, &ev);
        VT_ASSERT_EQ(static_cast<i64>(jit_res.value()), -2LL);
    }

    // Signed division by zero
    {
        ir::Builder builder;
        ir::Function* func = builder.create_function("div_i64_zero");
        ir::ValueId a = builder.create_const_i64(-100);
        ir::ValueId b = builder.create_const_i64(0);
        ir::ValueId q = builder.create_div(a, b, ir::Type::I64);
        builder.create_return(q);

        auto jit_res = jit::jit_execute(func, &ev);
        VT_ASSERT(jit_res.ok());
        VT_ASSERT_EQ(jit_res.value(), 0ULL);
    }

    // Negation
    {
        ir::Builder builder;
        ir::Function* func = builder.create_function("neg_i64");
        ir::ValueId a = builder.create_const_i64(42);
        ir::ValueId neg = builder.create_neg(a, ir::Type::I64);
        builder.create_return(neg);

        auto interp_res = interp.execute(func, &ev);
        VT_ASSERT_EQ(static_cast<i64>(interp_res.value()), -42LL);
        auto jit_res = jit::jit_execute(func, &ev);
        VT_ASSERT_EQ(static_cast<i64>(jit_res.value()), -42LL);
    }
}

VT_TEST(jit_differential_tests, host_jit_conditional_select_both_branches) {
    CanonicalEvent ev{};
    ev.quantity = 250;

    ir::Builder builder;
    ir::Function* func = builder.create_function("sel_test");
    VT_ASSERT(func != nullptr);

    func->add_parameter(func->create_value(ir::Type::U64, "ctx"));

    ir::ValueId qty_val = builder.create_load_column(8, func->parameters()[0], ir::Type::U32);
    ir::ValueId thresh = builder.create_const_u64(200);
    ir::ValueId cond = builder.create_gt(qty_val, thresh, ir::Type::U64);

    ir::ValueId val_true = builder.create_const_u64(999);
    ir::ValueId val_false = builder.create_const_u64(111);
    ir::ValueId sel = builder.create_select(cond, val_true, val_false, ir::Type::U64);
    builder.create_return(sel);

    ReferenceInterpreter interp;
    auto interp_res = interp.execute(func, &ev);
    VT_ASSERT(interp_res.ok());
    VT_ASSERT_EQ(interp_res.value(), 999ULL);

    auto jit_res = jit::jit_execute(func, &ev);
    VT_ASSERT(jit_res.ok());
    VT_ASSERT_EQ(jit_res.value(), 999ULL);

    VT_ASSERT_EQ(jit_res.value(), interp_res.value());

    // False branch: quantity = 50 <= 200 -> should select 111
    ev.quantity = 50;
    auto interp_res2 = interp.execute(func, &ev);
    VT_ASSERT_EQ(interp_res2.value(), 111ULL);

    auto jit_res2 = jit::jit_execute(func, &ev);
    VT_ASSERT_EQ(jit_res2.value(), 111ULL);
    VT_ASSERT_EQ(jit_res2.value(), interp_res2.value());
}

VT_TEST(jit_differential_tests, host_jit_high_register_pressure_spilling) {
    CanonicalEvent ev = event::make_empty_event();
    ir::Builder builder;
    ir::Function* func = builder.create_function("spill_heavy_sum");
    VT_ASSERT(func != nullptr);

    // Create 40 live constants simultaneously (exceeds all hardware registers)
    constexpr u64 kNumValues = 40;
    std::vector<ir::ValueId> values;
    values.reserve(kNumValues);
    u64 expected_sum = 0;

    for (u64 i = 1; i <= kNumValues; ++i) {
        values.push_back(builder.create_const_u64(i));
        expected_sum += i;
    }

    // Sum all 40 values together
    ir::ValueId running_sum = values[0];
    for (usize i = 1; i < values.size(); ++i) {
        running_sum = builder.create_add(running_sum, values[i], ir::Type::U64);
    }
    builder.create_return(running_sum);

    // 1. Reference interpreter
    ReferenceInterpreter interp;
    auto interp_res = interp.execute(func, &ev);
    VT_ASSERT(interp_res.ok());
    VT_ASSERT_EQ(interp_res.value(), expected_sum);

    // 2. JIT compiler execution (must spill to stack slots and reload correctly)
    auto jit_res = jit::jit_execute(func, &ev);
    VT_ASSERT(jit_res.ok());
    VT_ASSERT_EQ(jit_res.value(), expected_sum);
    VT_ASSERT_EQ(jit_res.value(), interp_res.value());

    // 3. Verify both x86 and aarch64 code generators handle the high register pressure
    auto x86_gen = jit::create_generator(jit::TargetArch::X86_64);
    VT_ASSERT(x86_gen != nullptr);
    auto x86_res = x86_gen->generate(func);
    VT_ASSERT(x86_res.ok());
    VT_ASSERT(x86_res.value().size() > 0);

    auto a64_gen = jit::create_generator(jit::TargetArch::AArch64);
    VT_ASSERT(a64_gen != nullptr);
    auto a64_res = a64_gen->generate(func);
    VT_ASSERT(a64_res.ok());
    VT_ASSERT(a64_res.value().size() > 0);
}

VT_TEST(jit_differential_tests, host_jit_query_oracle_crosscheck) {
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

    JitExecutor jit_exec;
    ReferenceExecutor ref_exec;

    for (const char* sql : queries) {
        query::Parser p(sql);
        auto q = p.parse_query();
        VT_ASSERT(q.ok());

        auto jit_res = jit_exec.execute_events(events, q.value().get());
        VT_ASSERT(jit_res.ok());

        auto ref_res = ref_exec.execute_events(events, q.value().get());
        VT_ASSERT(ref_res.ok());

        auto oracle_res = test::QueryOracle::evaluate(events, q.value().get());
        VT_ASSERT(oracle_res.ok());

        const auto& j = jit_res.value();
        const auto& r = ref_res.value();
        const auto& o = oracle_res.value();

        VT_ASSERT_EQ(j.rows_scanned, o.rows_scanned);
        VT_ASSERT_EQ(j.rows_matched, o.rows_matched);
        VT_ASSERT_EQ(j.column_names.size(), o.column_names.size());
        for (size_t c = 0; c < j.column_names.size(); ++c) {
            VT_ASSERT_EQ(j.column_names[c], o.column_names[c]);
            VT_ASSERT(j.column_types[c] == o.column_types[c]);
        }
        VT_ASSERT_EQ(j.rows.size(), o.rows.size());
        for (size_t row_i = 0; row_i < j.rows.size(); ++row_i) {
            VT_ASSERT_EQ(j.rows[row_i].size(), o.rows[row_i].size());
            for (size_t col_i = 0; col_i < j.rows[row_i].size(); ++col_i) {
                VT_ASSERT_EQ(j.rows[row_i][col_i], o.rows[row_i][col_i]);
                VT_ASSERT_EQ(j.rows[row_i][col_i], r.rows[row_i][col_i]);
            }
        }
    }
}

