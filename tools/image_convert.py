"""
Image to RGB565 Converter for ST7789 Display

将任意图片转换为 ST7789 屏幕可用的 RGB565 格式（C 数组 + bin 文件）。

用法:
    python image_convert.py <输入图片> [输出前缀] [宽度] [高度]

输出:
    - .c + .h 文件:  可直接编译到固件中的 C 数组
    - .bin 文件:     原始 RGB565 数据，放入 SD 卡后用 sdcard_display_raw() 加载

示例:
    python image_convert.py photo.jpg                # 默认 320x240
    python image_convert.py photo.jpg pic 100 50     # 自定义尺寸和输出名

依赖:
    pip install pillow
"""

import sys
import os
from pathlib import Path
import struct


def rgb_to_rgb565(r, g, b):
    """将 8-bit RGB 转换为 RGB565 格式"""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def convert_image(input_path, output_prefix, width=320, height=240):
    try:
        from PIL import Image
    except ImportError:
        print("错误: 需要安装 Pillow 库")
        print("请运行: pip install pillow")
        sys.exit(1)

    print(f"读取图片: {input_path}")
    img = Image.open(input_path).convert("RGB")
    original_w, original_h = img.size
    print(f"原始尺寸: {original_w}x{original_h}")

    # 缩放图片
    img = img.resize((width, height), Image.LANCZOS)
    print(f"缩放至: {width}x{height}")

    # 生成变量名（基于输出前缀）
    var_name = Path(output_prefix).stem
    var_name = "".join(c if c.isalnum() or c == "_" else "_" for c in var_name)

    pixels = list(img.getdata())

    c_path = output_prefix + ".c"
    h_path = output_prefix + ".h"

    # ── 生成 C 数组文件 ──────────────────────────────────
    with open(c_path, "w", encoding="utf-8") as f:
        f.write(f"// 图片: {os.path.basename(input_path)}\n")
        f.write(f"// 尺寸: {width}x{height}\n")
        f.write(f"// 格式: RGB565\n")
        f.write(f"// 自动生成，请勿手动编辑\n\n")
        f.write(f'#include <stdint.h>\n\n')
        f.write(f"// 图片宽度和高度\n")
        f.write(f"#define {var_name.upper()}_WIDTH  {width}\n")
        f.write(f"#define {var_name.upper()}_HEIGHT {height}\n\n")
        f.write(f"// RGB565 像素数据\n")
        f.write(f"static const uint16_t {var_name}_data[{width * height}] = {{\n")

        for y in range(height):
            f.write("    ")
            row_pixels = []
            for x in range(width):
                r, g, b = pixels[y * width + x]
                rgb565 = rgb_to_rgb565(r, g, b)
                row_pixels.append(f"0x{rgb565:04X}")
            f.write(", ".join(row_pixels))
            if y < height - 1:
                f.write(",")
            f.write("\n")

        f.write("};\n")

    file_size = os.path.getsize(c_path)
    print(f"已生成: {c_path} ({file_size:,} 字节)")
    print(f"已生成: {h_path} (头文件)")
    print(f"数组名: {var_name}_data")
    print(f"尺寸宏: {var_name.upper()}_WIDTH x {var_name.upper()}_HEIGHT")

    # ── 生成 .bin 二进制文件 ──────────────────────────────
    bin_path = output_prefix + ".bin"
    with open(bin_path, "wb") as f:
        for y in range(height):
            for x in range(width):
                r, g, b = pixels[y * width + x]
                val = rgb_to_rgb565(r, g, b)
                f.write(struct.pack('<H', val))  # little-endian uint16

    bin_size = os.path.getsize(bin_path)
    print(f"已生成: {bin_path} ({bin_size:,} 字节)\n")

    print("────────── 使用方法 ──────────")
    print("【方式1】编译到固件 (C 数组):")
    print(f'  1. 将 "{os.path.basename(c_path)}" "{os.path.basename(h_path)}" 复制到 main/')
    print(f'  2. 在 main/CMakeLists.txt 的 SRCS 中添加 "{os.path.basename(c_path)}"')
    print(f'  3. 代码中:')
    print(f'     #include "{os.path.basename(h_path)}"')
    print(f'     lv_disp_draw_bitmap(0, 0, {var_name.upper()}_WIDTH, {var_name.upper()}_HEIGHT, {var_name}_data);')
    print()
    print("【方式2】从 SD 卡加载 (.bin 文件):")
    print(f'  1. 将 "{os.path.basename(bin_path)}" 复制到 SD 卡根目录')
    print(f'  2. 代码中:')
    print(f'     sdcard_display_raw("{os.path.basename(bin_path)}", {width}, {height});')


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    input_path = sys.argv[1]
    # 输出前缀: 默认使用输入图片的文件名 (不含扩展名)
    if len(sys.argv) > 2:
        output_prefix = sys.argv[2]
    else:
        output_prefix = os.path.splitext(input_path)[0]

    width = int(sys.argv[3]) if len(sys.argv) > 3 else 320
    height = int(sys.argv[4]) if len(sys.argv) > 4 else 240

    if not os.path.exists(input_path):
        print(f"错误: 找不到文件 '{input_path}'")
        sys.exit(1)

    convert_image(input_path, output_prefix, width, height)
