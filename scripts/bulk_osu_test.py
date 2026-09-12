import os
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

# 本脚本从外部 osu! 曲库筛选 Mania 谱面，并批量执行往返一致性测试。
# 扫描阶段只读输入，测试导出统一写入构建树，禁止覆盖用户曲库。
# 每个子进程拥有超时边界，单文件异常不会阻断其余语料。

# 外部曲库位置属于本机诊断配置。
OSU_FILES_DIR = "/mnt/shareddata/osu/files"
# 测试程序接受原谱面与导出谱面路径。
TEST_EXE = "./build/bin/OSUConsistencyTest"
# 输出目录与常规测试产物分离，便于批量清理。
OUTPUT_DIR = "./build/test_output/bulk_test"
# 并发数按主机 CPU 与磁盘能力人工调整。
MAX_WORKERS = 8 # 根据CPU核心数调整

# 只读取文件头部识别 Mode: 3，避免完整解析所有候选文件。
def is_mania_map(file_path):
    try:
        # 无效字节以 ignore 处理，因为这里只查找 ASCII 元数据标记。
        with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
            # 模式字段位于文件前部，限制行数控制大曲库扫描成本。
            for _ in range(100): # 检查前100行通常足够
                line = f.readline()
                # EOF 前未发现 Mode: 3 即判定为非 Mania。
                if not line: break
                if "Mode: 3" in line:
                    return True
                if "[HitObjects]" in line: # 如果到了物件区还没看到Mode，基本不是Mania
                    # 物件区之后不再出现通用模式元数据。
                    break
    except:
        # 无法读取的候选文件按非 Mania 跳过，扫描继续。
        pass
    return False

# 执行单个 Mania 谱面测试并返回可跨进程序列化的结果。
def run_test(file_path):
    # 输出名追加 export 后缀，避免与测试输入含义混淆。
    file_name = os.path.basename(file_path)
    output_path = os.path.join(OUTPUT_DIR, f"{file_name}_export.osu")
    
    try:
        # 捕获输出避免并行子进程直接争用终端。
        result = subprocess.run(
            [TEST_EXE, file_path, output_path],
            capture_output=True,
            text=True,
            timeout=10 # 防止死循环或超长文件挂死
        )
        if result.returncode == 0:
            # 成功结果不携带冗余日志。
            return True, file_path, ""
        else:
            # 保留两路诊断供需要时取消下方注释打印。
            return False, file_path, result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        # 超时通常表示解析器卡住或输入异常，不停止整个批次。
        return False, file_path, "Timeout"
    except Exception as e:
        # 进程启动和文件错误也按单谱面失败记录。
        return False, file_path, str(e)

# 扫描 Mania 输入、并行运行一致性测试并设置批处理退出码。
def main():
    # 输出目录只在缺失时创建，已有诊断文件允许被测试程序更新。
    if not os.path.exists(OUTPUT_DIR):
        os.makedirs(OUTPUT_DIR)

    print(f"Scanning {OSU_FILES_DIR} for Mania maps...")
    mania_maps = []
    # 曲库可能多层嵌套，使用递归 walk 遍历所有普通文件。
    for root, dirs, files in os.walk(OSU_FILES_DIR):
        for f in files:
            full_path = os.path.join(root, f)
            # 先筛选模式，避免把其他 osu! 游戏模式送入一致性测试。
            if is_mania_map(full_path):
                mania_maps.append(full_path)
    
    total_found = len(mania_maps)
    print(f"Found {total_found} Mania maps. Starting tests with {MAX_WORKERS} workers...")

    results = []
    # ProcessPoolExecutor 隔离 C++ 测试进程调度并利用多核主机。
    with ProcessPoolExecutor(max_workers=MAX_WORKERS) as executor:
        future_to_path = {executor.submit(run_test, path): path for path in mania_maps}
        
        count = 0
        success_count = 0
        for future in future_to_path:
            # future.result 将子任务异常统一带回汇总进程。
            success, path, error = future.result()
            count += 1
            if success:
                success_count += 1
            else:
                # 失败路径即时输出，详细诊断默认保持安静。
                print(f"\n[FAIL] {path}")
                # print(error)
            
            if count % 100 == 0:
                # 大曲库每百项刷新一次，减少终端 I/O 对吞吐的影响。
                print(f"Progress: {count}/{total_found} (Success: {success_count})", end='\r')

    print(f"\n\nFinal Result: {success_count}/{total_found} passed.")
    
    if success_count < total_found:
        # 任一谱面失败向外层自动化返回非零状态。
        print("Some maps failed. Check the output above for details.")
        sys.exit(1)
    else:
        # 全部候选通过时显式成功退出。
        print("All Mania maps passed!")
        sys.exit(0)

if __name__ == "__main__":
    # 导入时不触发外部曲库扫描。
    main()
