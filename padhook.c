/* padhook.c - dinput8.dll 代理 (DLL proxy)  【vectormove 多游戏版】
 *
 * 支持的游戏世代:
 *   TH15 及更早世代: 键盘走 DirectInput 键盘设备 GetDeviceState;
 *                    手柄走游戏自建 DInput 手柄设备（搭便车 hook vtable[9]）。
 *   TH17 世代起     : 键盘改走 user32!GetKeyboardState（256 字节 VK 数组）;
 *                    手柄有三路: DInput 设备 / winmm joyGetPosEx /
 *                    原生 XInput（XINPUT1_3.dll 序号2 = XInputGetState,
 *                    游戏每帧无条件轮询 0..3）。
 *
 * 本代理在 mode=3/4 下做的事:
 *   1) DInput 路径: hook IDirectInputDevice8::GetDeviceState(vtable[9]),
 *      真实手柄数据存入全局, 交还游戏的缓冲中和为静止（旧方案, TH15 用）;
 *   2) XInput 路径: IAT hook（按序号, 不靠名字）, 真实摇杆/十字键/按钮存全局,
 *      交还游戏的 XINPUT_STATE 清零（游戏看到"已连接但无输入"的手柄）;
 *   3) winmm 路径: joyGetPos/Ex + joyGetDevCapsA/W 一律报 NODRIVER, 封死回退;
 *   4) 键盘注入双通道:
 *      - DInput 键盘 GDS（DIK 索引）  -> TH15 世代
 *      - GetKeyboardState（VK 索引） -> TH17+ 世代
 *      两个通道按同一套全局手柄状态注入, PWM 步进只与数据代次挂钩,
 *      同帧多次调用结果幂等;
 *   5) mode=4 关卡玩法中: 方向键不注入, 由 vector.c 运动层 inline hook
 *      直接把游戏位移矢量旋转到摇杆极角（极坐标矢量直映, 匀速）;
 *      菜单/暂停仍走 mode3 的 8 向凸包 PWM。
 *
 * 输入源仲裁: XInput 轮询在最近 500ms 内成功 -> 用 XInput（标准布局, 干净）;
 *             否则回退 DInput（第三方手柄/老游戏）。两源按钮都打包进统一的
 *             32 字节逻辑位表: DInput 物理按钮号 i -> 字节 i/8 的高位序;
 *             XInput wButtons 位号 n -> 同一逻辑号 n（0..15）,
 *             左右扳机 -> 逻辑号 16/17。ini 按钮号按当前活动源校准。
 *
 * 模式 (padhook.ini [padhook] mode):
 *   0 = 完全透明（默认, 等同未安装）
 *   1 = 仅压制 winmm 手柄（诊断）
 *   2 = 仅 hook DInput 键盘 GDS 且原样透传（诊断）
 *   3 = 完整全向移动（中和 + PWM 注入键盘）
 *   4 = 极坐标矢量直映（TH15/TH17 运动层签名匹配才装; 菜单仍 PWM）
 *
 * 注意: 真 dinput8.dll 用绝对路径 C:\Windows\System32\dinput8.dll 加载, 避免递归。
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
#include "vector.h"

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

#define POV_NONE  0xFFFFFFFFu

/* ---------- 日志 ---------- */
static CRITICAL_SECTION g_logcs;
static int g_logcs_init = 0;
void pad_log(const char* fmt, ...) {   /* 非 static: vector.c 共用日志 */
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
typedef MMRESULT (WINAPI *joyGetPos_t)(UINT, LPJOYINFO);
typedef MMRESULT (WINAPI *joyGetPosEx_t)(DWORD, LPJOYINFOEX);
typedef MMRESULT (WINAPI *joyGetDevCaps_t)(UINT, void*, UINT);
typedef BOOL    (WINAPI *GetKeyboardState_t)(PBYTE);
typedef DWORD   (WINAPI *XInputGetState_t)(DWORD, LPVOID);

/* ---------- 原始函数 / 接口指针 ---------- */
static HMODULE              g_real_dinput8 = NULL;
static DirectInput8Create_t g_real_DIC8 = NULL;
static CreateDevice_t       g_orig_CreateDevice = NULL;

/* ---------- 配置 ---------- */
static int g_mode = 0;
static int g_cfg_loaded = 0;
static int g_vec_enabled = 1;   /* ini vec=0 时不装运动 hook（A/B 排障用） */
/* 手柄按钮编号 -> 游戏动作 (load_config 从 ini 读取) */
static int g_btn_shoot = 0, g_btn_bomb = 1, g_btn_slow = 4, g_btn_pause = 7;

/* 轴范围来源优先级: ini 手动覆盖 > GetProperty(DIPROP_RANGE) > 自适应观察 > 默认1000. */
static LONG g_cfg_amin = 0, g_cfg_amax = 0; static int g_cfg_range = 0;
static LONG g_q_amin = 0,   g_q_amax = 0;   static int g_q_range = 0;
static double g_span_obs = 0.0;
/* 手柄真实数据帧代次: DInput 每真实帧 +1, XInput 每轮询到手柄 +1;
   键盘注入侧只在代次变化时推进一步 PWM（占空比与消费率无关）。 */
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
    if (g_mode < 0 || g_mode > 4) g_mode = 0;
    g_vec_enabled = GetPrivateProfileIntA("padhook", "vec", 1, path);
    /* 手柄按钮编号映射（数据驱动, 不写死）:
       DInput 源 = 物理按钮号; XInput 源 = wButtons 位号(0..15), 扳机16/17 */
    g_btn_shoot = GetPrivateProfileIntA("padhook", "btn_shoot", 0, path);
    g_btn_bomb  = GetPrivateProfileIntA("padhook", "btn_bomb",  1, path);
    g_btn_slow  = GetPrivateProfileIntA("padhook", "btn_slow",  4, path);
    g_btn_pause = GetPrivateProfileIntA("padhook", "btn_pause", 7, path);
    int amin = GetPrivateProfileIntA("padhook", "axis_min", -100000, path);
    int amax = GetPrivateProfileIntA("padhook", "axis_max", -100000, path);
    if (amax > amin) { g_cfg_amin = (LONG)amin; g_cfg_amax = (LONG)amax; g_cfg_range = 1; }
    pad_log("config: %s mode=%d vec=%d btns=%d/%d/%d/%d axis=%s", path, g_mode,
        g_vec_enabled,
        g_btn_shoot, g_btn_bomb, g_btn_slow, g_btn_pause,
        g_cfg_range ? "ini" : "auto");
}

