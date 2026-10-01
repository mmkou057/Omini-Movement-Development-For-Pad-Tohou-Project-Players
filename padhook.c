/* padhook.c - dinput8.dll 代理 (DLL proxy)  【帧率自适应版 / highrefreshrate】
 *
 * 本版与 th15_injector\padhook.c 的区别（仅输入节奏层，语义不变）:
 *   - PWM 占空比按"游戏实际消费输入的代次"步进（每 g_joy_seq +1 走一步），
 *     消费率 60Hz/120Hz/400Hz 皆自动跟随，占空比数学与消费率无关；
 *   - 用 QPC 实测消费节奏：停顿(暂停/掉帧)超过 4 倍常规间隔时重置 PWM 相位，
 *     避免恢复时打出一段陈旧占空；并把实测输入率写日志（诊断高刷链路）；
 *   - 方向扇区切换清零相位（沿用原版）+ 停顿清零（新增），无任何 60Hz 写死假设。
 *
 * 部署: 编译出的 dinput8.dll 放到 th15 游戏目录. Windows 优先加载本代理,
 * 游戏调用 DirectInput8Create 即进入我们的 hook.
 *
 * mode=3 策略 (化繁为简: 搭游戏自己手柄设备的便车, 不自行创建设备):
 *   游戏枚举并创建手柄设备后, 我们 hook 该设备的两个方法:
 *     - SetProperty: 记录游戏设置的轴范围 (归一化依据).
 *     - GetDeviceState: 调原版读到真实手柄数据后:
 *         1) 把真实摇杆/POV/按钮复制到全局状态 (键盘 hook 的唯一数据源);
 *         2) 把交还给游戏的缓冲"中和"成静止态 (轴写回自然归中点,
 *            POV=-1, 按钮清零) -> 游戏自身的 4/8 向手柄路径永远看到静止,
 *            不会与我们的 PWM 注入冲突.
 *   键盘 GetDeviceState: 调原版取真实键盘态, 再按全局手柄状态做 8 向凸包
 *   Bresenham PWM, 把帧均意义上的任意角度方向注入键盘; 物理键盘始终可用.
 *
 * 为什么不自行 CreateDevice: GUID_SysJoystick 是"控制面板首选手柄"伪 GUID,
 * 未配置时 CreateDevice 返回 REGDB_E_CLASSNOTAVAILABLE (0x80040154); 真实
 * 实例 GUID 只有 EnumDevices 能拿到. 而游戏已经替我们枚举+创建+获取好了.
 *
 * DIJOYSTATE 与 DIJOYSTATE2 中我们关心的偏移完全一致:
 *   lX@0 lY@4 rgdwPOV[0]@32 rgbButtons@48.
 *
 * 模式 (padhook.ini [padhook] mode):
 *   0 = 完全透明 (默认, 等同未安装, 已验证安全)
 *   1 = 仅压制 winmm 手柄 (诊断)
 *   2 = 仅 hook 键盘 GetDeviceState 且原样透传 (诊断)
 *   3 = 完整全向移动 (搭游戏手柄便车 -> PWM -> 注入键盘)
 *
 * 注意: 真 dinput8.dll 用绝对路径 C:\Windows\System32\dinput8.dll 加载, 避免递归.
 */
#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <mmsystem.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "pwm.h"

/* ---------- 自定义 GUID (不依赖 dxguid) ---------- */
static const GUID MY_GUID_SysKeyboard =
    { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 } };
static const GUID MY_GUID_SysMouse =
    { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 } };
static int eq_guid(REFGUID a, const GUID* b) { return memcmp(a, b, sizeof(GUID)) == 0; }

/* 游戏手柄状态缓冲内的标准偏移 (DIJOYSTATE / DIJOYSTATE2 共有) */
#define OFF_X     0
#define OFF_Y     4
#define OFF_POV0  32
#define OFF_BTN   48

