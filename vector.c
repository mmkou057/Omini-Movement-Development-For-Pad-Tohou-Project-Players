/* vector.c - 运动层速度矢量直接映射, 多游戏 profile（TH15 / TH17） */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include "vector.h"

/* padhook.c 提供 */
extern void pad_log(const char *fmt, ...);
/* 返回手柄设备是否存活; x/y 为归一化值（右正/上正）, pov 为十字键（0xFFFFFFFF=未按） */
extern int  pad_get_stick(double *x, double *y, unsigned long *pov);

#define IMG_BASE        0x00400000u
#define VEC_DEAD        0.24       /* 径向死区, 与 PWM 路径一致 */
#define POV_NONE        0xFFFFFFFFu

#define KIND_NONE       0
#define KIND_TH15       15
#define KIND_TH17       17

static BYTE *g_base = NULL;
static int   g_kind = KIND_NONE;

/* ====================================================================== */
/* Profile 注册表                                                           */
/* ====================================================================== */

typedef struct {
    int kind;
    const char* name;
    int (*install)(void);
    /* 玩家位置偏移（从玩家对象基址起），0=未知；单位：定点 1/128 px */
    DWORD pos_off_x;
    DWORD pos_off_y;
    /* IDirect3DDevice9* 全局指针 RVA（游戏 exe 内），0=未知 */
    DWORD device_rva;
} VecProfile;

/* 前向声明 install 函数 */
static int install_th15(void);
static int install_th17(void);

static const VecProfile g_profiles[] = {
    { KIND_TH15, "TH15", install_th15, 0x061c, 0x0620, 0x4e77d8 },
    { KIND_TH17, "TH17", install_th17, 0x061c, 0x0620, 0x4b5ae8 },
};
static int g_profile_idx = -1;

/* ====================================================================== */
/* 位置快照导出（供轨迹插件读取）                                            */
/* ====================================================================== */

typedef struct {
    volatile long seq;   /* 奇数=正在写, 偶数=可读, 0=未初始化 */
    volatile float x;    /* 逻辑像素（定点/128） */
    volatile float y;
    volatile int kind;
} VmPosSnap;

static VmPosSnap g_snap = {0};

/* 每 tick 在游戏运动 hook 顶部调用 */
static void vm_tick_pos(int kind, void *player) {
    if (!player) return;
    const VecProfile *pf = (g_profile_idx >= 0) ? &g_profiles[g_profile_idx] : NULL;
    if (!pf || pf->pos_off_x == 0) return;

    BYTE *b = (BYTE*)player;
    int px = *(int*)(b + pf->pos_off_x);
    int py = *(int*)(b + pf->pos_off_y);

    long seq = g_snap.seq + 1;
    g_snap.seq = seq;  /* 奇数：正在写 */
    g_snap.x = (float)px / 128.0f;
    g_snap.y = (float)py / 128.0f;
    g_snap.kind = kind;
    __sync_synchronize();
    g_snap.seq = seq + 1;  /* 偶数：可读 */
}

/* 导出：供 traj.dll 通过 GetProcAddress 读取 */
__declspec(dllexport) int vm_get_player_pos(float *ox, float *oy, long *oseq) {
    long seq1, seq2;
    do {
        seq1 = g_snap.seq;
        if ((seq1 & 1) || seq1 == 0) return 0;
        if (ox) *ox = g_snap.x;
        if (oy) *oy = g_snap.y;
        seq2 = g_snap.seq;
    } while (seq1 != seq2);
    if (oseq) *oseq = seq1;
    return 1;
}

__declspec(dllexport) int vm_get_game_kind(void) {
    return g_kind;
}

__declspec(dllexport) DWORD vm_get_device_rva(void) {
    const VecProfile *pf = (g_profile_idx >= 0) ? &g_profiles[g_profile_idx] : NULL;
    return pf ? pf->device_rva : 0;
}

