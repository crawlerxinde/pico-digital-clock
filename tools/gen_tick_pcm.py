#!/usr/bin/env python3
"""从音频文件提取“滴答”声，生成固件用的 PCM C 数组。

RP2350 无法实时解码 MP3，所以在 PC 上解码为 24kHz 单声道 16bit，
截取一两个“滴答”样本，归一化后写成 const int16_t 数组。

用法:
    tools/gen_tick_pcm.py <input.mp3|wav|...> [output.c]
默认输出 src/tick_pcm.c（生成 src/tick_pcm.h 的常量 TICK_PCM_N 保持不变）。

需要系统安装 ffmpeg。
"""
import array
import math
import os
import subprocess
import sys

SR         = 24000          # 与 audio_pio 的 PICO_SAMPLE_FREQ 一致
TICK_N     = 3840           # 每个样本 160ms @24k
TARGET_PEAK = 8000          # 归一化峰值（与原合成音一致）
FADE       = 240            # 末尾 10ms 淡出，避免截断爆音
ROOT       = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def decode(path):
    pcm = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(SR),
         "-f", "s16le", "-"],
        check=True, stdout=subprocess.PIPE).stdout
    a = array.array('h')
    a.frombytes(pcm)
    return a


def find_onsets(a):
    """返回按时间排序、间隔 >=0.5s 的若干个起音点（样本下标）。"""
    win = int(SR * 0.005)
    env = []
    for i in range(0, len(a) - win, win):
        s = sum(v * v for v in a[i:i + win])
        env.append(math.sqrt(s / win))
    if not env:
        return []
    mx = max(env)
    thr = mx * 0.2
    groups = []
    cur = None
    for i, e in enumerate(env):
        if e > thr:
            cur = [i, i, e] if cur is None else [cur[0], i, max(cur[2], e)]
        elif cur is not None:
            groups.append(cur)
            cur = None
    if cur:
        groups.append(cur)
    # 只保留间隔 >=0.5s 的起音，按峰值取最强的前几个，再按时间排序
    picked = []
    for g in sorted(groups, key=lambda g: -g[2]):
        t = g[0] * win
        if all(abs(t - p) >= SR // 2 for p in picked):
            picked.append(t)
        if len(picked) >= 4:
            break
    return sorted(picked)


def extract(a, onset):
    start = max(0, onset - int(0.005 * SR))
    seg = list(a[start:start + TICK_N])
    seg += [0] * (TICK_N - len(seg))
    peak = max((abs(v) for v in seg), default=0)
    if peak:
        seg = [int(v * TARGET_PEAK / peak) for v in seg]
    for i in range(FADE):
        seg[TICK_N - FADE + i] = seg[TICK_N - FADE + i] * (FADE - i) // FADE
    return seg


def to_c(name, seg):
    lines = [f"const int16_t {name}[TICK_PCM_N] = {{"]
    for i in range(0, len(seg), 12):
        lines.append("    " + ", ".join(f"{v:6d}" for v in seg[i:i + 12]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "src", "tick_pcm.c")

    a = decode(src)
    onsets = find_onsets(a)
    if len(onsets) < 2:
        print(f"只找到 {len(onsets)} 个起音，至少需要 2 个", file=sys.stderr)
        return 1
    print("onsets(s):", [round(t / SR, 3) for t in onsets])
    a_seg = extract(a, onsets[0])
    b_seg = extract(a, onsets[1])

    with open(out, "w") as f:
        f.write("/* 本文件由 tools/gen_tick_pcm.py 生成，请勿手改。 */\n")
        f.write(f"/* 来源: {os.path.basename(src)} */\n")
        f.write("#include \"tick_pcm.h\"\n\n")
        f.write(to_c("tick_pcm_a", a_seg) + "\n\n")
        f.write(to_c("tick_pcm_b", b_seg) + "\n")
    print("written", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
