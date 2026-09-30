// th15_injector: 手柄摇杆 -> 1:1 全向移动 注入器
//
// 原理:
//   TH15 用 DirectInput8 读键盘(非独占), 读手柄后量化为 8 向.
//   本程序独立用 XInput 读真手柄, 把摇杆角度 theta 分解成
//   8 向凸包上两相邻顶点的凸组合 (占空比 alpha),
//   每帧按 Bresenham 出其中一个方向, 用硬件扫描码 SendInput 注入键盘.
//   游戏每帧 GetDeviceState 读到的就是 PWM 帧均方向 -> 任意角度匀速移动.
//
// 前提:
//   - TH15 在 custom.exe 里若开了手柄, 关掉 (本程序独占手柄输入).
//   - 本程序以 60Hz 注入; 与游戏帧若漂移, 帧均方向仍收敛到 theta.
//
// 用法:
//   th15_injector.exe              # 只在 th15.exe 前台时注入
//   th15_injector.exe --any        # 任意前台窗口都注入 (调试用)
//   th15_injector.exe --rate=120   # 自定义注入帧率
//   运行中按 F8 退出.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace th15_injector {

internal static class Program {
    // ---------------- XInput ----------------
    [StructLayout(LayoutKind.Sequential)]
    private struct XINPUT_GAMEPAD {
        public ushort wButtons;
        public byte bLeftTrigger;
        public byte bRightTrigger;
        public short sThumbLX;
        public short sThumbLY;
        public short sThumbRX;
        public short sThumbRY;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct XINPUT_STATE {
        public uint dwPacketNumber;
        public XINPUT_GAMEPAD Gamepad;
    }
    [DllImport("XInput9_1_0.dll", EntryPoint = "XInputGetState", SetLastError = false)]
    private static extern uint XInputGetState(uint dwUserIndex, out XINPUT_STATE pState);

    private const uint ERROR_SUCCESS = 0;
    private const uint ERROR_DEVICE_NOT_CONNECTED = 1167;

    // XInput 按钮
    private const ushort XG_DPAD_UP    = 0x0001;
    private const ushort XG_DPAD_DOWN  = 0x0002;
    private const ushort XG_DPAD_LEFT = 0x0004;
    private const ushort XG_DPAD_RIGHT= 0x0008;
    private const ushort XG_START     = 0x0010;
    private const ushort XG_BACK     = 0x0020;
    private const ushort XG_LSHOULDER= 0x0100;
    private const ushort XG_RSHOULDER= 0x0200;
    private const ushort XG_A        = 0x1000;
    private const ushort XG_B        = 0x2000;
    private const ushort XG_X        = 0x4000;
    private const ushort XG_Y        = 0x8000;

    // ---------------- SendInput (硬件扫描码) ----------------
    private const uint INPUT_KEYBOARD = 1;
    private const uint KEYEVENTF_SCANCODE    = 0x0008;
    private const uint KEYEVENTF_KEYUP       = 0x0002;
    private const uint KEYEVENTF_EXTENDEDKEY = 0x0001;

    [StructLayout(LayoutKind.Sequential)]
    private struct KEYBDINPUT {
        public ushort wVk;
        public ushort wScan;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct INPUT {
        public uint type;
        public KEYBDINPUT ki;
    }
    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint cInputs, INPUT[] pInputs, int cbSize);

    // DirectInput 键码 (DIK_*). 扩展键 (DIK & 0x80 != 0) 需 KEYEVENTF_EXTENDEDKEY,
    // 且 SendInput 的 wScan 用硬件扫描码 = DIK & 0x7F.
    private const ushort DIK_ESCAPE = 0x01;
    private const ushort DIK_LSHIFT = 0x2A;
    private const ushort DIK_Z      = 0x2C;
    private const ushort DIK_X      = 0x2D;
    private const ushort DIK_UP     = 0xC8;
    private const ushort DIK_DOWN   = 0xD0;
    private const ushort DIK_LEFT   = 0xCB;
    private const ushort DIK_RIGHT  = 0xCD;

    // ---------------- 时钟 / 窗口 / 进程 ----------------
    [DllImport("kernel32.dll")] private static extern bool QueryPerformanceCounter(out long lpPerformanceCount);
    [DllImport("kernel32.dll")] private static extern bool QueryPerformanceFrequency(out long lpFrequency);
    [DllImport("kernel32.dll")] private static extern void Sleep(uint dwMilliseconds);
    [DllImport("winmm.dll")]   private static extern uint timeBeginPeriod(uint uPeriod);
    [DllImport("winmm.dll")]   private static extern uint timeEndPeriod(uint uPeriod);
    [DllImport("user32.dll")]  private static extern short GetAsyncKeyState(int vKey);
    [DllImport("user32.dll")]  private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
    private const int VK_F8 = 0x77;

