#pragma once

#include "../common/types.hpp"
#include "../common/status.hpp"
#include <vector>
#include <cstring>

namespace vectortick {

namespace jit {

// X86-64 register enumeration
enum class X86Reg : u8 {
    // General purpose 64-bit
    RAX = 0, RCX = 1, RDX = 2, RBX = 3,
    RSP = 4, RBP = 5, RSI = 6, RDI = 7,
    R8  = 8, R9  = 9, R10 = 10, R11 = 11,
    R12 = 12, R13 = 13, R14 = 14, R15 = 15,
    
    // 32-bit versions (same physical register index)
    EAX = 0, ECX = 1, EDX = 2, EBX = 3,
    ESP = 4, EBP = 5, ESI = 6, EDI = 7,
    R8D = 8, R9D = 9, R10D = 10, R11D = 11,
    R12D = 12, R13D = 13, R14D = 14, R15D = 15,
    
    // XMM registers
    XMM0 = 0, XMM1 = 1, XMM2 = 2, XMM3 = 3,
    XMM4 = 4, XMM5 = 5, XMM6 = 6, XMM7 = 7,
    XMM8 = 8, XMM9 = 9, XMM10 = 10, XMM11 = 11,
    XMM12 = 12, XMM13 = 13, XMM14 = 14, XMM15 = 15,
};

// Condition codes for conditional instructions (both signed and unsigned mnemonics)
enum class Condition : u8 {
    O   = 0x0,  // Overflow
    NO  = 0x1,  // No overflow
    B   = 0x2,  // Below (unsigned <)
    C   = 0x2,  // Carry
    NAE = 0x2,  // Not above or equal
    NB  = 0x3,  // Not below (unsigned >=)
    AE  = 0x3,  // Above or equal
    NC  = 0x3,  // Not carry
    E   = 0x4,  // Equal
    Z   = 0x4,  // Zero
    NE  = 0x5,  // Not equal
    NZ  = 0x5,  // Not zero
    BE  = 0x6,  // Below or equal (unsigned <=)
    NA  = 0x6,  // Not above
    A   = 0x7,  // Above (unsigned >)
    NBE = 0x7,  // Not below or equal
    S   = 0x8,  // Sign
    NS  = 0x9,  // No sign
    P   = 0xA,  // Parity
    PE  = 0xA,  // Parity even
    NP  = 0xB,  // No parity
    PO  = 0xB,  // Parity odd
    L   = 0xC,  // Less (signed <)
    NGE = 0xC,  // Not greater or equal
    GE  = 0xD,  // Greater or equal (signed >=)
    NL  = 0xD,  // Not less
    LE  = 0xE,  // Less or equal (signed <=)
    NG  = 0xE,  // Not greater
    G   = 0xF,  // Greater (signed >)
    NLE = 0xF,  // Not less or equal
};

// X86-64 assembler - emits raw machine code
class X86Assembler {
public:
    X86Assembler() { code_.reserve(4096); }
    
    // Get the emitted code
    [[nodiscard]] const std::vector<u8>& code() const noexcept { return code_; }
    [[nodiscard]] u8* data() noexcept { return code_.data(); }
    [[nodiscard]] usize size() const noexcept { return code_.size(); }
    
    // Clear the code buffer
    void clear() noexcept { code_.clear(); }
    
    // Emit a byte
    void emit(u8 byte) { code_.push_back(byte); }
    
    // Emit multiple bytes
    void emit(const u8* bytes, usize count) {
        code_.insert(code_.end(), bytes, bytes + count);
    }
    
    // Emit 16-bit value (little-endian)
    void emit16(u16 value) {
        code_.push_back(static_cast<u8>(value));
        code_.push_back(static_cast<u8>(value >> 8));
    }
    
    // Emit 32-bit value (little-endian)
    void emit32(u32 value) {
        code_.push_back(static_cast<u8>(value));
        code_.push_back(static_cast<u8>(value >> 8));
        code_.push_back(static_cast<u8>(value >> 16));
        code_.push_back(static_cast<u8>(value >> 24));
    }
    
    // Emit 64-bit value (little-endian)
    void emit64(u64 value) {
        emit32(static_cast<u32>(value));
        emit32(static_cast<u32>(value >> 32));
    }
    
    // === Register helpers ===
    [[nodiscard]] static constexpr u8 reg_index(X86Reg r) noexcept {
        return static_cast<u8>(r) & 7;
    }
    
    [[nodiscard]] static constexpr bool is_extended(X86Reg r) noexcept {
        return static_cast<u8>(r) >= 8;
    }

