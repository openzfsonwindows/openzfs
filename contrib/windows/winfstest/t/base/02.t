#!/usr/bin/python

# CreateDirectory
# RemoveDirectory

from winfstest import *

name = uniqname()

expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("CreateDirectory %s 0" % name, "ERROR_ALREADY_EXISTS")
expect("RemoveDirectory %s" % name, "ERROR_DIRECTORY")
expect("DeleteFile %s" % name, 0)

expect("CreateDirectory %s 0" % name, 0)
expect("CreateDirectory %s 0" % name, "ERROR_ALREADY_EXISTS")
expect("GetFileInformation %s" % name, lambda r: r[0]["FileAttributes"] == 0x10)
expect("DeleteFile %s" % name, "ERROR_ACCESS_DENIED")
expect("RemoveDirectory %s" % name, 0)
expect("RemoveDirectory %s" % name, "ERROR_FILE_NOT_FOUND")

expect("CreateDirectory %s 0" % name, 0)
expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, "ERROR_ACCESS_DENIED")
expect("RemoveDirectory %s" % name, 0)

testdone()
