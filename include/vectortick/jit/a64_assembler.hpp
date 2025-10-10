#pragma once

#include "../common/types.hpp"
#include "../common/status.hpp"
#include <vector>
#include <cstring>

namespace vectortick {

namespace jit {

// AArch64 register enumeration
enum class A64Reg : u8 {
    // General purpose registers (64-bit)
    X0 = 0,   X1 = 1,   X2 = 2,   X3 = 3,
    X4 = 4,   X5 = 5,   X6 = 6,   X7 = 7,
    X8 = 8,   X9 = 9,   X10 = 10, X11 = 11,
    X12 = 12, X13 = 13, X14 = 14, X15 = 15,
    X16 = 16, X17 = 17, X18 = 18, X19 = 19,
    X20 = 20, X21 = 21, X22 = 22, X23 = 23,
    X24 = 24, X25 = 25, X26 = 26, X27 = 27,
    X28 = 28, X29 = 29, X30 = 30, SP = 31,
    XZR = 31, // Zero register (in context of r/m or Rd)
    
    // 32-bit versions (W registers)
    W0 = 0,   W1 = 1,   W2 = 2,   W3 = 3,
    W4 = 4,   W5 = 5,   W6 = 6,   W7 = 7,
    W8 = 8,   W9 = 9,   W10 = 10, W11 = 11,
    W12 = 12, W13 = 13, W14 = 14, W15 = 15,
    W16 = 16, W17 = 17, W18 = 18, W19 = 19,
    W20 = 20, W21 = 21, W22 = 22, W23 = 23,
    W24 = 24, W25 = 25, W26 = 26, W27 = 27,
    W28 = 28, W29 = 29, W30 = 30, WZR = 31,
    WSP = 31,
    
    // Vector registers (128-bit)
    V0 = 0,   V1 = 1,   V2 = 2,   V3 = 3,
    V4 = 4,   V5 = 5,   V6 = 6,   V7 = 7,
    V8 = 8,   V9 = 9,   V10 = 10, V11 = 11,
    V12 = 12, V13 = 13, V14 = 14, V15 = 15,
    V16 = 16, V17 = 17, V18 = 18, V19 = 19,
    V20 = 20, V21 = 21, V22 = 22, V23 = 23,
    V24 = 24, V25 = 25, V26 = 26, V27 = 27,
    V28 = 28, V29 = 29, V30 = 30, V31 = 31,
};

// AArch64 condition codes
enum class A64Condition : u8 {
    EQ = 0x0,  // Equal
    NE = 0x1,  // Not equal
    CS = 0x2,  // Carry set (unsigned >=)
    HS = 0x2,  // Unsigned >= (alias for CS)
    CC = 0x3,  // Carry clear (unsigned <)
    LO = 0x3,  // Unsigned < (alias for CC)
    MI = 0x4,  // Minus/negative
    PL = 0x5,  // Plus/positive or zero
    VS = 0x6,  // Overflow
    VC = 0x7,  // No overflow
    HI = 0x8,  // Unsigned >
    LS = 0x9,  // Unsigned <=
    GE = 0xA,  // Signed >=
    LT = 0xB,  // Signed <
    GT = 0xC,  // Signed >
    LE = 0xD,  // Signed <=
    AL = 0xE,  // Always
    NV = 0xF,  // Never
};

// AArch64 shift types
enum class A64Shift : u8 {
    LSL = 0x0,  // Logical shift left
    LSR = 0x1,  // Logical shift right
    ASR = 0x2,  // Arithmetic shift right
    ROR = 0x3,  // Rotate right
};

// AArch64 extend types
enum class A64Extend : u8 {
    UXTB = 0x0,  // Zero-extend byte
    UXTH = 0x1,  // Zero-extend halfword
    UXTW = 0x2,  // Zero-extend word
    UXTX = 0x3,  // Zero-extend doubleword (identity)
    SXTB = 0x4,  // Sign-extend byte
    SXTH = 0x5,  // Sign-extend halfword
    SXTW = 0x6,  // Sign-extend word
    SXTX = 0x7,  // Sign-extend doubleword (identity)
};

// AArch64 assembler - emits raw machine code
class A64Assembler {
public:
    A64Assembler() { code_.reserve(1024); }
    
    // Get the emitted code
    [[nodiscard]] const std::vector<u32>& code() const noexcept { return code_; }
    [[nodiscard]] const u32* data() const noexcept { return code_.data(); }
    [[nodiscard]] u32* data() noexcept { return code_.data(); }
    [[nodiscard]] const u8* bytes() const noexcept { return reinterpret_cast<const u8*>(code_.data()); }
    [[nodiscard]] u8* bytes_data() noexcept { return reinterpret_cast<u8*>(code_.data()); }
    [[nodiscard]] usize size_bytes() const noexcept { return code_.size() * sizeof(u32); }
    [[nodiscard]] usize instruction_count() const noexcept { return code_.size(); }
    