/* ---------- 向量化异常捕获（定位运动 hook 崩溃 EIP 的决定性证据） ---------- */
static LONG WINAPI crash_veh(PEXCEPTION_POINTERS ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    int fatal =
        code == EXCEPTION_ACCESS_VIOLATION
        || code == EXCEPTION_ILLEGAL_INSTRUCTION
        || code == EXCEPTION_PRIV_INSTRUCTION
        || code == EXCEPTION_INT_DIVIDE_BY_ZERO
        || code == EXCEPTION_NONCONTINUABLE_EXCEPTION
        || code == EXCEPTION_STACK_OVERFLOW
        || code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
    if (fatal) {
        BYTE* base = (BYTE*)GetModuleHandleA(NULL);
        PCONTEXT c = ep->ContextRecord;
        EXCEPTION_RECORD* r = ep->ExceptionRecord;
        const char* rw = "?";
        void* bad = NULL;
        if (code == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
            rw = r->ExceptionInformation[0] == 0 ? "read"
               : r->ExceptionInformation[0] == 1 ? "write" : "exec";
            bad = (void*)r->ExceptionInformation[1];
        }
        pad_log("VEH! code=0x%08lX %s bad=%p fault=%p+rva=0x%08lX",
                code, rw, bad, r->ExceptionAddress,
                (DWORD)((BYTE*)r->ExceptionAddress - base));
        pad_log("VEH ctx EIP=%p EAX=%08lX ECX=%08lX EDX=%08lX EBX=%08lX "
                "ESP=%p EBP=%p ESI=%08lX EDI=%08lX",
                (void*)c->Eip, c->Eax, c->Ecx, c->Edx, c->Ebx,
                (void*)c->Esp, (void*)c->Ebp, c->Esi, c->Edi);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---------- IAT hook ---------- */
static int slot_is_resolved(DWORD val) {
    if (val < 0x10000) return 0;
    if (val & 0x80000000u) return 0;   /* 仍是序号 thunk */
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)val, &mbi, sizeof mbi)) return 0;
    return (mbi.State == MEM_COMMIT)
        && (mbi.Type == MEM_IMAGE)
        && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ
                         | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}
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
            if (orig == newf) return orig;               /* 已 hook */
            if (!slot_is_resolved((DWORD)orig)) return NULL; /* 加载器尚未填好 */
            DWORD old;
            if (!VirtualProtect(&at->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return NULL;
            at->u1.Function = (DWORD_PTR)newf;
            VirtualProtect(&at->u1.Function, sizeof(void*), old, &old);
            return orig;
        }
    }
    return NULL;
}

/* 按序号 hook: 遍历所有名字以 prefix 开头（不区分大小写）的导入描述符,
   匹配序号导入。TH17 的 XInputGetState = XINPUT1_3.dll 序号2（无函数名）。 */
