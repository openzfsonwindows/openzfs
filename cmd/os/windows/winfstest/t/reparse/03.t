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

e, r = expect("CreateSymbolicLink %s %s SYMBOLIC_LINK_FLAG_DIRECTORY" % (srcname, dstname), 0)
if e == "0":
    expect("GetFileInformation %s" % srcname, lambda r: r[0]["FileAttributes"] == 0x410)
    expect("GetReparsePoint %s" % srcname, lambda r:\
        r[0]["ReparseTag"] == "IO_REPARSE_TAG_SYMLINK" and \
        r[0]["SubstituteName"] == dstname and \
        r[0]["PrintName"] == dstname and \
        r[0]["Flags"] == 1)
    testeval(not safeopen(os.path.join(srcname, "1")))
    expect("CreateDirectory %s 0" % dstname, 0)
    expect("CreateFile %s\\1 GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % dstname, 0)
    testeval(safeopen(os.path.join(srcname, "1")))
    expect("DeleteFile %s\\1" % dstname, 0)
    expect("RemoveDirectory %s" % dstname, 0)
    testeval(not safeopen(os.path.join(srcname, "1")))
    expect("CreateDirectory %s 0" % srcname, "ERROR_ALREADY_EXISTS")
    expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % srcname, "ERROR_ACCESS_DENIED")
    expect("RemoveDirectory %s" % srcname, 0)

    # absolute symlink
    e, r = expect("CreateSymbolicLink %s %s SYMBOLIC_LINK_FLAG_DIRECTORY" % (srcname, dstpath), 0)
    expect("GetFileInformation %s" % srcname, lambda r: r[0]["FileAttributes"] == 0x410)
    expect("GetReparsePoint %s" % srcname, lambda r:\
        r[0]["ReparseTag"] == "IO_REPARSE_TAG_SYMLINK" and \
        r[0]["SubstituteName"] == "\\??\\" + (dstpath if "\\" != dstpath[:1] else "UNC" + dstpath[1:]) and \
        r[0]["PrintName"] == (dstpath if "\\" != dstpath[:1] else dstpath[1:]) and \
        r[0]["Flags"] == 0)
    testeval(not safeopen(os.path.join(srcname, "1")))
    expect("CreateDirectory %s 0" % dstname, 0)
    expect("CreateFile %s\\1 GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % dstname, 0)
    testeval(safeopen(os.path.join(srcname, "1")))
    expect("DeleteFile %s\\1" % dstname, 0)
    expect("RemoveDirectory %s" % dstname, 0)
    testeval(not safeopen(os.path.join(srcname, "1")))
    expect("CreateDirectory %s 0" % srcname, "ERROR_ALREADY_EXISTS")
    expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % srcname, "ERROR_ACCESS_DENIED")
    expect("RemoveDirectory %s" % srcname, 0)

testdone()
