#include "Platform.h"
#include "NDS.h"
#include "NDSCart.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace melonDS;

/* 21-B9yi(续32)：melonDS 的 GPU3D.cpp 在 namespace melonDS 内，符号也在这个命名空间。 */
namespace melonDS { extern unsigned long long RefGxHist[256]; }

/* 21-B9yi(续35)：给 GPU3D 的多边形 dump 用的当前帧号（-1 = 关闭）。
   GPU3D.cpp 在 namespace melonDS 里，符号必须定义在同一命名空间。 */
namespace melonDS { int RefDbgFrame = -1; }

static int g_trace_frame = -1;
static int g_fifo_trace_count = 0;

namespace melonDS { namespace Platform {

struct FileHandle { std::FILE* f; };

void SignalStop(StopReason, void*) {}
std::string GetLocalFilePath(const std::string& fn) { return fn; }
FileHandle* OpenFile(const std::string& path, FileMode mode) {
    const char* m = "rb";
    if (mode & Write) m = (mode & Preserve) ? "r+b" : "wb";
    if (mode & Append) m = "ab";
    std::FILE* f = std::fopen(path.c_str(), m);
    return f ? new FileHandle{f} : nullptr;
}
FileHandle* OpenLocalFile(const std::string& p, FileMode m) { return OpenFile(p, m); }
bool FileExists(const std::string& n) {
    auto* f = OpenFile(n, (FileMode)(Read | NoCreate));
    if (!f) return false;
    CloseFile(f);
    return true;
}
bool LocalFileExists(const std::string& n) { return FileExists(n); }
bool CheckFileWritable(const std::string& p) { auto* f=OpenFile(p,(FileMode)(Write|Preserve)); if(!f)return false; CloseFile(f); return true; }
bool CheckLocalFileWritable(const std::string& p) { return CheckFileWritable(p); }
bool CloseFile(FileHandle* f) { if(!f)return false; bool ok=std::fclose(f->f)==0; delete f; return ok; }
bool IsEndOfFile(FileHandle* f) { return f && std::feof(f->f); }
bool FileReadLine(char* s,int n,FileHandle* f){ return f && std::fgets(s,n,f->f); }
u64 FilePosition(FileHandle* f){ return f?(u64)std::ftell(f->f):0; }
bool FileSeek(FileHandle* f,s64 off,FileSeekOrigin o){ if(!f)return false; int w=o==FileSeekOrigin::Start?SEEK_SET:o==FileSeekOrigin::Current?SEEK_CUR:SEEK_END; return std::fseek(f->f,(long)off,w)==0; }
void FileRewind(FileHandle* f){ if(f) std::rewind(f->f); }
u64 FileRead(void* d,u64 sz,u64 cnt,FileHandle* f){ return f?(u64)std::fread(d,(size_t)sz,(size_t)cnt,f->f):0; }
bool FileFlush(FileHandle* f){ return f && std::fflush(f->f)==0; }
u64 FileWrite(const void* d,u64 sz,u64 cnt,FileHandle* f){ return f?(u64)std::fwrite(d,(size_t)sz,(size_t)cnt,f->f):0; }
u64 FileWriteFormatted(FileHandle* f,const char* fmt,...){ if(!f)return 0; va_list ap; va_start(ap,fmt); int n=std::vfprintf(f->f,fmt,ap); va_end(ap); return n>0?(u64)n:0; }
u64 FileLength(FileHandle* f){ if(!f)return 0; long p=std::ftell(f->f); std::fseek(f->f,0,SEEK_END); long e=std::ftell(f->f); std::fseek(f->f,p,SEEK_SET); return (u64)e; }

void Log(LogLevel,const char* fmt,...){ va_list ap; va_start(ap,fmt); std::vfprintf(stderr,fmt,ap); std::fputc('\n',stderr); va_end(ap); }

struct Thread { std::function<void()> fn; };
Thread* Thread_Create(std::function<void()> fn){ auto* t=new Thread{std::move(fn)}; t->fn(); return t; }
void Thread_Free(Thread* t){ delete t; }
void Thread_Wait(Thread*) {}
struct Semaphore { int v; };
Semaphore* Semaphore_Create(){ return new Semaphore{0}; }
void Semaphore_Free(Semaphore* s){ delete s; }
void Semaphore_Reset(Semaphore* s){ if(s)s->v=0; }
void Semaphore_Wait(Semaphore* s){ if(s) s->v--; }
bool Semaphore_TryWait(Semaphore* s,int){ if(!s)return false; if(s->v>0){s->v--;return true;} return false; }
void Semaphore_Post(Semaphore* s,int c){ if(s) s->v+=c; }
struct Mutex { bool locked; };
Mutex* Mutex_Create(){ return new Mutex{false}; }
void Mutex_Free(Mutex* m){ delete m; }
void Mutex_Lock(Mutex* m){ if(m)m->locked=true; }
void Mutex_Unlock(Mutex* m){ if(m)m->locked=false; }
bool Mutex_TryLock(Mutex* m){ if(!m)return false; if(m->locked)return false; m->locked=true; return true; }
void Sleep(u64){}
u64 GetMSCount(){ return 0; }
u64 GetUSCount(){ return 0; }
void WriteNDSSave(const u8*,u32,u32,u32,void*){}
void WriteGBASave(const u8*,u32,u32,u32,void*){}
void WriteFirmware(const Firmware&,u32,u32,void*){}
void WriteDateTime(int,int,int,int,int,int,void*){}
void MP_Begin(void*){} void MP_End(void*){}
int MP_SendPacket(u8*,int,u64,void*){return 0;}
int MP_RecvPacket(u8*,u64*,void*){return 0;}
int MP_SendCmd(u8*,int,u64,void*){return 0;}
int MP_SendReply(u8*,int,u64,u16,void*){return 0;}
int MP_SendAck(u8*,int,u64,void*){return 0;}
int MP_RecvHostPacket(u8*,u64*,void*){return 0;}
u16 MP_RecvReplies(u8*,u64,u16,void*){return 0;}
int Net_SendPacket(u8*,int,void*){return 0;}
int Net_RecvPacket(u8*,void*){return 0;}
void Camera_Start(int,void*){} void Camera_Stop(int,void*){}
void Camera_CaptureFrame(int,u32*,int,int,bool,void*){}
void Mic_Start(void*){} void Mic_Stop(void*){}
int Mic_ReadInput(s16*,int,void*){return 0;}
struct AACDecoder{};
AACDecoder* AAC_Init(){return nullptr;}
void AAC_DeInit(AACDecoder*){}
bool AAC_Configure(AACDecoder*,int,int){return false;}
bool AAC_DecodeFrame(AACDecoder*,const void*,int,void*,int){return false;}
bool Addon_KeyDown(KeyType,void*){return false;}
void Addon_RumbleStart(u32,void*){} void Addon_RumbleStop(void*){}
float Addon_MotionQuery(MotionQueryType,void*){return 0.f;}
struct DynamicLibrary{};
DynamicLibrary* DynamicLibrary_Load(const char*){return nullptr;}
void DynamicLibrary_Unload(DynamicLibrary*){}
void* DynamicLibrary_LoadFunction(DynamicLibrary*,const char*){return nullptr;}

}}

