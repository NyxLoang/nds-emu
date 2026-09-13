/* ARM9 高地址 BIOS 可读字节：FreeBIOS ARM9 镜像 + 运行期注入的 Nintendo logo。
   详细动机见 bios9_image.h / docs/21-rom-bringup.md 续108。 */

#include <string.h>
#include "bios9_image.h"
#include "bios9_rom.h"

static uint8_t s_logo[BIOS9_LOGO_SIZE];
static int s_logo_set;

void bios9_image_set_logo(const uint8_t *logo)
{
    if (logo == NULL) {
        memset(s_logo, 0, sizeof s_logo);
        s_logo_set = 0;
        return;
    }
    memcpy(s_logo, logo, sizeof s_logo);
    s_logo_set = 1;
}

int bios9_image_read8(uint32_t off, uint8_t *out)
{
    if (out == NULL || off >= BIOS9_ROM_SIZE)
        return 0;
    if (s_logo_set && off >= BIOS9_LOGO_OFFSET &&
        off < BIOS9_LOGO_OFFSET + BIOS9_LOGO_SIZE) {
        *out = s_logo[off - BIOS9_LOGO_OFFSET];
    } else {
        *out = bios9_rom[off];
    }
    return 1;
}