/* ---------- 日志 ---------- */
static CRITICAL_SECTION g_logcs;
static int g_logcs_init = 0;
static void pad_log(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!g_logcs_init) { InitializeCriticalSection(&g_logcs); g_logcs_init = 1; }
    EnterCriticalSection(&g_logcs);
    FILE* f = fopen("padhook.log", "a");
    if (f) { fprintf(f, "%s\n", buf); fclose(f); }
    LeaveCriticalSection(&g_logcs);
    OutputDebugStringA("[padhook] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

/* ---------- 诊断限流 (仅 -DPADHOOK_DIAG): 每秒最多 ~8 条 ---------- */
#ifdef PADHOOK_DIAG
static int diag_due(void) {
    static LARGE_INTEGER dlast; static int dinit = 0;
    LARGE_INTEGER n, f;
    QueryPerformanceCounter(&n);
    QueryPerformanceFrequency(&f);
    if (!dinit || (n.QuadPart - dlast.QuadPart) >= f.QuadPart / 8) { dinit = 1; dlast = n; return 1; }
    return 0;
}
#endif

/* ---------- 函数指针类型 ---------- */
typedef HRESULT (WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);
typedef HRESULT (WINAPI *CreateDevice_t)(void*, REFGUID, void**, IUnknown*);
typedef HRESULT (WINAPI *GetDeviceState_t)(void*, DWORD, LPVOID);
typedef HRESULT (WINAPI *SetProperty_t)(void*, REFGUID, const DIPROPHEADER*);
typedef MMRESULT (WINAPI *joyGetPos_t)(UINT, LPJOYINFO);
typedef MMRESULT (WINAPI *joyGetPosEx_t)(DWORD, LPJOYINFOEX);
typedef MMRESULT (WINAPI *joyGetDevCaps_t)(UINT, void*, UINT);

/* ---------- 原始函数 / 接口指针 ---------- */
static HMODULE              g_real_dinput8 = NULL;
static DirectInput8Create_t g_real_DIC8 = NULL;
static CreateDevice_t       g_orig_CreateDevice = NULL;

/* ---------- 配置 ---------- */
static int g_mode = 0;
static int g_cfg_loaded = 0;
/* 手柄按钮编号 -> 游戏动作 (load_config 从 ini 读取, 默认标准 DInput 编号) */
static int g_btn_shoot = 0, g_btn_bomb = 1, g_btn_slow = 4, g_btn_pause = 7;

/* 轴范围来源优先级: ini 手动覆盖 > GetProperty(DIPROP_RANGE) > 自适应观察 > 默认1000.
 * DInput 设备默认轴范围是 -1000..1000 (中心0), 并非 0..65535! */
static LONG g_cfg_amin = 0, g_cfg_amax = 0; static int g_cfg_range = 0;
static LONG g_q_amin = 0,   g_q_amax = 0;   static int g_q_range = 0;
static double g_span_obs = 0.0;   /* 自适应: 观察到的偏离中心最大值 */
/* 手柄真实数据帧代次 (joy_process 每真实帧 +1, kb_process 据此与游戏帧对齐) */
static volatile LONG g_joy_seq = 0;

static void load_config(void) {
    if (g_cfg_loaded) return;
    g_cfg_loaded = 1;
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) slash[1] = 0; else path[0] = 0;
    strncat(path, "padhook.ini", MAX_PATH - 1);
    g_mode = GetPrivateProfileIntA("padhook", "mode", 0, path);
    if (g_mode < 0 || g_mode > 3) g_mode = 0;
    /* 手柄按钮编号映射 (第三方设备差异由玩家校准; 数据驱动, 不写死) */
    g_btn_shoot = GetPrivateProfileIntA("padhook", "btn_shoot", 0, path);
    g_btn_bomb  = GetPrivateProfileIntA("padhook", "btn_bomb",  1, path);
    g_btn_slow  = GetPrivateProfileIntA("padhook", "btn_slow",  4, path);
    g_btn_pause = GetPrivateProfileIntA("padhook", "btn_pause", 7, path);
    /* 可选: 手动指定轴原始范围 (第三方设备/驱动兜底; 不设置则自动探测) */
    int amin = GetPrivateProfileIntA("padhook", "axis_min", -100000, path);
    int amax = GetPrivateProfileIntA("padhook", "axis_max", -100000, path);
    if (amax > amin) { g_cfg_amin = (LONG)amin; g_cfg_amax = (LONG)amax; g_cfg_range = 1; }
    pad_log("config: %s mode=%d btns=%d/%d/%d/%d axis=%s", path, g_mode,
        g_btn_shoot, g_btn_bomb, g_btn_slow, g_btn_pause,
        g_cfg_range ? "ini" : "auto");
}

/* ---------- IAT hook ---------- */
static void* iat_hook(HMODULE target, const char* dll, const char* func, void* newf) {
    BYTE* base = (BYTE*)target;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return NULL;
    PIMAGE_IMPORT_DESCRIPTOR imp = (PIMAGE_IMPORT_DESCRIPTOR)(base + rva);
    for (; imp->Name; imp++) {
        const char* name = (const char*)(base + imp->Name);
        if (_stricmp(name, dll) != 0) continue;
        DWORD olt_rva = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
        PIMAGE_THUNK_DATA olt = (PIMAGE_THUNK_DATA)(base + olt_rva);
        PIMAGE_THUNK_DATA at  = (PIMAGE_THUNK_DATA)(base + imp->FirstThunk);
        for (; olt->u1.AddressOfData; olt++, at++) {
            if (IMAGE_SNAP_BY_ORDINAL(olt->u1.Ordinal)) continue;
            PIMAGE_IMPORT_BY_NAME impn = (PIMAGE_IMPORT_BY_NAME)(base + olt->u1.AddressOfData);
            if (_stricmp((char*)impn->Name, func) != 0) continue;
            void* orig = (void*)at->u1.Function;
            DWORD old;
            if (!VirtualProtect(&at->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return NULL;
            at->u1.Function = (DWORD_PTR)newf;
            VirtualProtect(&at->u1.Function, sizeof(void*), old, &old);
            return orig;
        }
    }
    return NULL;
}
static int iat_is_hooked(HMODULE target, const char* dll, const char* func, void* newf) {
    BYTE* base = (BYTE*)target;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return 0;
    PIMAGE_IMPORT_DESCRIPTOR imp = (PIMAGE_IMPORT_DESCRIPTOR)(base + rva);
    for (; imp->Name; imp++) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        DWORD olt_rva = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
        PIMAGE_THUNK_DATA olt = (PIMAGE_THUNK_DATA)(base + olt_rva);
        PIMAGE_THUNK_DATA at  = (PIMAGE_THUNK_DATA)(base + imp->FirstThunk);
        for (; olt->u1.AddressOfData; olt++, at++) {
            if (IMAGE_SNAP_BY_ORDINAL(olt->u1.Ordinal)) continue;
            PIMAGE_IMPORT_BY_NAME impn = (PIMAGE_IMPORT_BY_NAME)(base + olt->u1.AddressOfData);
            if (_stricmp((char*)impn->Name, func) != 0) continue;
            return (void*)at->u1.Function == newf;
        }
    }
    return 0;
}

/* ---------- 加载真 dinput8.dll (绝对路径, 防递归) ---------- */
static void load_real_dinput8(void) {
    if (g_real_dinput8) return;
    char sys[MAX_PATH]; GetSystemDirectoryA(sys, MAX_PATH);
    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "%s\\dinput8.dll", sys);
    g_real_dinput8 = LoadLibraryA(path);
    if (g_real_dinput8) {
        g_real_DIC8 = (DirectInput8Create_t)GetProcAddress(g_real_dinput8, "DirectInput8Create");
        pad_log("real dinput8=%p DIC8=%p", g_real_dinput8, g_real_DIC8);
    } else {
        pad_log("FATAL: 无法加载 %s", path);
    }
}