    // ---------------- PWM: 8 向凸包分解 ----------------
    // 方向索引 (与手柄上正 Y 向上, 数学坐标, 角度逆时针):
    //   0=NONE, 1=E, 2=NE, 3=N, 4=NW, 5=W, 6=SW, 7=S, 8=SE
    // SEQ[k] (k=0..7) 为角度 k*45° 的方向索引, 顺/逆相邻构成凸包的一条边.
    private static readonly int[] SEQ = new int[] { 1, 2, 3, 4, 5, 6, 7, 8 };
    // 每方向需按下的 DIK 集合
    private static readonly ushort[][] DIR_SCANCODES = new ushort[][] {
        new ushort[] { },                      // 0 NONE
        new ushort[] { DIK_RIGHT },            // 1 E
        new ushort[] { DIK_RIGHT, DIK_UP },    // 2 NE
        new ushort[] { DIK_UP },               // 3 N
        new ushort[] { DIK_LEFT, DIK_UP },     // 4 NW
        new ushort[] { DIK_LEFT },             // 5 W
        new ushort[] { DIK_LEFT, DIK_DOWN },   // 6 SW
        new ushort[] { DIK_DOWN },             // 7 S
        new ushort[] { DIK_RIGHT, DIK_DOWN }   // 8 SE
    };
    private static readonly string[] DIR_NAME = new string[] {
        "----", "E  ", "NE ", "N  ", "NW ", "W  ", "SW ", "S  ", "SE "
    };

    private const double DEAD = 0.24; // 径向死区 (XInput 推荐 ~0.24)

    // 把摇杆 (x,y) [上为正 y] 分解成两相邻方向 a,b 与 b 的占空比 alpha
    private static void Decompose(double x, double y, out int a, out int b, out double alpha) {
        double mag = Math.Sqrt(x * x + y * y);
        if (mag < DEAD) { a = 0; b = 0; alpha = 0.0; return; }
        double ang = Math.Atan2(y, x);          // [-pi, pi], y 上正
        if (ang < 0) ang += 2.0 * Math.PI;
        double seg = ang / (Math.PI / 4.0);     // 0..8
        int k = (int)Math.Floor(seg);
        if (k > 7) k = 7;
        a = SEQ[k];
        b = SEQ[(k + 1) & 7];
        alpha = seg - (double)k;
    }

    // ---------------- 构造一次按键事件 ----------------
    private static INPUT MakeInput(ushort dik, bool down) {
        ushort scan = (ushort)(dik & 0x7F);
        uint flags = KEYEVENTF_SCANCODE;
        if ((dik & 0x80) != 0) flags |= KEYEVENTF_EXTENDEDKEY;
        if (!down) flags |= KEYEVENTF_KEYUP;
        INPUT inp = new INPUT();
        inp.type = INPUT_KEYBOARD;
        inp.ki.wVk = 0;
        inp.ki.wScan = scan;
        inp.ki.dwFlags = flags;
        inp.ki.time = 0;
        inp.ki.dwExtraInfo = IntPtr.Zero;
        return inp;
    }

    // 取前台进程名 (小写, 无扩展名)
    private static string ForegroundProcessName() {
        IntPtr hwnd = GetForegroundWindow();
        if (hwnd == IntPtr.Zero) return "";
        uint pid;
        GetWindowThreadProcessId(hwnd, out pid);
        try {
            System.Diagnostics.Process p = System.Diagnostics.Process.GetProcessById((int)pid);
            return (p.ProcessName ?? "").ToLowerInvariant();
        } catch { return ""; }
    }

    private static double Clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