    // === REX prefixes ===
    void rex() { emit(0x40); }
    void rex_w() { emit(0x48); }  // 64-bit operand size
    void rex_r() { emit(0x44); }  // MODRM reg extension
    void rex_x() { emit(0x42); }  // SIB index extension
    void rex_b() { emit(0x41); }  // R/M extension
    void rex_wr() { emit(0x4C); }
    void rex_wb() { emit(0x49); }
    void rex_wrb() { emit(0x4D); }

    void emit_rex64(X86Reg reg, X86Reg rm) {
        u8 prefix = 0x48;
        if (is_extended(reg)) prefix |= 0x04;
        if (is_extended(rm))  prefix |= 0x01;
        emit(prefix);
    }
    
    void emit_rex64_b(X86Reg b) {
        u8 prefix = 0x48;
        if (is_extended(b)) prefix |= 0x01;
        emit(prefix);
    }
    
    void emit_rex64_r(X86Reg r) {
        u8 prefix = 0x48;
        if (is_extended(r)) prefix |= 0x04;
        emit(prefix);
    }
    
    void emit_rex32(X86Reg reg, X86Reg rm) {
        u8 prefix = 0x40;
        if (is_extended(reg)) prefix |= 0x04;
        if (is_extended(rm))  prefix |= 0x01;
        if (prefix != 0x40) {
            emit(prefix);
        }
    }
    
    // === Move instructions ===
    
    // mov r64, imm64
    void mov_r64_imm64(X86Reg reg, u64 imm) {
        u8 rex = 0x48;
        if (is_extended(reg)) rex |= 0x01;
        emit(rex);
        emit(0xB8 | reg_index(reg));
        emit64(imm);
    }
    
    // mov r32, imm32
    void mov_r32_imm32(X86Reg reg, u32 imm) {
        if (is_extended(reg)) {
            emit(0x41);
        }
        emit(0xB8 | reg_index(reg));
        emit32(imm);
    }
    
    // mov r64, r64
    void mov_r64_r64(X86Reg dst, X86Reg src) {
        emit_rex64(src, dst);
        emit(0x89);  // MOV r/m64, r64
        emit(modrm(3, src, dst));
    }
    
    // mov r32, r32
    void mov_r32_r32(X86Reg dst, X86Reg src) {
        emit_rex32(src, dst);
        emit(0x89);  // MOV r/m32, r32
        emit(modrm(3, src, dst));
    }
    
    // mov r64, [mem] with displacement
    void mov_r64_mem(X86Reg dst, X86Reg base, i32 offset = 0) {
        emit_rex64(dst, base);
        emit(0x8B);  // MOV r64, r/m64
        emit_mem_operand(dst, base, offset);
    }
    
    // mov [mem], r64 with displacement
    void mov_mem_r64(X86Reg base, X86Reg src, i32 offset = 0) {
        emit_rex64(src, base);
        emit(0x89);  // MOV r/m64, r64
        emit_mem_operand(src, base, offset);
    }
    
    // mov r32, [mem] with displacement (zero-extends to 64-bit)
    void mov_r32_mem(X86Reg dst, X86Reg base, i32 offset = 0) {
        emit_rex32(dst, base);
        emit(0x8B);  // MOV r32, r/m32
        emit_mem_operand(dst, base, offset);
    }
    
    // movzx r64, word [mem] (zero-extend 16-bit to 64-bit)
    void movzx_r64_mem16(X86Reg dst, X86Reg base, i32 offset = 0) {
        emit_rex64(dst, base);
        emit(0x0F);
        emit(0xB7);  // MOVZX r64, r/m16
        emit_mem_operand(dst, base, offset);
    }
    
    // movzx r64, byte [mem] (zero-extend 8-bit to 64-bit)
    void movzx_r64_mem8(X86Reg dst, X86Reg base, i32 offset = 0) {
        emit_rex64(dst, base);
        emit(0x0F);
        emit(0xB6);  // MOVZX r64, r/m8
        emit_mem_operand(dst, base, offset);
    }
    
    // movzx r64, r8 (zero-extend 8-bit register to 64-bit)
    void movzx_r64_r8(X86Reg dst, X86Reg src) {
        emit_rex64(dst, src);
        emit(0x0F);
        emit(0xB6);  // MOVZX r64, r/m8
        emit(modrm(3, dst, src));
    }
    
    // === Arithmetic instructions ===
    
    // add r64, imm32 (sign-extended to 64)
    void add_r64_imm32(X86Reg dst, i32 imm) {
        emit_rex64_b(dst);
        emit(0x81);  // ADD r/m64, imm32
        emit(modrm(3, static_cast<u8>(0), dst));
        emit32(static_cast<u32>(imm));
    }
    
    // add r64, r64
    void add_r64_r64(X86Reg dst, X86Reg src) {
        emit_rex64(src, dst);
        emit(0x01);  // ADD r/m64, r64
        emit(modrm(3, src, dst));
    }
    
