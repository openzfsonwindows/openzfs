#!/usr/bin/python

# reparse points (files)

from winfstest import *

name = uniqname()

expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x20)
expect("SetReparsePoint %s 42 {92A23BD8-99F5-4FD6-807C-C56F3A063C52} 1A 2B 3C 4D 5F" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x420)
expect("GetReparsePoint %s" % name, lambda r:\
    r[0]["ReparseTag"] == 42 and \
    r[0]["ReparseDataLength"] == 5 and \
    r[0]["ReparseGuid"] == "{92A23BD8-99F5-4FD6-807C-C56F3A063C52}" and \
    r[0]["ReparseData"] == "1A 2B 3C 4D 5F")
expect("SetReparsePoint %s 42 {92A23BD8-99F5-4FD6-807C-C56F3A063C52} A1 B2 C3 D4 F5 1A 2B 3C 4D 5F" % name, 0)
expect("GetReparsePoint %s" % name, lambda r:\
    r[0]["ReparseTag"] == 42 and \
    r[0]["ReparseDataLength"] == 10 and \
    r[0]["ReparseGuid"] == "{92A23BD8-99F5-4FD6-807C-C56F3A063C52}" and \
    r[0]["ReparseData"] == "A1 B2 C3 D4 F5 1A 2B 3C 4D 5F")
expect("SetReparsePoint %s 43 {92A23BD8-99F5-4FD6-807C-C56F3A063C52} A1 B2 C3 D4 F5 1A 2B 3C 4D 5F" % name, "ERROR(4394)")
expect("SetReparsePoint %s 42 {02A23BD8-99F5-4FD6-807C-C56F3A063C52} A1 B2 C3 D4 F5 1A 2B 3C 4D 5F" % name, "ERROR(4391)")
expect("DeleteFile %s" % name, 0)

expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x20)
expect("SetReparsePoint %s 42 {92A23BD8-99F5-4FD6-807C-C56F3A063C52} 1A 2B 3C 4D 5F" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x420)
expect("DeleteReparsePoint %s 43 {92A23BD8-99F5-4FD6-807C-C56F3A063C52}" % name, "ERROR(4394)")
expect("DeleteReparsePoint %s 42 {02A23BD8-99F5-4FD6-807C-C56F3A063C52}" % name, "ERROR(4391)")
expect("DeleteReparsePoint %s 42 {92A23BD8-99F5-4FD6-807C-C56F3A063C52}" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x20)
expect("DeleteFile %s" % name, 0)

expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("WriteFile %s 0 41 42 43 44 45" % name, 0)
expect("ReadFile %s 0 10" % name, lambda r: r[0]["Length"] == 5 and r[0]["Data"] == "41 42 43 44 45")
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x20)
expect("SetReparsePoint %s 42 {92A23BD8-99F5-4FD6-807C-C56F3A063C52} 1A 2B 3C 4D 5F" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x420)
expect("WriteFile %s 0 61 62 63" % name, 0)
expect("ReadFile %s 0 10" % name, lambda r: r[0]["Length"] == 5 and r[0]["Data"] == "61 62 63 44 45")
expect("SetEndOfFile %s 2" % name, 0)
expect("ReadFile %s 0 10" % name, lambda r: r[0]["Length"] == 2 and r[0]["Data"] == "61 62")
expect("SetEndOfFile %s 5" % name, 0)
expect("ReadFile %s 0 10" % name, lambda r: r[0]["Length"] == 5 and r[0]["Data"] == "61 62 00 00 00")
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x420)
expect("GetReparsePoint %s" % name, lambda r:\
    r[0]["ReparseTag"] == 42 and \
    r[0]["ReparseDataLength"] == 5 and \
    r[0]["ReparseGuid"] == "{92A23BD8-99F5-4FD6-807C-C56F3A063C52}" and \
    r[0]["ReparseData"] == "1A 2B 3C 4D 5F")
expect("DeleteFile %s" % name, 0)

expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("SetReparsePoint %s 42 {92A23BD8-99F5-4FD6-807C-C56F3A063C52} 1A 2B 3C 4D 5F" % name, 0)
expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, "ERROR_CANT_ACCESS_FILE")
expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL+FILE_FLAG_OPEN_REPARSE_POINT 0" % name, "ERROR_FILE_EXISTS")
expect("DeleteFile %s" % name, 0)

testdone()