/* ========================================================================
 *  mode=3: 手柄全局状态 (手柄 GDS hook 写入, 键盘 GDS hook 读取, 唯一数据源)
 * ======================================================================== */
static double g_pad_x = 0.0;    /* 归一化 -1..1, 右正 */
static double g_pad_y = 0.0;    /* 归一化 -1..1, 上正 */
static DWORD  g_pad_pov = 0xFFFFFFFF;
static BYTE   g_pad_btn[32];    /* 32 个物理按钮, 每字节 bit7 */
static int    g_pad_alive = 0;

/* 自然归中点的经验估计 (EMA, 静止样本更新; 兼容 ±1000 / 0..65535 等任何范围) */
static double g_neu_x = 0.0, g_neu_y = 0.0;
static int g_neu_init = 0;

/* ========================================================================
 * 统一 GetDeviceState hook (mode=3)
 *
 * 已证实的设备拓扑 (2026-09-30 日志):
 *  1) 键盘/手柄/鼠标是同一个 COM 类的实例, 共享同一个 vtable;
 *  2) 第三方手柄驱动(或映射)软件在手柄创建时, 把 vtable[9] 换成自己的函数,
 *     并链式回调它替换前的旧槽内容;
 *  3) 因此绝不能按设备分别 patch 同一个槽 (旧版两个 hook 对每个设备各执行
 *     一次: joy hook 把键盘缓冲误当 DIJOYSTATE 读取后 ZeroMemory 清零,
 *     物理键盘 Z 就是这样被抹掉的).
 *
 * 对策:
 *  - 只装一个 hook, 按设备对象 self 指针分流 (g_kb_self / g_joy_self);
 *  - 每次 CreateDevice 后, 若槽 9 又被包装层覆盖, 重新抢为最外层;
 *  - 包装层链式回调旧槽会重入本 hook -> 重入时直接调真实 dinput8 GDS,
 *    保证数据只取一次, 且我们的中和/注入是游戏看到的最终结果.
 *
 * SetProperty(vtable[6]) 同样不可 hook (会崩溃), 轴范围用默认 0..65535,
 * 自然归中点 EMA 兼容任何实际范围.
 * ======================================================================== */
static void* g_kb_self = NULL;      /* 游戏的键盘设备对象 */
static void* g_joy_self = NULL;     /* 游戏的手柄设备对象 */
static GetDeviceState_t g_real_GDS  = NULL;  /* 真实 dinput8 GDS, 全程不变 */
static GetDeviceState_t g_chain_GDS = NULL;  /* 槽中上一个函数 (包装层或真GDS) */

/* 重入标志 (用 kernel32 TLS; 不用 __thread, 避免引入 libgcc 动态依赖) */
static DWORD g_tls_idx = 0;
static int reentry_get(void) {
    return g_tls_idx ? (int)(DWORD_PTR)TlsGetValue(g_tls_idx) : 0;
}
static void reentry_set(int v) {
    if (g_tls_idx) TlsSetValue(g_tls_idx, (LPVOID)(DWORD_PTR)v);
}

static void patch_vt(void** vt, int idx, void* newf);
static HRESULT WINAPI unified_GetDeviceState_hook(void* self, DWORD cb, LPVOID data);

/* 把统一 hook 安装/复位为槽 9 的最外层 */
static void install_outer(void** vt) {
    void* cur = vt[9];
    if (!g_real_GDS) g_real_GDS = (GetDeviceState_t)cur;   /* 首次见到的就是真 GDS */
    if (cur == (void*)0) return;
    if (cur != (void*)unified_GetDeviceState_hook) {
        /* 包装层: 它内部会链回我们; 记录模块名便于排查/校准 */
        if (cur != (void*)g_real_GDS) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(cur, &mbi, sizeof mbi) && mbi.AllocationBase) {
                char mn[MAX_PATH];
                if (GetModuleFileNameA((HMODULE)mbi.AllocationBase, mn, MAX_PATH))
                    pad_log("wrapper layer module: %s (slot9=%p)", mn, cur);
            }
        }
        g_chain_GDS = (GetDeviceState_t)cur;
        patch_vt(vt, 9, (void*)unified_GetDeviceState_hook);
    }
}

