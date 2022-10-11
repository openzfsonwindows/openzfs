#!/usr/bin/python

# STATUS_DELETE_PENDING
# FindFiles with delete pending

from winfstest import *

name = uniqname()

with expect_task("CreateFile %s GENERIC_WRITE FILE_SHARE_READ+FILE_SHARE_WRITE+FILE_SHARE_DELETE 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0):
    expect("CreateFile %s GENERIC_READ FILE_SHARE_READ+FILE_SHARE_WRITE+FILE_SHARE_DELETE 0 OPEN_EXISTING 0 0" % name, 0)
    expect("DeleteFile %s" % name, 0)
    # STATUS_DELETE_PENDING translated to ERROR_ACCESS_DENIED below
    expect("CreateFile %s GENERIC_READ FILE_SHARE_READ+FILE_SHARE_WRITE+FILE_SHARE_DELETE 0 OPEN_EXISTING 0 0" % name, "ERROR_ACCESS_DENIED")

expect("CreateDirectory %s 0" % name, 0)
with expect_task("CreateFile %s\\foo GENERIC_WRITE FILE_SHARE_READ+FILE_SHARE_WRITE+FILE_SHARE_DELETE 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0):
    expect("DeleteFile %s\\foo" % name, 0)
    e, r = expect("FindFiles %s\\*" % name, 0)
    s = set(l["FileName"] for l in r)
    testeval(len(s) == 3)
    testeval("." in s)
    testeval(".." in s)
    testeval("foo" in s)
e, r = expect("FindFiles %s\\*" % name, 0)
s = set(l["FileName"] for l in r)
testeval(len(s) == 2)
testeval("." in s)
testeval(".." in s)
expect("RemoveDirectory %s" % name, 0)

testdone()
