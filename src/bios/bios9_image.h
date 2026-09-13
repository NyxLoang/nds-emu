#ifndef NDS_EMU_BIOS9_IMAGE_H
#define NDS_EMU_BIOS9_IMAGE_H

#include <stdint.h>

/* ARM9 高地址 BIOS（0xFFFF0000-0xFFFF3FFF）的可读字节。

   21-B9yi（续108）：**新增**。起因是与参考核（melonDS）逐帧对账时发现，从第 50 帧起
   我方主存里始终有 156 字节对不上，而那 156 字节正好是**任天堂 logo**：

   ```
   melonDS  NDS::SetupDirectBoot():
       // Copy the Nintendo logo from the NDS ROM header to the ARM9 BIOS if using FreeBIOS
       // Games need this for DS<->GBA comm to work
       memcpy(ARM9BIOS.data() + 0x20, header.NintendoLogo, 0x9C);

   FFXII 自己的启动代码（用 trace + --watch 定位，pc=0x02012750）：
       memcpy(0x020798A4, 0xFFFF0020, 0x9C);   // 读的是 ARM9 BIOS 里的那份 logo
   ```

   本模拟器此前对 ARM9 BIOS 区**一律读 0**（只把 0xFFFF0018 那几条异常向量
   用 HLE 特判掉），于是这次拷贝得到全 0 ⇒ 游戏状态自此与参考核分叉。

   现在按参考核口径提供内容：
     * 偏移 0x20..0xBB = 由 `bios9_image_set_logo()` 注入的 Nintendo logo
       （调用方直接给 ROM 头里的那 156 字节，与参考核「从卡带头拷进 BIOS」等价，
        因此不额外携带任何 ROM 数据）；
     * 其余偏移 = FreeBIOS ARM9 镜像（BSD-2，来源见 bios9_rom.h）。
*/

#define BIOS9_LOGO_OFFSET 0x20u
#define BIOS9_LOGO_SIZE   0x9Cu

/* 读 BIOS 偏移 off（0..0x3FFF）的一个字节。命中返回 1 并写 *out。 */
int bios9_image_read8(uint32_t off, uint8_t *out);

/* 注入 Nintendo logo（`BIOS9_LOGO_SIZE` 字节）。传 NULL 复位成全 0。 */
void bios9_image_set_logo(const uint8_t *logo);

#endif /* NDS_EMU_BIOS9_IMAGE_H */
