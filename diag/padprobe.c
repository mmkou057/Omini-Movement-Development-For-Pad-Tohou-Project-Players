/* padprobe.c - 完全透明的 dinput8.dll 代理 (诊断用)
 *
 * 不 hook 任何东西, 不改任何返回值, 不起线程.
 * 只把 DirectInput8Create 原样转发给系统真 dinput8.dll.
 * 目的: 验证"仅存在一个代理 dinput8.dll"是否就会破坏游戏输入.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef HRESULT (WINAPI *DIC8_t)(HINSTANCE, DWORD, REFIID, LPVOID, LPVOID);
static DIC8_t g_real = NULL;

HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid,
                                  LPVOID ppv, LPVOID unk) {
    if (!g_real) {
        char sys[MAX_PATH], path[MAX_PATH];
        GetSystemDirectoryA(sys, MAX_PATH);
        wsprintfA(path, "%s\\dinput8.dll", sys);
        HMODULE m = LoadLibraryA(path);
        if (!m) return E_FAIL;
        g_real = (DIC8_t)GetProcAddress(m, "DirectInput8Create");
        if (!g_real) return E_FAIL;
    }
    return g_real(hinst, ver, riid, ppv, unk);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)hinst; (void)reason; (void)reserved;
    return TRUE;
}
