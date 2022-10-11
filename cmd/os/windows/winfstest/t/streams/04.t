#!/usr/bin/python

# reparse points (streams)

from winfstest import *

name = uniqname()
srcname, dstname = uniqname(), uniqname()

expect("CreateFile %s:strm GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x20)
expect("SetReparsePoint %s:strm 42 {92A23BD8-99F5-4FD6-807C-C56F3A063C52} 1A 2B 3C 4D 5F" % name, 0)
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x420)
expect("GetReparsePoint %s" % name, lambda r:\
    r[0]["ReparseTag"] == 42 and \
    r[0]["ReparseDataLength"] == 5 and \
    r[0]["ReparseGuid"] == "{92A23BD8-99F5-4FD6-807C-C56F3A063C52}" and \
    r[0]["ReparseData"] == "1A 2B 3C 4D 5F")
expect("SetReparsePoint %s 42 {92A23BD8-99F5-4FD6-807C-C56F3A063C52} A1 B2 C3 D4 F5 1A 2B 3C 4D 5F" % name, 0)
expect("GetReparsePoint %s:strm" % name, lambda r:\
    r[0]["ReparseTag"] == 42 and \
    r[0]["ReparseDataLength"] == 10 and \
    r[0]["ReparseGuid"] == "{92A23BD8-99F5-4FD6-807C-C56F3A063C52}" and \
    r[0]["ReparseData"] == "A1 B2 C3 D4 F5 1A 2B 3C 4D 5F")
expect("DeleteFile %s" % name, 0)

e, r = expect("CreateSymbolicLink %s %s 0" % (srcname, dstname), 0)
if e == "0":
    expect("CreateFile %s:strm GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % srcname, 0)
    expect("CreateFile %s:strm GENERIC_WRITE 0 0 OPEN_EXISTING FILE_ATTRIBUTE_NORMAL 0" % srcname, 0)
    expect("CreateFile %s:strm GENERIC_WRITE 0 0 OPEN_EXISTING FILE_ATTRIBUTE_NORMAL 0" % dstname, 0)
    expect("CreateFile %s:symstrm GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL+FILE_FLAG_OPEN_REPARSE_POINT 0" % srcname, 0)
    expect("CreateFile %s:symstrm GENERIC_WRITE 0 0 OPEN_EXISTING FILE_ATTRIBUTE_NORMAL+FILE_FLAG_OPEN_REPARSE_POINT 0" % srcname, 0)
    expect("CreateFile %s:symstrm GENERIC_WRITE 0 0 OPEN_EXISTING FILE_ATTRIBUTE_NORMAL 0" % srcname, "ERROR_FILE_NOT_FOUND")
    expect("CreateFile %s:symstrm GENERIC_WRITE 0 0 OPEN_EXISTING FILE_ATTRIBUTE_NORMAL 0" % dstname, "ERROR_FILE_NOT_FOUND")
    expect("DeleteFile %s" % dstname, 0)
    expect("DeleteFile %s" % srcname, 0)

    expect("CreateSymbolicLink %s:strm %s 0" % (srcname, dstname), 0)
    expect("GetFileInformation %s" % srcname, lambda r: r[0]["FileAttributes"] == 0x420)
    expect("GetReparsePoint %s" % srcname, lambda r:\
        r[0]["ReparseTag"] == "IO_REPARSE_TAG_SYMLINK" and \
        r[0]["SubstituteName"] == dstname and \
        r[0]["PrintName"] == dstname and \
        r[0]["Flags"] == 1)
    e, r = expect("FindStreams %s" % srcname, 0)
    s = set(l["StreamName"] for l in r)
    testeval(len(s) == 2)
    testeval("::$DATA" in s)
    testeval(":strm:$DATA" in s)
    expect("DeleteFile %s" % srcname, 0)

    expect("CreateSymbolicLink %s %s:strm 0" % (srcname, dstname), 0)
    expect("CreateFile %s:strm GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % srcname, "ERROR_INVALID_NAME")
    expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % srcname, 0)
    expect("CreateFile %s GENERIC_WRITE 0 0 OPEN_EXISTING FILE_ATTRIBUTE_NORMAL 0" % srcname, 0)
    expect("CreateFile %s GENERIC_WRITE 0 0 OPEN_EXISTING FILE_ATTRIBUTE_NORMAL 0" % dstname, 0)
    expect("CreateFile %s:strm GENERIC_WRITE 0 0 OPEN_EXISTING FILE_ATTRIBUTE_NORMAL 0" % dstname, 0)
    expect("DeleteFile %s" % dstname, 0)
    expect("DeleteFile %s" % srcname, 0)

    expect("CreateSymbolicLink %s %s SYMBOLIC_LINK_FLAG_DIRECTORY" % (srcname, dstname), 0)
    expect("CreateDirectory %s 0" % srcname, "ERROR_ALREADY_EXISTS")
    expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % srcname, "ERROR_ACCESS_DENIED")
    expect("CreateFile %s:strm GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % srcname, 0)
    expect("CreateFile %s:strm GENERIC_WRITE 0 0 OPEN_EXISTING FILE_ATTRIBUTE_NORMAL 0" % srcname, 0)
    expect("CreateFile %s:strm GENERIC_WRITE 0 0 OPEN_EXISTING FILE_ATTRIBUTE_NORMAL 0" % dstname, 0)
    expect("DeleteFile %s" % dstname, 0)
    expect("RemoveDirectory %s" % srcname, 0)

    expect("CreateDirectory %s 0" % dstname, 0)
    expect("CreateSymbolicLink %s %s SYMBOLIC_LINK_FLAG_DIRECTORY" % (srcname, dstname), 0)
    expect("CreateFile %s:strm GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % srcname, 0)
    expect("CreateFile %s:strm GENERIC_WRITE 0 0 OPEN_EXISTING FILE_ATTRIBUTE_NORMAL 0" % srcname, 0)
    expect("CreateFile %s:strm GENERIC_WRITE 0 0 OPEN_EXISTING FILE_ATTRIBUTE_NORMAL 0" % dstname, 0)
    expect("RemoveDirectory %s" % dstname, 0)
    expect("RemoveDirectory %s" % srcname, 0)

testdone()
