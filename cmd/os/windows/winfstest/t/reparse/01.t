#!/usr/bin/python

# reparse points (directories)

from winfstest import *

name = uniqname()

expect("CreateDirectory %s 0" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x10)
expect("SetReparsePoint %s 42 {92A23BD8-99F5-4FD6-807C-C56F3A063C52} 1A 2B 3C 4D 5F" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x410)
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
expect("RemoveDirectory %s" % name, 0)

expect("CreateDirectory %s 0" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x10)
expect("SetReparsePoint %s 42 {92A23BD8-99F5-4FD6-807C-C56F3A063C52} 1A 2B 3C 4D 5F" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x410)
expect("DeleteReparsePoint %s 43 {92A23BD8-99F5-4FD6-807C-C56F3A063C52}" % name, "ERROR(4394)")
expect("DeleteReparsePoint %s 42 {02A23BD8-99F5-4FD6-807C-C56F3A063C52}" % name, "ERROR(4391)")
expect("DeleteReparsePoint %s 42 {92A23BD8-99F5-4FD6-807C-C56F3A063C52}" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x10)
expect("RemoveDirectory %s" % name, 0)

expect("CreateDirectory %s 0" % name, 0)
expect("CreateFile %s\\1 GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x10)
expect("SetReparsePoint %s 42 {92A23BD8-99F5-4FD6-807C-C56F3A063C52} 1A 2B 3C 4D 5F" % name, "ERROR_DIR_NOT_EMPTY")
expect("DeleteFile %s\\1" % name, 0)
expect("SetReparsePoint %s 42 {92A23BD8-99F5-4FD6-807C-C56F3A063C52} 1A 2B 3C 4D 5F" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x410)
expect("GetReparsePoint %s" % name, lambda r:\
    r[0]["ReparseTag"] == 42 and \
    r[0]["ReparseDataLength"] == 5 and \
    r[0]["ReparseGuid"] == "{92A23BD8-99F5-4FD6-807C-C56F3A063C52}" and \
    r[0]["ReparseData"] == "1A 2B 3C 4D 5F")
expect("CreateFile %s\\2 GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, "ERROR_CANT_ACCESS_FILE")
expect("RemoveDirectory %s" % name, 0)

expect("CreateDirectory %s 0" % name, 0)
expect("SetReparsePoint %s 42 {92A23BD8-99F5-4FD6-807C-C56F3A063C52} 1A 2B 3C 4D 5F" % name, 0)
expect("CreateDirectory %s 0" % name, "ERROR_ALREADY_EXISTS")
expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, "ERROR_ACCESS_DENIED")
expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL+FILE_FLAG_OPEN_REPARSE_POINT 0" % name, "ERROR_ACCESS_DENIED")
expect("RemoveDirectory %s" % name, 0)

testdone()
