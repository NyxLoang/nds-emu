# 阶段 16 — NDS 存档（Backup Memory：EEPROM / Flash / FRAM）

> 目标是「顺畅玩 .nds 游戏」：游戏要能**保存进度**并在下次启动时读回。NDS 的存档芯片
> （backup memory）挂在卡带槽的**辅助 SPI 总线**上，与阶段 15 的 ROM 命令总线（ROMCTRL/
> CARD_COMMAND/CARD_DATA）是两套独立的接口。本文先讲清楚协议，再据此实现。

---

## 1. 存档类型（芯片）

NDS 卡带常见几种存档芯片，大小与「写页」不同：

| 类型 | 总大小 | 写页 | 典型芯片 / 游戏 |
|------|--------|------|------------------|
| EEPROM 0.5K | 512 字节 | 16 字节 | ST M95040（Metroid Demo） |
| EEPROM 8K | 8 KB | 32 字节 | ST M95640（Super Mario DS） |
| EEPROM 64K | 64 KB | 128 字节 | ST M95512（Downhill Jam） |
| EEPROM 128K | 128 KB | 128 字节 | （Explorers of Sky） |
| FLASH 256K | 256 KB | 256 字节 | ST M45PE20（Mariokart） |
| FLASH 512K | 512 KB | 256 字节 | ST 45PE40V6（Zelda） |
| FLASH 1M | 1 MB | 256 字节 | ST 45PE80V6（Spirit Tracks） |
| FLASH 8M | 8 MB | 256 字节 | MX25L6445（Art Academy） |
| FRAM 32K | 32 KB | 无限制 | Ramtron FM25L256（少见） |

- **FRAM** 与普通 EEPROM 完全向后兼容，只是写/擦无延迟、无写页限制、寿命无限。
- **Flash** 的命令集与主机固件 Flash 相同（多了 `RDID` 读芯片 ID、页擦除等）。
- 少数卡带把 Flash 与红外（Infrared）共用一个 SPI 总线，访问存档要先发一个 `00h` 前缀字节、
  并降到 1MHz 时钟加延时（本阶段先实现纯 Flash/EEPROM，红外前缀后续补）。

---

## 2. 寄存器：AUXSPICNT / AUXSPIDATA

存档芯片经两个 I/O 端口访问（NDS7 与 NDS9 同址，均可访问）：

- `0x040001A0` — **AUXSPICNT**（16 位，卡带槽 ROM/SPI 控制/状态）
- `0x040001A2` — **AUXSPIDATA**（8 位，SPI 数据/选通）

### AUXSPICNT 位定义（GBATEK）

| 位 | 名称 | 含义 |
|----|------|------|
| 0-1 | Baudrate | SPI 时钟：0=4MHz(默认) 1=2MHz 2=1MHz 3=512KHz |
| 2-5 | — | 未用（恒 0） |
| 6 | Hold Chipselect | 0=每传一字节后自动撤片选；1=保持片选 |
| 7 | Busy | 0=就绪 1=忙（只读） |
| 8-12 | — | 未用（恒 0） |
| 13 | Slot Mode | 0=并行/ROM 模式；**1=串行/SPI-Backup 模式**（选中存档芯片） |
| 14 | Transfer Ready IRQ | ROM 用（AUXSPI 不用） |
| 15 | Slot Enable | 0=禁用；1=启用（ROM 与 AUXSPI 都要） |

- 访问存档的典型写法：`AUXSPICNT = 0xA040`（bit15 使能 + bit13 SPI 模式 + bit6 保持片选）；
  传**最后一个字节前**清 bit6（`0xA000`），传完后片选自动撤下、命令结束。

### AUXSPIDATA

- 8 位数据口。**写这个寄存器即启动一次 SPI 字节传输**：写进去的值沿 MOSI 送到芯片，
  同时从芯片 MISO 收一个字节，传输完成后可从 `AUXSPIDATA` 读回收到的字节。
- 因此「只读」操作也要写一个哑元 `0x00` 来触发时钟，再读回结果。
- 传输期间 Busy（bit7）置位；本模拟器用**瞬时模型**（写即完成、Busy 恒 0）。

---

## 3. SPI 命令

### 3.1 EEPROM / FRAM 命令