__declspec(dllexport) int vm_get_stick(float *ox, float *oy) {
    double x, y;
    unsigned long pov;
    if (!pad_get_stick(&x, &y, &pov)) return 0;
    if (ox) *ox = (float)x;
    if (oy) *oy = (float)y;
    return 1;
}

/* ====================================================================== */
/* 纯数学                                                                   */
/* ====================================================================== */

int vector_unit(double x, double y, double dead, double *ux, double *uy) {
    double r2 = x*x + y*y;
    if (r2 < dead*dead) return 0;
    double r = sqrt(r2);
    if (ux) *ux = x / r;
    if (uy) *uy = -y / r;       /* 游戏 y 向下 */
    return 1;
}

int vector_compute(double x, double y, double dead, int spd, double gs,
                   float *vx, float *vy) {
    double ux, uy;
    if (!vector_unit(x, y, dead, &ux, &uy)) return 0;
    if (spd <= 0 || !(gs > 0.0)) return 0;
    double s = (double)spd * gs;
    *vx = (float)(s * ux);
    *vy = (float)(s * uy);
    return 1;
}

/* TH17 玩法心跳（运动协程每 tick 一次; 暂停/菜单时协程停跑, 超时即不算玩法） */
static LARGE_INTEGER g_qpf = {0}, g_last_tick = {0};
static int           g_tick_init = 0;

static void heartbeat(void) {
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_last_tick);
    g_tick_init = 1;
}

int vector_in_gameplay(void) {
    if (g_kind == KIND_TH15) {
        if (!g_base) g_base = (BYTE*)GetModuleHandleA(NULL);
        if (!g_base) return 0;
        void **pp = (void**)(g_base + 0x0e9bb8u);    /* TH15 4e9bb8 */
        return *pp != NULL;
    }
    if (g_kind == KIND_TH17) {
        if (!g_tick_init || !g_qpf.QuadPart) return 0;
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double ms = (double)(now.QuadPart - g_last_tick.QuadPart) * 1000.0
                  / (double)g_qpf.QuadPart;
        return ms <= 250.0;
    }
    return 0;
}

/* ====================================================================== */
/* TH15 v1.00b                                                             */
/* ====================================================================== */

#define T15_RVA_HOOK    0x05453du
#define T15_RVA_JE      0x05455bu
#define T15_RVA_CONT    0x054547u
#define T15_OFF_FOCUS   0x16240
#define T15_OFF_SPD_C   0x16298
#define T15_OFF_SPD_F   0x1629c
#define T15_OFF_VX      0x162a8
#define T15_OFF_VY      0x162ac
#define T15_RVA_GS      0x0e73e8u

static const BYTE T15_SIG[5] = { 0x74,0x1c,0xf3,0x0f,0x7e };
static const BYTE T15_MOVQ[8] = {
    0xf3,0x0f,0x7e,0x87, 0xa8,0x62,0x01,0x00   /* movq xmm0,[edi+0x162a8] */
};

void __cdecl vector_apply_th15(void *p) {
    vm_tick_pos(KIND_TH15, p);
    if (g_kind != KIND_TH15 || !p) return;
    heartbeat();
    double x, y; unsigned long pov;
    if (!pad_get_stick(&x, &y, &pov)) return;
    if (pov != POV_NONE) return;    /* POV 十字键优先: 走原生数字注入路径 */

    BYTE *b = (BYTE*)p;
    int focus = *(int*)(b + T15_OFF_FOCUS);
    int spd   = *(int*)(b + (focus ? T15_OFF_SPD_F : T15_OFF_SPD_C));
    float gs  = *(float*)(g_base + T15_RVA_GS);

    float vx, vy;
    if (!vector_compute(x, y, VEC_DEAD, spd, (double)gs, &vx, &vy)) return;

    *(float*)(b + T15_OFF_VX) = vx;
    *(float*)(b + T15_OFF_VY) = vy;
}

