import os
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

# 配置
MALODY_FILES_DIR = "/home/xiang/.steam/steam/steamapps/common/MalodyV/chart"
TEST_EXE = "./build/bin/MalodyConsistencyTest"
OUTPUT_DIR = "./build/test_output/bulk_malody_test"
MAX_WORKERS = 8

def run_test(file_path):
    file_name = os.path.basename(file_path)
    # 使用相对路径或简化名称以避免输出路径过长
    safe_name = "".join([c if c.isalnum() else "_" for c in file_name])
    output_path = os.path.join(OUTPUT_DIR, f"{safe_name}_export.mc")
    
    try:
        result = subprocess.run(
            [TEST_EXE, file_path, output_path],
            capture_output=True,
            text=True,
            timeout=30
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

    if not os.path.exists(MALODY_FILES_DIR):
        print(f"Error: Malody charts directory not found at {MALODY_FILES_DIR}")
        sys.exit(1)

    if not os.path.exists(TEST_EXE):
        print(f"Error: Test executable not found at {TEST_EXE}. Please build the project first.")
        sys.exit(1)

    print(f"Scanning {MALODY_FILES_DIR} for Malody (.mc) maps...")
    malody_maps = []
    for root, dirs, files in os.walk(MALODY_FILES_DIR):
        for f in files:
            if f.endswith(".mc"):
                full_path = os.path.join(root, f)
                malody_maps.append(full_path)
    
    total_found = len(malody_maps)
    if total_found == 0:
        print("No .mc files found.")
        return

    print(f"Found {total_found} .mc maps. Starting tests with {MAX_WORKERS} workers...")

    results = []
    with ProcessPoolExecutor(max_workers=MAX_WORKERS) as executor:
        future_to_path = {executor.submit(run_test, path): path for path in malody_maps}
        
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
            
            if count % 10 == 0 or count == total_found:
                print(f"Progress: {count}/{total_found} (Success: {success_count})", end='\r')

    print(f"\n\nFinal Result: {success_count}/{total_found} passed.")
    
    if success_count < total_found:
        print("Some maps failed. Check the output above for details.")
        # sys.exit(1) # 不一定非要退出，可能只是部分谱面不符合规范
    else:
        print("All Malody maps passed!")
        sys.exit(0)

if __name__ == "__main__":
    main()
