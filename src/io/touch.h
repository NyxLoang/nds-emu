#ifndef NDS_EMU_IO_TOUCH_H
#define NDS_EMU_IO_TOUCH_H

#include <stdint.h>

/* 阶段 17：触摸屏 + 主 SPI 总线。
   NDS7 主控主 SPI 总线，触摸屏控制器（TSC，TSC2046/AK4148AVT）挂 device=2。
   寄存器：SPICNT 0x040001C0（16 位）、SPIDATA 0x040001C2（8 位）。
   写 SPIDATA 即启动一次 8 位 SPI 传输：写出的字节（命令）沿 MOSI 送出，同时从 MISO
   收一个字节存进 SPIDATA。瞬时模型：写即完成，Busy 恒 0。 */

#define IO_SPICNT   0x040001C0u   /* SPI 控制/状态（16 位） */
#define IO_SPIDATA  0x040001C2u   /* SPI 数据（8 位） */
#define IO_SPI_END  0x040001C4u   /* 触摸 SPI 区间上界（不含） */

/* SPICNT 位定义 */
#define SPICNT_BAUD_MASK    0x0003u   /* bit0-1 波特率 */
#define SPICNT_BUSY         (1u << 7) /* 只读：0=就绪 1=忙（瞬时模型恒 0） */
#define SPICNT_DEVICE_MASK  (3u << 8) /* bit8-9 设备选择 */
#define SPICNT_DEVICE_FW    (1u << 8) /* 设备 1 = 固件 Flash */
#define SPICNT_DEVICE_TOUCH (2u << 8) /* 设备 2 = 触摸屏 */
#define SPICNT_XFERSIZE     (1u << 10)/* 0=8 位 1=16 位(有 bug，本阶段忽略) */
#define SPICNT_HOLD         (1u << 11)/* 0=传输后撤片选 1=保持 */
#define SPICNT_IRQ          (1u << 14)/* 传输完成中断（瞬时模型用不到） */
#define SPICNT_ENABLE       (1u << 15)/* SPI 总线使能 */

/* TSC 通道（命令 bit6-4） */
#define TSC_CH_TEMP0  0
#define TSC_CH_Y      1
#define TSC_CH_BAT    2
#define TSC_CH_Z1     3
#define TSC_CH_Z2     4
#define TSC_CH_X      5
#define TSC_CH_AUX    6
#define TSC_CH_TEMP1  7

/* 常用 TSC 命令（12 位差分，Power Down=0） */
#define TSC_CMD_X   0xD0u   /* X 位置 */
#define TSC_CMD_Y   0x90u   /* Y 位置 */
#define TSC_CMD_Z1  0xA0u   /* Z1 压力 */
#define TSC_CMD_Z2  0xB0u   /* Z2 压力 */
#define TSC_CMD_BAT 0xE0u   /* 电池（NDS 接地，恒 0） */
#define TSC_CMD_AUX 0xF0u   /* 麦克风 */

/* 触摸屏状态：SPICNT/SPIDATA + TSC 命令状态机 + 触摸位置（12 位 ADC）。 */
typedef struct touch {
    uint16_t spicnt;     /* SPICNT（busy 位只读，恒 0） */
    uint8_t  spidata;    /* SPIDATA（最近一次传输收到的字节） */
    int      fw_cs;      /* 21-B9j+：device1 片选是否仍保持（0=事务已结束） */
    uint8_t  fw_cmd;     /* 21-B9j+：当前事务命令：0x03=READ、0x05=RDSR */
    uint32_t fw_addr;    /* 21-B9j+：READ 地址（3 字节大端；数据阶段逐字节自增） */
    int      fw_addr_n;  /* 21-B9j+：已收到的 READ 地址字节数（0..3） */
    /* TSC 内部状态机 */
    uint8_t  cmd;        /* 最近一条命令字节 */
    uint8_t  channel;    /* 解码出的通道（0-7） */
    int      mode8;      /* 1=8 位转换，0=12 位 */
    uint16_t result;     /* 当前命令的 ADC 结果（8 位时存低 8 位） */
    int      reply_idx;  /* 下一个要回传的字节序号：0=首字节 1=次字节 >=2=填充 0 */
    /* 触摸位置（原始 12 位 ADC 值） */
    uint16_t adc_x;      /* X：0=未按下 */
    uint16_t adc_y;      /* Y：0xFFF=未按下 */
    int      down;       /* 1=笔按下 */
} touch_t;

int  touch_is_addr(uint32_t addr);
uint8_t touch_read8(touch_t *t, uint32_t addr);
void touch_write8(touch_t *t, uint32_t addr, uint8_t val);

/* 外部信号：设置触摸位置（12 位 ADC 值），down=1 表示笔按下。 */
void touch_set_pos(touch_t *t, uint16_t adc_x, uint16_t adc_y, int down);

#endif /* NDS_EMU_IO_TOUCH_H */