static int install_th15(void) {
    BYTE *site = g_base + T15_RVA_HOOK;
    if (memcmp(site, T15_SIG, sizeof T15_SIG) != 0) return 0;

    BYTE *cave = (BYTE*)VirtualAlloc(NULL, 64,
                        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave) { pad_log("VEC th15: VirtualAlloc failed (%lu)", GetLastError()); return 0; }

    int i = 0;
    cave[i++] = 0x9C;                                   /* pushfd */
    cave[i++] = 0x60;                                   /* pushad */
    cave[i++] = 0x57;                                   /* push edi */
    cave[i++] = 0xB8; *(DWORD*)(cave + i) = (DWORD)&vector_apply_th15; i += 4;
    cave[i++] = 0xFF; cave[i++] = 0xD0;                 /* call eax */
    cave[i++] = 0x83; cave[i++] = 0xC4; cave[i++] = 0x04; /* add esp,4 */
    cave[i++] = 0x61;                                   /* popad */
    cave[i++] = 0x9D;                                   /* popfd */
    cave[i++] = 0x0F; cave[i++] = 0x84;                 /* je rel32 */
    *(DWORD*)(cave + i) = (DWORD)((long)(g_base + T15_RVA_JE) - (long)(cave + i + 4)); i += 4;
    memcpy(cave + i, T15_MOVQ, 8); i += 8;
    cave[i++] = 0xE9;
    *(DWORD*)(cave + i) = (DWORD)((long)(g_base + T15_RVA_CONT) - (long)(cave + i + 4)); i += 4;

    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    site[0] = 0xE9;
    *(DWORD*)(site + 1) = (DWORD)((long)cave - (long)(site + 5));
    DWORD junk;
    VirtualProtect(site, 5, old, &junk);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    FlushInstructionCache(GetCurrentProcess(), cave, 64);
    return 1;
}

/* ====================================================================== */
/* TH17 v1.00a                                                             */
/* ---------------------------------------------------------------------- */
/* 运动函数 0x446ff0 (thiscall, EDI=玩家), 关键链路:                        */
/*   输入字 0x4b3448 方向位 0xf0 -> 方向号 [edi+0x19034]                    */
/*   方向表 0x4a20a0: 9 组 int(dx,dy), 1上2下3左4右, 5..8 对角              */
/*   速度槽(每tick定点位移,int) [edi+0x18e90 通常正/94 低速正/              */
/*                              98 通常斜/9c 低速斜]                        */
/*   低速标志 [edi+0x18dd0], 位移倍率 [edi+0x18ec8] (1.0/0.5)               */
/*   原公式 (acc=[edi+0x18ecc/d0] 实际恒 0; G=[0x4a3e84]=-128):            */
/*     dx = trunc(float(dirx*spd - trunc(accx*G)) * mul)                    */
/*     dy = trunc(float(diry*spd - trunc(accy*G)) * mul)                    */
/*   dx/dy 经贴墙事件后 -> vx浮点[edi+0x18ea0]/vy[0x18ea4](×1.0)            */
/*   -> 截断加位置 [edi+0x61c]/[edi+0x620] -> 边界硬钳 -> 渲染 ×1/128       */
/* 站点 44720f 是原公式起点 movss xmm0,[edi+0x18ecc](8B),                  */
/* hook 后洞内自行算出 dx/dy, 跳到 447273（贴墙判定入口）:                  */
/*   入口现场 ecx=游戏自选spd(int), [esp+0xc]/[esp+0x10] 原放 dx/dy        */
/* ====================================================================== */

#define T17_RVA_HOOK    0x04720fu
#define T17_RVA_CONT    0x047273u
#define T17_RVA_DIRTAB  0x0a20a0u
#define T17_RVA_G       0x0a3e84u

#define T17_OFF_DIR     0x19034
#define T17_OFF_FOCUS   0x18dd0
#define T17_OFF_SPD_N   0x18e90  /* 通常速·主向 */
#define T17_OFF_SPD_F   0x18e94  /* 低速·主向 */
#define T17_OFF_MUL     0x18ec8
#define T17_OFF_ACCX    0x18ecc
#define T17_OFF_ACCY    0x18ed0

