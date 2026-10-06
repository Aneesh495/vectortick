#include "vectortick/execution/reference_interpreter.hpp"
#include "vectortick/ir/builder.hpp"
#include "vectortick/jit/jit_compiler.hpp"
#include "vectortick/jit/native_plan.hpp"
#include "vectortick/model/event.hpp"
#include <charconv>
#include <fstream>
#include <iostream>
#include <random>

using namespace vectortick;
using namespace vectortick::ir;
namespace {
constexpr Type types[] = {Type::U64, Type::U64, Type::U64, Type::U32, Type::U8,  Type::U8,
                          Type::U16, Type::I64, Type::U32, Type::U16, Type::U16, Type::U64};
constexpr Opcode binary_ops[] = {
    Opcode::AddU64, Opcode::SubU64, Opcode::MulU64, Opcode::DivU64, Opcode::ModU64, Opcode::AddI64,
    Opcode::SubI64, Opcode::MulI64, Opcode::DivI64, Opcode::ModI64, Opcode::EqU64,  Opcode::NeU64,
    Opcode::LtU64,  Opcode::LeU64,  Opcode::GtU64,  Opcode::GeU64,  Opcode::EqI64,  Opcode::NeI64,
    Opcode::LtI64,  Opcode::LeI64,  Opcode::GtI64,  Opcode::GeI64,  Opcode::And,    Opcode::Or,
    Opcode::BitAnd, Opcode::BitOr,  Opcode::BitXor, Opcode::Shl,    Opcode::Shr};
constexpr Opcode unary_ops[] = {
    Opcode::NegI64, Opcode::NegU64,         Opcode::U64ToI64,       Opcode::Not,
    Opcode::BitNot, Opcode::ExtendI32ToI64, Opcode::ExtendU32ToU64, Opcode::TruncI64ToI32};
CanonicalEvent input(std::mt19937_64 &rng, u64 i) {
    CanonicalEvent e{};
    constexpr u64 edges[] = {0,
                             1,
                             2,
                             63,
                             64,
                             65,
                             0x7fffffff,
                             0x80000000,
                             0xffffffff,
                             0x7fffffffffffffffULL,
                             0x8000000000000000ULL,
                             ~u64{0},
                             ~u64{1}};
    auto next = [&]() {
        return i < sizeof(edges) / sizeof(edges[0])
                   ? edges[(i++) % (sizeof(edges) / sizeof(edges[0]))]
                   : rng();
    };
    e.exchange_ts_ns = next();
    e.receive_ts_ns = next();
    e.sequence = next();
    e.instrument_id = static_cast<u32>(next());
    e.event_type = static_cast<EventType>(next());
    e.side = static_cast<Side>(next());
    e.flags = static_cast<u16>(next());
    e.price_ticks = static_cast<i64>(next());
    e.quantity = static_cast<u32>(next());
    e.venue_id = static_cast<u16>(next());
    e.source_id = static_cast<u16>(next());
    e.trade_or_order_id = next();
    return e;
}
ValueId binary(Builder &b, Opcode op, ValueId x, ValueId y) {
    Type type = is_comparison(op) || op == Opcode::And || op == Opcode::Or ? Type::I1
                : op >= Opcode::AddI64 && op <= Opcode::ModI64             ? Type::I64
                                                                           : Type::U64;
    auto out = b.function()->create_value(type);
    b.insertion_point()->append(std::make_unique<BinaryOp>(op, x, y, out, type));
    return out;
}
void make_program(Builder &b, std::mt19937_64 &rng, u64 index) {
    auto fn = b.create_function("seeded_native");
    fn->add_parameter(fn->create_value(Type::U64, "context"));
    auto load = [&](u32 col) { return b.create_load_column(col, fn->parameters()[0], types[col]); };
    ValueId result = 0;
    switch (index % 6) {
    case 0: {
        std::vector<ValueId> live;
        usize count = index % 30 == 0 ? 800 : 40 + static_cast<usize>(rng() % 40);
        for (usize k = 0; k < count; ++k)
            live.push_back(k % 2 ? load(static_cast<u32>(rng() % 12)) : b.create_const_u64(rng()));
        result = live[0];
        for (usize k = 1; k < live.size(); ++k)
            result = b.create_bitxor(result, live[k]);
        break;
    }
    case 1: {
        auto x = load(static_cast<u32>(rng() % 12));
        auto y = index % 12 == 1 ? load(static_cast<u32>(rng() % 12))
                                 : b.create_const_u64(rng() % 4 ? rng() : 0);
        result = binary(b, binary_ops[rng() % (sizeof(binary_ops) / sizeof(binary_ops[0]))], x, y);
        break;
    }
    case 2: {
        auto x = load(static_cast<u32>(rng() % 12));
        auto op = unary_ops[rng() % (sizeof(unary_ops) / sizeof(unary_ops[0]))];
        auto out = fn->create_value(Type::U64);
        b.insertion_point()->append(std::make_unique<UnaryOp>(op, x, out, Type::U64));
        result = out;
        break;
    }
    case 3:
        result = b.create_select(load(6), load(7), load(11), Type::U64);
        break;
    case 4: {
        auto entry = b.insertion_point();
        auto yes = b.create_block("yes"), no = b.create_block("no"),
             merge = b.create_block("merge");
        b.set_insertion_point(entry);
        b.create_branch(load(5), yes, no);
        b.set_insertion_point(yes);
        auto y = binary(b, binary_ops[rng() % 10], load(7), b.create_const_u64(rng() % 3));
        b.create_jump(merge);
        b.set_insertion_point(no);
        auto n = b.create_bitxor(load(11), b.create_const_u64(rng()));
        b.create_jump(merge);
        b.set_insertion_point(merge);
        result = fn->create_value(Type::U64);
        merge->append(std::make_unique<PhiOp>(std::vector<BasicBlock *>{yes, no},
                                              std::vector<ValueId>{y, n}, result, Type::U64));
        break;
    }
    default: {
        result = load(static_cast<u32>(rng() % 12));
        for (usize k = 0; k < 100 + index % 100; ++k) {
            auto x = b.create_const_u64(rng());
            result =
                k % 3 ? b.create_bitxor(result, x) : b.create_select(load(6), result, x, Type::U64);
        }
        break;
    }
    }
    b.create_return(result);
}
void dump_failure(std::ostream &out, const Function *fn, const CanonicalEvent &e, u64 seed,
                  u64 program, u64 input_index) {
    out << "{\"seed\":" << seed << ",\"program\":" << program << ",\"input_index\":" << input_index
        << ",\"event_bytes\":\"";
    const auto *bytes = reinterpret_cast<const u8 *>(&e);
    constexpr char hex[] = "0123456789abcdef";
    for (usize k = 0; k < sizeof(e); ++k)
        out << hex[bytes[k] >> 4] << hex[bytes[k] & 15];
    out << "\",\"ir\":[";
    bool first = true;
    for (const auto &block : fn->blocks())
        for (const auto &ip : block->instructions()) {
            if (!first)
                out << ',';
            first = false;
            auto in = ip.get();
            out << "{\"block\":" << block->id() << ",\"opcode\":" << static_cast<int>(in->opcode())
                << ",\"result\":" << in->result() << ",\"operands\":[";
            for (usize k = 0; k < in->num_operands(); ++k) {
                if (k)
                    out << ',';
                out << in->operand(k);
            }
            out << ']';
            if (in->opcode() >= Opcode::ConstI64 && in->opcode() <= Opcode::ConstBool)
                out << ",\"constant\":" << static_cast<const ConstOp *>(in)->constant().as_u64();
            if (in->opcode() == Opcode::LoadColumn)
                out << ",\"column\":" << static_cast<const LoadColumnOp *>(in)->column_id();
            if (in->opcode() == Opcode::Jump)
                out << ",\"target\":" << static_cast<const JumpOp *>(in)->target()->id();
            if (in->opcode() == Opcode::Branch) {
                auto br = static_cast<const BranchOp *>(in);
                out << ",\"true\":" << br->true_block()->id()
                    << ",\"false\":" << br->false_block()->id();
            }
            if (in->opcode() == Opcode::Phi) {
                auto phi = static_cast<const PhiOp *>(in);
                out << ",\"predecessors\":[";
                for (usize k = 0; k < phi->incoming_blocks().size(); ++k) {
                    if (k)
                        out << ',';
                    out << phi->incoming_blocks()[k]->id();
                }
                out << ']';
            }
            out << '}';
        }
    out << "]}\n";
}
} // namespace
int main(int argc, char **argv) {
    u64 programs = 10000, inputs = 100, seed = 0x49520261005ULL;
    std::string json = "native-campaign.json", required;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (i + 1 == argc) {
            std::cerr << "Missing option argument\n";
            return 2;
        }
        std::string value = argv[++i];
        if (arg == "--json")
            json = value;
        else if (arg == "--require-arch")
            required = value;
        else {
            u64 *dest = arg == "--programs" ? &programs
                        : arg == "--inputs" ? &inputs
                        : arg == "--seed"   ? &seed
                                            : nullptr;
            if (!dest)
                return 2;
            auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), *dest);
            if (ec != std::errc{} || end != value.data() + value.size())
                return 2;
        }
    }
    std::string arch = jit::detect_host_arch() == jit::TargetArch::X86_64    ? "x86_64"
                       : jit::detect_host_arch() == jit::TargetArch::AArch64 ? "aarch64"
                                                                             : "unknown";
    if (arch == "unknown" || (!required.empty() && required != arch) || !programs || !inputs) {
        std::cerr << "Native architecture or campaign count mismatch\n";
        return 2;
    }
    std::mt19937_64 rng(seed);
    u64 compiled = 0, evaluated = 0, errors = 0, spilled = 0, failures = 0;
    for (u64 p = 0; p < programs; ++p) {
        Builder b;
        make_program(b, rng, p);
        jit::JitCompiler compiler;
        auto plan = jit::plan_native(b.function(), arch == "x86_64" ? 6 : 13);
        auto native = compiler.compile(b.function());
        if (!plan.ok() || !native.ok()) {
            std::cerr << "Compile failure program " << p << "\n";
            failures++;
            std::ofstream failure(json + ".failure.json");
            dump_failure(failure, b.function(), CanonicalEvent{}, seed, p, 0);
            break;
        }
        ++compiled;
        if (plan.value().spill_count)
            ++spilled;
        ReferenceInterpreter reference;
        for (u64 i = 0; i < inputs; ++i) {
            auto e = input(rng, i);
            auto expected = reference.execute(b.function(), &e);
            auto actual = jit::invoke_native(native.value(), &e);
            ++evaluated;
            if (!expected.ok())
                ++errors;
            if (expected.ok() != actual.ok() ||
                (expected.ok() && expected.value() != actual.value())) {
                std::cerr << "Differential mismatch seed " << seed << " program " << p << " input "
                          << i << " expected_ok " << expected.ok() << " actual_ok " << actual.ok()
                          << "\n";
                ++failures;
                std::ofstream failure(json + ".failure.json");
                dump_failure(failure, b.function(), e, seed, p, i);
                break;
            }
        }
        if (failures)
            break;
    }
    std::ofstream out(json);
    out << "{\"schema_version\":1,\"arch\":\"" << arch << "\",\"seed\":" << seed
        << ",\"programs_requested\":" << programs << ",\"programs_compiled\":" << compiled
        << ",\"inputs_per_program\":" << inputs << ",\"native_invocations\":" << evaluated
        << ",\"inputs_evaluated\":" << evaluated << ",\"spill_programs\":" << spilled
        << ",\"arithmetic_errors_checked\":" << errors << ",\"failures\":" << failures << "}\n";
    out.flush();
    if (!out)
        return 2;
    std::cout << "Native campaign " << arch << ": " << compiled << " programs, " << evaluated
              << " native calls, " << spilled << " spill programs, " << failures << " failures\n";
    return failures || compiled != programs || evaluated != programs * inputs || !spilled ? 1 : 0;
}
