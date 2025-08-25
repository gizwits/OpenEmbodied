from PIL import Image
import numpy as np
import os

def convert_png_to_lvgl_alpha8(png_path, output_path, var_name="spiral_img_64"):
    img = Image.open(png_path).convert('RGBA').resize((120, 120), Image.BICUBIC)
    pixels = np.array(img)
    # 只用alpha通道
    alpha = pixels[:, :, 3]
    c_array = []
    for row in alpha:
        for pixel in row:
            c_array.append(f"0x{pixel:02x}")
    c_code = f"""// 自动生成的螺旋图像数据
#include <lvgl.h>
static const uint8_t spiral_map64[] = {{
    {', '.join(c_array)}
}};

const lv_img_dsc_t {var_name} = {{
    .header = {{
        .cf = LV_COLOR_FORMAT_A8,
        .w = 120,
        .h = 120,
    }},
    .data_size = sizeof(spiral_map64),
    .data = spiral_map64,
}};
"""
    with open(output_path, 'w') as f:
        f.write(c_code)
    print(f"转换完成！输出文件：{output_path}")

if __name__ == "__main__":
    png_path = input("请输入PNG图片路径: ")
    output_path = input("请输入输出文件路径 (例如: spiral_image.h): ")
    convert_png_to_lvgl_alpha8(png_path, output_path)