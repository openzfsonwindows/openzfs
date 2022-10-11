#!/usr/bin/python

# CreateFile FlagsAndAttributes
# DeleteFile

from winfstest import *

name = uniqname()

expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_ALWAYS FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x20)
expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_ALWAYS FILE_ATTRIBUTE_READONLY 0" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x21)
expect("SetFileAttributes %s FILE_ATTRIBUTE_NORMAL" % name, 0)
expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_ALWAYS FILE_ATTRIBUTE_SYSTEM 0" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x24)
expect("SetFileAttributes %s FILE_ATTRIBUTE_NORMAL" % name, 0)
expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_ALWAYS FILE_ATTRIBUTE_HIDDEN 0" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x22)
expect("DeleteFile %s" % name, 0)

expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_ALWAYS FILE_ATTRIBUTE_READONLY 0" % name, 0)
expect("DeleteFile %s" % name, "ERROR_ACCESS_DENIED")
expect("SetFileAttributes %s FILE_ATTRIBUTE_NORMAL" % name, 0)
expect("DeleteFile %s" % name, 0)

testdone()
