# 喝水提醒 Drink Reminder

一个极简 Linux 原生桌面喝水提醒应用。用 C 语言 + X11 开发，运行时内存占用仅 **~5 MB**。

## 特色

- **极致轻量** — 纯 C + X11 实现，无 Electron/GTK/Qt 等重型框架
- **内存极低** — 运行时 RSS ~5MB（对比 Electron 版 ~200-400MB）
- **系统托盘** — 基于 DBus StatusNotifierItem 协议，GNOME/KDE 均支持
- **定时提醒** — 每 N 分钟弹出置顶提醒窗口
- **可配置** — 通过托盘菜单调整提醒间隔

## 编译

依赖：`libx11-dev` `libxft-dev` `libdbus-1-dev` `libpng-dev`

```bash
# Ubuntu/Debian
sudo apt install libx11-dev libxft-dev libdbus-1-dev libpng-dev

# 编译
./build.sh
```

## 安装

```bash
# 打包 .deb
./pack-deb.sh

# 安装
sudo dpkg -i drink-reminder_1.0-1_amd64.deb

# 卸载
sudo dpkg -r drink-reminder
```

## 使用

| 操作 | 功能 |
|------|------|
| 双击托盘图标 | 弹出菜单（立即提醒 / 设置 / 退出） |
| 点击"知道了" | 关闭提醒窗口 |
| 菜单 → 设置 | 调整提醒间隔（1-120 分钟） |
| `kill -USR1 $(cat /tmp/drink-reminder.pid)` | 立即触发提醒 |
| `drink-reminder --set interval=15` | 命令行设置间隔 |

启动后，程序在后台运行，每 30 分钟（默认）弹出置顶提醒窗口。

## 内存占用对比

| 版本 | 运行时 RSS | 二进制大小 |
|------|-----------|-----------|
| Electron 原版 | ~200-400 MB | ~150 MB |
| 本应用（C + X11） | **~5 MB** | **~47 KB** |

## 项目结构

```
drink-reminder/
├── main.c              ← 源码（含注释）
├── build.sh            ← 编译脚本
├── pack-deb.sh         ← .deb 打包脚本
├── water.png           ← 应用图标
└── .gitignore
```

## License

MIT