static void joy_process(DWORD cb, LPVOID data);
static void kb_process(DWORD cb, LPVOID data);

static HRESULT WINAPI unified_GetDeviceState_hook(void* self, DWORD cb, LPVOID data) {
    /* 重入 (包装层链式回调旧槽): 直接取真实数据, 不做任何处理 */
    if (reentry_get()) {
        return g_real_GDS ? g_real_GDS(self, cb, data) : DIERR_NOTACQUIRED;
    }
    GetDeviceState_t fn = g_chain_GDS ? g_chain_GDS : g_real_GDS;
    if (!fn) return DIERR_NOTACQUIRED;
    reentry_set(1);
    HRESULT hr = fn(self, cb, data);
    reentry_set(0);
    if (FAILED(hr)) return hr;

#ifdef PADHOOK_DIAG
    {
        static int s_topn = 0;
        if (s_topn < 16) {
            char cls = self == g_joy_self ? 'J' : self == g_kb_self ? 'K' : '?';
            pad_log("GDS[%d] self=%p cb=%u cls=%c hr=0x%08x",
                s_topn, self, (unsigned)cb, cls, (unsigned)hr);
        }
        s_topn++;
    }
#endif

    if (self == g_joy_self)       joy_process(cb, data);
    else if (self == g_kb_self)   kb_process(cb, data);
    return hr;
}

/* ---------- 轴范围查询: GetProperty(DIPROP_RANGE) (只读, 安全) ---------- */
static const GUID MY_DIPROP_RANGE =
    { 0xA36D02ED, 0xC9F3, 0x11CF, { 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00 } };
typedef HRESULT (WINAPI *GetProperty_t)(void*, REFGUID, LPDIPROPHEADER);

static void query_range_once(void) {
    static int done = 0;
    if (done || !g_joy_self || g_cfg_range) return;
    done = 1;
    void** vt = *(void***)g_joy_self;
    GetProperty_t f = (GetProperty_t)vt[5];
    pad_log("--- GetProperty RANGE vt[5]=%p ---", f);
    if (!f) return;
    DIPROPRANGE rg;
    ZeroMemory(&rg, sizeof rg);
    rg.diph.dwSize = sizeof(DIPROPRANGE);
    rg.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    rg.diph.dwHow = DIPH_DEVICE;
    rg.diph.dwObj = 0;
    HRESULT hr = f(g_joy_self, &MY_DIPROP_RANGE, &rg.diph);
    pad_log("GetProperty RANGE hr=0x%08lx min=%ld max=%ld",
            (unsigned long)hr, (long)rg.lMin, (long)rg.lMax);
    if (SUCCEEDED(hr) && rg.lMax > rg.lMin) {
        g_q_amin = rg.lMin; g_q_amax = rg.lMax; g_q_range = 1;
    }
}

/* ---------- 第三方手柄轴布局探测 (诊断) ---------- */
typedef BOOL (CALLBACK *enum_obj_cb_t)(LPCDIDEVICEOBJECTINSTANCEW, LPVOID);
typedef HRESULT (WINAPI *EnumObjects_t)(void*, enum_obj_cb_t, LPVOID, DWORD);

static BOOL CALLBACK obj_enum_cb(LPCDIDEVICEOBJECTINSTANCEW oi, LPVOID ref) {
    (void)ref;
    const unsigned char* g = (const unsigned char*)&oi->guidType;
    char gh[33]; int i;
    for (i = 0; i < 16; i++) sprintf(gh + i*2, "%02x", g[i]);
    pad_log("OBJ ofs=%lu type=0x%08lx flags=0x%08lx guid=%s",
            (unsigned long)oi->dwOfs, (unsigned long)oi->dwType,
            (unsigned long)oi->dwFlags, gh);
    return TRUE;
}

/* 第一次手柄 GDS 时枚举全部对象 (轴/按钮/POV), 让设备自报数据偏移 */
static void enum_objects_once(void) {
    static int done = 0;
    if (done || !g_joy_self) return;
    done = 1;
    void** vt = *(void***)g_joy_self;
    EnumObjects_t f = (EnumObjects_t)vt[4];
    pad_log("--- EnumObjects vt[4]=%p ---", f);
    if (f) f(g_joy_self, obj_enum_cb, NULL, 0 /*DIDFT_ALL*/);
    pad_log("--- EnumObjects done ---");
}

#ifdef PADHOOK_DIAG
/* dump 原始缓冲前 160 字节 (轴区/POV/前112按钮), 变化时 ~80ms 一条 */
static void dump_buffer(LPVOID data) {
    static BYTE prev[160]; static int have = 0; static DWORD last_tick = 0;
    DWORD tick = GetTickCount();
    if (have && memcmp(prev, data, 160) == 0) return;
    if (have && (tick - last_tick) < 80) return;
    char hex[160*2 + 1]; int j;
    for (j = 0; j < 160; j++) sprintf(hex + j*2, "%02x", ((BYTE*)data)[j]);
    pad_log("BUF %s", hex);
    memcpy(prev, data, 160);
    have = 1; last_tick = tick;
}
#endif

