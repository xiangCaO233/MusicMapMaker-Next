import os
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

# 配置
IMD_FILES_DIR = "/home/xiang/Documents/MusicMapRepo/rm"
TEST_EXE = "./build/bin/IMDConsistencyTest"
OUTPUT_DIR = "./build/test_output/bulk_imd_test"
MAX_WORKERS = 8

def run_test(file_path):
    file_name = os.path.basename(file_path)
    file_name = os.path.basename(file_path)
    output_path = os.path.join(OUTPUT_DIR, file_name)
    
    try:
        result = subprocess.run(
            [TEST_EXE, file_path, output_path],
            capture_output=True,
            text=True,
            timeout=10
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

    print(f"Scanning {IMD_FILES_DIR} for imd maps...")
    imd_maps = []
    for root, dirs, files in os.walk(IMD_FILES_DIR):
        for f in files:
            if f.endswith(".imd"):
                full_path = os.path.join(root, f)
                imd_maps.append(full_path)
    
    total_found = len(imd_maps)
    print(f"Found {total_found} imd maps. Starting tests with {MAX_WORKERS} workers...")

    results = []
    with ProcessPoolExecutor(max_workers=MAX_WORKERS) as executor:
        future_to_path = {executor.submit(run_test, path): path for path in imd_maps}
        
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
        sys.exit(1)
    else:
        print("All imd maps passed!")
        sys.exit(0)

if __name__ == "__main__":
    main()
