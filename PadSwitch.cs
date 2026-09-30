// PadSwitch.cs - 东方全向移动 输入方式开关 (C# 5, WinForms)
//
// 部署: 与 dinput8.dll 放同目录. 双击运行后:
//   1) 下拉框选游戏 (自动扫描 ..\test 与 ..\Tohou\STG 下所有 th*.exe)
//   2) 勾选 [启用全向移动] -> 复制 dinput8.dll + 写 padhook.ini(mode=3)
//   3) 取消勾选 -> 删除 dinput8.dll / padhook.ini / padhook.log (恢复原版)
//   4) [启动游戏] -> 运行选中的 exe
//
// 兼容: 任何用 DirectInput8 读键盘 + winmm 读手柄的东方作品, 同一个 dinput8.dll 即可.
// 手柄按钮编号因人而异: 启用后可直接编辑游戏目录 padhook.ini 的 btn_* 校准.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Text;
using System.Windows.Forms;

namespace PadSwitch
{
    internal static class Program
    {
        [STAThread]
        private static int Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
            return 0;
        }
    }

    internal sealed class MainForm : Form
    {
        private readonly ComboBox _games;
        private readonly CheckBox _enable;
        private readonly Button _browse;
        private readonly Button _launch;
        private readonly Label _status;
        private bool _suppress;

        public MainForm()
        {
            Text = "东方全向移动 - 输入方式开关";
            Width = 640;
            Height = 310;
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = SystemColors.Window;
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;

            var lblGame = new Label { Text = "游戏:", Left = 12, Top = 18, AutoSize = true };
            _games = new ComboBox {
                Left = 60, Top = 14, Width = 450,
                DropDownStyle = ComboBoxStyle.DropDownList
            };
            _games.SelectedIndexChanged += delegate { UpdateStatus(); };

            _browse = new Button { Text = "浏览...", Left = 520, Top = 13, Width = 96 };
            _browse.Click += Browse_Click;

            _enable = new CheckBox {
                Text = "启用全向移动 (覆盖游戏手柄输入)",
                Left = 12, Top = 54, Width = 600, AutoSize = false
            };
            _enable.CheckedChanged += Enable_CheckedChanged;

            _launch = new Button {
                Text = "启动游戏", Left = 260, Top = 92, Width = 120, Height = 38
            };
            _launch.Click += Launch_Click;

            _status = new Label {
                Left = 12, Top = 144, Width = 604, Height = 118,
                AutoSize = false, BorderStyle = BorderStyle.FixedSingle
            };

            Controls.AddRange(new Control[] { lblGame, _games, _browse, _enable, _launch, _status });

            Load += delegate { ScanGames(); UpdateStatus(); };
        }

        // 扫描 ..\test 与 ..\Tohou\STG 下所有主程序 th*.exe
        private void ScanGames()
        {
            _games.Items.Clear();
            string projectRoot = Path.GetFullPath(Path.Combine(Application.StartupPath, ".."));
            string[] roots = new[] {
                Path.Combine(projectRoot, "test"),
                Path.Combine(projectRoot, "Tohou", "STG")
            };
            var found = new List<string>();
            foreach (string root in roots) {
                if (!Directory.Exists(root)) continue;
                try {
                    string[] exes = Directory.GetFiles(root, "*.exe", SearchOption.AllDirectories);
                    foreach (string exe in exes) {
                        string name = Path.GetFileName(exe);
                        if (name == null) continue;
                        string low = name.ToLowerInvariant();
                        // 只保留主程序 th*.exe, 跳过 custom/replayview/score/update/setup/unins/prac/vpatch/converter
                        if (low.StartsWith("th") && low.EndsWith(".exe") &&
                            !low.Contains("custom") && !low.Contains("replay") &&
                            !low.Contains("score") && !low.Contains("update") &&
                            !low.Contains("setup") && !low.Contains("unins") &&
                            !low.Contains("prac") && !low.Contains("vpatch") &&
                            !low.Contains("converter") &&
                            !found.Exists(s => string.Equals(s, exe, StringComparison.OrdinalIgnoreCase))) {
                            found.Add(exe);
                        }
                    }
                } catch (Exception) { }
            }
            found.Sort(StringComparer.OrdinalIgnoreCase);
            foreach (string s in found) _games.Items.Add(s);
            if (_games.Items.Count > 0) _games.SelectedIndex = 0;
        }

        private void Browse_Click(object sender, EventArgs e)
        {
            using (OpenFileDialog dlg = new OpenFileDialog {
                Filter = "游戏可执行文件 (*.exe)|*.exe|所有文件 (*.*)|*.*",
                Title = "选择游戏 .exe"
            }) {
                if (dlg.ShowDialog() == DialogResult.OK) {
                    if (!_games.Items.Contains(dlg.FileName)) {
                        _games.Items.Add(dlg.FileName);
                    }
                    _games.SelectedItem = dlg.FileName;
                    UpdateStatus();
                }
            }
        }

        // ---------- 部署辅助 ----------

        /* 写 padhook.ini: 无 BOM UTF-8, mode=3 + 按钮默认映射 (玩家可事后编辑校准) */
        private static void WriteConfig(string gameDir)
        {
            string ini =
                "[padhook]\r\n" +
                "mode=3\r\n" +
                "btn_shoot=0\r\n" +
                "btn_bomb=1\r\n" +
                "btn_slow=4\r\n" +
                "btn_pause=7\r\n";
            File.WriteAllText(Path.Combine(gameDir, "padhook.ini"), ini, new UTF8Encoding(false));
        }

        private static bool IsInstalledAt(string gameDir)
        {
            return File.Exists(Path.Combine(gameDir, "dinput8.dll")) &&
                   File.Exists(Path.Combine(gameDir, "padhook.ini"));
        }

        /* 找出正在运行的该游戏进程 (本程序为 x86, 可读到 32 位游戏模块路径) */
        private static List<Process> FindRunningGames(string gamePath)
        {
            var list = new List<Process>();
            foreach (Process p in Process.GetProcesses()) {
                try {
                    if (p.MainModule != null &&
                        string.Equals(p.MainModule.FileName, gamePath, StringComparison.OrdinalIgnoreCase)) {
                        list.Add(p);
                    }
                } catch { }
            }
            return list;
        }

        /* 切换前确保游戏已关闭; 运行中则提示可自动关闭. 返回 false=用户取消 */
        private static bool EnsureGameClosed(string gamePath)
        {
            List<Process> running = FindRunningGames(gamePath);
            if (running.Count == 0) return true;
            DialogResult r = MessageBox.Show(
                "游戏正在运行，切换输入方式必须先关闭游戏。\n是否自动关闭游戏？",
                "需要关闭游戏", MessageBoxButtons.YesNo, MessageBoxIcon.Warning);
            if (r != DialogResult.Yes) return false;
            foreach (Process p in running) {
                try { p.Kill(); p.WaitForExit(3000); } catch { }
            }
            System.Threading.Thread.Sleep(400);
            return true;
        }

        private void Enable_CheckedChanged(object sender, EventArgs e)
        {
            if (_suppress) return;
            string gamePath = _games.SelectedItem as string;
            if (string.IsNullOrEmpty(gamePath)) return;
            string gameDir = Path.GetDirectoryName(gamePath);
            if (string.IsNullOrEmpty(gameDir)) return;
            string targetDll = Path.Combine(gameDir, "dinput8.dll");

            try {
                if (!EnsureGameClosed(gamePath)) return;
                if (_enable.Checked) {
                    string srcDll = Path.Combine(Application.StartupPath, "dinput8.dll");
                    if (!File.Exists(srcDll)) {
                        MessageBox.Show(
                            "未找到同目录的 dinput8.dll, 无法启用.\n" +
                            "请把 dinput8.dll 与本程序放在同一目录.",
                            "错误", MessageBoxButtons.OK, MessageBoxIcon.Error);
                        return;
                    }
                    File.Copy(srcDll, targetDll, true);
                    WriteConfig(gameDir);
                    /* 清掉旧日志, 便于新一轮诊断 */
                    string oldLog = Path.Combine(gameDir, "padhook.log");
                    if (File.Exists(oldLog)) File.Delete(oldLog);
                } else {
                    string[] ours = { "dinput8.dll", "padhook.ini", "padhook.log" };
                    foreach (string f in ours) {
                        string fp = Path.Combine(gameDir, f);
                        if (File.Exists(fp)) File.Delete(fp);
                    }
                }
            } catch (Exception ex) {
                MessageBox.Show("切换失败: " + ex.Message,
                    "错误", MessageBoxButtons.OK, MessageBoxIcon.Error);
            } finally {
                UpdateStatus();
            }
        }

        private void Launch_Click(object sender, EventArgs e)
        {
            string gamePath = _games.SelectedItem as string;
            if (string.IsNullOrEmpty(gamePath) || !File.Exists(gamePath)) {
                MessageBox.Show("请先选择有效的游戏 .exe",
                    "提示", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            try {
                Process.Start(new ProcessStartInfo {
                    FileName = gamePath,
                    WorkingDirectory = Path.GetDirectoryName(gamePath) ?? "",
                    UseShellExecute = false
                });
            } catch (Exception ex) {
                MessageBox.Show("启动失败: " + ex.Message,
                    "错误", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private void UpdateStatus()
        {
            string gamePath = _games.SelectedItem as string;
            if (string.IsNullOrEmpty(gamePath)) {
                _status.Text = "未选择游戏.\n请用 [浏览...] 选择 .exe, 或确认 ..\\test 目录存在.";
                _suppress = true;
                _enable.Checked = false;
                _enable.Enabled = false;
                _suppress = false;
                return;
            }
            string gameDir = Path.GetDirectoryName(gamePath) ?? "";
            string targetIni = Path.Combine(gameDir, "padhook.ini");
            bool installed = IsInstalledAt(gameDir);
            bool running = FindRunningGames(gamePath).Count > 0;

            _enable.Enabled = true;
            if (_enable.Checked != installed) {
                _suppress = true;
                _enable.Checked = installed;
                _suppress = false;
            }
            _status.Text = "游戏: " + gamePath + "\n" +
                "状态: " + (installed ? "● 全向移动已启用 (mode=3)" : "○ 原版输入") +
                          (running ? "  [游戏运行中]" : "") + "\n" +
                "配置: " + targetIni + "\n" +
                "  (按钮编号不对应时，退出游戏后编辑此 ini 的 btn_* 项)";
        }
    }
}