/* ---------- 手柄处理: 真实数据存全局, 游戏缓冲中和 ---------- */
static void joy_process(DWORD cb, LPVOID data) {
    if (cb < OFF_BTN + 32 || !data) { g_pad_alive = 0; return; }
    query_range_once();
    enum_objects_once();
    BYTE* b = (BYTE*)data;
    LONG rx = *(LONG*)(b + OFF_X);
    LONG ry = *(LONG*)(b + OFF_Y);
    DWORD pov = *(DWORD*)(b + OFF_POV0);

    /* 归一化半量程: ini 覆盖 > GetProperty 查询 > 自适应观察 > DInput 默认1000 */
    double span;
    if (g_cfg_range)          span = (double)(g_cfg_amax - g_cfg_amin) * 0.5;
    else if (g_q_range)       span = (double)(g_q_amax  - g_q_amin)  * 0.5;
    else if (g_span_obs > 1.0) span = g_span_obs;
    else                      span = 1000.0;

    /* 是否有按钮按下 (看前 32 个按钮位) */
    int anybtn = 0; int i;
    for (i = 0; i < 32; i++) if (b[OFF_BTN + i] & 0x80) { anybtn = 1; break; }

    /* 静止样本: 更新自然归中点 EMA (阈值约 16% 半量程) */
    double th = span * 0.16;
    if (!g_neu_init) { g_neu_x = (double)rx; g_neu_y = (double)ry; g_neu_init = 1; }
    int quiet = !anybtn && pov == 0xFFFFFFFF
        && ((double)rx - g_neu_x) * ((double)rx - g_neu_x) < th*th
        && ((double)ry - g_neu_y) * ((double)ry - g_neu_y) < th*th;
    if (quiet) {
        g_neu_x += ((double)rx - g_neu_x) * 0.10;
        g_neu_y += ((double)ry - g_neu_y) * 0.10;
    }

    /* 自适应观察 (无 ini/query 时兜底): 满推立即学到, 之后极慢回缩 */
    if (!g_cfg_range && !g_q_range) {
        double devx = (double)rx - g_neu_x; if (devx < 0.0) devx = -devx;
        double devy = (double)ry - g_neu_y; if (devy < 0.0) devy = -devy;
        double dev = devx > devy ? devx : devy;
        if (dev > g_span_obs) g_span_obs = dev;
        else if (g_span_obs > 1.0) g_span_obs *= 0.99995;
        if (g_span_obs > 1.0) span = g_span_obs;
    }

    /* 归一化 (y 轴 DInput 向下为正, 取反成"上正") */
    double nx = ((double)rx - g_neu_x) / span;
    double ny = -((double)ry - g_neu_y) / span;
    if (nx >  1.0) nx =  1.0; if (nx < -1.0) nx = -1.0;
    if (ny >  1.0) ny =  1.0; if (ny < -1.0) ny = -1.0;

    g_pad_x = nx; g_pad_y = ny;
    g_pad_pov = pov;
    memcpy(g_pad_btn, b + OFF_BTN, sizeof(g_pad_btn));
    g_pad_alive = 1;
    InterlockedIncrement(&g_joy_seq);

#ifdef PADHOOK_DIAG
    dump_buffer(data);   /* 中和前 dump 原始数据 */
#endif

    /* 中和交还给游戏的缓冲: 全 0 + 轴写回归中点 + POV 全部居中 + 按钮已清零 */
    ZeroMemory(data, cb);
    *(LONG*)(b + OFF_X) = (LONG)g_neu_x;
    *(LONG*)(b + OFF_Y) = (LONG)g_neu_y;
    if (cb >= OFF_POV0 + 4*4) {
        /* 4 个 POV (DIJOYSTATE / DIJOYSTATE2 都有 4 个槽) */
        *(DWORD*)(b + OFF_POV0 + 0)  = 0xFFFFFFFF;
        *(DWORD*)(b + OFF_POV0 + 4)  = 0xFFFFFFFF;
        *(DWORD*)(b + OFF_POV0 + 8)  = 0xFFFFFFFF;
        *(DWORD*)(b + OFF_POV0 + 12) = 0xFFFFFFFF;
    }

#ifdef PADHOOK_DIAG
    /* 边沿日志: 按钮或 POV 状态一变化就无条件记录 (拿真实按钮编号, 不刷屏) */
    {
        static BYTE s_prev[32]; static DWORD s_prev_pov = 0xFFFFFFFF; static int s_have = 0;
        if (!s_have || memcmp(s_prev, g_pad_btn, sizeof(g_pad_btn)) || s_prev_pov != pov) {
            char hex[sizeof(g_pad_btn)*2 + 1]; int j;
            for (j = 0; j < (int)sizeof(g_pad_btn); j++) sprintf(hex + j*2, "%02x", g_pad_btn[j]);
            pad_log("BTN raw=(%ld,%ld) pov=%08lx %s", rx, ry, pov, hex);
            memcpy(s_prev, g_pad_btn, sizeof(g_pad_btn));
            s_prev_pov = pov; s_have = 1;
        }
    }
#endif
}