| 命令 | 操作码 | 作用 |
|------|--------|------|
| WREN | `06` | 写使能（置 WEL 锁存） |
| WRDI | `04` | 写禁止（清 WEL） |
| RDSR | `05` | 读状态寄存器 |
| WRSR | `01` | 写状态寄存器 |
| READ | `03` | 读数据（命令 + 地址 + 读字节） |
| WRITE | `02` | 写数据（命令 + 地址 + 数据字节） |

- 状态寄存器：bit0 = WIP（写忙，本模拟器恒 0）、bit1 = WEL（写使能锁存）。
- **地址宽度**（READ/WRITE 的地址字节数）：
  - 0.5K EEPROM：9 位地址（操作码 bit3 携带 A8，后跟 1 个地址字节）。
  - 8K EEPROM / 32K FRAM：16 位地址（2 字节）。
  - 64K / 128K EEPROM：24 位地址（3 字节，实际只用到低 16/17 位）。

### 3.2 Flash 命令（在 EEPROM 基础上增加）

| 命令 | 操作码 | 作用 |
|------|--------|------|
| RDID (JEDEC) | `9F` | 读芯片 ID（返回 3 字节厂商/型号） |
| READ | `03` | 读数据（24 位地址） |
| FAST READ | `0B` | 快速读（+1 哑元字节） |
| PP / PW | `02` / `0A` | 页写（256 字节页；Flash 写是「与」操作，只能 1→0） |
| SE / PE | `D8` / `DB` | 扇区/页擦除（把区域擦成 `FF`） |
| CE | `C7` / `60` | 整片擦除 |
| DP | `B9` | 深度掉电 |
| RDP | `AB` | 退出深度掉电 |

- Flash 地址一律 **24 位（3 字节）**。
- 写/擦前通常要先 `WREN`；擦除后对应区域全 `0xFF`。

---

## 4. 典型读写流程

### 4.1 读 EEPROM（8K，读 4 字节）

```
AUXSPICNT = 0xA040            ; 使能 + SPI 模式 + 保持片选
AUXSPIDATA = 0x03             ; READ 命令
AUXSPIDATA = addr_hi          ; 地址高字节
AUXSPIDATA = addr_lo          ; 地址低字节
AUXSPICNT  = 0xA000           ; 清 bit6（最后一字节前）
AUXSPIDATA = 0x00             ; 哑元触发传输，收 data[addr]
b0 = AUXSPIDATA               ; （片选已自动撤下）
AUXSPICNT = 0xA040            ; 再选片
AUXSPIDATA = 0x00             ; 收 data[addr+1]
...（重复）
```

### 4.2 写 EEPROM

```
WREN（0x06） → WRITE（0x02）→ 地址 → 数据字节（按页对齐，跨页要分段）
```

### 4.3 写 Flash

```
WREN（0x06） → PP（0x02）→ 24 位地址 → 最多 256 字节数据
（擦除：WREN → PE/SE → 24 位地址）
```

---

## 5. 模拟器实现要点（本阶段）

- 新模块 `src/cart/save.h/.c`：`save_t` 持存档缓冲 + SPI 状态机（命令/地址/数据三态）。
- `cartbus`（阶段 15）里的 AUXSPICNT/AUXSPIDATA 本阶段接上真语义：写 AUXSPIDATA 触发
  `save_transfer`，按 bit13/bit15 判定是否选中存档芯片、按 bit6 判定传完是否撤片选复位命令。
- **瞬时模型**：一次写即完成一次 SPI 字节传输，Busy 恒 0，写/擦无延迟。
- 持久化：启动时从 `<rom>.sav` 读回存档缓冲，退出时写回；文件不存在则按类型清空。
- 地址宽度按存档类型区分；Flash 写用「与」语义、擦除填 `0xFF`。

---

## 6. 验收标准（阶段 16）

- 单元：`save_transfer` 直接喂命令序列——WREN/RDSR/READ/WRITE（EEPROM）、RDID/READ/PP/SE（Flash）
  读写结果正确、写使能锁存与状态位正确、跨页/越界行为合理。
- 综合：一段 CPU 程序经 AUXSPICNT/AUXSPIDATA 读写存档，断言 RAM/存档缓冲内容一致。
- 持久化：写存档 → `save_save_file` → 重新 `save_load_file` → 读回一致。
- `ctest` 全绿、`test_nds.exe` 全量 0 失败。
