/* test_load.c - 烟雾测试: 加载 dinput8.dll, 验证导出符号可解析.
 * 编译: i686 gcc -o test_load.exe test_load.c
 * 运行: test_load.exe   (输出 DirectInput8Create 指针即通过)
 */
#include <windows.h>
#include <stdio.h>

int main(void) {
    SetDllDirectoryA(".");  /* 优先从当前目录加载 dinput8.dll */
    HMODULE h = LoadLibraryA("dinput8.dll");
    if (!h) {
        printf("[FAIL] LoadLibraryA(dinput8.dll) error=%lu\n", GetLastError());
        return 1;
    }
    FARPROC p = GetProcAddress(h, "DirectInput8Create");
    if (!p) {
        printf("[FAIL] GetProcAddress(DirectInput8Create) error=%lu\n", GetLastError());
        FreeLibrary(h);
        return 2;
    }
    printf("[OK] dinput8.dll loaded at %p, DirectInput8Create=%p\n", (void*)h, (void*)p);
    FreeLibrary(h);
    return 0;
}
