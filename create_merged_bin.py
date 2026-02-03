# Create Merged Firmware Binary for KiKi Robot
# ESP32-S3, 16MB Flash

import os
import sys

print("=" * 50)
print("Creating Merged Firmware Binary")
print("=" * 50)

# Change to build directory
build_dir = r"f:\kikichatwiath_ai-fwlenwed\kikichatwiath_ai-master\build"
os.chdir(build_dir)

# Import esptool from ESP-IDF
esptool_path = r"C:\Espressif\python_env\idf5.5_py3.14_env\Lib\site-packages"
if esptool_path not in sys.path:
    sys.path.insert(0, esptool_path)

try:
    import esptool
    
    # Prepare arguments
    args = [
        '--chip', 'esp32s3',
        'merge_bin',
        '-o', 'kiki_merged_flash.bin',
        '--flash_mode', 'dio',
        '--flash_freq', '80m',
        '--flash_size', '16MB',
        '0x0', 'bootloader/bootloader.bin',
        '0x8000', 'partition_table/partition-table.bin',
        '0xd000', 'ota_data_initial.bin',
        '0x20000', 'xiaozhi.bin',
        '0x800000', 'generated_assets.bin'
    ]
    
    print("\nMerging binaries...")
    print(f"Output: {os.path.join(build_dir, 'kiki_merged_flash.bin')}")
    print()
    
    # Run esptool merge_bin
    esptool.main(args)
    
    print("\n" + "=" * 50)
    print("SUCCESS! Merged binary created:")
    print(f"{os.path.join(build_dir, 'kiki_merged_flash.bin')}")
    print("=" * 50)
    print("\nTo flash:")
    print("esptool.py -p COM31 -b 921600 write_flash 0x0 build\\kiki_merged_flash.bin")
    print()
    
    # Show file size
    merged_file = os.path.join(build_dir, 'kiki_merged_flash.bin')
    if os.path.exists(merged_file):
        size_mb = os.path.getsize(merged_file) / (1024 * 1024)
        print(f"File size: {size_mb:.2f} MB")
    
except ImportError as e:
    print(f"\nERROR: Cannot import esptool: {e}")
    print("\nTrying alternative method with subprocess...")
    
    import subprocess
    
    # Use esptool from ESP-IDF python environment
    python_exe = r"C:\Espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe"
    esptool_script = r"C:\Espressif\python_env\idf5.5_py3.14_env\Scripts\esptool.py"
    
    if os.path.exists(python_exe) and os.path.exists(esptool_script):
        cmd = [
            python_exe,
            esptool_script,
            '--chip', 'esp32s3',
            'merge_bin',
            '-o', 'kiki_merged_flash.bin',
            '--flash_mode', 'dio',
            '--flash_freq', '80m',
            '--flash_size', '16MB',
            '0x0', 'bootloader/bootloader.bin',
            '0x8000', 'partition_table/partition-table.bin',
            '0xd000', 'ota_data_initial.bin',
            '0x20000', 'xiaozhi.bin',
            '0x800000', 'generated_assets.bin'
        ]
        
        result = subprocess.run(cmd, capture_output=False)
        
        if result.returncode == 0:
            print("\n" + "=" * 50)
            print("SUCCESS! Merged binary created")
            print("=" * 50)
        else:
            print("\nERROR: Failed to create merged binary")
            sys.exit(1)
    else:
        print(f"\nERROR: Cannot find Python or esptool.py")
        print(f"Python: {python_exe}")
        print(f"esptool: {esptool_script}")
        sys.exit(1)

except Exception as e:
    print(f"\nERROR: {e}")
    import traceback
    traceback.print_exc()
    sys.exit(1)
