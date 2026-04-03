#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef float    f32;
typedef double   f64;

typedef struct {
    u32 gpr[32];
    f64 fpr[32];
    u32 lr;
    u32 ctr;
    u32 pc;
    u32 msr;
    u32 cr;
    u32 xer;
    u32 fpscr;
    f64 ps0[32];
    f64 ps1[32];
    u32 spr[1024];
} CPUContext;

extern u8* ram;

#define GC_RAM_MASK 0x01FFFFFFu
#define GC_RAM_SIZE (GC_RAM_MASK + 1u)

#ifdef GCRECOMP_ENABLE_MMIO_HOOKS
int gcrecomp_mmio_read8(u32 addr, u8* value);
int gcrecomp_mmio_read16(u32 addr, u16* value);
int gcrecomp_mmio_read32(u32 addr, u32* value);
int gcrecomp_mmio_write8(u32 addr, u8 value);
int gcrecomp_mmio_write16(u32 addr, u16 value);
int gcrecomp_mmio_write32(u32 addr, u32 value);
#else
static inline int gcrecomp_mmio_read8(u32 addr, u8* value) {
    (void)addr;
    (void)value;
    return 0;
}

static inline int gcrecomp_mmio_read16(u32 addr, u16* value) {
    (void)addr;
    (void)value;
    return 0;
}

static inline int gcrecomp_mmio_read32(u32 addr, u32* value) {
    (void)addr;
    (void)value;
    return 0;
}

static inline int gcrecomp_mmio_write8(u32 addr, u8 value) {
    (void)addr;
    (void)value;
    return 0;
}

static inline int gcrecomp_mmio_write16(u32 addr, u16 value) {
    (void)addr;
    (void)value;
    return 0;
}

static inline int gcrecomp_mmio_write32(u32 addr, u32 value) {
    (void)addr;
    (void)value;
    return 0;
}
#endif

static inline int translate_ram_addr(u32 addr, u32* out) {
    if (addr < GC_RAM_SIZE) {
        *out = addr;
        return 1;
    }
    if (addr >= 0x80000000u && addr < (0x80000000u + GC_RAM_SIZE)) {
        *out = addr - 0x80000000u;
        return 1;
    }
    if (addr >= 0xC0000000u && addr < (0xC0000000u + GC_RAM_SIZE)) {
        *out = addr - 0xC0000000u;
        return 1;
    }
    return 0;
}

static inline u32 ram_addr(u32 addr) {
    u32 translated = 0;
    if (translate_ram_addr(addr, &translated)) {
        return translated;
    }
    return 0;
}

static inline u16 read_be16_mem(u32 addr) {
    u16 mmioValue;
    if (gcrecomp_mmio_read16(addr, &mmioValue)) {
        return mmioValue;
    }
    u32 a;
    if (!translate_ram_addr(addr, &a)) {
        return 0;
    }
    return (u16)(((u16)ram[a] << 8) | (u16)ram[a + 1]);
}

static inline u32 read_be32_mem(u32 addr) {
    u32 mmioValue;
    if (gcrecomp_mmio_read32(addr, &mmioValue)) {
        return mmioValue;
    }
    u32 a;
    if (!translate_ram_addr(addr, &a)) {
        return 0;
    }
    return ((u32)ram[a] << 24) |
           ((u32)ram[a + 1] << 16) |
           ((u32)ram[a + 2] << 8) |
           (u32)ram[a + 3];
}

static inline u64 read_be64_mem(u32 addr) {
    const u32 hi = read_be32_mem(addr);
    const u32 lo = read_be32_mem(addr + 4);
    return ((u64)hi << 32) | lo;
}

static inline void write_be16_mem(u32 addr, u16 value) {
    if (gcrecomp_mmio_write16(addr, value)) {
        return;
    }
    u32 a;
    if (!translate_ram_addr(addr, &a)) {
        return;
    }
    ram[a] = (u8)(value >> 8);
    ram[a + 1] = (u8)value;
}

static inline void write_be32_mem(u32 addr, u32 value) {
    if (gcrecomp_mmio_write32(addr, value)) {
        return;
    }
    u32 a;
    if (!translate_ram_addr(addr, &a)) {
        return;
    }
    ram[a] = (u8)(value >> 24);
    ram[a + 1] = (u8)(value >> 16);
    ram[a + 2] = (u8)(value >> 8);
    ram[a + 3] = (u8)value;
}

static inline void write_be64_mem(u32 addr, u64 value) {
    write_be32_mem(addr, (u32)(value >> 32));
    write_be32_mem(addr + 4, (u32)value);
}

