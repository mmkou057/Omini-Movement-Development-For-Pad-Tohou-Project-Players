/* test_vector.c - vector.c 纯数学自测（控制台）
 * build: ..\..\mingw32\bin\gcc.exe -m32 -O2 -Wall -o test_vector.exe test_vector.c -lm
 * run:   test_vector.exe          （全绿退出码 0）
 *
 * 直接 #include "vector.c" 以访问 static 的 TH17 原公式复刻函数。
 */
#include <stdio.h>
#include <math.h>

void pad_log(const char *fmt, ...) { (void)fmt; }
int  pad_get_stick(double *x, double *y, unsigned long *pov) {
    (void)x; (void)y; (void)pov; return 0;
}

#include "vector.c"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
    else         { printf("ok:   %s\n", msg); } } while (0)

static int near_eq(double a, double b, double eps) { return fabs(a - b) <= eps; }

int main(void) {
    float vx, vy;
    double ux, uy;
    const int SPD = 1000;

    /* ---------- vector_unit: 死区 / 单位化 / y 翻转 ---------- */
    CHECK(!vector_unit(0.0, 0.0, 0.24, &ux, &uy), "unit 居中死区: 不激活");
    CHECK(!vector_unit(0.2, 0.0, 0.24, &ux, &uy), "unit 0.20 < 死区: 不激活");
    CHECK( vector_unit(0.3, 0.0, 0.24, &ux, &uy), "unit 0.30 > 死区: 激活");

    vector_unit(0.0, 1.0, 0.24, &ux, &uy);
    CHECK(near_eq(ux, 0, 1e-9) && near_eq(uy, -1.0, 1e-9),
          "unit 上(y=1) -> (0,-1) 游戏坐标 y 翻转");
    vector_unit(1.0, 0.0, 0.24, &ux, &uy);
    CHECK(near_eq(ux, 1.0, 1e-9) && near_eq(uy, 0, 1e-9), "unit 右(x=1) -> (1,0)");
    vector_unit(1.0, 1.0, 0.24, &ux, &uy);
    CHECK(near_eq(ux, 0.70710678, 1e-6) && near_eq(uy, -0.70710678, 1e-6),
          "unit 右上 -> (0.707,-0.707)");
    {
        int ang; double worst = 0.0;
        for (ang = 0; ang < 360; ang += 3) {
            double r = ang * 3.14159265358979 / 180.0;
            vector_unit(cos(r), sin(r), 0.24, &ux, &uy);
            double m = sqrt(ux*ux + uy*uy);
            double e = fabs(m - 1.0); if (e > worst) worst = e;
        }
        CHECK(worst < 1e-9, "unit 各方向模长恒为 1");
    }

    /* ---------- vector_compute: TH15 浮点矢量 ---------- */
    CHECK(!vector_compute(0.0, 0.0, 0.24, SPD, 1.0, &vx, &vy), "居中死区: 不激活");
    CHECK(!vector_compute(0.2, 0.0, 0.24, SPD, 1.0, &vx, &vy), "0.20 < 死区: 不激活");
    CHECK( vector_compute(0.3, 0.0, 0.24, SPD, 1.0, &vx, &vy), "0.30 > 死区: 激活");

    vector_compute(1.0, 0.0, 0.24, SPD, 1.0, &vx, &vy);
    CHECK(near_eq(vx, 1000, 0.01) && near_eq(vy, 0, 0.01), "0°   -> (1000,0)");
    vector_compute(0.0, 1.0, 0.24, SPD, 1.0, &vx, &vy);
    CHECK(near_eq(vx, 0, 0.01) && near_eq(vy, -1000, 0.01), "90°  -> (0,-1000)");
    vector_compute(-1.0, 0.0, 0.24, SPD, 1.0, &vx, &vy);
    CHECK(near_eq(vx, -1000, 0.01) && near_eq(vy, 0, 0.01), "180° -> (-1000,0)");
    vector_compute(0.0, -1.0, 0.24, SPD, 1.0, &vx, &vy);
    CHECK(near_eq(vx, 0, 0.01) && near_eq(vy, 1000, 0.01), "270° -> (0,+1000)");
    vector_compute(cos(30.0*3.14159265358979/180.0), sin(30.0*3.14159265358979/180.0),
                   0.24, SPD, 1.0, &vx, &vy);
    CHECK(near_eq(vx, 866.0254, 0.05) && near_eq(vy, -500.0, 0.05), "30°  -> (866,-500)");
    vector_compute(1.0, 1.0, 0.24, SPD, 1.0, &vx, &vy);
    CHECK(near_eq(vx, 707.1068, 0.05) && near_eq(vy, -707.1068, 0.05), "45°  -> (707,-707)");

    {
        int ang; double worst = 0.0;
        for (ang = 0; ang < 360; ang += 3) {
            double r = ang * 3.14159265358979 / 180.0;
            vector_compute(cos(r), sin(r), 0.24, SPD, 1.0, &vx, &vy);
            double m = sqrt((double)vx*vx + (double)vy*vy);
            double e = fabs(m - SPD); if (e > worst) worst = e;
        }
        CHECK(worst < 0.01, "0..357° 模长恒为 1000（匀速）");
    }
    vector_compute(0.5, 0.0, 0.24, SPD, 1.0, &vx, &vy);
    CHECK(near_eq(vx, 1000, 0.01), "半推 0.5 仍满速（幅值不调制）");
    vector_compute(1.0, 0.0, 0.24, SPD, 0.25, &vx, &vy);
    CHECK(near_eq(vx, 250, 0.01), "gs=0.25（HFR 1/4 tick）-> 250");
    vector_compute(1.0, 0.0, 0.24, 400, 1.0, &vx, &vy);
    CHECK(near_eq(vx, 400, 0.01), "低速槽 spd=400 -> 400");
    CHECK(!vector_compute(1.0, 0.0, 0.24, SPD, 0.0, &vx, &vy), "gs=0 防护");
    CHECK(!vector_compute(1.0, 0.0, 0.24, 0, 1.0, &vx, &vy), "spd=0 防护");

    /* ---------- t17_passive_axis: TH17 原公式逐项复刻 ----------
     * 原: dx = trunc(float(dir*spd - trunc(acc*G)) * mul), G=-128, 常态 acc=0 */
    CHECK(t17_passive_axis(0, 1000, 0.0f, -128.0f, 1.0f) == 0,  "T17 无方向 -> 0");
    CHECK(t17_passive_axis(1, 1000, 0.0f, -128.0f, 1.0f) == 1000, "T17 正向 spd=1000");
    CHECK(t17_passive_axis(-1, 1000, 0.0f, -128.0f, 1.0f) == -1000, "T17 负向 -1000");
    CHECK(t17_passive_axis(1, 1000, 0.0f, -128.0f, 0.5f) == 500,  "T17 mul=0.5 -> 500");
    CHECK(t17_passive_axis(-1, 1000, 0.0f, -128.0f, 0.5f) == -500,"T17 mul=0.5 负向 -500");
    CHECK(t17_passive_axis(1, 400, 0.0f, -128.0f, 1.0f) == 400,  "T17 低速槽 400");
    /* acc 补偿项: acc=1 -> trunc(-128)=-128 -> dir0 时 v=0-(-128)=128 */
    CHECK(t17_passive_axis(0, 1000, 1.0f, -128.0f, 1.0f) == 128, "T17 acc=1 补偿项");
    CHECK(t17_passive_axis(0, 1000, 1.0f, -128.0f, 0.5f) == 64,  "T17 acc=1 mul=0.5");
    CHECK(t17_passive_axis(1, 1000, 1.0f, -128.0f, 1.0f) == 1128,"T17 正向+acc 补偿");
    /* 向零截断一致性（cvttss2si == C int 强转） */
    CHECK(t17_passive_axis(1, 333, 0.0f, -128.0f, 1.0f) == 333, "T17 整数精确");
    CHECK(t17_passive_axis(1, 333, 0.0f, -128.0f, 0.5f) == 166, "T17 333*0.5 截断=166");

    if (fails) { printf("\n%d FAILURE(S)\n", fails); return 1; }
    printf("\nALL TESTS PASSED\n");
    return 0;
}
