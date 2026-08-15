# NDS ROM 安全区加密（KEY1 / Blowfish）

## 一句话

商业 NDS 卡带用 **Blowfish**（任天堂自定义的 KEY1 变体）加密了一段 2KB 的「安全区」（secure area）。要顺畅玩商业 `.nds`，模拟器必须在装载时用**游戏码（gamecode）**派生出密钥，把这段安全区解密出来。

## 为什么要有安全区

任天堂为了防止盗版卡带，给每张卡加了一道锁：

- 卡带里有段特殊数据（2KB 的安全区），里面放了游戏自己的「加密钥匙」和校验信息。
- 这段安全区是用 **游戏码（gamecode）** 加密的。游戏码是每款游戏唯一的 4 字节标识（如 `NTRJ`、`ASME`）。
- 只有知道正确游戏码的（真机 BIOS 或模拟器）才能解开安全区。游戏启动后会读这段解密后的数据做自校验 / 拿自己的钥匙。

> 注意：**加密的只有「安全区」这一段**（ARM9 镜像最前面的 0x800 字节），不是整个 ARM9/ARM7 二进制。ARM7 二进制和 ARM9 剩余部分都是明文。

## 安全区在哪

- 安全区长度固定 **0x800 字节（2KB）**。
- 位置在 **ARM9 ROM offset 指向的地方**（头字段 `0x020`）。商业 ROM 的 ARM9 offset 通常是 `0x4000`，所以安全区就是文件 `0x4000..0x47FF`。
- 解密后的安全区开头 8 字节是固定魔数 **`"encryObj"`**（`e n c r y O b j`），用来校验「解密成功」。

## KEY1 是什么

KEY1 就是 **Blowfish 块密码**，但换了一张**定制的密钥表**。

标准 Blowfish 靠「P 数组 + S 盒」工作：

| 部件 | 大小 | 作用 |
|------|------|------|
| P 数组 | 18 个 32 位字 | 每轮与数据异或 |
| S 盒 1..4 | 各 256 个 32 位字 | 非线性查表混淆 |

18 + 4×256 = **1042 字 = 0x1048 字节**。

任天堂的定制之处：这张表不是标准 Blowfish 的「圆周率小数」表，而是一张**写死在 NDS BIOS 里的表**，位于 ARM7 BIOS `0x30..0x1077`，开头字节是 `99 D5 20 5F 57 44 F5 B9 ...`。模拟器里我们把它作为常量表硬编码（来源：ndstool 的 `encr_data`，与 melonDS/nds-bootstrap 使用的一致）。

## Blowfish 块加解密（64 位）

每块是 8 字节，看成两个字：`x`（高字，后 4 字节）和 `y`（低字，前 4 字节）。

**加密**（16 轮）：

```text
for i = 0..15:
    z = P[i] ^ x
    x = S1[z 的高 8 位] + S2[z 的次 8 位] ^ S3[z 的次低 8 位] + S4[z 的低 8 位] ^ y
    y = z
输出：低字 = x ^ P[16]，高字 = y ^ P[17]
```

**解密**（逆序 16 轮，从 P[17] 往回用到 P[2]，最后 `^ P[1]`、`^ P[0]`）。

## 密钥派生：init_keycode(level)

把「游戏码」变成「工作密钥表」的过程，分 3 个 level：

```text
keycode[0] = gamecode
keycode[1] = gamecode >> 1
keycode[2] = gamecode << 1

level >= 1: apply_keycode()
level >= 2: apply_keycode()
level >= 3: keycode[1] <<= 1; keycode[2] >>= 1; apply_keycode()
```

`apply_keycode()` 做三件事：

1. 用当前密钥表加密 `keycode` 的后两个字，再加密前两个字（keycode 被就地改写）。
2. 把 P 数组每个字 `^= bswap32(keycode[i % 2])`（注意只用到 `keycode[0]`、`keycode[1]` 两个字，`keycode[2]` 不参与这里的异或）。
3. 用新的 P 数组把整张表（P + S 盒）重新「滚」一遍。

> 关键：level 3 的 `keycode[1]<<=1 / keycode[2]>>=1` 是**在 level 2 已经滚过两遍的 keycode 上再调整**，不是从原始 gamecode 重新算。melonDS 用「整体重新 init」实现，ndstool 用「增量继续」实现，两者结果一致。

## 安全区解密流程

```text
1. 用 init_keycode(level=2) 得到 level2 密钥
2. 用 level2 密钥解密安全区前 8 字节        （剥掉最外层）
3. 用 init_keycode(level=3) 得到 level3 密钥
4. 用 level3 密钥解密安全区全部 0x800 字节   （每 8 字节一块）
5. 检查开头 8 字节是否等于 "encryObj"
   - 是 → 解密成功（这 8 字节可保留，或按真机改成 0xE7FFDEFF 防执行）
   - 否 → 说明这卡没加密（homebrew），保持原样
```

对应的**加密**流程正好反过来：先设 `"encryObj"` 头 → 用 level3 加密 body（8..0x800）→ 用 level3 加密头 8 字节 → 用 level2 加密头 8 字节（最外层）。

所以「前 8 字节」被 **level3 + level2 两层**加密，而 body 只被 level3 加密一层。

## 对我们模拟器意味着什么

阶段 1 只能装 homebrew（无安全区、明文）。本阶段补上：

1. **Blowfish（KEY1）块加解密** + 密钥表常量。
2. **按 gamecode 派生密钥**（init_keycode 三级）。
3. **安全区就地解密**（ARM9 镜像前 0x800 字节）。
4. 装载时：把 ROM 缓冲里安全区解密后，再拷进 Main RAM，让游戏看到明文安全区。

## 自测（先别看答案）

1. 安全区多长、在 ROM 的哪个位置？
2. KEY1 的密钥表有多少字节，由哪几部分组成？
3. 解密后怎么知道「解对了」？
4. level 3 与 level 2 的密钥派生有什么本质区别？

<details>
<summary>答案</summary>

1. 0x800 字节（2KB），在 ARM9 ROM offset（头 `0x020`）指向处，商业 ROM 通常是 `0x4000`。
2. 0x1048 字节 = P 数组 18 字 + S 盒 4×256 字（1042 字），来自 NDS ARM7 BIOS `0x30..0x1077`。
3. 开头 8 字节解密后等于 `"encryObj"`（魔数校验）。
4. level 3 是在 level 2 滚过的 keycode 上再 `keycode[1]<<=1 / keycode[2]>>=1` 后继续滚一遍，不是重新从 gamecode 算。

</details>
