#!/usr/bin/python

# CreateFile (file stream) security
# CreateDirectory (file stream) security
# GetFileSecurity (file stream)
# SetFileSecurity (file stream)

from winfstest import *

name = uniqname()

expect("CreateFile %s:strm GENERIC_WRITE 0 D:P(A;;GA;;;WD) CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("GetFileSecurity %s DACL_SECURITY_INFORMATION" % name, lambda r: r[0]["Sddl"] == "D:P(A;;FA;;;WD)")
expect("GetFileSecurity %s:strm DACL_SECURITY_INFORMATION" % name, lambda r: r[0]["Sddl"] == "D:P(A;;FA;;;WD)")
expect("SetFileSecurity %s:strm DACL_SECURITY_INFORMATION D:P(A;;GA;;;WD)(A;;GR;;;SY)" % name, 0)
expect("GetFileSecurity %s DACL_SECURITY_INFORMATION" % name, lambda r: r[0]["Sddl"] == "D:P(A;;FA;;;WD)(A;;FR;;;SY)")
expect("GetFileSecurity %s:strm DACL_SECURITY_INFORMATION" % name, lambda r: r[0]["Sddl"] == "D:P(A;;FA;;;WD)(A;;FR;;;SY)")
expect("DeleteFile %s" % name, 0)

expect("CreateFile %s:strm GENERIC_WRITE 0 D:P(A;;GA;;;WD) CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("CreateFile %s GENERIC_READ 0 0 OPEN_EXISTING 0 0" % name, 0)
expect("CreateFile %s:strm GENERIC_READ 0 0 OPEN_EXISTING 0 0" % name, 0)
expect("SetFileSecurity %s DACL_SECURITY_INFORMATION D:P(D;;GR;;;WD)" % name, 0)
expect("CreateFile %s GENERIC_READ 0 0 OPEN_EXISTING 0 0" % name, "ERROR_ACCESS_DENIED")
expect("CreateFile %s:strm GENERIC_READ 0 0 OPEN_EXISTING 0 0" % name, "ERROR_ACCESS_DENIED")
expect("DeleteFile %s" % name, 0)

expect("CreateDirectory %s D:P(A;;GA;;;WD)" % name, 0)
expect("CreateFile %s:strm GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("GetFileSecurity %s DACL_SECURITY_INFORMATION" % name, lambda r: r[0]["Sddl"] == "D:P(A;;FA;;;WD)")
expect("GetFileSecurity %s:strm DACL_SECURITY_INFORMATION" % name, lambda r: r[0]["Sddl"] == "D:P(A;;FA;;;WD)")
expect("SetFileSecurity %s:strm DACL_SECURITY_INFORMATION D:P(A;;GA;;;WD)(A;;GR;;;SY)" % name, 0)
expect("GetFileSecurity %s DACL_SECURITY_INFORMATION" % name, lambda r: r[0]["Sddl"] == "D:P(A;;FA;;;WD)(A;;FR;;;SY)")
expect("GetFileSecurity %s:strm DACL_SECURITY_INFORMATION" % name, lambda r: r[0]["Sddl"] == "D:P(A;;FA;;;WD)(A;;FR;;;SY)")
expect("RemoveDirectory %s" % name, 0)

expect("CreateDirectory %s D:P(A;;GA;;;WD)" % name, 0)
expect("CreateFile %s\\foobar GENERIC_READ 0 D:P(A;;FR;;;WD) CREATE_NEW 0 0" % name, 0)
expect("CreateFile %s\\foobar:strm GENERIC_READ 0 0 CREATE_NEW 0 0" % name, "ERROR_ACCESS_DENIED")
expect("SetFileSecurity %s\\foobar DACL_SECURITY_INFORMATION D:P(A;;FRFW;;;WD)" % name, 0)
expect("CreateFile %s\\foobar:strm GENERIC_READ 0 0 CREATE_NEW 0 0" % name, 0)
expect("SetFileSecurity %s DACL_SECURITY_INFORMATION D:P(A;;FR;;;WD)" % name, 0)
expect("SetFileSecurity %s\\foobar DACL_SECURITY_INFORMATION D:P(A;;FR;;;WD)" % name, 0)
expect("DeleteFile %s\\foobar:strm" % name, "ERROR_ACCESS_DENIED")
expect("SetFileSecurity %s\\foobar DACL_SECURITY_INFORMATION D:P(A;;FRSD;;;WD)" % name, 0)
expect("DeleteFile %s\\foobar:strm" % name, 0)
expect("SetFileSecurity %s\\foobar DACL_SECURITY_INFORMATION D:P(A;;FRFW;;;WD)" % name, 0)
expect("CreateFile %s\\foobar:strm GENERIC_READ 0 0 CREATE_NEW 0 0" % name, 0)
expect("DeleteFile %s\\foobar:strm" % name, "ERROR_ACCESS_DENIED")
expect("SetFileSecurity %s DACL_SECURITY_INFORMATION D:P(A;;GA;;;WD)" % name, 0)
expect("DeleteFile %s\\foobar:strm" % name, 0)
expect("DeleteFile %s\\foobar" % name, 0)
expect("RemoveDirectory %s" % name, 0)

testdone()
