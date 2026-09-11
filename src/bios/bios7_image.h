#ifndef NDS_EMU_BIOS7_IMAGE_H
#define NDS_EMU_BIOS7_IMAGE_H

#include <stdint.h>

/* 21-B9wt：ARM7 低地址 BIOS 的“可读字节影子”。

   真机 ARM7 的 0x0000-0x3FFF 是 BIOS ROM，任何一次读都返回 FreeBIOS 镜像里
   的字节。FFXII 的 ARM7 调度器会把「在 BIOS 内部被打断」的现场存进任务
   上下文（PC 可能是 0x0008 向量或 0x11xx 等待循环），恢复后 CPU 会直接落在
   这些低地址上继续执行；SWI 分发器的 `ldrb r12,[lr,#-2]` 也会趁这个时候从
   BIOS 里读编号字节。本地没有 BIOS ROM，低地址一律读 0，会让这类恢复路径
   拿到 0 号（SoftReset）而不是参考核的 0x04，随后栈帧错位跑飞。

   这里提供与低地址路径有关的可读字节（BSD-2 许可的 FreeBIOS ARM7 镜像：
   melonDS `FreeBIOS_Data.h` 的 `bios_ntr_arm7`，只影射向量表与 SWI 函数表；
   其余低地址仍读 0——本项目用 HLE 实现函数体，不执行 BIOS 字节码）。 */
int bios7_image_read8(uint32_t addr, uint8_t *out);

#endif /* NDS_EMU_BIOS7_IMAGE_H */