    // sub r64, r64
    void sub_r64_r64(X86Reg dst, X86Reg src) {
        emit_rex64(src, dst);
        emit(0x29);  // SUB r/m64, r64
        emit(modrm(3, src, dst));
    }
    
    // imul r64, r64
    void imul_r64_r64(X86Reg dst, X86Reg src) {
        emit_rex64(dst, src);
        emit(0x0F);
        emit(0xAF);  // IMUL r64, r/m64
        emit(modrm(3, dst, src));
    }
    
    // xor r64, r64
    void xor_r64_r64(X86Reg dst, X86Reg src) {
        emit_rex64(src, dst);
        emit(0x31);  // XOR r/m64, r64
        emit(modrm(3, src, dst));
    }
    
    // and r64, r64
    void and_r64_r64(X86Reg dst, X86Reg src) {
        emit_rex64(src, dst);
        emit(0x21);  // AND r/m64, r64
        emit(modrm(3, src, dst));
    }
    
    // or r64, r64
    void or_r64_r64(X86Reg dst, X86Reg src) {
        emit_rex64(src, dst);
        emit(0x09);  // OR r/m64, r64
        emit(modrm(3, src, dst));
    }
    
    // div r64 (unsigned divide RDX:RAX by r64; quotient in RAX, remainder in RDX)
    void div_r64(X86Reg divisor) {
        emit_rex64_b(divisor);
        emit(0xF7);
        emit(modrm(3, static_cast<u8>(6), divisor));
    }
    
    // idiv r64 (signed divide RDX:RAX by r64; quotient in RAX, remainder in RDX)
    void idiv_r64(X86Reg divisor) {
        emit_rex64_b(divisor);
        emit(0xF7);
        emit(modrm(3, static_cast<u8>(7), divisor));
    }
    
    // cqo (sign-extend RAX into RDX:RAX)
    void cqo() {
        emit(0x48);
        emit(0x99);
    }
    
    // neg r64 (two's complement negation)
    void neg_r64(X86Reg dst) {
        emit_rex64_b(dst);
        emit(0xF7);
        emit(modrm(3, static_cast<u8>(3), dst));
    }
    
    // not r64 (one's complement bitwise not)
    void not_r64(X86Reg dst) {
        emit_rex64_b(dst);
        emit(0xF7);
        emit(modrm(3, static_cast<u8>(2), dst));
    }
    
    // cmp r64, r64
    void cmp_r64_r64(X86Reg a, X86Reg b) {
        emit_rex64(b, a);
        emit(0x39);  // CMP r/m64, r64
        emit(modrm(3, b, a));
    }
    
    // test r64, r64
    void test_r64_r64(X86Reg a, X86Reg b) {
        emit_rex64(b, a);
        emit(0x85);  // TEST r/m64, r64
        emit(modrm(3, b, a));
    }
    
    // === Control flow ===
    
    // jmp rel32
    void jmp_rel32(i32 offset) {
        emit(0xE9);
        emit32(static_cast<u32>(offset));
    }

    // jcc rel32
    void jcc_rel32(Condition cond, i32 offset) {
        emit(0x0F);
        emit(0x80 | static_cast<u8>(cond));
        emit32(static_cast<u32>(offset));
    }
    
    // jmp r64
    void jmp_r64(X86Reg target) {
        if (is_extended(target)) emit(0x41);
        emit(0xFF);
        emit(modrm(3, static_cast<u8>(4), target));
    }
    
    // call r64
    void call_r64(X86Reg target) {
        if (is_extended(target)) emit(0x41);
        emit(0xFF);
        emit(modrm(3, static_cast<u8>(2), target));
    }
    
    // ret
    void ret() {
        emit(0xC3);
    }
    
    // nop
    void nop() {
        emit(0x90);
    }
    
    // push r64
    void push_r64(X86Reg reg) {
        if (is_extended(reg)) {
            emit(0x41);  // REX.B
        }
        emit(0x50 | reg_index(reg));
    }
    
    // pop r64
    void pop_r64(X86Reg reg) {
        if (is_extended(reg)) {
            emit(0x41);  // REX.B
        }
        emit(0x58 | reg_index(reg));
    }
    
    // === Conditional moves and sets ===
    
    // setcc r8
    void setcc(Condition cond, X86Reg dst) {
        if (is_extended(dst) || static_cast<u8>(dst) >= 4) {
            u8 rex = 0x40;
            if (is_extended(dst)) rex |= 0x01;
            emit(rex);
        }
        emit(0x0F);
        emit(0x90 | static_cast<u8>(cond));
        emit(modrm(3, static_cast<u8>(0), dst));
    }
    
