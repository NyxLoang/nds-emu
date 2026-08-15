#ifndef NDS_EMU_AUDIO_H
#define NDS_EMU_AUDIO_H

struct nds; /* 前向声明：避免 audio.h 依赖 nds.h */

/* 打开 SDL 音频设备并启动回调（回调从 snd_render 取样本送声卡）。失败返回 -1。 */
int audio_init(struct nds *nds);

/* 关闭设备并解除绑定。 */
void audio_shutdown(void);

#endif /* NDS_EMU_AUDIO_H */
