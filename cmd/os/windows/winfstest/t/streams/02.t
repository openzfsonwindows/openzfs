#!/usr/bin/python

# FindStreams (FindFirstStreamW, FindNextStreamW, FindClose)

from winfstest import *

name = uniqname()

expect("CreateFile %s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
e, r = expect("FindStreams %s" % name, 0)
s = set(l["StreamName"] for l in r)
testeval(len(s) == 1)
testeval("::$DATA" in s)
expect("DeleteFile %s" % name, 0)
expect("FindStreams %s" % name, "ERROR_FILE_NOT_FOUND")

expect("CreateFile %s:foo GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("CreateFile %s:bar GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
e, r = expect("FindStreams %s" % name, 0)
s = set(l["StreamName"] for l in r)
testeval(len(s) == 3)
testeval("::$DATA" in s)
testeval(":foo:$DATA" in s)
testeval(":bar:$DATA" in s)
expect("DeleteFile %s" % name, 0)
expect("FindStreams %s" % name, "ERROR_FILE_NOT_FOUND")

expect("CreateFile %s:foo:$DATA GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("CreateFile %s:bar:$DATA GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
e, r = expect("FindStreams %s" % name, 0)
s = set(l["StreamName"] for l in r)
testeval(len(s) == 3)
testeval("::$DATA" in s)
testeval(":foo:$DATA" in s)
testeval(":bar:$DATA" in s)
expect("DeleteFile %s" % name, 0)
expect("FindStreams %s" % name, "ERROR_FILE_NOT_FOUND")

expect("CreateDirectory %s 0" % name, 0)
e, r = expect("FindStreams %s" % name, "ERROR_HANDLE_EOF")
expect("RemoveDirectory %s" % name, 0)
expect("FindStreams %s" % name, "ERROR_FILE_NOT_FOUND")

expect("CreateDirectory %s 0" % name, 0)
expect("CreateFile %s:foo GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("CreateFile %s:bar GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
e, r = expect("FindStreams %s" % name, 0)
s = set(l["StreamName"] for l in r)
testeval(len(s) == 2)
testeval(":foo:$DATA" in s)
testeval(":bar:$DATA" in s)
expect("RemoveDirectory %s" % name, 0)
expect("FindStreams %s" % name, "ERROR_FILE_NOT_FOUND")

expect("CreateDirectory %s 0" % name, 0)
expect("CreateFile %s:foo:$DATA GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
expect("CreateFile %s:bar:$DATA GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % name, 0)
e, r = expect("FindStreams %s" % name, 0)
s = set(l["StreamName"] for l in r)
testeval(len(s) == 2)
testeval(":foo:$DATA" in s)
testeval(":bar:$DATA" in s)
expect("RemoveDirectory %s" % name, 0)
expect("FindStreams %s" % name, "ERROR_FILE_NOT_FOUND")

for i in xrange(100):
	expect("CreateFile %s:strm%s GENERIC_WRITE 0 0 CREATE_NEW FILE_ATTRIBUTE_NORMAL 0" % (name, i), 0)
e, r = expect("FindStreams %s" % name, 0)
s = set(l["StreamName"] for l in r)
testeval(len(s) == 101)
for i in xrange(100):
	testeval(":strm%s:$DATA" % i in s)
expect("DeleteFile %s" % name, 0)
expect("FindStreams %s" % name, "ERROR_FILE_NOT_FOUND")

testdone()
