/* vector.h - 极坐标 -> 游戏速度矢量 直接映射（空间域，区别于 pwm.c 的时间域抖动）
 *
 * 独立项目 vectormove（从 highrefreshrate\padhook_adaptive 发展而来）:
 * 不再用两相邻 8 向数字方向按占空比抖动, 而是在游戏运动函数内部直接把速度
 * 矢量旋转到摇杆极角, 模长恒为游戏设计速度, 与摇杆幅值无关:
 *     vx =  S * cos(theta)
 *     vy = -S * sin(theta)      (游戏 y 轴向下为正, 摇杆归一化 y 上正)
 *
 * 多游戏 profile, 安装前做机器码签名校验, 不匹配的作品静默不安装:
 *   TH15 v1.00b : 浮点速度槽覆写（站点 45453d, 避让 HFR 的 45455b 桩）
 *   TH17 v1.00a : 定点位移重算（站点 44720f, 游戏每 tick 整数位移入口）
 */
#ifndef VECTOR_H
#define VECTOR_H

/* 安装运动层 inline hook。无任何签名匹配/失败返回 0（已记日志），成功返回 1。 */
int vector_install(void);

/* 当前是否在关卡玩法中。菜单/暂停时返回 0, 菜单仍走 PWM 键盘注入。
 * TH15 用玩家对象指针判定; TH17 用运动函数心跳（玩家协程停跑即超时）。 */
int vector_in_gameplay(void);

/* TH15: hook 站点每 tick 调用, p = 玩家对象（游戏 EDI）。仅摇杆激活时改写浮点 vx/vy。 */
void __cdecl vector_apply_th15(void *p);

/* TH17: 站点在"每 tick 整数位移"算出之前。spd = 游戏本 tick 自选速度槽
 * （已含通常/低速与正斜向选择）。函数必须始终写出 dx/dy:
 *   摇杆激活 -> 单位化矢量匀速位移;
 *   否则     -> 按游戏原公式逐项复刻（键盘/POV/原生 XInput 方向语义不变）。 */
void __cdecl vector_apply_th17(void *p, int spd, int *dx, int *dy);

/* 纯数学: 归一化摇杆 (x,y, y 上正) + 游戏主向速度 spd + 全局系数 gs。
 * 激活(超出死区)返回 1 并写出 vx/vy（游戏坐标, y 下正）；死区内返回 0。 */
int vector_compute(double x, double y, double dead, int spd, double gs,
                   float *vx, float *vy);

/* 纯数学: 仅做死区判定+单位化, 输出单位矢量（y 已翻成游戏坐标）。 */
int vector_unit(double x, double y, double dead, double *ux, double *uy);

/* 轨迹插件导出（由 dinput8.dll 导出，traj.dll 通过 GetProcAddress 获取） */
__declspec(dllexport) int  vm_get_player_pos(float *x, float *y, long *seq);
__declspec(dllexport) int  vm_get_game_kind(void);
__declspec(dllexport) DWORD vm_get_device_rva(void);
__declspec(dllexport) int  vm_get_stick(float *x, float *y);

#endif