class RefNDS : public NDS
{
public:
    using NDS::NDS;
    void DumpBios(const char* path)
    {
        FILE* fb = std::fopen(path, "wb");
        if (fb) { std::fwrite(ARM7BIOS.data(), 1, ARM7BIOS.size(), fb); std::fclose(fb); }
    }
    u16 GetIPCSync9() const { return IPCSync9; }
    u16 GetIPCSync7() const { return IPCSync7; }
    u16 GetCnt9() const { return IPCFIFOCnt9; }
    u16 GetCnt7() const { return IPCFIFOCnt7; }
    int GetFIFO9Count() const { return (int)IPCFIFO9.Level(); }
    int GetFIFO7Count() const { return (int)IPCFIFO7.Level(); }

    void TraceWram7(const char* op, u32 addr, u64 val, u32 newval)
    {
        u32 off = addr & 0xFFFF;
        bool in_a = off >= 0x8240 && off <= 0x8260;
        bool in_b = off >= 0x8480 && off <= 0x84A0;
        bool in_b2 = off >= 0x82D0 && off <= 0x8300;
        bool in_c = off >= 0xA7E0 && off <= 0xA860;
        bool in_d = off >= 0xA990 && off <= 0xA9D0;
        bool in_e = off >= 0xBA80 && off <= 0xBB00;
        if (!in_a && !in_b && !in_b2 && !in_c && !in_d && !in_e) return;
        if (TraceWram7Count >= 5000) return;
        TraceWram7Count++;
        std::printf("wram7 %s pc=%08X lr=%08X a=%08X v=%08llX new=%08X"
                    " r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X sp=%08X"
                    " cpsr=%08X ipc=%04X/%04X cnt=%04X/%04X f=%d/%d if9=%08X if7=%08X\n",
            op, ARM7.R[15], ARM7.R[14], addr, (unsigned long long)val, newval,
            ARM7.R[0], ARM7.R[1], ARM7.R[2], ARM7.R[3], ARM7.R[4], ARM7.R[5],
            ARM7.R[13], ARM7.CPSR,
            GetIPCSync9(), GetIPCSync7(), GetCnt9(), GetCnt7(),
            GetFIFO9Count(), GetFIFO7Count(), IF[0], IF[1]);
        std::fflush(stdout);
    }

