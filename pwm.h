/* pwm.h - 8 向凸包分解 + Bresenham 占空比
 *
 * 原理: 8 个离散速度向量 (4 主向 + 4 对角, 对角模长 = 主向模长) 构成正八边形.
 * 任意落在八边形内的目标向量可写成两相邻顶点向量的凸组合:
 *     v = (1-alpha)*v_A + alpha*v_B
 * 每帧只输出 A 或 B 之一, 按 alpha 比例用 Bresenham 交替出帧,
 * 帧均速度即趋近 v. 由于东方匀速移动, 摇杆幅值 r 不参与调制 (只要 >死区即满速).
 *
 * 坐标: 输入 (x,y) 上正 y 向上 (与 XInput sThumbLY 一致).
 * 方向索引: 0=NONE 1=E 2=NE 3=N 4=NW 5=W 6=SW 7=S 8=SE (逆时针, 角度 0..2pi)
 */
#ifndef PWM_H
#define PWM_H

typedef struct {
    int a;          /* 第一方向 (1..8); 0 表示死区内无输入 */
    int b;          /* 第二方向 (1..8) */
    double alpha;   /* b 的占空比 [0,1] */
} pwm_dir_t;

/* 把摇杆 (x,y) 分解为两相邻方向与 b 占空比. dead 为径向死区. */
void pwm_decompose(double x, double y, double dead, pwm_dir_t *out);

/* Bresenham 累加器. 每帧调用一次, 返回 1 -> 本帧用 b, 0 -> 用 a. */
typedef struct { double acc; } pwm_acc_t;
int pwm_step(pwm_acc_t *st, double alpha);

#endif
