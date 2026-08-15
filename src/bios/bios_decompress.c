#include <stdint.h>
#include "bios_decompress.h"
#include "cpu/cpu.h"
#include "bus/bus.h"

/* 从源位流读 n 位（MSB-first：每个字节的高位先出）。
   用于 BitUnPack 的源位流解包。 */
static uint32_t src_bits(bus_t *bus, uint32_t src, uint32_t bitpos, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        uint32_t byte = src + (bitpos >> 3);
        unsigned shift = 7 - (bitpos & 7);
        v = (v << 1) | ((bus_read8(bus, byte) >> shift) & 1u);
        bitpos++;
    }
    return v;
}

/* SWI 0x10 BitUnPack：位流解包（把窄位宽的源单元展开为宽位宽的目的单元）。
   入：r0=源、r1=目的（32 位对齐）、r2=UnPack 信息指针
     +0 16bit 源数据长度（字节）
     +2 8bit  源单元位宽（1/2/4/8）
     +3 8bit  目的单元位宽（1/2/4/8/16/32）
     +4 32bit bit0-30 数据偏移、bit31 零单元也加偏移标志
   数据按 32 位单位写入。 */
int bios_bitunpack(arm_cpu_t *cpu)
{
    bus_t *bus = cpu->nds->bus;
    uint32_t src = cpu->r[0];
    uint32_t dst = cpu->r[1];
    uint32_t info = cpu->r[2];

    uint32_t src_len = bus_read16(bus, info);
    unsigned src_w = bus_read8(bus, info + 2);
    unsigned dst_w = bus_read8(bus, info + 3);
    uint32_t off = bus_read32(bus, info + 4);
    uint32_t data_off = off & 0x7FFFFFFFu;
    int zero_flag = (off >> 31) & 1;

    uint32_t units = ((uint32_t)src_len * 8u) / src_w;
    uint32_t unit_mask = (dst_w >= 32) ? 0xFFFFFFFFu : ((1u << dst_w) - 1u);

    uint32_t bitpos = 0;
    uint64_t out = 0;       /* 用 64 位累积，避免边界移位 UB */
    unsigned outbits = 0;
    uint32_t written = 0;

    for (uint32_t i = 0; i < units; i++) {
        uint32_t v = src_bits(bus, src, bitpos, src_w);
        bitpos += src_w;
        if (v != 0 || zero_flag)
            v += data_off;
        out |= (uint64_t)(v & unit_mask) << outbits;
        outbits += dst_w;
        while (outbits >= 32) {
            bus_write32(bus, dst + written, (uint32_t)(out & 0xFFFFFFFFu));
            written += 4;
            out >>= 32;
            outbits -= 32;
        }
    }
    if (outbits > 0)
        bus_write32(bus, dst + written, (uint32_t)(out & 0xFFFFFFFFu));
    return 1;
}

/* SWI 0x11/0x12 LZ77UnComp：LZ77 解压。
   入：r0=源（含 32 位头）、r1=目的
   头：bit0-3 保留、bit4-7 类型(=1)、bit8-31 解压后字节数
   其后：每字节标志（MSB 先，8 块）；块 0=1 字节字面量，块 1=2 字节回溯
   （低字节 bit0-3=位移高 4 位、bit4-7=长度-3，高字节=位移低 8 位）。 */
int bios_lz77(arm_cpu_t *cpu)
{
    bus_t *bus = cpu->nds->bus;
    uint32_t src = cpu->r[0];
    uint32_t dst = cpu->r[1];

    uint32_t header = bus_read32(bus, src);
    uint32_t size = (header >> 8) & 0xFFFFFFu;
    uint32_t sp = src + 4;
    uint32_t written = 0;

    while (written < size) {
        uint8_t flags = bus_read8(bus, sp++);
        for (int b = 0; b < 8 && written < size; b++) {
            if (flags & 0x80u) {
                uint16_t v = bus_read16(bus, sp);
                sp += 2;
                uint32_t disp = ((v & 0xFu) << 8) | ((v >> 8) & 0xFFu);
                uint32_t len = ((v >> 4) & 0xFu) + 3;
                for (uint32_t i = 0; i < len; i++) {
                    uint8_t byte = bus_read8(bus, dst + written - disp - 1);
                    bus_write8(bus, dst + written, byte);
                    written++;
                }
            } else {
                bus_write8(bus, dst + written, bus_read8(bus, sp++));
                written++;
            }
            flags <<= 1;
        }
    }
    return 1;
}