    void ARM7Write8(u32 addr, u8 val) override
    {
        u32 oldv = 0;
        bool interested = (addr & 0xFF800000) == 0x03800000;
        if (interested) {
            u32 o = addr & 0xFFFF;
            interested = (o >= 0x8240 && o <= 0x8260) ||
                         (o >= 0x8480 && o <= 0x84A0) ||
                         (o >= 0x82D0 && o <= 0x8300) ||
                         (o >= 0xA7E0 && o <= 0xA860) ||
                         (o >= 0xA990 && o <= 0xA9D0) ||
                         (o >= 0xBA80 && o <= 0xBB00);
        }
        if (interested) oldv = *(u8*)&ARM7WRAM[addr & (ARM7WRAMSize - 1)];
        NDS::ARM7Write8(addr, val);
        if (interested && oldv != val) TraceWram7("w8", addr, val, val);
    }

    void ARM7Write16(u32 addr, u16 val) override
    {
        u32 oldv = 0;
        bool interested = (addr & 0xFF800000) == 0x03800000;
        if (interested) {
            u32 o = addr & 0xFFFF;
            interested = (o >= 0x8240 && o <= 0x8260) ||
                         (o >= 0x8480 && o <= 0x84A0) ||
                         (o >= 0x82D0 && o <= 0x8300) ||
                         (o >= 0xA7E0 && o <= 0xA860) ||
                         (o >= 0xA990 && o <= 0xA9D0) ||
                         (o >= 0xBA80 && o <= 0xBB00);
        }
        if (interested) oldv = *(u16*)&ARM7WRAM[addr & (ARM7WRAMSize - 1)];
        NDS::ARM7Write16(addr, val);
        if (interested && oldv != val) TraceWram7("w16", addr, val, val);
    }

    void ARM7Write32(u32 addr, u32 val) override
    {
        u32 oldv = 0;
        bool interested = (addr & 0xFF800000) == 0x03800000;
        if (interested) {
            u32 o = addr & 0xFFFF;
            interested = (o >= 0x8240 && o <= 0x8260) ||
                         (o >= 0x8480 && o <= 0x84A0) ||
                         (o >= 0x82D0 && o <= 0x8300) ||
                         (o >= 0xA7E0 && o <= 0xA860) ||
                         (o >= 0xA990 && o <= 0xA9D0) ||
                         (o >= 0xBA80 && o <= 0xBB00);
        }
        if (interested) oldv = *(u32*)&ARM7WRAM[addr & (ARM7WRAMSize - 1)];
        if ((addr & ~3u) == 0x04000188u || (addr & ~3u) == 0x04000180u)
            TraceFifoSend(1, addr & ~3u, val);
        NDS::ARM7Write32(addr, val);
        if (interested && oldv != val) TraceWram7("w32", addr, val, val);
    }

