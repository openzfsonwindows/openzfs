#!/usr/bin/python

# symbolic links (files)

from winfstest import *

import os, sys

def safeopen(name, mode = "r"):
    try:
        return open(name, mode)
    except:
        return None

srcname, dstname = uniqname(), uniqname()
dstpath = os.path.realpath(dstname)

# poor man's cygpath!
if sys.platform == "cygwin":
    cygdrive = os.readlink("/proc/cygdrive")
    if not cygdrive.endswith("/"):
        cygdrive += "/"
    if dstpath.startswith(cygdrive):
        dstpath = dstpath[len(cygdrive):]
        dstpath = dstpath[0:1].upper() + ":" + dstpath[1:]
    dstpath = dstpath.replace('/', '\\')

e, r = expect("CreateSymbolicLink %s %s 0" % (srcname, dstname), 0)
if e == "0":
    expect("GetFileInformation %s" % srcname, lambda r: r[0]["FileAttributes"] == 0x420)
    expect("GetReparsePoint %s" % srcname, lambda r:\
        r[0]["ReparseTag"] == "IO_REPARSE_TAG_SYMLINK" and \
        r[0]["SubstituteName"] == dstname and \
        r[0]["PrintName"] == dstname and \
        r[0]["Flags"] == 1)
    testeval(not safeopen(srcname))
    expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % dstname, 0)
    testeval(safeopen(srcname))
    expect("DeleteFile %s" % dstname, 0)
    expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % srcname, 0)
    testeval(safeopen(srcname))
    testeval(safeopen(dstname))
    expect("DeleteFile %s" % dstname, 0)
    testeval(not safeopen(srcname))
    expect("DeleteFile %s" % srcname, 0)

    expect("CreateSymbolicLink %s %s 0" % (srcname, dstname), 0)
    expect("WriteFile %s 0 41 42 43 44 45" % srcname, 0)
    expect("ReadFile %s 0 10" % srcname, lambda r: r[0]["Length"] == 5 and r[0]["Data"] == "41 42 43 44 45")
    expect("GetFileInformation %s" % srcname, lambda r: r[0]["FileAttributes"] == 0x420)
    expect("GetReparsePoint %s" % srcname, lambda r:\
        r[0]["ReparseTag"] == "IO_REPARSE_TAG_SYMLINK" and \
        r[0]["SubstituteName"] == dstname and \
        r[0]["PrintName"] == dstname and \
        r[0]["Flags"] == 1)
    testeval(not safeopen(srcname))
    expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % dstname, 0)
    testeval(safeopen(srcname))
    expect("DeleteFile %s" % dstname, 0)
    expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % srcname, 0)
    testeval(safeopen(srcname))
    testeval(safeopen(dstname))
    expect("DeleteFile %s" % dstname, 0)
    testeval(not safeopen(srcname))
    expect("DeleteFile %s" % srcname, 0)

    # absolute symlink
    e, r = expect("CreateSymbolicLink %s %s 0" % (srcname, dstpath), 0)
    expect("GetFileInformation %s" % srcname, lambda r: r[0]["FileAttributes"] == 0x420)
    expect("GetReparsePoint %s" % srcname, lambda r:\
        r[0]["ReparseTag"] == "IO_REPARSE_TAG_SYMLINK" and \
        r[0]["SubstituteName"] == "\\??\\" + (dstpath if "\\" != dstpath[:1] else "UNC" + dstpath[1:]) and \
        r[0]["PrintName"] == (dstpath if "\\" != dstpath[:1] else dstpath[1:]) and \
        r[0]["Flags"] == 0)
    testeval(not safeopen(srcname))
    expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % dstname, 0)
    testeval(safeopen(srcname))
    expect("DeleteFile %s" % dstname, 0)
    expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % srcname, 0)
    testeval(safeopen(srcname))
    testeval(safeopen(dstname))
    expect("DeleteFile %s" % dstname, 0)
    testeval(not safeopen(srcname))
    expect("DeleteFile %s" % srcname, 0)

testdone()
