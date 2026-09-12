#ifndef NDS_EMU_BIOS7_IMAGE_H
#define NDS_EMU_BIOS7_IMAGE_H

#include <stdint.h>

/* ARM7 低地址（0x0000-0x3FFF）BIOS 镜像。

   真机 ARM7 的这段地址是 BIOS ROM，任何一次读都返回 FreeBIOS 镜像里的字节。
   FFXII 的 ARM7 调度器会把「在 BIOS 内部被打断」的现场存进任务上下文
   （PC 可能是 0x0008 向量或 0x11xx 等待循环），恢复后 CPU 会直接落在这些
   低地址上继续执行；SWI 分发器的 `ldrb r12,[lr,#-2]` 也会趁这个时候从 BIOS
   里读编号字节。本地没有 BIOS ROM，低地址一律读 0，会让这类恢复路径拿到
   0 号（SoftReset）而不是参考核的 0x04，随后栈帧错位跑飞。

   21-B9wt：先只影子化「向量表 + SWI 函数表」两类字节（足够让分发器取到编号）。
   21-B9yd（本步）：改为**完整 0x4000 字节镜像**（BSD-2 许可的 FreeBIOS ARM7，
   melonDS `FreeBIOS_Data.h` 的 `bios_ntr_arm7`，有效长度 0x2030，其余补 0，
   见 `bios7_rom.h`）。两个直接收益：

   <1> 游戏读 BIOS 低地址的任意字节（自检、取表、拷贝）与参考核逐字节一致；
   <2> 未被 `bios7_low.c` 逐条建模的 SWI 函数体（Div/CpuSet/Sqrt/LZ77/…）
       可以在真地址上被 CPU 真实执行，BIOS 内部的 PC/lr/栈帧/周期数与
       参考核一致，而不是「在分发器里算完直接跳 swi_complete」。 */
int bios7_image_read8(uint32_t addr, uint8_t *out);

/* 镜像本体（测试与诊断用）：有效长度 BIOS7_ROM_VALID，其余为 0。 */
const unsigned char *bios7_image_data(void);
uint32_t bios7_image_size(void);
uint32_t bios7_image_valid_size(void);

#endif /* NDS_EMU_BIOS7_IMAGE_H */