    void TraceFifoSend(int who, u32 addr, u32 val)
    {
        if (addr != 0x04000188u && addr != 0x04000180u) return;
        if (g_fifo_trace_count >= 100) return;
        g_fifo_trace_count++;
        u32 pc = who == 0 ? ARM9.R[15] : ARM7.R[15];
        u32 lr = who == 0 ? ARM9.R[14] : ARM7.R[14];
        u32 sp = who == 0 ? ARM9.R[13] : ARM7.R[13];
        u32 stk[20] = {0};
        for (int i = 0; i < 20; i++)
        {
            u32 ra = sp + 4 + i * 4;
            if (who == 0)
            {
                if ((ra & ARM9.DTCMMask) == ARM9.DTCMBase && ARM9.DTCM)
                    stk[i] = *(u32*)&ARM9.DTCM[ra & (0x4000 - 1)];
                else if (ra >= 0x02000000u && ra < 0x04000000u)
                    stk[i] = *(u32*)&MainRAM[ra & MainRAMMask];
            }
            else
            {
                if (ra >= 0x03800000u && ra < 0x03810000u)
                    stk[i] = *(u32*)&ARM7WRAM[ra & (ARM7WRAMSize - 1)];
                else if (ra >= 0x02000000u && ra < 0x04000000u)
                    stk[i] = *(u32*)&MainRAM[ra & MainRAMMask];
            }
        }
        std::printf("fifo%s pc=%08X lr=%08X a=%08X v=%08X frame=%d"
                    " ipc=%04X/%04X cnt=%04X/%04X f=%d/%d if9=%08X if7=%08X"
                    " r0=%08X r1=%08X r2=%08X cpsr=%08X sp=%08X t9=%llu t7=%llu"
                    " st=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X"
                    "/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X\n",
            who == 0 ? "9" : "7", pc, lr, addr, val, g_trace_frame,
            GetIPCSync9(), GetIPCSync7(), GetCnt9(), GetCnt7(),
            GetFIFO9Count(), GetFIFO7Count(), IF[0], IF[1],
            who == 0 ? ARM9.R[0] : ARM7.R[0],
            who == 0 ? ARM9.R[1] : ARM7.R[1],
            who == 0 ? ARM9.R[2] : ARM7.R[2],
            who == 0 ? ARM9.CPSR : ARM7.CPSR,
            sp,
            (unsigned long long)ARM9Timestamp, (unsigned long long)ARM7Timestamp,
            stk[0], stk[1], stk[2], stk[3], stk[4], stk[5], stk[6], stk[7],
            stk[8], stk[9], stk[10], stk[11], stk[12], stk[13], stk[14], stk[15],
            stk[16], stk[17], stk[18], stk[19]);
        if (who == 1 && (val == 0xC0204006u || val == 0xC0240046u)) {
            std::printf("queue dump:");
            std::printf(" q82D8=%04X q82DA=%04X q82C4=%08X q8248=%08X",
                *(u16*)&ARM7WRAM[0x82D8], *(u16*)&ARM7WRAM[0x82DA],
                *(u32*)&ARM7WRAM[0x82C4], *(u32*)&ARM7WRAM[0x8248]);
            for (u32 a = 0x0380A780u; a < 0x0380AAC0u; a += 4)
                std::printf(" %08X=%08X", a,
                    *(u32*)&ARM7WRAM[(a & 0xFFFF) & (ARM7WRAMSize - 1)]);
            std::printf(" t80=%08X t84=%08X t88=%08X t8C=%08X t90=%08X",
                *(u32*)&ARM7WRAM[0x8480], *(u32*)&ARM7WRAM[0x8484],
                *(u32*)&ARM7WRAM[0x8488], *(u32*)&ARM7WRAM[0x848C],
                *(u32*)&ARM7WRAM[0x8490]);
            std::printf(" st244=%08X", *(u32*)&ARM7WRAM[0x8244]);
            std::printf("\n");
        }
        std::fflush(stdout);
    }