    private static int Main(string[] args) {
      try {
        bool anyWindow = false;
        double rate = 60.0;
        for (int i = 0; i < args.Length; i++) {
            string a = args[i];
            if (a == "--any") anyWindow = true;
            else if (a.StartsWith("--rate=")) {
                double r;
                if (double.TryParse(a.Substring(7), out r) && r > 0) rate = r;
            }
        }
        double frameMs = 1000.0 / rate;

        long freq;
        QueryPerformanceFrequency(out freq);
        timeBeginPeriod(1);
        Console.WriteLine("th15_injector 启动. rate={0:F1}Hz, gate={1}",
            rate, anyWindow ? "any-window" : "th15-only");
        Console.WriteLine("F8 退出. 把手柄左摇杆推到任意角度, 角色会沿该角度匀速移动.");
        Console.WriteLine("按键映射: A=Z(射击) B=X(弹幕) LB=LShift(低速) Start=Esc(暂停)");

        HashSet<ushort> held = new HashSet<ushort>();
        double pwmAcc = 0.0;          // Bresenham 累加器
        long nextPerf;
        QueryPerformanceCounter(out nextPerf);
        long periodTicks = (long)(freq / rate);
        bool xinputReported = false;

        while (true) {
            if ((GetAsyncKeyState(VK_F8) & 0x8000) != 0) break;

            string fg = ForegroundProcessName();
            bool inject = anyWindow || fg == "th15" || fg == "th15c";

            XINPUT_STATE st = default(XINPUT_STATE);
            uint rc = ERROR_DEVICE_NOT_CONNECTED;
            try { rc = XInputGetState(0, out st); }
            catch (Exception) {
                if (!xinputReported) {
                    Console.Error.WriteLine("XInput9_1_0.dll 不可用, 手柄读取失败.");
                    xinputReported = true;
                }
                rc = ERROR_DEVICE_NOT_CONNECTED;
            }

            HashSet<ushort> want = new HashSet<ushort>();
            string dirStr = "----";
            double thetaDeg = 0, alphaShow = 0;

            if (inject && rc == ERROR_SUCCESS) {
                double lx = st.Gamepad.sThumbLX / 32768.0;
                double ly = st.Gamepad.sThumbLY / 32768.0;
                int a, b; double alpha;
                Decompose(lx, ly, out a, out b, out alpha);
                int dirThis;
                if (a == 0) { dirThis = 0; pwmAcc = 0.0; }
                else {
                    pwmAcc += alpha;
                    if (pwmAcc >= 1.0) { pwmAcc -= 1.0; dirThis = b; }
                    else dirThis = a;
                }
                ushort[] scs = DIR_SCANCODES[dirThis];
                for (int i = 0; i < scs.Length; i++) want.Add(scs[i]);
                dirStr = DIR_NAME[dirThis];
                thetaDeg = (lx == 0 && ly == 0) ? 0 : Math.Atan2(ly, lx) * 180.0 / Math.PI;
                alphaShow = alpha;

                // 按钮 -> 键
                ushort btn = st.Gamepad.wButtons;
                if ((btn & XG_A) != 0)         want.Add(DIK_Z);
                if ((btn & XG_B) != 0)         want.Add(DIK_X);
                if ((btn & XG_LSHOULDER) != 0) want.Add(DIK_LSHIFT);
                if ((btn & XG_START) != 0)     want.Add(DIK_ESCAPE);
                // D-Pad 也镜像方向键, 方便纯方向玩家
                if ((btn & XG_DPAD_UP)    != 0) want.Add(DIK_UP);
                if ((btn & XG_DPAD_DOWN)  != 0) want.Add(DIK_DOWN);
                if ((btn & XG_DPAD_LEFT)  != 0) want.Add(DIK_LEFT);
                if ((btn & XG_DPAD_RIGHT) != 0) want.Add(DIK_RIGHT);
            }

            // diff: 按新按下的, 松开不再按下的
            List<INPUT> inputs = new List<INPUT>();
            foreach (ushort sc in want) if (!held.Contains(sc)) inputs.Add(MakeInput(sc, true));
            foreach (ushort sc in held) if (!want.Contains(sc)) inputs.Add(MakeInput(sc, false));
            if (inputs.Count > 0) {
                SendInput((uint)inputs.Count, inputs.ToArray(), Marshal.SizeOf(typeof(INPUT)));
            }
            held = want;

            // 状态行
            Console.Write("\r{0} fg={1,-10} dir={2} theta={3,7:F2}° alpha={4:F2} keys={5}   ",
                rc == ERROR_SUCCESS ? "PAD " : "NOPAD",
                inject ? fg.PadRight(10) : "blocked",
                dirStr, thetaDeg, alphaShow, held.Count);

            // 60Hz 定时
            nextPerf += periodTicks;
            long now;
            QueryPerformanceCounter(out now);
            long waitTicks = nextPerf - now;
            if (waitTicks < 0) {
                // 落后过多, 重置基准避免追赶风暴
                nextPerf = now;
                waitTicks = 0;
            }
            int sleepMs = (int)(waitTicks * 1000 / freq);
            if (sleepMs > 0) Sleep((uint)sleepMs);
        }

        // 退出前松开所有键
        if (held.Count > 0) {
            List<INPUT> rel = new List<INPUT>();
            foreach (ushort sc in held) rel.Add(MakeInput(sc, false));
            SendInput((uint)rel.Count, rel.ToArray(), Marshal.SizeOf(typeof(INPUT)));
        }
        timeEndPeriod(1);
        Console.WriteLine("\n已退出.");
        return 0;
      } catch (Exception e) {
        try { Console.Error.WriteLine("FATAL: " + e.GetType().FullName + ": " + e.Message); } catch {}
        try { Console.Error.WriteLine(e.StackTrace); } catch {}
        try { timeEndPeriod(1); } catch {}
        // 退出前尽量松开所有键
        return 1;
      }
    }
}

} // namespace
