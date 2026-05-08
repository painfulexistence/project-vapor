#!/usr/bin/env python3
import os
import subprocess
import sys
import glob
import multiprocessing
import shutil

def find_source_files(root_dir):
    extensions = ('*.cpp', '*.hpp', '*.c', '*.h', '*.mm', '*.m')
    files = []
    # 這裡可以自訂要掃描的目錄，排除掉 third-party 等不需要 format 的目錄
    search_dirs = ['Vapor/src', 'Vapor/include', 'Vaporware/src', 'Vaporware/include', 'tests']
    
    for search_dir in search_dirs:
        dir_path = os.path.join(root_dir, search_dir)
        if not os.path.exists(dir_path):
            continue
        for ext in extensions:
            files.extend(glob.glob(os.path.join(dir_path, '**', ext), recursive=True))
    return files

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)
    os.chdir(project_root)

    source_files = find_source_files(project_root)
    if not source_files:
        print("No source files found!")
        sys.exit(0)

    print("========================================")
    print("      Running run-clang-tidy            ")
    print("========================================")

    # Find run-clang-tidy
    clang_tidy_exe = shutil.which('run-clang-tidy')
    if not clang_tidy_exe:
        # Check homebrew default path for macOS
        brew_path = '/opt/homebrew/opt/llvm/bin/run-clang-tidy'
        if os.path.exists(brew_path):
            clang_tidy_exe = brew_path
        else:
            print("run-clang-tidy not found in PATH or Homebrew. Please install llvm (e.g. brew install llvm).")
            sys.exit(1)

    if not os.path.exists('build/compile_commands.json'):
        print("Error: build/compile_commands.json not found.")
        print("Please run CMake configuration first (e.g., cmake --preset dev).")
        sys.exit(1)

    cores = multiprocessing.cpu_count()
    cmd = [clang_tidy_exe, '-p', 'build', '-fix', '-j', str(cores)] + source_files
    
    print(f"Executing: {' '.join(cmd)}")
    try:
        result = subprocess.run(cmd, check=False)
        if result.returncode == 0:
            print("run-clang-tidy finished successfully.\n")
        else:
            print(f"run-clang-tidy finished with warnings/errors (Exit code: {result.returncode}).\n")
    except Exception as e:
        print(f"\nError running clang-tidy: {e}")
        sys.exit(1)

    print("========================================")
    print("      Running clang-format              ")
    print("========================================")
    
    # Check for clang-format
    clang_format_exe = 'clang-format'
    try:
        subprocess.run([clang_format_exe, '--version'], check=True, capture_output=True)
        
        print(f"Formatting {len(source_files)} files...")
        # 分批執行避免指令長度過長
        chunk_size = 50
        for i in range(0, len(source_files), chunk_size):
            chunk = source_files[i:i+chunk_size]
            subprocess.run([clang_format_exe, '-i'] + chunk, check=True)
        print("clang-format finished successfully.\n")
        
    except FileNotFoundError:
        print("Warning: clang-format not found in PATH. Skipping formatting.")

    print("All checks and fixes completed successfully!")

if __name__ == '__main__':
    main()