    void ARM9Write32(u32 addr, u32 val) override
    {
        if ((addr & ~3u) == 0x04000188u || (addr & ~3u) == 0x04000180u)
            TraceFifoSend(0, addr & ~3u, val);
        if (addr == 0x02079104u || addr == 0x02076F18u ||
            addr == 0x02077048u || addr == 0x0207704Cu ||
            addr == 0x02076F88u || addr == 0x02076F8Cu ||
            addr == 0x020799C4u || addr == 0x020799C8u ||
            (addr >= 0x02077020u && addr < 0x02077040u) ||
            (addr >= 0x020799A0u && addr < 0x020799C0u))
        {
            static int dbg9 = 0;
            if (dbg9 < 5000)
            {
                std::printf("a9w a=%08X v=%08X pc=%08X r0=%08X r1=%08X r2=%08X r7=%08X\n",
                    addr, val, ARM9.R[15], ARM9.R[0], ARM9.R[1], ARM9.R[2],
                    ARM9.R[7]);
                if (addr == 0x02076F18u && val == 0x02076FE4u)
                {
                    std::printf("ctx e4 20=%08X 24=%08X 28=%08X 2C=%08X"
                                " 30=%08X 34=%08X 38=%08X 3C=%08X\n",
                        *(u32*)&MainRAM[0x77020], *(u32*)&MainRAM[0x77024],
                        *(u32*)&MainRAM[0x77028], *(u32*)&MainRAM[0x7702C],
                        *(u32*)&MainRAM[0x77030], *(u32*)&MainRAM[0x77034],
                        *(u32*)&MainRAM[0x77038], *(u32*)&MainRAM[0x7703C]);
                }
                std::fflush(stdout);
            }
            dbg9++;
        }
        NDS::ARM9Write32(addr, val);
    }

    int TraceWram7Count = 0;
};

