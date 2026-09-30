/* pwm.c - 见 pwm.h */
#include "pwm.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* SEQ[k] = 角度 k*45° 对应的方向索引 (k=0..7, 角度从 +x 起逆时针) */
static const int SEQ[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

void pwm_decompose(double x, double y, double dead, pwm_dir_t *out) {
    double mag = sqrt(x * x + y * y);
    if (mag < dead) { out->a = 0; out->b = 0; out->alpha = 0.0; return; }
    double ang = atan2(y, x);              /* [-pi, pi], y 上正 */
    if (ang < 0.0) ang += 2.0 * M_PI;
    double seg = ang / (M_PI / 4.0);       /* 0..8 */
    int k = (int)floor(seg);
    if (k > 7) k = 7;
    out->a = SEQ[k];
    out->b = SEQ[(k + 1) & 7];
    out->alpha = seg - (double)k;
}

int pwm_step(pwm_acc_t *st, double alpha) {
    st->acc += alpha;
    if (st->acc >= 1.0) { st->acc -= 1.0; return 1; }
    return 0;
}
