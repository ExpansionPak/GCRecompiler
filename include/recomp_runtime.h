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

#define EXTSB(x) ((u32)(s32)(s8)(x))
#define EXTSH(x) ((u32)(s32)(s16)(x))
#define FSEL(a, b, c) ((a) >= 0.0 ? (b) : (c))
#define FRSP(x) ((f32)(x))