/* ---------- 方向注入小工具 ---------- */
static void clear_udlr(BYTE* k) {
    k[DIK_UP] &= 0x7F; k[DIK_DOWN] &= 0x7F; k[DIK_LEFT] &= 0x7F; k[DIK_RIGHT] &= 0x7F;
}
static void inject_dir(BYTE* k, int d) {
    switch (d) {
        case 1: k[DIK_RIGHT] |= 0x80; break;                                  /* E */
        case 2: k[DIK_RIGHT] |= 0x80; k[DIK_UP] |= 0x80; break;           /* NE */
        case 3: k[DIK_UP] |= 0x80; break;                                     /* N */
        case 4: k[DIK_LEFT] |= 0x80; k[DIK_UP] |= 0x80; break;            /* NW */
        case 5: k[DIK_LEFT] |= 0x80; break;                                   /* W */
        case 6: k[DIK_LEFT] |= 0x80; k[DIK_DOWN] |= 0x80; break;          /* SW */
        case 7: k[DIK_DOWN] |= 0x80; break;                                   /* S */
        case 8: k[DIK_RIGHT] |= 0x80; k[DIK_DOWN] |= 0x80; break;         /* SE */
        default: break;
    }
}
/* POV: 0=N,1=NE,...,7=NW (单位 4500) */
static void inject_pov(BYTE* k, DWORD pov) {
    static const BYTE sek[8][2] = {
        {DIK_UP,0}, {DIK_UP,DIK_RIGHT}, {DIK_RIGHT,0}, {DIK_RIGHT,DIK_DOWN},
        {DIK_DOWN,0}, {DIK_DOWN,DIK_LEFT}, {DIK_LEFT,0}, {DIK_LEFT,DIK_UP}
    };
    int s = (int)(pov / 4500);
    if (s < 0 || s > 7) return;
    k[sek[s][0]] |= 0x80;
    if (sek[s][1]) k[sek[s][1]] |= 0x80;
}

/* ---------- 键盘处理: 全局手柄态 -> PWM -> 注入 ---------- */
static void kb_process(DWORD cb, LPVOID data) {
    if (cb < 256 || !data) return;
    BYTE* keys = (BYTE*)data;

    int stick_active = g_pad_alive
        && (g_pad_x*g_pad_x + g_pad_y*g_pad_y) > 0.24*0.24;
    int pov_active = g_pad_alive && (g_pad_pov != 0xFFFFFFFF);

    /* 帧对齐（自适应版）: 手柄数据每被游戏消费一次 +1 代次; 仅新代次推进一步 PWM.
       消费率本身是任意的（60Hz 原版 / 高刷子步进下未来的 120/400Hz），
       Bresenham 按代次推进 => 占空比与消费率无关，自动跟随。
       同代次内多余键盘轮询只重复当前方向, 占空比不被轮询节奏扭曲。
       新增 QPC 节奏测量：相邻代次间隔 > 4 倍 EMA 视为停顿（暂停/掉帧），
       清零 PWM 相位防止恢复时打出陈旧占空；每 ~5 秒记录一次实测消费率。 */
    static int last_seq = -1;
    static int cur_dir = 0;
    static pwm_acc_t acc = {0.0};
    static int s_la = 0, s_lb = 0;
    static LARGE_INTEGER s_qpf, s_prev_tick, s_rate_win;
    static unsigned s_seq_in_win = 0;
    static double s_ema_ms = 0.0;
    if (!s_qpf.QuadPart) QueryPerformanceFrequency(&s_qpf);
    if ((int)g_joy_seq != last_seq) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if (last_seq >= 0 && s_prev_tick.QuadPart) {
            double ms = (double)(now.QuadPart - s_prev_tick.QuadPart) * 1000.0
                      / (double)s_qpf.QuadPart;
            if (s_ema_ms <= 0.0) s_ema_ms = ms;
            else if (ms < s_ema_ms * 4.0) s_ema_ms += (ms - s_ema_ms) * 0.05;
            if (ms > s_ema_ms * 4.0) {          /* 停顿：重置相位与方向缓存 */
                acc.acc = 0.0; s_la = s_lb = 0;
                if (cur_dir) pad_log("input stall %.0fms (ema %.1fms): pwm phase reset", ms, s_ema_ms);
            }
            s_seq_in_win++;
            if (s_rate_win.QuadPart
                && (now.QuadPart - s_rate_win.QuadPart) >= s_qpf.QuadPart * 5) {
                double sec = (double)(now.QuadPart - s_rate_win.QuadPart) / (double)s_qpf.QuadPart;
                pad_log("input consume rate: %.1f/s (ema interval %.2fms)",
                        s_seq_in_win / sec, s_ema_ms);
                s_rate_win = now; s_seq_in_win = 0;
            } else if (!s_rate_win.QuadPart) { s_rate_win = now; s_seq_in_win = 0; }
        }
        s_prev_tick = now;
        last_seq = (int)g_joy_seq;
        if (stick_active) {
            pwm_dir_t pd;
            pwm_decompose(g_pad_x, g_pad_y, 0.24, &pd);
            if (pd.a == 0) { cur_dir = 0; acc.acc = 0.0; s_la = s_lb = 0; }
            else {
                /* 方向扇区切换时清 PWM 相位, 消除"相位惯性"滞涩 */
                if (pd.a != s_la || pd.b != s_lb) {
                    acc.acc = 0.0; s_la = pd.a; s_lb = pd.b;
                }
                int b = pwm_step(&acc, pd.alpha);
                cur_dir = b ? pd.b : pd.a;
            }
        } else {
            cur_dir = 0; acc.acc = 0.0; s_la = s_lb = 0;
        }
    }

    if (stick_active || pov_active) {
        /* 摇杆/十字键激活时接管方向; 回中后物理键盘方向键立即恢复 */
        clear_udlr(keys);
        if (pov_active) inject_pov(keys, g_pad_pov);
        else if (stick_active) inject_dir(keys, cur_dir);
    }

    /* 按钮 OR 进真实键盘态 (物理键盘始终可用); 按钮编号来自 ini 校准 */
    {
        int nbtn = (int)sizeof(g_pad_btn) * 8; int idx;
        idx = g_btn_shoot;
        if (idx >= 0 && idx < nbtn && (g_pad_btn[idx>>3] & (0x80>>(idx&7))))
            keys[DIK_Z] |= 0x80;
        idx = g_btn_bomb;
        if (idx >= 0 && idx < nbtn && (g_pad_btn[idx>>3] & (0x80>>(idx&7))))
            keys[DIK_X] |= 0x80;
        idx = g_btn_slow;
        if (idx >= 0 && idx < nbtn && (g_pad_btn[idx>>3] & (0x80>>(idx&7))))
            keys[DIK_LSHIFT] |= 0x80;
        idx = g_btn_pause;
        if (idx >= 0 && idx < nbtn && (g_pad_btn[idx>>3] & (0x80>>(idx&7))))
            keys[DIK_ESCAPE] |= 0x80;
    }