    // Clear the code buffer
    void clear() noexcept { code_.clear(); }
    
    // Emit a 32-bit instruction word (little-endian on ARM)
    void emit(u32 instr) { code_.push_back(instr); }
    
    [[nodiscard]] static constexpr u32 r_idx(A64Reg reg) noexcept {
        return static_cast<u32>(reg) & 0x1F;
    }
    
    // === Move instructions ===
    
    // mov xD, xN (alias for ORR Xd, XZR, Xm)
    void mov_x64_x64(A64Reg dst, A64Reg src) {
        emit(0xAA0003E0 | (r_idx(src) << 16) | r_idx(dst));
    }
    
    // mov wD, wN (alias for ORR Wd, WZR, Wm)
    void mov_w32_w32(A64Reg dst, A64Reg src) {
        emit(0x2A0003E0 | (r_idx(src) << 16) | r_idx(dst));
    }
    
    // movz xD, #imm16, lsl #shift (shift = 0, 16, 32, 48)
    void movz_x64(A64Reg dst, u16 imm, u8 shift = 0) {
        u32 hw = (shift / 16) & 3;
        emit(0xD2800000 | (hw << 21) | (static_cast<u32>(imm) << 5) | r_idx(dst));
    }
    
    // movk xD, #imm16, lsl #shift (keep other bits)
    void movk_x64(A64Reg dst, u16 imm, u8 shift = 0) {
        u32 hw = (shift / 16) & 3;
        emit(0xF2800000 | (hw << 21) | (static_cast<u32>(imm) << 5) | r_idx(dst));
    }
    
    // mov xD, #imm64 (movz + up to 3 movk)
    void mov_x64_imm64(A64Reg dst, u64 imm) {
        movz_x64(dst, static_cast<u16>(imm & 0xFFFF), 0);
        if (imm & 0xFFFF0000ULL) {
            movk_x64(dst, static_cast<u16>((imm >> 16) & 0xFFFF), 16);
        }
        if (imm & 0xFFFF00000000ULL) {
            movk_x64(dst, static_cast<u16>((imm >> 32) & 0xFFFF), 32);
        }
        if (imm & 0xFFFF000000000000ULL) {
            movk_x64(dst, static_cast<u16>((imm >> 48) & 0xFFFF), 48);
        }
    }
    
    // === Arithmetic instructions ===
    
    // add xD, xN, xM
    void add_x64_x64_x64(A64Reg dst, A64Reg src1, A64Reg src2) {
        emit(0x8B000000 | (r_idx(src2) << 16) | (r_idx(src1) << 5) | r_idx(dst));
    }
    
    // add wD, wN, wM
    void add_w32_w32_w32(A64Reg dst, A64Reg src1, A64Reg src2) {
        emit(0x0B000000 | (r_idx(src2) << 16) | (r_idx(src1) << 5) | r_idx(dst));
    }
    
    // add xD, xN, #imm12
    void add_x64_imm12(A64Reg dst, A64Reg src, u16 imm) {
        emit(0x91000000 | ((static_cast<u32>(imm) & 0xFFF) << 10) | (r_idx(src) << 5) | r_idx(dst));
    }
    
    // sub xD, xN, xM
    void sub_x64_x64_x64(A64Reg dst, A64Reg src1, A64Reg src2) {
        emit(0xCB000000 | (r_idx(src2) << 16) | (r_idx(src1) << 5) | r_idx(dst));
    }
    
    // sub xD, xN, #imm12
    void sub_x64_imm12(A64Reg dst, A64Reg src, u16 imm) {
        emit(0xD1000000 | ((static_cast<u32>(imm) & 0xFFF) << 10) | (r_idx(src) << 5) | r_idx(dst));
    }
    
    // mul xD, xN, xM (alias for MADD Xd, Xn, Xm, XZR)
    void mul_x64_x64_x64(A64Reg dst, A64Reg src1, A64Reg src2) {
        emit(0x9B007C00 | (r_idx(src2) << 16) | (r_idx(src1) << 5) | r_idx(dst));
    }
    
    // udiv xD, xN, xM
    void udiv_x64_x64_x64(A64Reg dst, A64Reg src1, A64Reg src2) {
        emit(0x9AC00800 | (r_idx(src2) << 16) | (r_idx(src1) << 5) | r_idx(dst));
    }
    
    // sdiv xD, xN, xM
    void sdiv_x64_x64_x64(A64Reg dst, A64Reg src1, A64Reg src2) {
        emit(0x9AC00C00 | (r_idx(src2) << 16) | (r_idx(src1) << 5) | r_idx(dst));
    }
    
