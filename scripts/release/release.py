import sys
import os
import json
import zipfile
import subprocess
import re
import boto3
from botocore.exceptions import ClientError

def load_env_file():
    """加载 .env 文件中的环境变量"""
    env_file = ".env"
    if os.path.exists(env_file):
        print(f"正在加载环境变量文件: {env_file}")
        try:
            with open(env_file, 'r', encoding='utf-8') as f:
                for line in f:
                    line = line.strip()
                    print(f"读取行: {line}")
                    
                    # 跳过空行和注释行
                    if not line or line.startswith('#'):
                        continue
                    
                    # 支持两种格式：
                    # 1. KEY=VALUE (标准 .env 格式)
                    # 2. export KEY="VALUE" (shell 格式)
                    
                    if '=' in line:
                        if line.startswith('export '):
                            # 解析 export KEY="VALUE" 格式
                            parts = line[7:].split('=', 1)  # 去掉 'export '
                        else:
                            # 解析 KEY=VALUE 格式
                            parts = line.split('=', 1)
                        
                        if len(parts) == 2:
                            key = parts[0].strip()
                            value = parts[1].strip()
                            
                            # 去掉引号
                            if (value.startswith('"') and value.endswith('"')) or \
                               (value.startswith("'") and value.endswith("'")):
                                value = value[1:-1]
                            
                            os.environ[key] = value
                            print(f"  已设置: {key} = {value[:8]}..." if len(value) > 8 else f"  已设置: {key} = {value}")
            
            print("环境变量加载完成")
            return True
        except Exception as e:
            print(f"加载环境变量文件失败: {e}")
            return False
    else:
        print(f"环境变量文件不存在: {env_file}")
        return False

# 加载环境变量
load_env_file()

