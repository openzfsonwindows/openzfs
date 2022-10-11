#!/usr/bin/python

# FindFiles (FindFirstFile, FindNextFile, FindClose)

from winfstest import *

name = uniqname()

expect("CreateDirectory %s 0" % name, 0)
expect("CreateFile %s\\foo GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("RemoveDirectory %s" % name, "ERROR_DIR_NOT_EMPTY")
expect("RemoveDirectory %s\\foo" %name, "ERROR_DIRECTORY")
expect("DeleteFile %s\\foo" %name, 0)
expect("RemoveDirectory %s" % name, 0)

expect("CreateDirectory %s 0" % name, 0)
expect("CreateFile %s\\foo GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("CreateFile %s\\bar GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
e, r = expect("FindFiles %s\\*" % name, 0)
s = set(l["FileName"] for l in r)
testeval(len(s) == 4)
testeval("." in s)
testeval(".." in s)
testeval("foo" in s)
testeval("bar" in s)
expect("DeleteFile %s\\bar" %name, 0)
expect("DeleteFile %s\\foo" %name, 0)
expect("RemoveDirectory %s" % name, 0)

testdone()
