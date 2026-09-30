#define PADHOOK_DIAG
#include "padhook.c"

static BYTE g_tpl[272];
static void run_frame(BYTE* joy, BYTE* kb) {
    memcpy(joy, g_tpl, 272);   /* joy_process 会中和缓冲, 每帧从模板重建 */
    ZeroMemory(kb, 256);
    joy_process(272, joy);
    kb_process(256, kb);
}

static const char* bits(BYTE* k) {
    static char s[64];
    sprintf(s, "%d%d%d%d Z=%d X=%d S=%d",
        !!(k[DIK_UP]&0x80), !!(k[DIK_DOWN]&0x80),
        !!(k[DIK_LEFT]&0x80), !!(k[DIK_RIGHT]&0x80),
        !!(k[DIK_Z]&0x80), !!(k[DIK_X]&0x80), !!(k[DIK_LSHIFT]&0x80));
    return s;
}

static void set_tpl(LONG x, LONG y, DWORD pov, int btn) {
    ZeroMemory(g_tpl, sizeof g_tpl);
    *(LONG*)(g_tpl+OFF_X) = x; *(LONG*)(g_tpl+OFF_Y) = y;
    *(DWORD*)(g_tpl+OFF_POV0) = pov;
    if (btn >= 0) g_tpl[OFF_BTN + (btn>>3)] |= (0x80 >> (btn&7));
}

static void calibrate(void) {
    BYTE kb[256];
    set_tpl(32767, 32767, 0xFFFFFFFF, -1);
    g_neu_init = 0;
    for (int i=0;i<40;i++){ run_frame(g_tpl,kb); Sleep(12); }
}

static void scenario(const char* name, LONG x, LONG y, DWORD pov, int btn) {
    calibrate();
    BYTE joy[272]; BYTE kb[256];
    set_tpl(x, y, pov, btn);

    printf("== %s (x=%ld y=%ld pov=%08lx btn=%d) ==\n", name, x, y, pov, btn);
    run_frame(joy,kb);
    printf("first frame: %s\n", bits(kb));
    for (int i=0;i<9;i++){ run_frame(joy,kb); printf("  %s\n", bits(kb)); Sleep(12); }
    /* 回中 */
    set_tpl(32767, 32767, 0xFFFFFFFF, -1);
    for (int i=0;i<3;i++){ run_frame(joy,kb); Sleep(12); }
    printf("after release: %s\n\n", bits(kb));
}

int main(void) {
    /* 模拟 ini axis_min/axis_max = 0..65535 (等价于 padhook.ini 手动覆盖) */
    g_cfg_amin = 0; g_cfg_amax = 65535; g_cfg_range = 1;
    scenario("E  pure right (0deg)",  65535, 32767, 0xFFFFFFFF, -1);
    scenario("NE (45deg)",            65535, 0,     0xFFFFFFFF, -1);
    scenario("N  (90deg)",            32767, 0,     0xFFFFFFFF, -1);
    scenario("30deg (near E)",        61200, 16400, 0xFFFFFFFF, -1);
    scenario("deadzone (10%)",        36000, 32767, 0xFFFFFFFF, -1);
    scenario("POV north",             32767, 32767, 0,          -1);
    scenario("button0 -> Z",          32767, 32767, 0xFFFFFFFF, 0);
    return 0;
}