    // cmovcc r64, r64
    void cmovcc_r64_r64(Condition cond, X86Reg dst, X86Reg src) {
        emit_rex64(dst, src);
        emit(0x0F);
        emit(0x40 | static_cast<u8>(cond));
        emit(modrm(3, dst, src));
    }
    
    // === SIMD instructions ===
    
    // pxor xmm, xmm
    void pxor_xmm_xmm(X86Reg dst, X86Reg src) {
        emit(0x66);
        if (is_extended(dst) || is_extended(src)) {
            emit(0x40 | (is_extended(dst) ? 0x04 : 0) | (is_extended(src) ? 0x01 : 0));
        }
        emit(0x0F);
        emit(0xEF);  // PXOR
        emit(modrm(3, dst, src));
    }
    
    // movdqu xmm, [mem]
    void movdqu_xmm_mem(X86Reg dst, X86Reg base, i32 offset = 0) {
        emit(0xF3);
        if (is_extended(dst) || is_extended(base)) {
            emit(0x40 | (is_extended(dst) ? 0x04 : 0) | (is_extended(base) ? 0x01 : 0));
        }
        emit(0x0F);
        emit(0x6F);  // MOVDQU
        emit_mem_operand(dst, base, offset);
    }
    
    // movdqu [mem], xmm
    void movdqu_mem_xmm(X86Reg base, X86Reg src, i32 offset = 0) {
        emit(0xF3);
        if (is_extended(src) || is_extended(base)) {
            emit(0x40 | (is_extended(src) ? 0x04 : 0) | (is_extended(base) ? 0x01 : 0));
        }
        emit(0x0F);
        emit(0x7F);  // MOVDQU
        emit_mem_operand(src, base, offset);
    }
    
    // === Patching ===
    
    // Get current offset for patching
    [[nodiscard]] usize current_offset() const noexcept { return code_.size(); }
    
    // Patch a 32-bit value at given offset
    void patch32(usize offset, u32 value) {
        code_[offset]     = static_cast<u8>(value);
        code_[offset + 1] = static_cast<u8>(value >> 8);
        code_[offset + 2] = static_cast<u8>(value >> 16);
        code_[offset + 3] = static_cast<u8>(value >> 24);
    }
    
    // Build MODRM byte
    [[nodiscard]] static constexpr u8 modrm(u8 mod, u8 reg, u8 rm) noexcept {
        return static_cast<u8>(((mod & 3) << 6) | ((reg & 7) << 3) | (rm & 7));
    }
    
    [[nodiscard]] static constexpr u8 modrm(u8 mod, X86Reg reg, X86Reg rm) noexcept {
        return modrm(mod, reg_index(reg), reg_index(rm));
    }
    
    [[nodiscard]] static constexpr u8 modrm(u8 mod, u8 reg_code, X86Reg rm) noexcept {
        return modrm(mod, reg_code & 7, reg_index(rm));
    }
    
    [[nodiscard]] static constexpr u8 modrm(u8 mod, X86Reg reg, u8 rm_code) noexcept {
        return modrm(mod, reg_index(reg), rm_code & 7);
    }
    
    // Build SIB byte
    [[nodiscard]] static constexpr u8 sib(u8 scale, u8 index, u8 base) noexcept {
        return static_cast<u8>(((scale & 3) << 6) | ((index & 7) << 3) | (base & 7));
    }
    
    [[nodiscard]] static constexpr u8 sib(u8 scale, X86Reg index, X86Reg base) noexcept {
        return sib(scale, reg_index(index), reg_index(base));
    }

private:
    std::vector<u8> code_;

    // Helper to emit memory operand (ModRM + optional SIB + displacement)
    void emit_mem_operand(X86Reg reg_op, X86Reg base, i32 offset) {
        u8 r = reg_index(reg_op);
        u8 b = reg_index(base);
        bool needs_sib = (b == 4); // RSP or R12
        bool is_bp = (b == 5);      // RBP or R13
        
        if (offset == 0 && !is_bp) {
            // Mod = 00
            emit(modrm(0, r, b));
            if (needs_sib) {
                emit(sib(0, 4, b)); // index = 4 (none)
            }
        } else if (offset >= -128 && offset <= 127) {
            // Mod = 01 (disp8)
            emit(modrm(1, r, b));
            if (needs_sib) {
                emit(sib(0, 4, b));
            }
            emit(static_cast<u8>(offset));
        } else {
            // Mod = 10 (disp32)
            emit(modrm(2, r, b));
            if (needs_sib) {
                emit(sib(0, 4, b));
            }
            emit32(static_cast<u32>(offset));
        }
    }
};

} // namespace jit

} // namespace vectortick
