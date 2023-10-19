# https://apps.microsoft.com/store/detail/python-39/9P7QFQMJRFP7
# https://download.microsoft.com/download/7/9/6/
# 7962e9ce-cd69-4574-978c-1202654bd729/windowssdk/
# Installers/X64 Debuggers And Tools-x64_en-us.msi

import re
import subprocess
import pathlib
import os

pf64 = pathlib.WindowsPath(os.environ["ProgramFiles"])
pf86 = pathlib.WindowsPath(os.environ["ProgramFiles(x86)"])


def find_first_existing_file(file_paths):
    for file_path in file_paths:
        if file_path.exists():
            return file_path
    return None  # Return None if no file is found


# List of file paths to check
cdb_file_paths_to_check = [
    pf64 / "Windows Kits" / "10" / "Debuggers" / "x64" / "cdb.exe",
    pf86 / "Windows Kits" / "10" / "Debuggers" / "x64" / "cdb.exe",
    pf64 / "Windows Kits" / "10" / "Debuggers" / "x86" / "cdb.exe",
    pf86 / "Windows Kits" / "10" / "Debuggers" / "x86" / "cdb.exe"
]

cdb = find_first_existing_file(cdb_file_paths_to_check)

if cdb:
    print(cdb)
else:
    print("cdb not found.")
    exit()

dumpfilestr = "C:\\Windows\\MEMORY.DMP"
symbolstr = "srv*;C:\\Program Files\\OpenZFS On Windows\\symbols\\;"


def run(arg):
    result = subprocess.run(
        [str(cdb), "-z", dumpfilestr, "-c", arg, "-y", symbolstr],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return re.search(
        r"Reading initial command[\s\S]+quit:", result.stdout.decode()
    ).group()


analyze = run("!analyze -v ; q")

stack = run("k ; q")

cbuf = run(".writemem C:\\cbuf.txt poi(OpenZFS!cbuf) L100000 ; q")

with open("C:\\stack.txt", "w") as file:
    file.write(analyze)
    file.write("\n")
    file.write(stack)


print("Please upload C:\\stack.txt and C:\\cbuf.txt "
      "when creating an issue on GitHub")