static void* iat_hook_ordinal(const char* prefix, WORD ordinal, void* newf) {
    HMODULE target = GetModuleHandleA(NULL);
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
        size_t n = strlen(prefix);
        if (_strnicmp(name, prefix, n) != 0) continue;
        DWORD olt_rva = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
        PIMAGE_THUNK_DATA olt = (PIMAGE_THUNK_DATA)(base + olt_rva);
        PIMAGE_THUNK_DATA at  = (PIMAGE_THUNK_DATA)(base + imp->FirstThunk);
        for (; olt->u1.AddressOfData; olt++, at++) {
            if (!IMAGE_SNAP_BY_ORDINAL(olt->u1.Ordinal)) continue;
            if ((WORD)olt->u1.Ordinal != ordinal) continue;
            void* orig = (void*)at->u1.Function;
            if (orig == newf) return orig;
            if (!slot_is_resolved((DWORD)orig)) return NULL;
            DWORD old;
            if (!VirtualProtect(&at->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return NULL;
            pad_log("IAT ordinal hook: %s!%u @%p orig=%p",
                    name, (unsigned)ordinal, (void*)&at->u1.Function, orig);
            at->u1.Function = (DWORD_PTR)newf;
            VirtualProtect(&at->u1.Function, sizeof(void*), old, &old);
            return orig;
        }
    }
    return (void*)1;   /* 遍历完没找到: 此游戏没有该导入 */
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
 *  统一手柄状态（两个源: DInput / XInput; 注入侧只读仲裁后的结果）
 * ======================================================================== */

/* --- DInput 源（joy_process 写） --- */
static double g_pad_x = 0.0;    /* 归一化 -1..1, 右正 */
static double g_pad_y = 0.0;    /* 归一化 -1..1, 上正 */
static DWORD  g_pad_pov = POV_NONE;
static BYTE   g_pad_btn[32];    /* 逻辑按钮位, 32 字节 */
static int    g_pad_alive = 0;

/* 自然归中点的经验估计 (EMA; 兼容 ±1000 / 0..65535 等任何范围) */
static double g_neu_x = 0.0, g_neu_y = 0.0;
static int g_neu_init = 0;

/* --- XInput 源（XInputGetState_hook 写） ---
 * XINPUT_STATE(16B): +0 dwPacketNumber +4 wButtons +6 bLTrig +7 bRTrig
 *                   +8 sThumbLX +0x0a sThumbLY +0x0c sThumbRX +0x0e sThumbRY
 * wButtons 位定义（与游戏 4019b0 反汇编一致）:
 *   0 dpadU 1 dpadD 2 dpadL 3 dpadR 4 START 5 BACK 6 LTHUMB 7 RTHUMB
 *   8 LSHOULDER 9 RSHOULDER 12 A 13 B 14 X 15 Y
 */
static volatile long long g_xi_x64 = 0, g_xi_y64 = 0;  /* GCC 内置原子读写 */
static volatile DWORD  g_xi_pov = POV_NONE;
static BYTE   g_xi_btn[32];     /* 逻辑号 = wButtons 位号; 16/17 = 左右扳机 */
static volatile DWORD  g_xi_last_tick = 0;
static volatile int    g_xi_seen = 0;

#define XI_FRESH_MS     500
#define XI_STICK_MAX    32767.0
#define XI_TRIG_THRESH  30

/* 仲裁: 返回活动源, 1=XInput, 0=DInput, -1=无。
 * 同时输出归一化摇杆/POV 和 32 字节逻辑按钮表。 */
static int active_input(double *ox, double *oy, DWORD *opov, BYTE obtn[32]) {
    DWORD now = GetTickCount();
    int xi_fresh = g_xi_seen && (now - g_xi_last_tick) <= XI_FRESH_MS;
    if (xi_fresh) {
        /* 直接读 volatile long long, 32 位下拆成两个 DWORD 读;
           MemoryBarrier 在读前确保看到最新值 */
        MemoryBarrier();
        long long bx = g_xi_x64, by = g_xi_y64;
        if (ox)   *ox = *(double*)&bx;
        if (oy)   *oy = *(double*)&by;
        if (opov) *opov = g_xi_pov;
        if (obtn) memcpy(obtn, g_xi_btn, 32);
        {
            static unsigned s_ai = 0;
            if (++s_ai <= 8 || (s_ai & 127u) == 0)
                pad_log("AI[XI] x64=%lld y64=%lld -> (%.3f,%.3f)",
                        bx, by, *(double*)&bx, *(double*)&by);
        }
        return 1;
    }
    if (g_pad_alive) {
        if (ox)   *ox = g_pad_x;
        if (oy)   *oy = g_pad_y;
        if (opov) *opov = g_pad_pov;
        if (obtn) memcpy(obtn, g_pad_btn, 32);
        return 0;
    }
    if (ox) *ox = 0.0;
    if (oy) *oy = 0.0;
    if (opov) *opov = POV_NONE;
    if (obtn) ZeroMemory(obtn, 32);
    return -1;
}

/* 供 vector.c 读取当前真实摇杆状态（x/y 归一化, y 上正; pov POV_NONE=未按） */
int pad_get_stick(double *x, double *y, unsigned long *pov) {
    double sx, sy; DWORD sp;
    int src = active_input(&sx, &sy, &sp, NULL);
    if (src < 0) return 0;
    if (x) *x = sx;
    if (y) *y = sy;
    if (pov) *pov = sp;
    return 1 + src;   /* 1=DInput, 2=XInput, 便于诊断区分源 */
}

/* ========================================================================
 *  DInput: 统一 GetDeviceState hook
 * ======================================================================== */
static void* g_kb_self = NULL;
static void* g_joy_self = NULL;
static GetDeviceState_t g_real_GDS  = NULL;
static GetDeviceState_t g_chain_GDS = NULL;

static DWORD g_tls_idx = 0;
static int reentry_get(void) {
    return g_tls_idx ? (int)(DWORD_PTR)TlsGetValue(g_tls_idx) : 0;
}
static void reentry_set(int v) {
    if (g_tls_idx) TlsSetValue(g_tls_idx, (LPVOID)(DWORD_PTR)v);
}

static void patch_vt(void** vt, int idx, void* newf);
static HRESULT WINAPI unified_GetDeviceState_hook(void* self, DWORD cb, LPVOID data);

static void install_outer(void** vt) {
    void* cur = vt[9];
    if (!g_real_GDS) g_real_GDS = (GetDeviceState_t)cur;
    if (cur == (void*)0) return;
    if (cur != (void*)unified_GetDeviceState_hook) {
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
    if (reentry_get()) {
        return g_real_GDS ? g_real_GDS(self, cb, data) : DIERR_NOTACQUIRED;
    }
    GetDeviceState_t fn = g_chain_GDS ? g_chain_GDS : g_real_GDS;
    if (!fn) return DIERR_NOTACQUIRED;
    reentry_set(1);
    HRESULT hr = fn(self, cb, data);
    reentry_set(0);
    if (FAILED(hr)) return hr;

    if (self == g_joy_self)       joy_process(cb, data);
    else if (self == g_kb_self)   kb_process(cb, data);
    return hr;
}

/* ---------- 轴范围查询 / 对象枚举（诊断） ---------- */
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
static void enum_objects_once(void) {
    static int done = 0;
    if (done || !g_joy_self) return;
    done = 1;
    void** vt = *(void***)g_joy_self;
    EnumObjects_t f = (EnumObjects_t)vt[4];
    pad_log("--- EnumObjects vt[4]=%p ---", f);
    if (f) f(g_joy_self, obj_enum_cb, NULL, 0);
    pad_log("--- EnumObjects done ---");
}

/* ---------- DInput 手柄处理: 真实数据存全局, 游戏缓冲中和 ---------- */
static void joy_process(DWORD cb, LPVOID data) {
    if (cb < OFF_BTN + 32 || !data) { g_pad_alive = 0; return; }
    query_range_once();
    enum_objects_once();
    BYTE* b = (BYTE*)data;
    LONG rx = *(LONG*)(b + OFF_X);
    LONG ry = *(LONG*)(b + OFF_Y);
    DWORD pov = *(DWORD*)(b + OFF_POV0);

    double span;
    if (g_cfg_range)          span = (double)(g_cfg_amax - g_cfg_amin) * 0.5;
    else if (g_q_range)       span = (double)(g_q_amax  - g_q_amin)  * 0.5;
    else if (g_span_obs > 1.0) span = g_span_obs;
    else                      span = 1000.0;

    int anybtn = 0; int i;
    for (i = 0; i < 32; i++) if (b[OFF_BTN + i] & 0x80) { anybtn = 1; break; }

    double th = span * 0.16;
    if (!g_neu_init) { g_neu_x = (double)rx; g_neu_y = (double)ry; g_neu_init = 1; }
    int quiet = !anybtn && pov == POV_NONE
        && ((double)rx - g_neu_x) * ((double)rx - g_neu_x) < th*th
        && ((double)ry - g_neu_y) * ((double)ry - g_neu_y) < th*th;
    if (quiet) {
        g_neu_x += ((double)rx - g_neu_x) * 0.10;
        g_neu_y += ((double)ry - g_neu_y) * 0.10;
    }

    if (!g_cfg_range && !g_q_range) {
        double devx = (double)rx - g_neu_x; if (devx < 0.0) devx = -devx;
        double devy = (double)ry - g_neu_y; if (devy < 0.0) devy = -devy;
        double dev = devx > devy ? devx : devy;
        if (dev > g_span_obs) g_span_obs = dev;
        else if (g_span_obs > 1.0) g_span_obs *= 0.99995;
        if (g_span_obs > 1.0) span = g_span_obs;
    }

    double nx = ((double)rx - g_neu_x) / span;
    double ny = -((double)ry - g_neu_y) / span;   /* DInput y 下正 -> 上正 */
    if (nx >  1.0) nx =  1.0; if (nx < -1.0) nx = -1.0;
    if (ny >  1.0) ny =  1.0; if (ny < -1.0) ny = -1.0;

    g_pad_x = nx; g_pad_y = ny;
    g_pad_pov = pov;
    memcpy(g_pad_btn, b + OFF_BTN, sizeof(g_pad_btn));
    g_pad_alive = 1;
    InterlockedIncrement(&g_joy_seq);

    /* 中和交还游戏的缓冲 */
    ZeroMemory(data, cb);
    *(LONG*)(b + OFF_X) = (LONG)g_neu_x;
    *(LONG*)(b + OFF_Y) = (LONG)g_neu_y;
    if (cb >= OFF_POV0 + 4*4) {
        *(DWORD*)(b + OFF_POV0 + 0)  = POV_NONE;
        *(DWORD*)(b + OFF_POV0 + 4)  = POV_NONE;
        *(DWORD*)(b + OFF_POV0 + 8)  = POV_NONE;
        *(DWORD*)(b + OFF_POV0 + 12) = POV_NONE;
    }

#ifdef PADHOOK_DIAG
    {
        static BYTE s_prev[32]; static DWORD s_prev_pov = POV_NONE; static int s_have = 0;
        if (!s_have || memcmp(s_prev, g_pad_btn, sizeof(g_pad_btn)) || s_prev_pov != pov) {
            char hex[sizeof(g_pad_btn)*2 + 1]; int j;
            for (j = 0; j < (int)sizeof(g_pad_btn); j++) sprintf(hex + j*2, "%02x", g_pad_btn[j]);
            pad_log("BTN[DI] raw=(%ld,%ld) pov=%08lx %s", rx, ry, pov, hex);
            memcpy(s_prev, g_pad_btn, sizeof(g_pad_btn));
            s_prev_pov = pov; s_have = 1;
        }
    }
#endif
}

/* ========================================================================
 *  XInput: IAT 序号 hook（TH17+ 原生手柄路径）
 * ======================================================================== */
static XInputGetState_t g_real_XInputGetState = NULL;

#pragma pack(push,1)
typedef struct {
    WORD  wButtons;       /* +0 */
    BYTE  bLeftTrigger;   /* +2 */
    BYTE  bRightTrigger;  /* +3 */
    SHORT sThumbLX;       /* +4 */
    SHORT sThumbLY;       /* +6 */
    SHORT sThumbRX;       /* +8 */
    SHORT sThumbRY;       /* +10 */
} XI_GAMEPAD;
typedef struct {
    DWORD      dwPacketNumber;
    XI_GAMEPAD gp;
} XI_STATE;
#pragma pack(pop)

/* 逻辑按钮位表打包: 逻辑号 idx -> 字节 idx/8 的 (0x80>>(idx&7))（与 DInput 同序） */
static void xi_set_btn(BYTE btn[32], int idx, int on) {
    if (idx < 0 || idx > 31) return;
    if (on) btn[idx >> 3] |= (BYTE)(0x80 >> (idx & 7));
}
static DWORD xi_dpad_to_pov(WORD w) {
    int u = w & 0x0001, d = w & 0x0002, l = w & 0x0004, r = w & 0x0008;
    if (u && !d) {
        if (l && !r) return 31500;
        if (r && !l) return  4500;
        return 0;
    }
    if (d && !u) {
        if (l && !r) return 22500;
        if (r && !l) return 13500;
        return 18000;
    }
    if (l && !r) return 27000;
    if (r && !l) return  9000;
    return POV_NONE;
}

static DWORD WINAPI XInputGetState_hook(DWORD index, LPVOID pstate) {
    XInputGetState_t fn = g_real_XInputGetState;
    DWORD hr = fn ? fn(index, pstate) : (DWORD)ERROR_DEVICE_NOT_CONNECTED;
    if (hr != ERROR_SUCCESS || !pstate || index > 3) {
        static DWORD s_bad_log = 0;
        if (s_bad_log++ < 8)
            pad_log("XI[bad] idx=%lu hr=0x%08lx", (unsigned long)index, (unsigned long)hr);
        return hr;
    }

    XI_STATE *s = (XI_STATE*)pstate;
    WORD  w = s->gp.wButtons;
    SHORT lx = s->gp.sThumbLX, ly = s->gp.sThumbLY;
    BYTE  lt = s->gp.bLeftTrigger, rt = s->gp.bRightTrigger;

    double nx = (double)lx / XI_STICK_MAX;
    double ny = (double)ly / XI_STICK_MAX;   /* XInput y 已上正, 不翻 */
    if (nx >  1.0) nx =  1.0; if (nx < -1.0) nx = -1.0;
    if (ny >  1.0) ny =  1.0; if (ny < -1.0) ny = -1.0;

    /* 摇杆幅值变化时也打一条（BTN[XI] 原本只在按钮/POV 变化时打） */
    {
        static double s_px = 0.0, s_py = 0.0;
        double dx_ = nx - s_px, dy_ = ny - s_py;
        if (dx_*dx_ + dy_*dy_ > 0.05*0.05) {
            pad_log("STK[XI] idx=%lu stick=(%.3f,%.3f) raw=(%d,%d)",
                    (unsigned long)index, nx, ny, (int)lx, (int)ly);
            s_px = nx; s_py = ny;
        }
    }

    BYTE btn[32]; ZeroMemory(btn, 32);
    {
        int n;
        for (n = 0; n < 16; n++) if (w & (1u << n)) xi_set_btn(btn, n, 1);
        xi_set_btn(btn, 16, lt > XI_TRIG_THRESH);
        xi_set_btn(btn, 17, rt > XI_TRIG_THRESH);
    }

    /* 只写 index 0: idx=1 等虚拟设备会返回成功但数据为 0, 覆盖真实手柄 */
    if (index == 0) {
        g_xi_x64 = *(volatile long long*)&nx;
        g_xi_y64 = *(volatile long long*)&ny;
        g_xi_pov = xi_dpad_to_pov(w);
        memcpy(g_xi_btn, btn, 32);
        MemoryBarrier();
        g_xi_last_tick = GetTickCount();
    }
    if (!g_xi_seen) {
        g_xi_seen = 1;
        pad_log("XInput pad online (user index %lu) buttons=0x%04x",
                (unsigned long)index, (unsigned)w);
    }
    InterlockedIncrement(&g_joy_seq);

    {
        /* 状态变化即记一条（发布版也保留: ini 按钮号校准需要真实位号;
           一次游玩至多几十条, 不刷屏）。位号: 0-3 十字键 4 START 8 LB 9 RB
           12 A 13 B 14 X 15 Y; 16/17 左右扳机 */
        static WORD s_prev_w = 0; static BYTE s_prev_lt = 0, s_prev_rt = 0;
        static DWORD s_prev_pov = POV_NONE;
        if (w != s_prev_w || lt != s_prev_lt || rt != s_prev_rt
            || g_xi_pov != s_prev_pov) {
            pad_log("BTN[XI] btn=0x%04x lt=%u rt=%u pov=%08lx stick=(%.3f,%.3f)",
                    (unsigned)w, (unsigned)lt, (unsigned)rt,
                    (unsigned long)g_xi_pov, nx, ny);
            s_prev_w = w; s_prev_lt = lt; s_prev_rt = rt; s_prev_pov = g_xi_pov;
        }
    }

    /* 中和: 游戏看到"已连接但完全静止"的手柄, 方向/按钮全部由我们键盘通道注入 */
    ZeroMemory(pstate, sizeof(XI_STATE));
    return ERROR_SUCCESS;
}

/* ========================================================================
 *  注入核心: 把统一手柄状态写进一份 256 键状态数组（DIK 或 VK 索引皆可）
 * ======================================================================== */
typedef struct {
    BYTE up, down, left, right;
    BYTE shoot, bomb, slow, pause;
} keymap_t;

static const keymap_t KM_DIK = {
    DIK_UP, DIK_DOWN, DIK_LEFT, DIK_RIGHT,
    DIK_Z, DIK_X, DIK_LSHIFT, DIK_ESCAPE
};
static const keymap_t KM_VK = {
    VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
    'Z', 'X', VK_SHIFT, VK_ESCAPE
};

static void clear_udlr(BYTE* k, const keymap_t* m) {
    k[m->up] &= 0x7F; k[m->down] &= 0x7F; k[m->left] &= 0x7F; k[m->right] &= 0x7F;
}
static void inject_dir(BYTE* k, const keymap_t* m, int d) {
    switch (d) {
        case 1: k[m->right] |= 0x80; break;                                  /* E */
        case 2: k[m->right] |= 0x80; k[m->up] |= 0x80; break;            /* NE */
        case 3: k[m->up] |= 0x80; break;                                    /* N */
        case 4: k[m->left] |= 0x80; k[m->up] |= 0x80; break;             /* NW */
        case 5: k[m->left] |= 0x80; break;                                  /* W */
        case 6: k[m->left] |= 0x80; k[m->down] |= 0x80; break;           /* SW */
        case 7: k[m->down] |= 0x80; break;                                  /* S */
        case 8: k[m->right] |= 0x80; k[m->down] |= 0x80; break;          /* SE */
        default: break;
    }
}
/* POV: 0=N,4500=NE,...,31500=NW（与 DInput POV 约定一致） */
static void inject_pov(BYTE* k, const keymap_t* m, DWORD pov) {
    int s = (int)(pov / 4500);
    switch (s) {
        case 0: k[m->up] |= 0x80; break;
        case 1: k[m->up] |= 0x80; k[m->right] |= 0x80; break;
        case 2: k[m->right] |= 0x80; break;
        case 3: k[m->right] |= 0x80; k[m->down] |= 0x80; break;
        case 4: k[m->down] |= 0x80; break;
        case 5: k[m->down] |= 0x80; k[m->left] |= 0x80; break;
        case 6: k[m->left] |= 0x80; break;
        case 7: k[m->left] |= 0x80; k[m->up] |= 0x80; break;
        default: break;
    }
}

/* PWM 步进（按数据代次, 通道无关; 同代次内多通道调用结果相同） */
static int s_last_seq = -1;
static int s_cur_dir = 0;
static pwm_acc_t s_pwm = {0.0};
static int s_la = 0, s_lb = 0;
static LARGE_INTEGER s_qpf, s_prev_tick, s_rate_win;
static unsigned s_seq_in_win = 0;
static double s_ema_ms = 0.0;

static int pwm_dir_for(double x, double y, int stick_active) {
    if ((LONG)g_joy_seq == s_last_seq) return s_cur_dir;

    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!s_qpf.QuadPart) QueryPerformanceFrequency(&s_qpf);
    if (s_last_seq >= 0 && s_prev_tick.QuadPart) {
        double ms = (double)(now.QuadPart - s_prev_tick.QuadPart) * 1000.0
                  / (double)s_qpf.QuadPart;
        if (s_ema_ms <= 0.0) s_ema_ms = ms;
        else if (ms < s_ema_ms * 4.0) s_ema_ms += (ms - s_ema_ms) * 0.05;
        if (ms > s_ema_ms * 4.0) {          /* 停顿：重置相位与方向缓存 */
            s_pwm.acc = 0.0; s_la = s_lb = 0;
            if (s_cur_dir) pad_log("input stall %.0fms (ema %.1fms): pwm phase reset",
                                   ms, s_ema_ms);
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
    s_last_seq = (LONG)g_joy_seq;

    if (stick_active) {
        pwm_dir_t pd;
        pwm_decompose(x, y, 0.24, &pd);
        if (pd.a == 0) { s_cur_dir = 0; s_pwm.acc = 0.0; s_la = s_lb = 0; }
        else {
            if (pd.a != s_la || pd.b != s_lb) {
                s_pwm.acc = 0.0; s_la = pd.a; s_lb = pd.b;
            }
            int bit = pwm_step(&s_pwm, pd.alpha);
            s_cur_dir = bit ? pd.b : pd.a;
        }
    } else {
        s_cur_dir = 0; s_pwm.acc = 0.0; s_la = s_lb = 0;
    }
    return s_cur_dir;
}

/* 向一份 256 字节键状态数组应用注入。两个键盘通道每帧各调一次, 幂等。 */
static void apply_to_keys(BYTE* k, const keymap_t* km) {
    double x, y; DWORD pov; BYTE btn[32];
    int src = active_input(&x, &y, &pov, btn);
    int alive = (src >= 0);
    int stick_active = alive && (x*x + y*y) > 0.24*0.24;
    int pov_active   = alive && (pov != POV_NONE);
    int cur = pwm_dir_for(x, y, stick_active);

    if (g_mode == 4 && vector_in_gameplay()) {
        /* 关卡玩法: 方向由运动层矢量 hook 接管, 不注入移动键;
           POV 仍走数字注入（矢量 hook 对 POV 自动让位）。
           摇杆/POV 激活时清掉物理方向键, 保证手柄接管; 回中立即恢复。 */
        if (stick_active || pov_active) {
            clear_udlr(k, km);
            if (pov_active) inject_pov(k, km, pov);
        }
    } else if (stick_active || pov_active) {
        /* 菜单/暂停 或 mode=3: 时间域 PWM */
        clear_udlr(k, km);
        if (pov_active)        inject_pov(k, km, pov);
        else if (stick_active) inject_dir(k, km, cur);
    }

    /* 按钮 OR 进真实键态（物理键盘始终可用）; 编号来自 ini, 按活动源校准 */
    if (alive) {
        int idx;
        idx = g_btn_shoot;
        if (idx >= 0 && idx < 256 && (btn[idx>>3] & (0x80>>(idx&7))))
            k[km->shoot] |= 0x80;
        idx = g_btn_bomb;
        if (idx >= 0 && idx < 256 && (btn[idx>>3] & (0x80>>(idx&7))))
            k[km->bomb] |= 0x80;
        idx = g_btn_slow;
        if (idx >= 0 && idx < 256 && (btn[idx>>3] & (0x80>>(idx&7))))
            k[km->slow] |= 0x80;
        idx = g_btn_pause;
        if (idx >= 0 && idx < 256 && (btn[idx>>3] & (0x80>>(idx&7))))
            k[km->pause] |= 0x80;
    }

#ifdef PADHOOK_DIAG
    if (diag_due())
        pad_log("KEY[%s] src=%d stick=%d pov=%d dir=%d -> %d%d%d%d shoot=%d bomb=%d slow=%d",
            km == &KM_VK ? "VK" : "DIK", src, stick_active, pov_active, cur,
            !!(k[km->up]&0x80), !!(k[km->down]&0x80),
            !!(k[km->left]&0x80), !!(k[km->right]&0x80),
            !!(k[km->shoot]&0x80), !!(k[km->bomb]&0x80),
            !!(k[km->slow]&0x80));
#endif
}

/* ---------- DInput 键盘通道（TH15 世代） ---------- */
static void kb_process(DWORD cb, LPVOID data) {
    if (cb < 256 || !data) return;
    apply_to_keys((BYTE*)data, &KM_DIK);
}

/* ---------- GetKeyboardState 通道（TH17+ 世代） ---------- */
static GetKeyboardState_t g_real_GetKeyboardState = NULL;
static BOOL WINAPI GetKeyboardState_hook(PBYTE keys) {
    GetKeyboardState_t fn = g_real_GetKeyboardState;
    BOOL ok = fn ? fn(keys) : FALSE;
    if (ok && keys) apply_to_keys(keys, &KM_VK);
    return ok;
}

/* ---------- 键盘透传 hook (mode=2 诊断) ---------- */
static GetDeviceState_t g_orig_kb_passthru = NULL;
static HRESULT WINAPI kb_passthru_hook(void* self, DWORD cb, LPVOID data) {
    HRESULT hr = g_orig_kb_passthru ? g_orig_kb_passthru(self, cb, data) : DIERR_NOTACQUIRED;
#ifdef PADHOOK_DIAG
    if (diag_due() && SUCCEEDED(hr) && cb >= 256 && data) {
        BYTE* k = (BYTE*)data;
        pad_log("KB-PASSTHRU UDLR=%d%d%d%d ZX=%d%d",
            !!(k[DIK_UP]&0x80),!!(k[DIK_DOWN]&0x80),
            !!(k[DIK_LEFT]&0x80),!!(k[DIK_RIGHT]&0x80),
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
        } else /* mode=3/4 */ {
            g_kb_self = *ppdev;
            install_outer(vt);
            pad_log("CD keyboard self=%p realGDS=%p", *ppdev, g_real_GDS);
        }
    } else if (eq_guid(rguid, &MY_GUID_SysMouse)) {
        if (g_mode == 3 || g_mode == 4) install_outer(vt);
        if (g_mode == 2 || g_mode == 3 || g_mode == 4) pad_log("CD mouse self=%p", *ppdev);
    } else /* 手柄 */ if (g_mode == 3 || g_mode == 4) {
        g_joy_self = *ppdev;
        install_outer(vt);
        pad_log("CD joystick self=%p chain=%p", *ppdev, g_chain_GDS);
    }
    return hr;
}

/* ---------- DirectInput8Create 代理入口 ---------- */
HRESULT WINAPI MyDirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID* ppv, IUnknown* unk) {
    load_config();
    if (!g_real_dinput8) load_real_dinput8();
    if (!g_real_DIC8) return E_FAIL;
    HRESULT hr = g_real_DIC8(hinst, ver, riid, ppv, unk);
    if (SUCCEEDED(hr) && ppv && *ppv && (g_mode == 2 || g_mode == 3 || g_mode == 4)) {
        void** vt = *(void***)*ppv;
        if (!g_orig_CreateDevice) g_orig_CreateDevice = (CreateDevice_t)vt[3];
        patch_vt(vt, 3, (void*)CreateDevice_hook);
        pad_log("IDirectInput8 hooked (CreateDevice orig=%p, mode=%d)", g_orig_CreateDevice, g_mode);
    }
    return hr;
}

/* ---------- winmm 手柄 hook: 一律报"无设备", 封死 legacy 回退 ---------- */
static MMRESULT WINAPI joyGetPos_hook(UINT id, LPJOYINFO pji) {
    (void)id; (void)pji;
    return MMSYSERR_NODRIVER;
}
static MMRESULT WINAPI joyGetPosEx_hook(DWORD id, LPJOYINFOEX pjiex) {
    (void)id; (void)pjiex;
    return MMSYSERR_NODRIVER;
}
static MMRESULT WINAPI joyGetDevCaps_hook(UINT id, void* caps, UINT cb) {
    (void)id; (void)caps; (void)cb;
    return MMSYSERR_NODRIVER;
}

static HMODULE g_host = NULL;
static void try_hook_winmm(void) {
    if (!g_host) g_host = GetModuleHandleA(NULL);
    void* p;
    p = iat_hook(g_host, "winmm.dll", "joyGetPos", (void*)joyGetPos_hook);
    if (p && p != (void*)joyGetPos_hook) pad_log("winmm joyGetPos hooked @%p", p);
    p = iat_hook(g_host, "winmm.dll", "joyGetPosEx", (void*)joyGetPosEx_hook);
    if (p && p != (void*)joyGetPosEx_hook) pad_log("winmm joyGetPosEx hooked @%p", p);
    p = iat_hook(g_host, "winmm.dll", "joyGetDevCapsA", (void*)joyGetDevCaps_hook);
    if (p && p != (void*)joyGetDevCaps_hook) pad_log("winmm joyGetDevCapsA hooked @%p", p);
    p = iat_hook(g_host, "winmm.dll", "joyGetDevCapsW", (void*)joyGetDevCaps_hook);
    if (p && p != (void*)joyGetDevCaps_hook) pad_log("winmm joyGetDevCapsW hooked @%p", p);
}

static int g_xi_hook_done = 0;
static int g_gks_hook_done = 0;
static void try_hook_xinput(void) {
    if (g_xi_hook_done) return;
    void* p = iat_hook_ordinal("xinput", 2, (void*)XInputGetState_hook);
    if (p == (void*)1) { g_xi_hook_done = 1; return; }      /* 无 XInput 导入 */
    if (p == NULL) return;                                   /* 加载器还没填好, 重试 */
    if (p != (void*)XInputGetState_hook) {
        g_real_XInputGetState = (XInputGetState_t)p;
        pad_log("XInputGetState hooked, real=%p", p);
    }
    g_xi_hook_done = 1;
}
static void try_hook_keyboard_state(void) {
    if (g_gks_hook_done) return;
    if (!g_host) g_host = GetModuleHandleA(NULL);
    void* p = iat_hook(g_host, "user32.dll", "GetKeyboardState",
                       (void*)GetKeyboardState_hook);
    if (p == NULL) return;
    if (p != (void*)GetKeyboardState_hook) {
        g_real_GetKeyboardState = (GetKeyboardState_t)p;
        pad_log("GetKeyboardState hooked, real=%p", p);
    }
    g_gks_hook_done = 1;
}

static DWORD WINAPI worker_thread(LPVOID _) {
    (void)_;
    /* 代理 DllMain 可能早于部分静态导入解析完成（导入表顺序在我们之后的 DLL）,
       故所有 IAT hook 都在工作线程里重试到稳定。 */
    int i;
    for (i = 0; i < 40; i++) {
        if (g_mode == 1 || g_mode == 3 || g_mode == 4) try_hook_winmm();
        if (g_mode == 3 || g_mode == 4) {
            try_hook_xinput();
            try_hook_keyboard_state();
        }
        if (g_xi_hook_done && g_gks_hook_done) break;
        Sleep(100);
    }
    int vec_retry = 0;
    for (;;) {
        if (g_mode == 1 || g_mode == 3 || g_mode == 4) try_hook_winmm();
        if (g_mode == 3 || g_mode == 4) { try_hook_xinput(); try_hook_keyboard_state(); }
        if (g_mode == 4 && g_vec_enabled && !vector_install() && ++vec_retry <= 10)
            pad_log("vector install retry %d/10", vec_retry);
        Sleep(1000);
    }
    return 0;
}

/* ---------- DllMain ---------- */
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID _) {
    (void)_;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX
                   | SEM_NOOPENFILEERRORBOX);
        g_tls_idx = TlsAlloc();
        pad_log("DLL_PROCESS_ATTACH (vectormove padhook)");
        load_real_dinput8();
        load_config();
        g_host = GetModuleHandleA(NULL);
        AddVectoredExceptionHandler(1, crash_veh);
        if (g_mode == 4 && g_vec_enabled) {
            if (!vector_install()) {
                pad_log("vector install deferred: will retry in worker thread");
            }
        }
        if (!g_vec_enabled) pad_log("vector motion hook DISABLED by ini vec=0");
        if (g_mode == 1 || g_mode == 3 || g_mode == 4) {
            try_hook_winmm();
            if (g_mode == 3 || g_mode == 4) { try_hook_xinput(); try_hook_keyboard_state(); }
            DWORD tid;
            HANDLE h = CreateThread(NULL, 0, worker_thread, NULL, 0, &tid);
            if (h) CloseHandle(h);
        }
    }
    return TRUE;
}