    // msub xD, xN, xM, xA (Xd = Xa - Xn * Xm)
    void msub_x64(A64Reg dst, A64Reg src1, A64Reg src2, A64Reg src_add) {
        emit(0x9B008000 | (r_idx(src2) << 16) | (r_idx(src_add) << 10) | (r_idx(src1) << 5) | r_idx(dst));
    }
    
    // and xD, xN, xM
    void and_x64_x64_x64(A64Reg dst, A64Reg src1, A64Reg src2) {
        emit(0x8A000000 | (r_idx(src2) << 16) | (r_idx(src1) << 5) | r_idx(dst));
    }
    
    // orr xD, xN, xM
    void orr_x64_x64_x64(A64Reg dst, A64Reg src1, A64Reg src2) {
        emit(0xAA000000 | (r_idx(src2) << 16) | (r_idx(src1) << 5) | r_idx(dst));
    }
    
    // eor xD, xN, xM (XOR)
    void eor_x64_x64_x64(A64Reg dst, A64Reg src1, A64Reg src2) {
        emit(0xCA000000 | (r_idx(src2) << 16) | (r_idx(src1) << 5) | r_idx(dst));
    }

    // mvn xD, xN (alias for ORN Xd, XZR, Xm)
    void mvn_x64(A64Reg dst, A64Reg src) {
        emit(0xAA2003E0 | (r_idx(src) << 16) | r_idx(dst));
    }
    
    // === Comparison instructions ===
    
    // cmp xN, xM (alias for SUBS XZR, Xn, Xm)
    void cmp_x64_x64(A64Reg src1, A64Reg src2) {
        emit(0xEB00001F | (r_idx(src2) << 16) | (r_idx(src1) << 5));
    }
    
    // tst xN, xM (alias for ANDS XZR, Xn, Xm)
    void tst_x64_x64(A64Reg src1, A64Reg src2) {
        emit(0xEA00001F | (r_idx(src2) << 16) | (r_idx(src1) << 5));
    }
    
    // === Branch instructions ===
    
    // b label (unconditional branch, instruction offset)
    void b(i32 offset) {
        u32 imm26 = static_cast<u32>(offset) & 0x03FFFFFF;
        emit(0x14000000 | imm26);
    }
    
    // bl label (branch with link)
    void bl(i32 offset) {
        u32 imm26 = static_cast<u32>(offset) & 0x03FFFFFF;
        emit(0x94000000 | imm26);
    }
    
    // br xN (branch to register)
    void br(A64Reg target) {
        emit(0xD61F0000 | (r_idx(target) << 5));
    }
    
    // blr xN (branch with link to register)
    void blr(A64Reg target) {
        emit(0xD63F0000 | (r_idx(target) << 5));
    }
    
    // ret (default X30)
    void ret(A64Reg target = A64Reg::X30) {
        emit(0xD65F0000 | (r_idx(target) << 5));
    }
    
    // b.cond label
    void b_cond(A64Condition cond, i32 offset) {
        u32 imm19 = (static_cast<u32>(offset) & 0x7FFFF) << 5;
        emit(0x54000000 | imm19 | (static_cast<u32>(cond) & 0xF));
    }
    
    // === Load/Store instructions ===
    
    // ldr xD, [xN, #offset] (offset must be multiple of 8, 0 <= offset <= 32760)
    void ldr_x64(A64Reg dst, A64Reg base, i32 offset = 0) {
        if (offset >= 0 && (offset & 7) == 0 && offset <= 32760) {
            u32 imm12 = static_cast<u32>(offset / 8) & 0xFFF;
            emit(0xF9400000 | (imm12 << 10) | (r_idx(base) << 5) | r_idx(dst));
        } else {
            // Unscaled LDUR (-256 to 255)
            u32 imm9 = (static_cast<u32>(offset) & 0x1FF) << 12;
            emit(0xF8400000 | imm9 | (r_idx(base) << 5) | r_idx(dst));
        }
    }

    // ldr wD, [xN, #offset] (load 32-bit unsigned, zero-extended)
    void ldr_w32(A64Reg dst, A64Reg base, i32 offset = 0) {
        if (offset >= 0 && (offset & 3) == 0 && offset <= 16380) {
            u32 imm12 = static_cast<u32>(offset / 4) & 0xFFF;
            emit(0xB9400000 | (imm12 << 10) | (r_idx(base) << 5) | r_idx(dst));
        } else {
            u32 imm9 = (static_cast<u32>(offset) & 0x1FF) << 12;
            emit(0xB8400000 | imm9 | (r_idx(base) << 5) | r_idx(dst));
        }
    }