int main(int argc, char** argv)
{
    if (argc < 2) return 1;
    std::FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 2;
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    auto rom = std::make_unique<u8[]>(len);
    std::fread(rom.get(), 1, len, f);
    std::fclose(f);

    auto nds = std::make_unique<RefNDS>();
    std::printf("nds created\n"); std::fflush(stdout);

    auto cart = NDSCart::ParseROM(std::move(rom), (u32)len);
    std::printf("cart %p\n", (void*)cart.get()); std::fflush(stdout);
    if (!cart) return 3;

    nds->SetNDSCart(std::move(cart));
    std::printf("cart set\n"); std::fflush(stdout);
    nds->Reset();
    std::printf("reset done\n"); std::fflush(stdout);
    nds->SetupDirectBoot(std::string(argv[1]));
    std::printf("directboot done\n"); std::fflush(stdout);
    nds->Start();
    std::printf("start done\n"); std::fflush(stdout);

    int ref_frames = 6000;
    if (const char* ef = std::getenv("REF_FRAMES"))
        ref_frames = std::atoi(ef);
    std::printf("ref frames=%d\n", ref_frames);

    /* 21-B9yi(续32)：按键注入 + 画面统计，使参考核与本地
       `--key-frame/--key-mask/--key-period`、`--stats-every` 逐帧同口径。
       按键时序：f >= KEY_FRAME 且 ((f-KEY_FRAME) % PERIOD) < 12 时为按下。 */
    int key_frame = 1700, key_mask = 0x3F7, key_period = 120;
    if (const char* v = std::getenv("REF_KEY_FRAME")) key_frame = std::atoi(v);
    if (const char* v = std::getenv("REF_KEY_MASK")) key_mask = (int)std::strtoul(v, nullptr, 0);
    if (const char* v = std::getenv("REF_KEY_PERIOD")) key_period = std::atoi(v);
    int stats_every = 0;
    if (const char* v = std::getenv("REF_STATS_EVERY")) stats_every = std::atoi(v);
    int iodump_frame = -1;
    if (const char* v = std::getenv("REF_IODUMP_FRAME")) iodump_frame = std::atoi(v);

    for (int frame = 0; frame < ref_frames; frame++)
    {
        g_trace_frame = frame;
        RefDbgFrame = frame;
        if (key_period > 0 && frame >= key_frame) {
            int phase = (frame - key_frame) % key_period;
            nds->SetKeyMask((phase < 12) ? (u32)key_mask : 0u);
        }
        nds->RunFrame();
        /* 21-B9yi(续36)：REF_IODUMP_FRAME=N → 打印第 N 帧的 2D 显示寄存器
           （与本地 runner 的 `NDS_IODUMP_FRAME` 同口径对照）。 */
        if (frame == iodump_frame) {
            static const u32 regs[] = {
                0x04000000, 0x04000008, 0x0400000A, 0x0400000C, 0x0400000E,
                0x04000010, 0x04000012, 0x04000014, 0x04000016,
                0x04001000, 0x04001008, 0x0400100A, 0x0400100C, 0x0400100E,
                0x04001010, 0x04001012, 0x04001014, 0x04001016,
                0x04000060, 0x04000064, 0x04000240, 0x04000241, 0x04000242,
                0x04000243, 0x04000244, 0x04000245, 0x04000246, 0x04000247,
                0x04000248,
                0x04000050, 0x04000052, 0x04000054, 0x0400006C,
                0x04001050, 0x04001052, 0x04001054, 0x0400106C
            };
            for (u32 a : regs)
                std::printf("refio: %08X=%04X\n", a, (unsigned)nds->ARM9IORead16(a));
            std::fflush(stdout);
        }
        /* 画面统计：与本地 runner 的 `--stats-every` 同一口径（非黑像素数 +
           各通道均值），用于判定某段画面是「定格」还是「持续推进」。 */
        if (stats_every > 0 && (frame % stats_every) == 0) {
            void* top = nullptr; void* bottom = nullptr;
            if (nds->GPU.GetFramebuffers(&top, &bottom) && top && bottom) {
                const u32* ft = (const u32*)top;
                const u32* fb = (const u32*)bottom;
                const int n = 256 * 192;
                unsigned nzt = 0, nzb = 0;
                unsigned long long tr = 0, tg = 0, tb = 0, br = 0, bg = 0, bb = 0;
                for (int i = 0; i < n; i++) {
                    u32 p = ft[i];
                    if (p & 0x00FFFFFFu) nzt++;
                    tr += (p >> 16) & 0xFFu; tg += (p >> 8) & 0xFFu; tb += p & 0xFFu;
                    p = fb[i];
                    if (p & 0x00FFFFFFu) nzb++;
                    br += (p >> 16) & 0xFFu; bg += (p >> 8) & 0xFFu; bb += p & 0xFFu;
                }
                std::printf("stats: f=%d top nz=%u/%d rgb=%llu,%llu,%llu"
                            " bot nz=%u/%d rgb=%llu,%llu,%llu\n",
                            frame, nzt, n, tr / n, tg / n, tb / n,
                            nzb, n, br / n, bg / n, bb / n);
                std::fflush(stdout);
            }
        }
        /* 21-B9yi(续28)：REF_SHOT_EVERY=N → 每 N 帧把两块 256x192 帧缓冲
           （顶屏在前、32bpp）写到 %TEMP%\ref_fb_<frame>.bin，
           供本地 `--shot-every` 的 BMP 逐像素对照。 */
        {
            const char* se = std::getenv("REF_SHOT_EVERY");
            if (se != nullptr) {
                int every = std::atoi(se);
                if (every > 0 && (frame % every) == 0) {
                    const char* tp = std::getenv("TEMP");
                    if (!tp) tp = ".";
                    char fn[160];
                    std::snprintf(fn, sizeof fn, "%s\\ref_fb_%d.bin", tp, frame);
                    FILE* fs = std::fopen(fn, "wb");
                    if (fs) {
                        void* top = nullptr; void* bottom = nullptr;
                        nds->GPU.GetFramebuffers(&top, &bottom);
                        if (top && bottom) {
                            std::fwrite(top, 1, 256 * 192 * 4, fs);
                            std::fwrite(bottom, 1, 256 * 192 * 4, fs);
                        }
                        std::fclose(fs);
                    }
                }
            }
        }
        u32 disp = nds->ARM9IORead32(0x04000000);
        if (disp != 0 || frame < 100 || (frame % 50) == 0)
            std::printf("frame %d disp=%08X\n", frame, disp);
        if (frame == 20 || frame == 30 || frame == 50 || frame == 80 ||
            frame == 120 || frame == 180 || frame == 250 ||
            frame == 350 || frame == 500 || frame == 590)
        {
            const char* tmp = std::getenv("TEMP");
            if (!tmp) tmp = ".";
            char fname[64];
            std::snprintf(fname, sizeof fname, "\\ref_f%d_", frame);
            std::string base = std::string(tmp) + fname;
            FILE* fa = std::fopen((base + "arm7wram.bin").c_str(), "wb");
            FILE* fs = std::fopen((base + "sharedwram.bin").c_str(), "wb");
            FILE* fm = std::fopen((base + "mainram.bin").c_str(), "wb");
            if (fa) { std::fwrite(nds->ARM7WRAM, 1, 0x10000, fa); std::fclose(fa); }
            if (fs) { std::fwrite(nds->SharedWRAM, 1, 0x8000, fs); std::fclose(fs); }
            if (fm) { std::fwrite(nds->MainRAM, 1, 0x400000, fm); std::fclose(fm); }
            nds->DumpBios((base + "arm7bios.bin").c_str());
        }
        u32 a9 = nds->ARM9.R[15];
        bool interesting = (a9 >= 0x0200B830u && a9 <= 0x0200B860u) ||
                           (a9 >= 0x0200F1A0u && a9 <= 0x0200F1C0u) ||
                           (a9 >= 0x02009570u && a9 <= 0x02009590u);
        if ((frame % 50) == 0 || interesting || frame < 25)
        {
            std::printf("frame %d ARM9=%08X ARM7=%08X ipc=%04X/%04X cnt=%04X/%04X f=%d/%d if=%08X fw=%08X t0=%08X",
                frame, a9, nds->ARM7.R[15],
                nds->GetIPCSync9(), nds->GetIPCSync7(),
                nds->GetCnt9(), nds->GetCnt7(),
                nds->GetFIFO9Count(), nds->GetFIFO7Count(),
                nds->IF[0], *(u32*)&nds->ARM7WRAM[0xBA94],
                *(u32*)&nds->ARM7WRAM[0x8498]);
            std::fflush(stdout);
            /* 21-B9yi(续32)：默认不再“命中空闲任务就退出”——长程对照需要跑到
               REF_FRAMES。设 REF_STOP_IDLE=1 可恢复旧行为（快速定位 idle）。 */
            if (interesting && frame > 1500 &&
                std::getenv("REF_STOP_IDLE") != nullptr) break;
        }
    }

    std::printf("done arm9=%08X arm7=%08X\n",
        nds->ARM9.R[15], nds->ARM7.R[15]);
    /* 21-B9yi(续32)：GX 命令直方图（与本地 `NDS_GXHIST=1` 的 gxhist 行对照）。 */
    {
        if (std::getenv("REF_GXHIST") != nullptr) {
            std::printf("refgxhist:");
            for (int c = 0; c < 256; c++)
                if (RefGxHist[c]) std::printf(" %02X=%llu", c, RefGxHist[c]);
            std::printf("\n");
        }
    }
    return 0;
}