static inline u8 MEM_READ8_FN(u32 addr) {
    u8 mmioValue;
    if (gcrecomp_mmio_read8(addr, &mmioValue)) {
        return mmioValue;
    }
    {
        u32 a;
        if (!translate_ram_addr(addr, &a)) {
            return 0;
        }
        return ram[a];
    }
}

static inline u16 MEM_READ16_FN(u32 addr) {
    return read_be16_mem(addr);
}

static inline u32 MEM_READ16A_FN(u32 addr) {
    return (u32)(s32)(s16)read_be16_mem(addr);
}

static inline u32 MEM_READ32_FN(u32 addr) {
    return read_be32_mem(addr);
}

static inline f32 MEM_READ_FLOAT_FN(u32 addr) {
    const u32 bits = read_be32_mem(addr);
    f32 value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline f64 MEM_READ_DOUBLE_FN(u32 addr) {
    const u64 bits = read_be64_mem(addr);
    f64 value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline void MEM_WRITE8_FN(u32 addr, u8 value) {
    if (gcrecomp_mmio_write8(addr, value)) {
        return;
    }
    {
        u32 a;
        if (!translate_ram_addr(addr, &a)) {
            return;
        }
        ram[a] = value;
    }
}

static inline void MEM_WRITE16_FN(u32 addr, u16 value) {
    write_be16_mem(addr, value);
}

static inline void MEM_WRITE32_FN(u32 addr, u32 value) {
    write_be32_mem(addr, value);
}

static inline void MEM_WRITE_FLOAT_FN(u32 addr, f32 value) {
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    write_be32_mem(addr, bits);
}

static inline void MEM_WRITE_DOUBLE_FN(u32 addr, f64 value) {
    u64 bits;
    memcpy(&bits, &value, sizeof(bits));
    write_be64_mem(addr, bits);
}

#define MEM_READ8(addr)        MEM_READ8_FN((addr))
#define MEM_READ16(addr)       MEM_READ16_FN((addr))
#define MEM_READ16A(addr)      MEM_READ16A_FN((addr))
#define MEM_READ32(addr)       MEM_READ32_FN((addr))
#define MEM_READ_FLOAT(addr)   MEM_READ_FLOAT_FN((addr))
#define MEM_READ_DOUBLE(addr)  MEM_READ_DOUBLE_FN((addr))
#define MEM_WRITE8(addr, val)      MEM_WRITE8_FN((addr), (u8)(val))
#define MEM_WRITE16(addr, val)     MEM_WRITE16_FN((addr), (u16)(val))
#define MEM_WRITE32(addr, val)     MEM_WRITE32_FN((addr), (u32)(val))
#define MEM_WRITE_FLOAT(addr, val) MEM_WRITE_FLOAT_FN((addr), (f32)(val))
#define MEM_WRITE_DOUBLE(addr, val) MEM_WRITE_DOUBLE_FN((addr), (f64)(val))

static inline u32 rotl32(u32 value, u32 shift) {
    shift &= 31;
    if (shift == 0) {
        return value;
    }
    return (value << shift) | (value >> (32 - shift));
}

static inline u32 make_ppc_mask(u32 mb, u32 me) {
    mb &= 31;
    me &= 31;

    u32 mask = 0;
    for (u32 bit = 0; bit < 32; ++bit) {
        const int inRange = mb <= me ? (bit >= mb && bit <= me) : (bit >= mb || bit <= me);
        if (inRange) {
            mask |= 0x80000000u >> bit;
        }
    }
    return mask;
}

static inline u32 MASK32(u32 value, u32 mb, u32 me) {
    return value & make_ppc_mask(mb, me);
}

#define RLWIMI(dst, src, sh, mb, me) (((dst) & ~make_ppc_mask((mb), (me))) | (rotl32((src), (sh)) & make_ppc_mask((mb), (me))))

static inline u32 ADDIC(CPUContext* ctx, u32 a, u32 b) {
    const u32 result = a + b;
    if (result < a) {
        ctx->xer |= 0x20000000u;
    } else {
        ctx->xer &= ~0x20000000u;
    }
    return result;
}

static inline u32 PPC_DIVWU(CPUContext* ctx, u32 dividend, u32 divisor) {
    (void)ctx;
    if (divisor == 0) {
        return 0;
    }
    return dividend / divisor;
}

static inline u32 PPC_MULHWU(CPUContext* ctx, u32 lhs, u32 rhs) {
    (void)ctx;
    return (u32)(((u64)lhs * (u64)rhs) >> 32);
}

static inline u32 PPC_MULHW(CPUContext* ctx, u32 lhs, u32 rhs) {
    (void)ctx;
    return (u32)(((s64)(s32)lhs * (s64)(s32)rhs) >> 32);
}

static inline u32 PPC_DIVW(CPUContext* ctx, u32 dividend, u32 divisor) {
    (void)ctx;
    const s32 lhs = (s32)dividend;
    const s32 rhs = (s32)divisor;
    if (rhs == 0) {
        return 0;
    }
    if (lhs == INT32_MIN && rhs == -1) {
        return 0;
    }
    return (u32)(lhs / rhs);
}

static inline void LMW(CPUContext* ctx, u32 startReg, u32 base, u32 simm) {
    u32 addr = base + simm;
    for (u32 reg = startReg; reg < 32; ++reg) {
        ctx->gpr[reg] = MEM_READ32(addr);
        addr += 4;
    }
}

static inline void STMW(CPUContext* ctx, u32 startReg, u32 base, u32 simm) {
    u32 addr = base + simm;
    for (u32 reg = startReg; reg < 32; ++reg) {
        MEM_WRITE32(addr, ctx->gpr[reg]);
        addr += 4;
    }
}

static inline u32 get_cr_bit(const CPUContext* ctx, u32 bit) {
    return (ctx->cr >> (31 - bit)) & 1u;
}

static inline void set_cr_bit(CPUContext* ctx, u32 bit, u32 value) {
    const u32 mask = 1u << (31 - bit);
    ctx->cr = (ctx->cr & ~mask) | ((value & 1u) << (31 - bit));
}

static inline void set_cr_field(CPUContext* ctx, u32 field, u32 a, u32 b, int isSigned) {
    u32 result = 0;
    if (isSigned) {
        if ((s32)a < (s32)b) {
            result = 8;
        } else if ((s32)a > (s32)b) {
            result = 4;
        } else {
            result = 2;
        }
    } else {
        if (a < b) {
            result = 8;
        } else if (a > b) {
            result = 4;
        } else {
            result = 2;
        }
    }

    const u32 shift = 28 - (field * 4);
    ctx->cr = (ctx->cr & ~(0xFu << shift)) | (result << shift);
}

static inline void set_fp_cr_field(CPUContext* ctx, u32 field, f64 a, f64 b, int ordered) {
    u32 result = 0;
    if (isnan(a) || isnan(b)) {
        result = 1;
        (void)ordered;
    } else if (a < b) {
        result = 8;
    } else if (a > b) {
        result = 4;
    } else {
        result = 2;
    }

    const u32 shift = 28 - (field * 4);
    ctx->cr = (ctx->cr & ~(0xFu << shift)) | (result << shift);
}

static inline void mtcrf(CPUContext* ctx, u32 fxm, u32 value) {
    for (u32 field = 0; field < 8; ++field) {
        if (fxm & (0x80u >> field)) {
            const u32 shift = 28 - (field * 4);
            ctx->cr = (ctx->cr & ~(0xFu << shift)) | (value & (0xFu << shift));
        }
    }
}

static inline void cr_and(CPUContext* ctx, u32 bt, u32 ba, u32 bb) {
    set_cr_bit(ctx, bt, get_cr_bit(ctx, ba) & get_cr_bit(ctx, bb));
}

static inline void cr_or(CPUContext* ctx, u32 bt, u32 ba, u32 bb) {
    set_cr_bit(ctx, bt, get_cr_bit(ctx, ba) | get_cr_bit(ctx, bb));
}

static inline void cr_xor(CPUContext* ctx, u32 bt, u32 ba, u32 bb) {
    set_cr_bit(ctx, bt, get_cr_bit(ctx, ba) ^ get_cr_bit(ctx, bb));
}

static inline void cr_nor(CPUContext* ctx, u32 bt, u32 ba, u32 bb) {
    set_cr_bit(ctx, bt, ~(get_cr_bit(ctx, ba) | get_cr_bit(ctx, bb)) & 1u);
}

static inline int CHECK_COND(CPUContext* ctx, u32 bo, u32 bi) {
    int ctrOk = 1;
    int condOk = 1;

    /* PPC BO decoding:
     * - bit 2 (0x04) controls whether CTR is decremented/tested
     * - bit 4 (0x10) controls whether the CR condition is ignored
     */
    if ((bo & 0x04u) == 0) {
        ctx->ctr--;
        ctrOk = ((ctx->ctr != 0) ^ ((bo & 0x02u) != 0));
    }

    if ((bo & 0x10u) == 0) {
        condOk = ((int)get_cr_bit(ctx, bi) == ((bo & 0x08u) != 0));
    }

    return ctrOk && condOk;
}

static inline u32 cntlzw(u32 value) {
    if (value == 0) {
        return 32;
    }

    u32 count = 0;
    if (value <= 0x0000FFFFu) { count += 16; value <<= 16; }
    if (value <= 0x00FFFFFFu) { count += 8; value <<= 8; }
    if (value <= 0x0FFFFFFFu) { count += 4; value <<= 4; }
    if (value <= 0x3FFFFFFFu) { count += 2; value <<= 2; }
    if (value <= 0x7FFFFFFFu) { count += 1; }
    return count;
}

static inline u64 host_timebase_ticks(void) {
    struct timespec ts;
    const u64 timerClock = 40500000ull;
    timespec_get(&ts, TIME_UTC);
    return (((u64)ts.tv_sec * 1000000000ull) + (u64)ts.tv_nsec) * timerClock / 1000000000ull;
}

static inline u32 get_spr(CPUContext* ctx, u32 spr) {
    if (spr == 1) return ctx->xer;
    if (spr == 8) return ctx->lr;
    if (spr == 9) return ctx->ctr;
    if (spr == 268) return (u32)host_timebase_ticks();
    if (spr == 269) return (u32)(host_timebase_ticks() >> 32);
    if (spr < 1024) return ctx->spr[spr];
    return 0;
}

static inline void set_spr(CPUContext* ctx, u32 spr, u32 value) {
    if (spr == 1) ctx->xer = value;
    else if (spr == 8) ctx->lr = value;
    else if (spr == 9) ctx->ctr = value;
    else if (spr < 1024) ctx->spr[spr] = value;
}

static inline f64 FCTIW(f64 value) {
    const s32 converted = (s32)value;
    const u64 bits = 0xFFF8000000000000ull | (u32)converted;
    f64 result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static inline f64 FCTIWZ(f64 value) {
    s32 converted;
    u64 bits;
    f64 result;
    if (value >= 2147483647.0) {
        converted = 0x7FFFFFFF;
    } else if (value <= -2147483648.0) {
        converted = (s32)0x80000000;
    } else {
        converted = (s32)value; /* C truncates toward zero */
    }
    bits = 0xFFF8000000000000ull | (u32)converted;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

/* ---------- Carry-chained arithmetic ---------- */

#define XER_CA_BIT 0x20000000u

static inline u32 PPC_SUBFC(CPUContext* ctx, u32 ra, u32 rb) {
    const u32 result = ~ra + rb + 1u;
    if (rb == 0u && ra != 0u) {
        ctx->xer &= ~XER_CA_BIT;
    } else if (result <= rb || (ra == 0u)) {
        ctx->xer |= XER_CA_BIT;
    } else {
        ctx->xer &= ~XER_CA_BIT;
    }
    /* Precise carry: (~ra + rb + 1) carries iff (u64)(~ra) + rb + 1 > 0xFFFFFFFF */
    {
        const u64 wide = (u64)(~ra) + (u64)rb + 1ull;
        if (wide > 0xFFFFFFFFull) ctx->xer |= XER_CA_BIT;
        else ctx->xer &= ~XER_CA_BIT;
    }
    return result;
}

static inline u32 PPC_ADDC(CPUContext* ctx, u32 ra, u32 rb) {
    const u32 result = ra + rb;
    if (result < ra) {
        ctx->xer |= XER_CA_BIT;
    } else {
        ctx->xer &= ~XER_CA_BIT;
    }
    return result;
}

static inline u32 PPC_SUBFE(CPUContext* ctx, u32 ra, u32 rb) {
    const u32 ca = (ctx->xer & XER_CA_BIT) ? 1u : 0u;
    const u64 wide = (u64)(~ra) + (u64)rb + (u64)ca;
    const u32 result = (u32)wide;
    if (wide > 0xFFFFFFFFull) ctx->xer |= XER_CA_BIT;
    else ctx->xer &= ~XER_CA_BIT;
    return result;
}

static inline u32 PPC_ADDE(CPUContext* ctx, u32 ra, u32 rb) {
    const u32 ca = (ctx->xer & XER_CA_BIT) ? 1u : 0u;
    const u64 wide = (u64)ra + (u64)rb + (u64)ca;
    const u32 result = (u32)wide;
    if (wide > 0xFFFFFFFFull) ctx->xer |= XER_CA_BIT;
    else ctx->xer &= ~XER_CA_BIT;
    return result;
}

static inline u32 PPC_SUBFZE(CPUContext* ctx, u32 ra) {
    const u32 ca = (ctx->xer & XER_CA_BIT) ? 1u : 0u;
    const u64 wide = (u64)(~ra) + (u64)ca;
    const u32 result = (u32)wide;
    if (wide > 0xFFFFFFFFull) ctx->xer |= XER_CA_BIT;
    else ctx->xer &= ~XER_CA_BIT;
    return result;
}

static inline u32 PPC_ADDZE(CPUContext* ctx, u32 ra) {
    const u32 ca = (ctx->xer & XER_CA_BIT) ? 1u : 0u;
    const u64 wide = (u64)ra + (u64)ca;
    const u32 result = (u32)wide;
    if (wide > 0xFFFFFFFFull) ctx->xer |= XER_CA_BIT;
    else ctx->xer &= ~XER_CA_BIT;
    return result;
}

static inline u32 PPC_SUBFME(CPUContext* ctx, u32 ra) {
    const u32 ca = (ctx->xer & XER_CA_BIT) ? 1u : 0u;
    const u64 wide = (u64)(~ra) + 0xFFFFFFFFull + (u64)ca;
    const u32 result = (u32)wide;
    if (wide > 0xFFFFFFFFull) ctx->xer |= XER_CA_BIT;
    else ctx->xer &= ~XER_CA_BIT;
    return result;
}

static inline u32 PPC_ADDME(CPUContext* ctx, u32 ra) {
    const u32 ca = (ctx->xer & XER_CA_BIT) ? 1u : 0u;
    const u64 wide = (u64)ra + 0xFFFFFFFFull + (u64)ca;
    const u32 result = (u32)wide;
    if (wide > 0xFFFFFFFFull) ctx->xer |= XER_CA_BIT;
    else ctx->xer &= ~XER_CA_BIT;
    return result;
}

/* ---------- FPSCR manipulation ---------- */

static inline f64 MFFS(CPUContext* ctx) {
    u64 bits;
    f64 result;
    bits = (u64)ctx->fpscr;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static inline void MTFSF(CPUContext* ctx, u32 fm, f64 frbValue) {
    u64 bits;
    u32 newFpscr;
    u32 mask = 0;
    u32 field;
    memcpy(&bits, &frbValue, sizeof(bits));
    newFpscr = (u32)bits;
    for (field = 0; field < 8; ++field) {
        if (fm & (0x80u >> field)) {
            mask |= (0xFu << (28 - field * 4));
        }
    }
    ctx->fpscr = (ctx->fpscr & ~mask) | (newFpscr & mask);
}

static inline void MTFSFI(CPUContext* ctx, u32 crfD, u32 imm) {
    const u32 shift = 28 - (crfD * 4);
    ctx->fpscr = (ctx->fpscr & ~(0xFu << shift)) | ((imm & 0xFu) << shift);
}

static inline void MTFSB0(CPUContext* ctx, u32 bit) {
    if (bit < 32) {
        ctx->fpscr &= ~(1u << (31 - bit));
    }
}

static inline void MTFSB1(CPUContext* ctx, u32 bit) {
    if (bit < 32) {
        ctx->fpscr |= (1u << (31 - bit));
    }
}

/* ---------- Store float as integer word (stfiwx) ---------- */

static inline void STFIWX(CPUContext* ctx, u32 frs, u32 addr) {
    u64 bits;
    u32 lo;
    memcpy(&bits, &ctx->fpr[frs], sizeof(bits));
    lo = (u32)bits;
    write_be32_mem(addr, lo);
}

/* ---------- FPU reciprocal estimates ---------- */

static inline f64 FRES(f64 value) {
    return (f64)(1.0f / (f32)value);
}

static inline f64 FRSQRTE(f64 value) {
    return 1.0 / sqrt(value);
}

/* ---------- Additional CR logical ops ---------- */

static inline void cr_andc(CPUContext* ctx, u32 bt, u32 ba, u32 bb) {
    set_cr_bit(ctx, bt, get_cr_bit(ctx, ba) & (~get_cr_bit(ctx, bb) & 1u));
}

static inline void cr_eqv(CPUContext* ctx, u32 bt, u32 ba, u32 bb) {
    set_cr_bit(ctx, bt, ~(get_cr_bit(ctx, ba) ^ get_cr_bit(ctx, bb)) & 1u);
}

static inline void cr_nand(CPUContext* ctx, u32 bt, u32 ba, u32 bb) {
    set_cr_bit(ctx, bt, ~(get_cr_bit(ctx, ba) & get_cr_bit(ctx, bb)) & 1u);
}

static inline void cr_orc(CPUContext* ctx, u32 bt, u32 ba, u32 bb) {
    set_cr_bit(ctx, bt, (get_cr_bit(ctx, ba) | (~get_cr_bit(ctx, bb) & 1u)) & 1u);
}

/* ---------- GQR / Paired-Single Quantized Load/Store ---------- */

/*
 * GQR (Graphics Quantization Register) layout:
 *   Bits 16-18: LD_TYPE  (0=f32, 4=u8, 5=u16, 6=s8, 7=s16)
 *   Bits 19-23: LD_SCALE (0-63; dequant multiplier = 2^(-scale), or 2^(scale-0) for ints)
 *   Bits  0-2:  ST_TYPE
 *   Bits  3-7:  ST_SCALE
 *
 * For integer types, dequant = value * 2^(-ld_scale), quant = value * 2^(st_scale)
 *
 * GQRs are SPRs 912-919.
 */

#define GQR_SPR_BASE 912

static inline u32 gqr_ld_type(u32 gqr)  { return (gqr >> 16) & 0x7u; }
static inline u32 gqr_ld_scale(u32 gqr) { return (gqr >> 24) & 0x3Fu; }
static inline u32 gqr_st_type(u32 gqr)  { return gqr & 0x7u; }
static inline u32 gqr_st_scale(u32 gqr) { return (gqr >> 8) & 0x3Fu; }

static inline f64 gqr_dequantize_one(u32 addr, u32 type, u32 scale) {
    const f64 dqScale = (scale == 0) ? 1.0 : (1.0 / (f64)(1u << scale));
    switch (type) {
        case 4: /* u8 */
            return (f64)MEM_READ8(addr) * dqScale;
        case 5: /* u16 */
            return (f64)MEM_READ16(addr) * dqScale;
        case 6: /* s8 */
            return (f64)(s8)MEM_READ8(addr) * dqScale;
        case 7: /* s16 */
            return (f64)(s16)MEM_READ16(addr) * dqScale;
        case 0: /* f32 */
        default:
            return (f64)MEM_READ_FLOAT(addr);
    }
}

static inline u32 gqr_type_size(u32 type) {
    switch (type) {
        case 4: case 6: return 1; /* u8/s8 */
        case 5: case 7: return 2; /* u16/s16 */
        case 0: default: return 4; /* f32 */
    }
}

static inline void PSQ_LOAD(CPUContext* ctx, u32 frd, u32 addr, u32 w, u32 gqrIdx) {
    const u32 gqr = ctx->spr[GQR_SPR_BASE + gqrIdx];
    const u32 type = gqr_ld_type(gqr);
    const u32 scale = gqr_ld_scale(gqr);
    const u32 elemSize = gqr_type_size(type);
    ctx->ps0[frd] = gqr_dequantize_one(addr, type, scale);
    if (w == 0) {
        ctx->ps1[frd] = gqr_dequantize_one(addr + elemSize, type, scale);
    } else {
        ctx->ps1[frd] = 1.0;
    }
    ctx->fpr[frd] = ctx->ps0[frd];
}

static inline void gqr_quantize_one(u32 addr, f64 value, u32 type, u32 scale) {
    const f64 qScale = (scale == 0) ? 1.0 : (f64)(1u << scale);
    switch (type) {
        case 4: { /* u8 */
            s32 v = (s32)(value * qScale + 0.5);
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            MEM_WRITE8(addr, (u8)v);
            break;
        }
        case 5: { /* u16 */
            s32 v = (s32)(value * qScale + 0.5);
            if (v < 0) v = 0;
            if (v > 65535) v = 65535;
            MEM_WRITE16(addr, (u16)v);
            break;
        }
        case 6: { /* s8 */
            s32 v = (s32)(value * qScale);
            if (v < -128) v = -128;
            if (v > 127) v = 127;
            MEM_WRITE8(addr, (u8)(s8)v);
            break;
        }
        case 7: { /* s16 */
            s32 v = (s32)(value * qScale);
            if (v < -32768) v = -32768;
            if (v > 32767) v = 32767;
            MEM_WRITE16(addr, (u16)(s16)v);
            break;
        }
        case 0: /* f32 */
        default:
            MEM_WRITE_FLOAT(addr, (f32)value);
            break;
    }
}

static inline void PSQ_STORE(CPUContext* ctx, u32 frs, u32 addr, u32 w, u32 gqrIdx) {
    const u32 gqr = ctx->spr[GQR_SPR_BASE + gqrIdx];
    const u32 type = gqr_st_type(gqr);
    const u32 scale = gqr_st_scale(gqr);
    const u32 elemSize = gqr_type_size(type);
    gqr_quantize_one(addr, ctx->ps0[frs], type, scale);
    if (w == 0) {
        gqr_quantize_one(addr + elemSize, ctx->ps1[frs], type, scale);
    }
}

/* ---------- Paired-single merge helpers ---------- */

static inline void PS_MERGE00(CPUContext* ctx, u32 fd, u32 fa, u32 fb) {
    ctx->ps0[fd] = ctx->ps0[fa];
    ctx->ps1[fd] = ctx->ps0[fb];
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_MERGE01(CPUContext* ctx, u32 fd, u32 fa, u32 fb) {
    ctx->ps0[fd] = ctx->ps0[fa];
    ctx->ps1[fd] = ctx->ps1[fb];
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_MERGE10(CPUContext* ctx, u32 fd, u32 fa, u32 fb) {
    ctx->ps0[fd] = ctx->ps1[fa];
    ctx->ps1[fd] = ctx->ps0[fb];
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_MERGE11(CPUContext* ctx, u32 fd, u32 fa, u32 fb) {
    ctx->ps0[fd] = ctx->ps1[fa];
    ctx->ps1[fd] = ctx->ps1[fb];
    ctx->fpr[fd] = ctx->ps0[fd];
}

/* ---------- Paired-single arithmetic helpers ---------- */

static inline void PS_ADD(CPUContext* ctx, u32 fd, u32 fa, u32 fb) {
    ctx->ps0[fd] = ctx->ps0[fa] + ctx->ps0[fb];
    ctx->ps1[fd] = ctx->ps1[fa] + ctx->ps1[fb];
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_SUB(CPUContext* ctx, u32 fd, u32 fa, u32 fb) {
    ctx->ps0[fd] = ctx->ps0[fa] - ctx->ps0[fb];
    ctx->ps1[fd] = ctx->ps1[fa] - ctx->ps1[fb];
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_MUL(CPUContext* ctx, u32 fd, u32 fa, u32 fc) {
    ctx->ps0[fd] = ctx->ps0[fa] * ctx->ps0[fc];
    ctx->ps1[fd] = ctx->ps1[fa] * ctx->ps1[fc];
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_DIV(CPUContext* ctx, u32 fd, u32 fa, u32 fb) {
    ctx->ps0[fd] = ctx->ps0[fa] / ctx->ps0[fb];
    ctx->ps1[fd] = ctx->ps1[fa] / ctx->ps1[fb];
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_MADD(CPUContext* ctx, u32 fd, u32 fa, u32 fc, u32 fb) {
    ctx->ps0[fd] = (ctx->ps0[fa] * ctx->ps0[fc]) + ctx->ps0[fb];
    ctx->ps1[fd] = (ctx->ps1[fa] * ctx->ps1[fc]) + ctx->ps1[fb];
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_MSUB(CPUContext* ctx, u32 fd, u32 fa, u32 fc, u32 fb) {
    ctx->ps0[fd] = (ctx->ps0[fa] * ctx->ps0[fc]) - ctx->ps0[fb];
    ctx->ps1[fd] = (ctx->ps1[fa] * ctx->ps1[fc]) - ctx->ps1[fb];
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_NMSUB(CPUContext* ctx, u32 fd, u32 fa, u32 fc, u32 fb) {
    ctx->ps0[fd] = -((ctx->ps0[fa] * ctx->ps0[fc]) - ctx->ps0[fb]);
    ctx->ps1[fd] = -((ctx->ps1[fa] * ctx->ps1[fc]) - ctx->ps1[fb]);
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_NMADD(CPUContext* ctx, u32 fd, u32 fa, u32 fc, u32 fb) {
    ctx->ps0[fd] = -((ctx->ps0[fa] * ctx->ps0[fc]) + ctx->ps0[fb]);
    ctx->ps1[fd] = -((ctx->ps1[fa] * ctx->ps1[fc]) + ctx->ps1[fb]);
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_SUM0(CPUContext* ctx, u32 fd, u32 fa, u32 fc, u32 fb) {
    ctx->ps0[fd] = ctx->ps0[fa] + ctx->ps1[fb];
    ctx->ps1[fd] = ctx->ps1[fc];
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_SUM1(CPUContext* ctx, u32 fd, u32 fa, u32 fc, u32 fb) {
    ctx->ps0[fd] = ctx->ps0[fc];
    ctx->ps1[fd] = ctx->ps0[fa] + ctx->ps1[fb];
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_MULS0(CPUContext* ctx, u32 fd, u32 fa, u32 fc) {
    ctx->ps0[fd] = ctx->ps0[fa] * ctx->ps0[fc];
    ctx->ps1[fd] = ctx->ps1[fa] * ctx->ps0[fc];
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_MULS1(CPUContext* ctx, u32 fd, u32 fa, u32 fc) {
    ctx->ps0[fd] = ctx->ps0[fa] * ctx->ps1[fc];
    ctx->ps1[fd] = ctx->ps1[fa] * ctx->ps1[fc];
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_MADDS0(CPUContext* ctx, u32 fd, u32 fa, u32 fc, u32 fb) {
    ctx->ps0[fd] = (ctx->ps0[fa] * ctx->ps0[fc]) + ctx->ps0[fb];
    ctx->ps1[fd] = (ctx->ps1[fa] * ctx->ps0[fc]) + ctx->ps1[fb];
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_MADDS1(CPUContext* ctx, u32 fd, u32 fa, u32 fc, u32 fb) {
    ctx->ps0[fd] = (ctx->ps0[fa] * ctx->ps1[fc]) + ctx->ps0[fb];
    ctx->ps1[fd] = (ctx->ps1[fa] * ctx->ps1[fc]) + ctx->ps1[fb];
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_SEL(CPUContext* ctx, u32 fd, u32 fa, u32 fc, u32 fb) {
    ctx->ps0[fd] = (ctx->ps0[fa] >= 0.0) ? ctx->ps0[fc] : ctx->ps0[fb];
    ctx->ps1[fd] = (ctx->ps1[fa] >= 0.0) ? ctx->ps1[fc] : ctx->ps1[fb];
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_NEG_FN(CPUContext* ctx, u32 fd, u32 fb) {
    ctx->ps0[fd] = -ctx->ps0[fb];
    ctx->ps1[fd] = -ctx->ps1[fb];
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_MR_FN(CPUContext* ctx, u32 fd, u32 fb) {
    ctx->ps0[fd] = ctx->ps0[fb];
    ctx->ps1[fd] = ctx->ps1[fb];
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_ABS_FN(CPUContext* ctx, u32 fd, u32 fb) {
    ctx->ps0[fd] = fabs(ctx->ps0[fb]);
    ctx->ps1[fd] = fabs(ctx->ps1[fb]);
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_NABS_FN(CPUContext* ctx, u32 fd, u32 fb) {
    ctx->ps0[fd] = -fabs(ctx->ps0[fb]);
    ctx->ps1[fd] = -fabs(ctx->ps1[fb]);
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_RES_FN(CPUContext* ctx, u32 fd, u32 fb) {
    ctx->ps0[fd] = (ctx->ps0[fb] != 0.0) ? (1.0 / ctx->ps0[fb]) : 0.0;
    ctx->ps1[fd] = (ctx->ps1[fb] != 0.0) ? (1.0 / ctx->ps1[fb]) : 0.0;
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void PS_RSQRTE_FN(CPUContext* ctx, u32 fd, u32 fb) {
    ctx->ps0[fd] = (ctx->ps0[fb] > 0.0) ? (1.0 / sqrt(ctx->ps0[fb])) : 0.0;
    ctx->ps1[fd] = (ctx->ps1[fb] > 0.0) ? (1.0 / sqrt(ctx->ps1[fb])) : 0.0;
    ctx->fpr[fd] = ctx->ps0[fd];
}

static inline void set_ps_cr_field(CPUContext* ctx, u32 field, f64 a, f64 b) {
    u32 result = 0;
    if (a != a || b != b) { result = 1; }
    else if (a < b) { result = 8; }
    else if (a > b) { result = 4; }
    else { result = 2; }
    {
        const u32 shift = 28 - (field * 4);
        ctx->cr = (ctx->cr & ~(0xFu << shift)) | (result << shift);
    }
}

/* ---------- dcbz (clear 32-byte cache line) ---------- */

static inline void DCBZ(u32 addr) {
    u32 a;
    addr &= ~0x1Fu; /* align to 32-byte boundary */
    if (translate_ram_addr(addr, &a)) {
        memset(&ram[a], 0, 32);
    }
}

#define EXTSB(x) ((u32)(s32)(s8)(x))
#define EXTSH(x) ((u32)(s32)(s16)(x))
#define FSEL(a, b, c) ((a) >= 0.0 ? (b) : (c))
#define FRSP(x) ((f32)(x))
#define FNMSUB(a, c, b) (-((a) * (c) - (b)))
#define FNMADD(a, c, b) (-((a) * (c) + (b)))