#ifdef PADHOOK_DIAG
    if (diag_due())
        pad_log("KB3 stick=%d pov=%d dir=%d -> %d%d%d%d Z=%d X=%d",
            stick_active, pov_active, cur_dir,
            !!(keys[DIK_UP]&0x80),!!(keys[DIK_DOWN]&0x80),
            !!(keys[DIK_LEFT]&0x80),!!(keys[DIK_RIGHT]&0x80),
            !!(keys[DIK_Z]&0x80),!!(keys[DIK_X]&0x80));
#endif
}

/* ---------- 键盘透传 hook (mode=2): 不改数据, 只记录 ---------- */
static GetDeviceState_t g_orig_kb_passthru = NULL;
static HRESULT WINAPI kb_passthru_hook(void* self, DWORD cb, LPVOID data) {
    HRESULT hr = g_orig_kb_passthru ? g_orig_kb_passthru(self, cb, data) : DIERR_NOTACQUIRED;
#ifdef PADHOOK_DIAG
    if (diag_due() && SUCCEEDED(hr) && cb >= 256 && data) {
        BYTE* k = (BYTE*)data;
        pad_log("KB-PASSTHRU tid=%u UDLR=%d%d%d%d ZX=%d%d", GetCurrentThreadId(),
            !!(k[DIK_UP]&0x80),!!(k[DIK_DOWN]&0x80),!!(k[DIK_LEFT]&0x80),!!(k[DIK_RIGHT]&0x80),
            !!(k[DIK_Z]&0x80),!!(k[DIK_X]&0x80));
    }
#endif
    return hr;
}

/* ---------- vtable 槽替换小工具 ---------- */
static void patch_vt(void** vt, int idx, void* newf) {
    DWORD old;
    VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old);
    vt[idx] = newf;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
}

/* ---------- CreateDevice hook ---------- */
static HRESULT WINAPI CreateDevice_hook(void* self, REFGUID rguid, void** ppdev, IUnknown* unk) {
    if (!g_orig_CreateDevice) return E_FAIL;
    HRESULT hr = g_orig_CreateDevice(self, rguid, ppdev, unk);
    if (FAILED(hr) || !ppdev || !*ppdev) return hr;
    void** vt = *(void***)*ppdev;
    pad_log("CD guid=%08lx pp=%p vt=%p vt9=%p",
        (unsigned long)((const GUID*)rguid)->Data1, *ppdev, vt, vt[9]);

    if (eq_guid(rguid, &MY_GUID_SysKeyboard)) {
        if (g_mode == 2) {
            if (!g_orig_kb_passthru) g_orig_kb_passthru = (GetDeviceState_t)vt[9];
            patch_vt(vt, 9, (void*)kb_passthru_hook);
            pad_log("CreateDevice keyboard PASSTHRU hooked (orig=%p)", g_orig_kb_passthru);
        } else /* mode=3 */ {
            g_kb_self = *ppdev;
            install_outer(vt);
            pad_log("CD keyboard self=%p realGDS=%p", *ppdev, g_real_GDS);
        }
    } else if (eq_guid(rguid, &MY_GUID_SysMouse)) {
        if (g_mode == 3) install_outer(vt);   /* 包装层可能在鼠标创建时也换槽 */
        if (g_mode == 2 || g_mode == 3) pad_log("CD mouse self=%p", *ppdev);
    } else /* 手柄 */ if (g_mode == 3) {
        g_joy_self = *ppdev;
        install_outer(vt);
        pad_log("CD joystick self=%p chain=%p", *ppdev, g_chain_GDS);
    } else if (g_mode == 2) {
        pad_log("CreateDevice other device (untouched, mode=2)");
    }
    return hr;
}