# 切换到项目根目录
os.chdir(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

def get_board_type():
    with open("build/compile_commands.json") as f:
        data = json.load(f)
        for item in data:
            if not item["file"].endswith("main.cc"):
                continue
            command = item["command"]
            # extract -DBOARD_TYPE=xxx
            board_type = command.split("-DBOARD_TYPE=\\\"")[1].split("\\\"")[0].strip()
            return board_type
    return None

def get_project_version():
    with open("CMakeLists.txt") as f:
        for line in f:
            if line.startswith("set(PROJECT_VER"):
                return line.split("\"")[1].split("\"")[0].strip()
    return None

def merge_bin():
    if os.system("idf.py merge-bin") != 0:
        print("merge bin failed")
        sys.exit(1)

def upload_to_cos(file_path, object_name=None):
    """上传文件到腾讯云 COS
    
    Args:
        file_path: 本地文件路径
        object_name: COS 中的对象名称，如果为 None 则使用文件名
    
    Returns:
        bool: 上传是否成功
    """
    # 从环境变量读取腾讯云 COS 配置
    secret_id = os.environ.get('COS_SECRET_ID')
    secret_key = os.environ.get('COS_SECRET_KEY')
    region = os.environ.get('COS_REGION', 'ap-guangzhou')
    bucket_name = os.environ.get('COS_BUCKET_NAME')
    app_id = os.environ.get('APP_ID')
    print(f"secret_id: {secret_id}")
    print(f"secret_key: {secret_key}")
    print(f"region: {region}")
    print(f"bucket_name: {bucket_name}")
    print(f"app_id: {app_id}")

    if not all([secret_id, secret_key, bucket_name, app_id]):
        print("Error: 缺少腾讯云 COS 配置环境变量")
        print("请设置以下环境变量:")
        print("  COS_SECRET_ID: 腾讯云 SecretId")
        print("  COS_SECRET_KEY: 腾讯云 SecretKey")
        print("  COS_BUCKET_NAME: COS 存储桶名称")
        print("  APP_ID: 腾讯云 AppId")
        print("  COS_REGION: COS 地域 (可选，默认为 ap-guangzhou)")
        return False
    
    if not os.path.exists(file_path):
        print(f"Error: 文件不存在 {file_path}")
        return False
    
    # 如果没有指定对象名称，使用文件名
    if object_name is None:
        object_name = os.path.basename(file_path)
    
    try:
        # 创建 COS 客户端
        cos_client = boto3.client(
            's3',
            aws_access_key_id=secret_id,
            aws_secret_access_key=secret_key,
            region_name=region,
            endpoint_url=f'https://cos.{region}.myqcloud.com'
        )
        
        # 设置上传路径到 firmwares 目录
        if not object_name.startswith('firmwares/'):
            object_name = f"firmwares/{object_name}"
        
        # 使用完整的存储桶名称（包含 AppId）
        full_bucket_name = f"{bucket_name}-{app_id}"
        print(f"使用存储桶名称: {full_bucket_name}")
        
        # 检查文件是否已经存在于 COS 中
        try:
            cos_client.head_object(Bucket=full_bucket_name, Key=object_name)
            print(f"Error: 文件已存在于 COS 中: {object_name}")
            return False
        except ClientError as e:
            
            raise
        
        # 上传文件
        print(f"正在上传 {file_path} 到 COS...")
        print(f"目标路径: {object_name}")
        
        # 设置额外的请求头，包括必需的 Appid 头部
        extra_args = {
            'Metadata': {'Appid': app_id}
        }
        
        cos_client.upload_file(file_path, full_bucket_name, object_name, ExtraArgs=extra_args)
        
        # 生成访问 URL
        url = f"https://{full_bucket_name}.cos.{region}.myqcloud.com/{object_name}"
        print(f"上传成功: {url}")

        
        return True
        
    except ClientError as e:
        print(f"上传失败: {e}")
        return False
    except Exception as e:
        print(f"上传过程中发生错误: {e}")
        return False

def zip_bin(board_type, project_version, flash_command=None):
    if not os.path.exists("releases"):
        os.makedirs("releases")
    output_path = f"releases/v{project_version}_{board_type}.zip"
    if os.path.exists(output_path):
        os.remove(output_path)
    
    # 定义需要复制的文件列表
    files_to_zip = [
        "build/partition_table/partition-table.bin",
        "build/srmodels/srmodels.bin", 
        "build/bootloader/bootloader.bin",
        "build/ota_data_initial.bin",
        "build/xiaozhi.bin",
        "build/xiaozhi.elf",
        "build/xiaozhi.map"
    ]
    
    with zipfile.ZipFile(output_path, 'w', compression=zipfile.ZIP_DEFLATED) as zipf:
        # 添加二进制文件
        for file_path in files_to_zip:
            if os.path.exists(file_path):
                # 获取文件名作为压缩包内的路径
                arcname = os.path.basename(file_path)
                zipf.write(file_path, arcname=arcname)
                print(f"Added {file_path} to zip")
            else:
                print(f"Skipped {file_path} (file not found)")
        
        # 添加烧录脚本
        if flash_command:
            flash_script_content = f"""#!/bin/bash
# Flash script for {board_type}
# Generated automatically

{flash_command}
"""
            zipf.writestr("flash.sh", flash_script_content)
            print("Added flash.sh to zip")
    
    print(f"zip bin to {output_path} done")
    
    # 上传到腾讯云 COS
    if upload_to_cos(output_path):
        print(f"文件已成功上传到腾讯云 COS")
    else:
        print(f"文件上传到腾讯云 COS 失败或文件已存在")
    

def release_current():
    board_type = get_board_type()
    print("board type:", board_type)
    project_version = get_project_version()
    print("project version:", project_version)
    
    # 获取 board_config
    board_configs = get_all_board_types()
    board_config = None
    for config_name, config_board_type in board_configs.items():
        if config_board_type == board_type:
            board_config = config_name
            break
    
    if board_config:
        release(board_type, board_config)
    else:
        print(f"未找到 {board_type} 对应的 board_config")

def get_all_board_types():
    board_configs = {}
    with open("main/CMakeLists.txt", encoding='utf-8') as f:
        lines = f.readlines()
        for i, line in enumerate(lines):
            # 查找 if(CONFIG_BOARD_TYPE_*) 行
            if "if(CONFIG_BOARD_TYPE_" in line:
                config_name = line.strip().split("if(")[1].split(")")[0]
                # 查找下一行的 set(BOARD_TYPE "xxx") 
                next_line = lines[i + 1].strip()
                if next_line.startswith("set(BOARD_TYPE"):
                    board_type = next_line.split('"')[1]
                    board_configs[config_name] = board_type
    return board_configs

def release(board_type, board_config):
    config_path = f"main/boards/{board_type}/config.json"
    if not os.path.exists(config_path):
        print(f"跳过 {board_type} 因为 config.json 不存在")
        return

    # Print Project Version
    project_version = get_project_version()
    print(f"Project Version: {project_version}", config_path)

    with open(config_path, "r") as f:
        config = json.load(f)
    target = config["target"]
    builds = config["builds"]
    
    for build in builds:
        name = build["name"]
        if not name.startswith(board_type):
            raise ValueError(f"name {name} 必须以 {board_type} 开头")
        output_path = f"releases/v{project_version}_{name}.zip"
        if os.path.exists(output_path):
            print(f"文件已存在: {output_path}")
            print("跳过构建，直接上传...")
            # 直接上传已存在的文件
            if upload_to_cos(output_path):
                print(f"文件已成功上传到腾讯云 COS")
            else:
                print(f"文件上传到腾讯云 COS 失败或文件已存在")
            print("-" * 80)
            continue

        sdkconfig_append = [f"{board_config}=y"]
        for append in build.get("sdkconfig_append", []):
            sdkconfig_append.append(append)
        print(f"name: {name}")
        print(f"target: {target}")
        for append in sdkconfig_append:
            print(f"sdkconfig_append: {append}")
        # unset IDF_TARGET
        os.environ.pop("IDF_TARGET", None)
        # Call set-target
        if os.system(f"idf.py set-target {target}") != 0:
            print("set-target failed")
            sys.exit(1)
        # Append sdkconfig
        with open("sdkconfig", "a") as f:
            f.write("\n")
            for append in sdkconfig_append:
                f.write(f"{append}\n")
                # Build with macro BOARD_NAME defined to name and capture output
        build_cmd = f"idf.py -DBOARD_NAME={name} build"
        print(f"Running: {build_cmd}")
        
        # 使用实时输出捕获
        result = subprocess.run(build_cmd, shell=True, capture_output=True, text=True, env=os.environ.copy())
        if result.returncode != 0:
            print("build failed")
            print("STDOUT:", result.stdout)
            print("STDERR:", result.stderr)
            sys.exit(1)
        
        # Extract flash command from build output
        flash_command = None
        
        # 搜索 stdout
        for line in result.stdout.split('\n'):
            if line.strip().startswith('python -m esptool'):
                flash_command = line.strip()
                break
        # Zip bin
        zip_bin(name, project_version, flash_command)
        print("-" * 80)

        

if __name__ == "__main__":
    if len(sys.argv) > 1:
        board_configs = get_all_board_types()
        found = False
        for board_config, board_type in board_configs.items():
            if sys.argv[1] == 'all' or board_type == sys.argv[1]:
                release(board_type, board_config)
                found = True
        if not found:
            print(f"未找到板子类型: {sys.argv[1]}")
            print("可用的板子类型:")
            for board_type in board_configs.values():
                print(f"  {board_type}")
    else:
        release_current()
