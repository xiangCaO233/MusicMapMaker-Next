import os
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

# 配置
OSU_FILES_DIR = "/mnt/shareddata/osu/files"
TEST_EXE = "./build/bin/OSUConsistencyTest"
OUTPUT_DIR = "./build/test_output/bulk_test"
MAX_WORKERS = 8 # 根据CPU核心数调整

def is_mania_map(file_path):
    try:
        with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
            for _ in range(100): # 检查前100行通常足够
                line = f.readline()
                if not line: break
                if "Mode: 3" in line:
                    return True
                if "[HitObjects]" in line: # 如果到了物件区还没看到Mode，基本不是Mania
                    break
    except:
        pass
    return False

def run_test(file_path):
    file_name = os.path.basename(file_path)
    output_path = os.path.join(OUTPUT_DIR, f"{file_name}_export.osu")
    
    try:
        result = subprocess.run(
            [TEST_EXE, file_path, output_path],
            capture_output=True,
            text=True,
            timeout=10 # 防止死循环或超长文件挂死
        )
        if result.returncode == 0:
            return True, file_path, ""
        else:
            return False, file_path, result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        return False, file_path, "Timeout"
    except Exception as e:
        return False, file_path, str(e)

def main():
    if not os.path.exists(OUTPUT_DIR):
        os.makedirs(OUTPUT_DIR)

    print(f"Scanning {OSU_FILES_DIR} for Mania maps...")
    mania_maps = []
    for root, dirs, files in os.walk(OSU_FILES_DIR):
        for f in files:
            full_path = os.path.join(root, f)
            if is_mania_map(full_path):
                mania_maps.append(full_path)
    
    total_found = len(mania_maps)
    print(f"Found {total_found} Mania maps. Starting tests with {MAX_WORKERS} workers...")

    results = []
    with ProcessPoolExecutor(max_workers=MAX_WORKERS) as executor:
        future_to_path = {executor.submit(run_test, path): path for path in mania_maps}
        
        count = 0
        success_count = 0
        for future in future_to_path:
            success, path, error = future.result()
            count += 1
            if success:
                success_count += 1
            else:
                print(f"\n[FAIL] {path}")
                # print(error)
            
            if count % 100 == 0:
                print(f"Progress: {count}/{total_found} (Success: {success_count})", end='\r')

    print(f"\n\nFinal Result: {success_count}/{total_found} passed.")
    
    if success_count < total_found:
        print("Some maps failed. Check the output above for details.")
        sys.exit(1)
    else:
        print("All Mania maps passed!")
        sys.exit(0)

if __name__ == "__main__":
    main()
