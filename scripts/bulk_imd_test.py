import os
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

# 本脚本对外部 IMD 谱面仓库批量运行一致性测试，不属于常规 CTest。
# 输入目录只读，所有导出产物统一写入构建树下的隔离输出目录。
# 固定进程数与单文件超时用于控制损坏或极端谱面对主机的影响。

# 本机诊断配置需要按实际谱面仓库位置调整。
IMD_FILES_DIR = "/home/xiang/Documents/MusicMapRepo/rm"
# 测试程序接收输入谱面和导出目标两个位置参数。
TEST_EXE = "./build/bin/IMDConsistencyTest"
# 同名输入只取 basename，调用者应确保仓库中不存在需要区分的冲突文件。
OUTPUT_DIR = "./build/test_output/bulk_imd_test"
# 多进程隔离单个解析器崩溃并提升大规模语料吞吐。
MAX_WORKERS = 8

# 执行单个 IMD 输入并返回成功标志、原路径和诊断文本。
# 返回元组保持可序列化，便于 ProcessPoolExecutor 跨进程传递。
def run_test(file_path):
    # 导出文件位于专用输出根，绝不覆盖原始谱面。
    file_name = os.path.basename(file_path)
    # 保留现有重复归一化步骤，不在纯注释任务中调整行为。
    file_name = os.path.basename(file_path)
    output_path = os.path.join(OUTPUT_DIR, file_name)
    
    try:
        # 捕获子进程输出，仅在失败时由汇总端决定是否显示。
        result = subprocess.run(
            [TEST_EXE, file_path, output_path],
            capture_output=True,
            text=True,
            timeout=10
        )
        if result.returncode == 0:
            # 空错误文本减少父进程成功结果的序列化负担。
            return True, file_path, ""
        else:
            # stdout 与 stderr 合并保存，兼容测试程序两种诊断通道。
            return False, file_path, result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        # 超时视为该谱面失败，但不终止其余批量任务。
        return False, file_path, "Timeout"
    except Exception as e:
        # 文件系统或进程创建错误按输入粒度回传。
        return False, file_path, str(e)

# 扫描全部 IMD 文件、并行运行测试并汇总退出状态。
def main():
    # 输出目录可重复使用，测试程序负责覆盖对应导出文件。
    if not os.path.exists(OUTPUT_DIR):
        os.makedirs(OUTPUT_DIR)

    print(f"Scanning {IMD_FILES_DIR} for imd maps...")
    imd_maps = []
    # os.walk 递归覆盖仓库所有子目录，不跟随额外筛选清单。
    for root, dirs, files in os.walk(IMD_FILES_DIR):
        for f in files:
            if f.endswith(".imd"):
                # 保存完整输入路径，避免子进程依赖扫描时的当前目录。
                full_path = os.path.join(root, f)
                imd_maps.append(full_path)
    
    total_found = len(imd_maps)
    print(f"Found {total_found} imd maps. Starting tests with {MAX_WORKERS} workers...")

    results = []
    # executor 生命周期包围全部 future，退出时会回收工作进程。
    with ProcessPoolExecutor(max_workers=MAX_WORKERS) as executor:
        # 映射保留路径便于未来诊断，当前结果本身也携带原路径。
        future_to_path = {executor.submit(run_test, path): path for path in imd_maps}
        
        count = 0
        success_count = 0
        for future in future_to_path:
            # 按提交顺序等待，不改变最终成功计数语义。
            success, path, error = future.result()
            count += 1
            if success:
                success_count += 1
            else:
                # 失败路径立即换行打印，避免被回车式进度信息覆盖。
                print(f"\n[FAIL] {path}")
                # print(error)
            
            if count % 10 == 0 or count == total_found:
                # 小批量间隔兼顾交互反馈与终端输出成本。
                print(f"Progress: {count}/{total_found} (Success: {success_count})", end='\r')

    print(f"\n\nFinal Result: {success_count}/{total_found} passed.")
    
    if success_count < total_found:
        # 任一失败令脚本返回非零，便于外层批处理识别回归。
        print("Some maps failed. Check the output above for details.")
        sys.exit(1)
    else:
        # 全部通过时显式返回零，而不是依赖解释器自然退出。
        print("All imd maps passed!")
        sys.exit(0)

if __name__ == "__main__":
    # 导入模块时不扫描用户的大型谱面目录。
    main()