static const BYTE T17_SIG[8] = { 0xf3,0x0f,0x10,0x87,0xcc,0x8e,0x01,0x00 };
/* 辅助锚点: 方向表两次查表（防同形误匹配） */
static const BYTE T17_ANCHOR1[7] = { 0x8b,0x34,0xc5,0xa0,0x20,0x4a,0x00 };
static const BYTE T17_ANCHOR2[7] = { 0x8b,0x04,0xc5,0xa4,0x20,0x4a,0x00 };

/* 原公式逐项复刻（float32 + cvttss2si 语义: 向零截断 = C int 强转） */
static int t17_passive_axis(int comp, int spd, float acc, float g, float mul) {
    float t = acc * g;
    int   n = (int)t;
    float v = (float)(comp * spd - n);
    v *= mul;
    return (int)v;
}

void __cdecl vector_apply_th17(void *p, int spd_sel, int *dx_out, int *dy_out) {
    vm_tick_pos(KIND_TH17, p);
    heartbeat();
    if (g_kind != KIND_TH17 || !p || !dx_out || !dy_out) return;
    *dx_out = 0; *dy_out = 0;

    BYTE *b = (BYTE*)p;
    int   dir = *(int*)(b + T17_OFF_DIR);
    int   focus = (*(int*)(b + T17_OFF_FOCUS) != 0);
    float mul = *(float*)(b + T17_OFF_MUL);
    float g   = *(float*)(g_base + T17_RVA_G);
    float ax  = *(float*)(b + T17_OFF_ACCX);
    float ay  = *(float*)(b + T17_OFF_ACCY);
    int   dxc = *(int*)(g_base + T17_RVA_DIRTAB + dir * 8);
    int   dyc = *(int*)(g_base + T17_RVA_DIRTAB + dir * 8 + 4);

    double x, y; unsigned long pov;
    int gs = pad_get_stick(&x, &y, &pov);
    if (gs && pov == POV_NONE) {
        double ux, uy;
        if (vector_unit(x, y, VEC_DEAD, &ux, &uy)) {
            /* 匀速: 恒取主向速度槽（focus 自动）, 斜向扇区不再换慢速槽 */
            int spd = *(int*)(b + (focus ? T17_OFF_SPD_F : T17_OFF_SPD_N));
            float s = (float)spd;
            s *= mul;
            float vx = s * (float)ux;
            float vy = s * (float)uy;
            *dx_out = (int)vx;
            *dy_out = (int)vy;
            return;
        }
    }

    /* 摇杆未激活 / POV / 仅键盘 / 原生 XInput 方向键: 完全按游戏原公式 */
    *dx_out = t17_passive_axis(dxc, spd_sel, ax, g, mul);
    *dy_out = t17_passive_axis(dyc, spd_sel, ay, g, mul);
}

