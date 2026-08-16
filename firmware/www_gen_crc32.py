import os
import re
import zlib


def gen_crc32(filename, chunksize=65536):
    """Compute the CRC-32 checksum of the contents of the given filename"""
    with open(filename, "rb") as f:
        checksum = 0
        while (chunk := f.read(chunksize)):
            checksum = zlib.crc32(chunk, checksum)
        return checksum


data_path = r'www'
file_count = 0

for path in os.listdir(data_path):
    # check if current path is a file
    if os.path.isfile(os.path.join(data_path, path)):
        filepath = os.path.join(data_path, path)
        if not filepath.endswith(".crc"):
            print(f"Generating CRC for {filepath}")
            file_count += 1
            with open(filepath+".crc", "w") as file:
                file.write(f'{gen_crc32(filepath):08x}')

# Add ETAG_CACHE_MAX definition to main/CMakeLists.txt
cmake_file = "main/CMakeLists.txt"
if os.path.exists(cmake_file):
    with open(cmake_file, "r") as f:
        content = f.read()

    if "ETAG_CACHE_MAX" in content:
        content = re.sub(r'ETAG_CACHE_MAX=\d+',
                         f'ETAG_CACHE_MAX={file_count}', content)
    else:
        content += f"\nset(ETAG_CACHE_MAX {file_count})\n"

    with open(cmake_file, "w") as f:
        f.write(content)

    print(f"Updated main/CMakeLists.txt with ETAG_CACHE_MAX={file_count}")
else:
    print(f"main/CMakeLists.txt not found in current directory")