/* SWI 0x13 HuffUnComp：Huffman 解压。
   入：r0=源（4 字节对齐）、r1=目的
   头：bit0-3 数据位宽（4/8）、bit4-7 类型(=2)、bit8-31 解压后字节数
   +4：树表字节数/2-1（也即位流偏移量）
   树节点（8bit）：bit0-5 子节点偏移、bit6 node1 是数据、bit7 node0 是数据
   子节点地址 = (节点地址 & ~1) + 偏移*2 + 2（+0=node0，+1=node1）
   位流按 32 位字、bit31 为第一位读取。 */
int bios_huff(arm_cpu_t *cpu)
{
    bus_t *bus = cpu->nds->bus;
    uint32_t src = cpu->r[0];
    uint32_t dst = cpu->r[1];

    uint32_t header = bus_read32(bus, src);
    uint32_t size = (header >> 8) & 0xFFFFFFu;
    uint32_t tree_root = src + 5;                        /* 跳过 4 字节头 + 1 字节树长 */
    uint32_t tree_size_byte = bus_read8(bus, src + 4);
    uint32_t bitstream = tree_root + ((tree_size_byte + 1u) * 2u);

    uint32_t written = 0;
    uint32_t node = tree_root;
    uint32_t word = bus_read32(bus, bitstream);
    bitstream += 4;
    int bitidx = 31;

    while (written < size) {
        int bit = (word >> bitidx) & 1u;
        bitidx--;
        if (bitidx < 0) {
            word = bus_read32(bus, bitstream);
            bitstream += 4;
            bitidx = 31;
        }

        uint8_t nb = bus_read8(bus, node);
        uint32_t offset = nb & 0x3Fu;
        uint32_t base = (node & ~1u) + offset * 2u + 2u;
        uint32_t child = base + (uint32_t)bit;
        int is_data = bit ? ((nb >> 6) & 1) : ((nb >> 7) & 1);

        if (is_data) {
            bus_write8(bus, dst + written, bus_read8(bus, child));
            written++;
            node = tree_root;
        } else {
            node = child;
        }
    }
    return 1;
}

/* SWI 0x14/0x15 RLUnComp：RLE 解压。
   入：r0=源（含 32 位头）、r1=目的
   头：bit0-3 保留、bit4-7 类型(=3)、bit8-31 解压后字节数
   其后：每字节标志（bit7=0 未压缩 N-1 字节字面量；bit7=1 压缩 1 字节重复 N-3 次）。 */
int bios_rl(arm_cpu_t *cpu)
{
    bus_t *bus = cpu->nds->bus;
    uint32_t src = cpu->r[0];
    uint32_t dst = cpu->r[1];

    uint32_t header = bus_read32(bus, src);
    uint32_t size = (header >> 8) & 0xFFFFFFu;
    uint32_t sp = src + 4;
    uint32_t written = 0;

    while (written < size) {
        uint8_t flag = bus_read8(bus, sp++);
        uint32_t len = flag & 0x7Fu;
        if (flag & 0x80u) {
            len += 3; /* 压缩：1 字节重复 len 次 */
            uint8_t v = bus_read8(bus, sp++);
            for (uint32_t i = 0; i < len; i++)
                bus_write8(bus, dst + written++, v);
        } else {
            len += 1; /* 未压缩：len 字节字面量 */
            for (uint32_t i = 0; i < len; i++)
                bus_write8(bus, dst + written++, bus_read8(bus, sp++));
        }
    }
    return 1;
}