static int install_th17(void) {
    BYTE *site = g_base + T17_RVA_HOOK;
    if (memcmp(site, T17_SIG, sizeof T17_SIG) != 0) return 0;
    if (memcmp(g_base + 0x0470efu, T17_ANCHOR1, sizeof T17_ANCHOR1) != 0) return 0;
    if (memcmp(g_base + 0x0470f6u, T17_ANCHOR2, sizeof T17_ANCHOR2) != 0) return 0;

    BYTE *cave = (BYTE*)VirtualAlloc(NULL, 128,
                        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave) { pad_log("VEC th17: VirtualAlloc failed (%lu)", GetLastError()); return 0; }

    BYTE *pdx = cave + 96;
    BYTE *pdy = cave + 100;
    int i = 0;
    cave[i++] = 0x9C;                                   /* pushfd            */
    cave[i++] = 0x60;                                   /* pushad            */
    cave[i++] = 0x89; cave[i++] = 0xE6;                 /* mov esi,esp       */
    cave[i++] = 0x83; cave[i++] = 0xE4; cave[i++] = 0xF0; /* and esp,-16    */
    cave[i++] = 0xC7; cave[i++] = 0x05; *(DWORD*)(cave + i) = (DWORD)pdx; i += 4;
    cave[i++] = 0x00; cave[i++] = 0x00; cave[i++] = 0x00; cave[i++] = 0x00;
    cave[i++] = 0xC7; cave[i++] = 0x05; *(DWORD*)(cave + i) = (DWORD)pdy; i += 4;
    cave[i++] = 0x00; cave[i++] = 0x00; cave[i++] = 0x00; cave[i++] = 0x00;
    cave[i++] = 0x68; *(DWORD*)(cave + i) = (DWORD)pdy; i += 4;  /* arg3 &dy     */
    cave[i++] = 0x68; *(DWORD*)(cave + i) = (DWORD)pdx; i += 4;  /* arg2 &dx     */
    cave[i++] = 0x51;                                   /* push ecx (arg1 spd) */
    cave[i++] = 0x57;                                   /* push edi (arg0 p) */
    cave[i++] = 0xB8; *(DWORD*)(cave + i) = (DWORD)&vector_apply_th17; i += 4;
    cave[i++] = 0xFF; cave[i++] = 0xD0;                 /* call eax          */
    cave[i++] = 0x89; cave[i++] = 0xF4;                 /* mov esp,esi       */
    cave[i++] = 0xA1; *(DWORD*)(cave + i) = (DWORD)pdx; i += 4;  /* mov eax,[&dx] */
    cave[i++] = 0x89; cave[i++] = 0x44; cave[i++] = 0x24; cave[i++] = 0x18; /* mov [esp+0x18],eax (ECX槽) */
    cave[i++] = 0xA1; *(DWORD*)(cave + i) = (DWORD)pdy; i += 4;  /* mov eax,[&dy] */
    cave[i++] = 0x89; cave[i++] = 0x44; cave[i++] = 0x24; cave[i++] = 0x1C; /* mov [esp+0x1c],eax (EAX槽) */
    cave[i++] = 0x61;                                   /* popad -> eax=dy ecx=dx */
    cave[i++] = 0x9D;                                   /* popfd             */
    cave[i++] = 0x89; cave[i++] = 0x4C; cave[i++] = 0x24; cave[i++] = 0x0C; /* mov [esp+0xc],ecx */
    cave[i++] = 0x89; cave[i++] = 0x44; cave[i++] = 0x24; cave[i++] = 0x10; /* mov [esp+0x10],eax */
    cave[i++] = 0xE9;
    *(DWORD*)(cave + i) = (DWORD)((long)(g_base + T17_RVA_CONT) - (long)(cave + i + 4)); i += 4;

    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    site[0] = 0xE9;
    *(DWORD*)(site + 1) = (DWORD)((long)cave - (long)(site + 5));
    DWORD junk;
    VirtualProtect(site, 5, old, &junk);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    FlushInstructionCache(GetCurrentProcess(), cave, 128);
    pad_log("VEC th17 cave size=%d (scratch @%p/%p)", i, (void*)pdx, (void*)pdy);
    return 1;
}

/* ====================================================================== */
/* 安装入口                                                                */
/* ====================================================================== */

int vector_install(void) {
    if (g_kind != KIND_NONE) return 1;
    g_base = (BYTE*)GetModuleHandleA(NULL);
    if (!g_base) { pad_log("VEC install: no module base"); return 0; }

    size_t n = sizeof(g_profiles) / sizeof(g_profiles[0]);
    for (size_t i = 0; i < n; i++) {
        if (g_profiles[i].install()) {
            g_profile_idx = (int)i;
            g_kind = g_profiles[i].kind;
            pad_log("VEC install OK: %s hook -> cave", g_profiles[i].name);
            return 1;
        }
    }

    pad_log("VEC install: 无匹配游戏签名（TH15/TH17）, 运动层不安装");
    return 0;
}
