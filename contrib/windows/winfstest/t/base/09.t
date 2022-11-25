#!/usr/bin/python

# CreateFile share mode

from winfstest import *

name = uniqname()

with expect_task("CreateFile %s GENERIC_WRITE FILE_SHARE_READ 0 CREATE_ALWAYS FILE_ATTRIBUTE_NORMAL 0" % name, 0):
    expect("CreateFile %s GENERIC_READ FILE_SHARE_READ 0 OPEN_EXISTING 0 0" % name, "ERROR_SHARING_VIOLATION")
    expect("CreateFile %s GENERIC_READ FILE_SHARE_WRITE 0 OPEN_EXISTING 0 0" % name, 0)
    expect("CreateFile %s DELETE FILE_SHARE_DELETE 0 OPEN_EXISTING 0 0" % name, "ERROR_SHARING_VIOLATION")
    expect("CreateFile %s DELETE FILE_SHARE_WRITE 0 OPEN_EXISTING 0 0" % name, "ERROR_SHARING_VIOLATION")

with expect_task("CreateFile %s GENERIC_READ FILE_SHARE_WRITE 0 CREATE_ALWAYS FILE_ATTRIBUTE_NORMAL 0" % name, 0):
    expect("CreateFile %s GENERIC_WRITE FILE_SHARE_WRITE 0 OPEN_EXISTING 0 0" % name, "ERROR_SHARING_VIOLATION")
    expect("CreateFile %s GENERIC_WRITE FILE_SHARE_READ 0 OPEN_EXISTING 0 0" % name, 0)
    expect("CreateFile %s DELETE FILE_SHARE_DELETE 0 OPEN_EXISTING 0 0" % name, "ERROR_SHARING_VIOLATION")
    expect("CreateFile %s DELETE FILE_SHARE_WRITE 0 OPEN_EXISTING 0 0" % name, "ERROR_SHARING_VIOLATION")

with expect_task("CreateFile %s DELETE FILE_SHARE_DELETE 0 CREATE_ALWAYS FILE_ATTRIBUTE_NORMAL 0" % name, 0):
    expect("CreateFile %s GENERIC_WRITE FILE_SHARE_DELETE 0 OPEN_EXISTING 0 0" % name, "ERROR_SHARING_VIOLATION")
    expect("CreateFile %s DELETE FILE_SHARE_WRITE 0 OPEN_EXISTING 0 0" % name, "ERROR_SHARING_VIOLATION")
    expect("CreateFile %s DELETE FILE_SHARE_DELETE 0 OPEN_EXISTING 0 0" % name, 0)

expect("DeleteFile %s" % name, 0)

expect("CreateDirectory %s 0" % name, 0)
with expect_task("CreateFile %s GENERIC_READ 0 0 OPEN_EXISTING FILE_FLAG_BACKUP_SEMANTICS 0" % name, 0):
    expect("CreateFile %s GENERIC_READ 0 0 OPEN_EXISTING FILE_FLAG_BACKUP_SEMANTICS 0" % name, "ERROR_SHARING_VIOLATION")
expect("RemoveDirectory %s" % name, 0)

testdone()
