#!/usr/bin/python

# CreateFile CreationDisposition
# DeleteFile

from winfstest import *

name = uniqname()

expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, "ERROR_FILE_EXISTS")
expect("DeleteFile %s" % name, 0)
expect("DeleteFile %s" % name, "ERROR_FILE_NOT_FOUND")

expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_ALWAYS FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_ALWAYS FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("-e CreateFile %s GENERIC_WRITE 0 0 CREATE_ALWAYS FILE_ATTRIBUTE_NORMAL 0" % name, "ERROR_ALREADY_EXISTS")
expect("DeleteFile %s" % name, 0)

expect("CreateFile %s GENERIC_WRITE 0 0 OPEN_ALWAYS FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("CreateFile %s GENERIC_WRITE 0 0 OPEN_ALWAYS FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("-e CreateFile %s GENERIC_WRITE 0 0 OPEN_ALWAYS FILE_ATTRIBUTE_NORMAL 0" % name, "ERROR_ALREADY_EXISTS")
expect("DeleteFile %s" % name, 0)

expect("CreateFile %s GENERIC_WRITE 0 0 OPEN_EXISTING FILE_ATTRIBUTE_NORMAL 0" % name, "ERROR_FILE_NOT_FOUND")
expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_ALWAYS FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("CreateFile %s GENERIC_WRITE 0 0 OPEN_EXISTING FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("DeleteFile %s" % name, 0)

expect("CreateFile %s GENERIC_WRITE 0 0 TRUNCATE_EXISTING FILE_ATTRIBUTE_NORMAL 0" % name, "ERROR_FILE_NOT_FOUND")
expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_ALWAYS FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("CreateFile %s GENERIC_WRITE 0 0 TRUNCATE_EXISTING FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("DeleteFile %s" % name, 0)

expect("CreateFile %s\\bar GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, "ERROR_PATH_NOT_FOUND")

testdone()