    // ldrh wD, [xN, #offset] (load 16-bit unsigned, zero-extended)
    void ldrh(A64Reg dst, A64Reg base, i32 offset = 0) {
        if (offset >= 0 && (offset & 1) == 0 && offset <= 8190) {
            u32 imm12 = static_cast<u32>(offset / 2) & 0xFFF;
            emit(0x79400000 | (imm12 << 10) | (r_idx(base) << 5) | r_idx(dst));
        } else {
            u32 imm9 = (static_cast<u32>(offset) & 0x1FF) << 12;
            emit(0x78400000 | imm9 | (r_idx(base) << 5) | r_idx(dst));
        }
    }

    // ldrb wD, [xN, #offset] (load 8-bit unsigned, zero-extended)
    void ldrb(A64Reg dst, A64Reg base, i32 offset = 0) {
        if (offset >= 0 && offset <= 4095) {
            u32 imm12 = static_cast<u32>(offset) & 0xFFF;
            emit(0x39400000 | (imm12 << 10) | (r_idx(base) << 5) | r_idx(dst));
        } else {
            u32 imm9 = (static_cast<u32>(offset) & 0x1FF) << 12;
            emit(0x38400000 | imm9 | (r_idx(base) << 5) | r_idx(dst));
        }
    }
    
    // str xD, [xN, #offset] (offset must be multiple of 8, 0 <= offset <= 32760)
    void str_x64(A64Reg src, A64Reg base, i32 offset = 0) {
        if (offset >= 0 && (offset & 7) == 0 && offset <= 32760) {
            u32 imm12 = static_cast<u32>(offset / 8) & 0xFFF;
            emit(0xF9000000 | (imm12 << 10) | (r_idx(base) << 5) | r_idx(src));
        } else {
            // Unscaled STUR (-256 to 255)
            u32 imm9 = (static_cast<u32>(offset) & 0x1FF) << 12;
            emit(0xF8000000 | imm9 | (r_idx(base) << 5) | r_idx(src));
        }
    }
    
    // === Stack operations (pair load/store) ===
    
    // stp x1, x2, [base, #offset]! (pre-indexed: base = base + offset, stores x1, x2)
    void stp_pre(A64Reg src1, A64Reg src2, A64Reg base, i32 offset) {
        u32 imm7 = (static_cast<u32>(offset / 8) & 0x7F) << 15;
        emit(0xA9800000 | (0b11 << 23) | imm7 | (r_idx(src2) << 10) | (r_idx(base) << 5) | r_idx(src1));
    }
    
    // ldp x1, x2, [base], #offset (post-indexed: loads x1, x2, base = base + offset)
    void ldp_post(A64Reg dst1, A64Reg dst2, A64Reg base, i32 offset) {
        u32 imm7 = (static_cast<u32>(offset / 8) & 0x7F) << 15;
        emit(0xA8C00000 | (0b01 << 23) | imm7 | (r_idx(dst2) << 10) | (r_idx(base) << 5) | r_idx(dst1));
    }
    
    // === Conditional select ===
    
    // csel xD, xN, xM, cond
    void csel_x64(A64Reg dst, A64Reg src1, A64Reg src2, A64Condition cond) {
        emit(0x9A800000 | (r_idx(src2) << 16) | (static_cast<u32>(cond) << 12) | 
             (r_idx(src1) << 5) | r_idx(dst));
    }
    
    // cset xD, cond (set to 1 if condition true, else 0; CSINC Xd, XZR, XZR, !cond)
    void cset_x64(A64Reg dst, A64Condition cond) {
        u32 inv_cond = static_cast<u32>(cond) ^ 1;
        emit(0x9A9F07E0 | (inv_cond << 12) | r_idx(dst));
    }
    
    // nop
    void nop() {
        emit(0xD503201F);
    }
    
    // === Patching ===
    [[nodiscard]] usize current_offset() const noexcept { return code_.size(); }
    
    void patch_branch(usize instr_idx, i32 target_instr_offset) {
        if (instr_idx >= code_.size()) return;
        u32 instr = code_[instr_idx];
        if ((instr & 0x7C000000) == 0x14000000) {
            // Unconditional B or BL: imm26
            u32 imm26 = static_cast<u32>(target_instr_offset) & 0x03FFFFFF;
            code_[instr_idx] = (instr & 0xFC000000) | imm26;
        } else if ((instr & 0xFF000000) == 0x54000000) {
            // B.cond: imm19
            u32 imm19 = (static_cast<u32>(target_instr_offset) & 0x7FFFF) << 5;
            code_[instr_idx] = (instr & 0xFF00001F) | imm19;
        }
    }

private:
    std::vector<u32> code_;
};

} // namespace jit

} // namespace vectortick
