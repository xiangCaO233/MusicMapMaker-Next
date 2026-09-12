import os
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

# 本脚本对本机 Malody 曲库进行离线兼容性抽样，不进入默认测试套件。
# 原始 .mc 文件保持只读，导出结果集中写入构建树测试输出目录。
# 单文件故障通过返回元组汇总，不中止其他并行谱面。

# 本机曲库路径可按实际 Steam 安装位置调整。
MALODY_FILES_DIR = "/home/xiang/.steam/steam/steamapps/common/MalodyV/chart"
# 一致性测试程序负责读取、导出并比较 Malody 数据。
TEST_EXE = "./build/bin/MalodyConsistencyTest"
# 导出文件只用于诊断，不应写回曲库。
OUTPUT_DIR = "./build/test_output/bulk_malody_test"
# 固定进程池上限避免同时解码过多大型谱面。
MAX_WORKERS = 8

# 执行一个 Malody 文件并返回父进程可汇总的轻量结果。
def run_test(file_path):
    # 输出名去除目录，随后替换潜在路径或 shell 特殊字符。
    file_name = os.path.basename(file_path)
    # 使用相对路径或简化名称以避免输出路径过长
    safe_name = "".join([c if c.isalnum() else "_" for c in file_name])
    # 添加 export 后缀，避免与输入文件产生名称歧义。
    output_path = os.path.join(OUTPUT_DIR, f"{safe_name}_export.mc")
    
    try:
        # 捕获输出避免多个子进程并发写终端造成日志交错。
        result = subprocess.run(
            [TEST_EXE, file_path, output_path],
            capture_output=True,
            text=True,
            timeout=30
        )
        if result.returncode == 0:
            # 成功结果无需传回子进程完整输出。
            return True, file_path, ""
        else:
            # 合并两个输出通道以保留测试程序诊断。
            return False, file_path, result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        # 30 秒上限用于隔离解析死循环或异常超大输入。
        return False, file_path, "Timeout"
    except Exception as e:
        # 单文件异常降级为失败记录，批处理继续推进。
        return False, file_path, str(e)

# 校验本机前置条件、扫描曲库并汇总全部子进程结果。
def main():
    if not os.path.exists(OUTPUT_DIR):
        os.makedirs(OUTPUT_DIR)

    if not os.path.exists(MALODY_FILES_DIR):
        # 输入根缺失意味着本次诊断没有有效语料，应返回失败。
        print(f"Error: Malody charts directory not found at {MALODY_FILES_DIR}")
        sys.exit(1)

    if not os.path.exists(TEST_EXE):
        # 避免为每个输入重复报告相同的可执行文件缺失错误。
        print(f"Error: Test executable not found at {TEST_EXE}. Please build the project first.")
        sys.exit(1)

    print(f"Scanning {MALODY_FILES_DIR} for Malody (.mc) maps...")
    malody_maps = []
    # 递归扫描所有子目录，扩展名严格匹配 Malody .mc。
    for root, dirs, files in os.walk(MALODY_FILES_DIR):
        for f in files:
            if f.endswith(".mc"):
                full_path = os.path.join(root, f)
                malody_maps.append(full_path)
    
    total_found = len(malody_maps)
    if total_found == 0:
        # 空曲库不是解析失败，保持自然零退出。
        print("No .mc files found.")
        return

    print(f"Found {total_found} .mc maps. Starting tests with {MAX_WORKERS} workers...")

    results = []
    # future 在上下文结束前全部完成，工作进程由 executor 自动回收。
    with ProcessPoolExecutor(max_workers=MAX_WORKERS) as executor:
        future_to_path = {executor.submit(run_test, path): path for path in malody_maps}
        
        count = 0
        success_count = 0
        for future in future_to_path:
            # 按提交次序读取结果，计数与扫描列表保持可比。
            success, path, error = future.result()
            count += 1
            if success:
                success_count += 1
            else:
                # 默认只显示失败输入路径，详细输出保留在变量中供临时诊断。
                print(f"\n[FAIL] {path}")
                # print(error)
            
            if count % 10 == 0 or count == total_found:
                # 最后一项强制刷新，确保不足十个文件也显示最终进度。
                print(f"Progress: {count}/{total_found} (Success: {success_count})", end='\r')

    print(f"\n\nFinal Result: {success_count}/{total_found} passed.")
    
    if success_count < total_found:
        # 当前脚本把部分历史兼容失败视为诊断结果，不强制非零退出。
        print("Some maps failed. Check the output above for details.")
        # sys.exit(1) # 不一定非要退出，可能只是部分谱面不符合规范
    else:
        # 全部通过时显式返回成功状态。
        print("All Malody maps passed!")
        sys.exit(0)

if __name__ == "__main__":
    # 仅直接执行时访问用户曲库。
    main()
