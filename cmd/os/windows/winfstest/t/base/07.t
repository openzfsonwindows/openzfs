#!/usr/bin/python

# SetFileAttributes

from winfstest import *

name = uniqname()

expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_ALWAYS FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x20)
expect("SetFileAttributes %s FILE_ATTRIBUTE_READONLY" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x1)
expect("SetFileAttributes %s FILE_ATTRIBUTE_SYSTEM" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x4)
expect("SetFileAttributes %s FILE_ATTRIBUTE_HIDDEN" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x2)
expect("SetFileAttributes %s FILE_ATTRIBUTE_ARCHIVE" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x20)
expect("DeleteFile %s" % name, 0)

testdone()
