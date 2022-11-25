#!/usr/bin/python

# CreateFile (directory stream)
# DeleteFile (directory stream)

from winfstest import *

name = uniqname()

expect("CreateDirectory %s:foo 0" % name, "ERROR_DIRECTORY")
expect("CreateDirectory %s 0" % name, 0)
expect("CreateDirectory %s 0" % name, "ERROR_ALREADY_EXISTS")
expect("CreateFile %s:foo GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("CreateFile %s:foo GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, "ERROR_FILE_EXISTS")
e, r = expect("GetFileInformation %s"% name, 0)
e, s = expect("GetFileInformation %s:foo"% name, 0)
testeval(r[0]["FileIndex"] == s[0]["FileIndex"])
expect("RemoveDirectory %s:foo" % name, "ERROR_DIRECTORY")
expect("DeleteFile %s:foo" % name, 0)
expect("DeleteFile %s:foo" % name, "ERROR_FILE_NOT_FOUND")
expect("RemoveDirectory %s" % name, 0)
expect("RemoveDirectory %s" % name, "ERROR_FILE_NOT_FOUND")

expect("CreateDirectory %s 0" % name, 0)
expect("CreateFile %s:foo:$DATA GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
e, r = expect("GetFileInformation %s"% name, 0)
e, s = expect("GetFileInformation %s:foo"% name, 0)
testeval(r[0]["FileIndex"] == s[0]["FileIndex"])
expect("RemoveDirectory %s" % name, 0)

testdone()