/* ---------- DirectInput8Create 代理入口 ---------- */
HRESULT WINAPI MyDirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID* ppv, IUnknown* unk) {
    load_config();
    if (!g_real_dinput8) load_real_dinput8();
    if (!g_real_DIC8) return E_FAIL;
    HRESULT hr = g_real_DIC8(hinst, ver, riid, ppv, unk);
    if (SUCCEEDED(hr) && ppv && *ppv && (g_mode == 2 || g_mode == 3)) {
        void** vt = *(void***)*ppv;
        if (!g_orig_CreateDevice) g_orig_CreateDevice = (CreateDevice_t)vt[3];
        patch_vt(vt, 3, (void*)CreateDevice_hook);
        pad_log("IDirectInput8 hooked (CreateDevice orig=%p, mode=%d)", g_orig_CreateDevice, g_mode);
    }
    return hr;
}

/* ---------- winmm 手柄 hook: 一律报"无设备", 封死游戏的 legacy 回退 ----------
 * 不能返回"成功+居中": 游戏 DInput 路径被中和后若回退 winmm, 居中数据可能
 * 被按错误范围解读为方向 (实测表现为菜单持续向下滚动).
 */
static MMRESULT WINAPI joyGetPos_hook(UINT id, LPJOYINFO pji) {
#ifdef PADHOOK_DIAG
    static int s_calls = 0;
    if (s_calls < 5) { s_calls++; pad_log("winmm joyGetPos id=%u -> NODRIVER", id); }
#endif
    (void)id; (void)pji;
    return MMSYSERR_NODRIVER;
}
static MMRESULT WINAPI joyGetPosEx_hook(DWORD id, LPJOYINFOEX pjiex) {
#ifdef PADHOOK_DIAG
    static int s_calls = 0;
    if (s_calls < 5) { s_calls++; pad_log("winmm joyGetPosEx id=%u -> NODRIVER", id); }
#endif
    (void)id; (void)pjiex;
    return MMSYSERR_NODRIVER;
}
static MMRESULT WINAPI joyGetDevCaps_hook(UINT id, void* caps, UINT cb) {
#ifdef PADHOOK_DIAG
    static int s_caps = 0;
    if (s_caps < 5) { s_caps++; pad_log("winmm joyGetDevCaps id=%u -> NODRIVER", id); }
#endif
    (void)id; (void)caps; (void)cb;
    return MMSYSERR_NODRIVER;
}

static HMODULE g_host = NULL;
static void try_hook_winmm(void) {
    if (!g_host) g_host = GetModuleHandleA(NULL);
    if (!iat_is_hooked(g_host, "winmm.dll", "joyGetPos", (void*)joyGetPos_hook))
        iat_hook(g_host, "winmm.dll", "joyGetPos", (void*)joyGetPos_hook);
    if (!iat_is_hooked(g_host, "winmm.dll", "joyGetPosEx", (void*)joyGetPosEx_hook))
        iat_hook(g_host, "winmm.dll", "joyGetPosEx", (void*)joyGetPosEx_hook);
    if (!iat_is_hooked(g_host, "winmm.dll", "joyGetDevCapsA", (void*)joyGetDevCaps_hook))
        iat_hook(g_host, "winmm.dll", "joyGetDevCapsA", (void*)joyGetDevCaps_hook);
    if (!iat_is_hooked(g_host, "winmm.dll", "joyGetDevCapsW", (void*)joyGetDevCaps_hook))
        iat_hook(g_host, "winmm.dll", "joyGetDevCapsW", (void*)joyGetDevCaps_hook);
}

static DWORD WINAPI worker_thread(LPVOID _) {
    (void)_;
    if (g_mode != 1 && g_mode != 3) return 0;
    for (int i = 0; i < 40; i++) { try_hook_winmm(); Sleep(100); }
    for (;;) { try_hook_winmm(); Sleep(1000); }
    return 0;
}

/* ---------- DllMain ---------- */
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID _) {
    (void)_;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        /* 抑制 "找不到 xxx.dll" 硬错误弹窗: 第三方环境可能触发良性加载失败,
           让其静默返回错误即可, 不得用模态框打断游戏 */
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX
                   | SEM_NOOPENFILEERRORBOX);
        g_tls_idx = TlsAlloc();
        pad_log("DLL_PROCESS_ATTACH (adaptive padhook, highrefreshrate build)");
        load_real_dinput8();
        load_config();
        g_host = GetModuleHandleA(NULL);
        if (g_mode == 1 || g_mode == 3) {
            try_hook_winmm();
            DWORD tid;
            HANDLE h = CreateThread(NULL, 0, worker_thread, NULL, 0, &tid);
            if (h) CloseHandle(h);
        }
    }
    return TRUE;
}
