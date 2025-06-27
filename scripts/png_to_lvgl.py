from PIL import Image
import numpy as np
import os

def convert_png_to_lvgl_array(png_path, output_path):
    # 打开PNG图片
    img = Image.open(png_path)
    
    # 转换为RGBA模式
    img = img.convert('RGBA')
    
    # 调整大小为16x16（使用 BICUBIC 替代 LANCZOS）
    img = img.resize((64, 64), Image.BICUBIC)
    
    # 获取像素数据
    pixels = np.array(img)
    
    # 提取alpha通道
    alpha = pixels[:, :, 3]
    
    # 生成C数组
    c_array = []
    for row in alpha:
        for pixel in row:
            c_array.append(f"0x{pixel:02x}")
    
    # 生成C代码
    c_code = f"""// 自动生成的图像数据
static const uint8_t star_map[] = {{
    {', '.join(c_array)}
}};

static lv_img_dsc_t star_img = {{
    .header = {{
        .cf = LV_IMG_CF_ALPHA_8BIT,
        .always_zero = 0,
        .reserved = 0,
        .w = 16,
        .h = 16,
    }},
    .data_size = {len(c_array)},
    .data = star_map,
}};
"""
    
    # 写入文件
    with open(output_path, 'w') as f:
        f.write(c_code)
    
    print(f"转换完成！输出文件：{output_path}")

if __name__ == "__main__":
    # 使用示例
    png_path = input("请输入PNG图片路径: ")
    output_path = input("请输入输出文件路径 (例如: star_image.h): ")
    
    if not os.path.exists(png_path):
        print(f"错误：找不到文件 {png_path}")
    else:
        convert_png_to_lvgl_array(png_path, output_path)