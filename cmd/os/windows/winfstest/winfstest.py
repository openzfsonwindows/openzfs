# winfstest.py
#
# Copyright (c) 2015, Bill Zissimopoulos. All rights reserved.
#
# Redistribution  and use  in source  and  binary forms,  with or  without
# modification, are  permitted provided that the  following conditions are
# met:
#
# 1.  Redistributions  of source  code  must  retain the  above  copyright
# notice, this list of conditions and the following disclaimer.
#
# 2. Redistributions  in binary  form must  reproduce the  above copyright
# notice,  this list  of conditions  and the  following disclaimer  in the
# documentation and/or other materials provided with the distribution.
#
# 3.  Neither the  name  of the  copyright  holder nor  the  names of  its
# contributors may  be used  to endorse or  promote products  derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY  THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
# IS" AND  ANY EXPRESS OR  IMPLIED WARRANTIES, INCLUDING, BUT  NOT LIMITED
# TO,  THE  IMPLIED  WARRANTIES  OF  MERCHANTABILITY  AND  FITNESS  FOR  A
# PARTICULAR  PURPOSE ARE  DISCLAIMED.  IN NO  EVENT  SHALL THE  COPYRIGHT
# HOLDER OR CONTRIBUTORS  BE LIABLE FOR ANY  DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL,  EXEMPLARY,  OR  CONSEQUENTIAL   DAMAGES  (INCLUDING,  BUT  NOT
# LIMITED TO,  PROCUREMENT OF SUBSTITUTE  GOODS OR SERVICES; LOSS  OF USE,
# DATA, OR  PROFITS; OR BUSINESS  INTERRUPTION) HOWEVER CAUSED AND  ON ANY
# THEORY  OF LIABILITY,  WHETHER IN  CONTRACT, STRICT  LIABILITY, OR  TORT
# (INCLUDING NEGLIGENCE  OR OTHERWISE) ARISING IN  ANY WAY OUT OF  THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import inspect, os, random, re, subprocess, sys, threading, types

__all__ = [
    "testline", "testeval", "testdone", "uniqname",
    "fstest", "fstest_task", "expect", "expect_task"]

_ntests = 0
def testline(ok, diag = ""):
    global _ntests
    _ntests += 1
    print "%sok %s%s%s" % ("" if ok else "not ", _ntests, " - " if diag else "", diag)
def testeval(ok):
    diag = inspect.stack()[1]
    if diag and diag[4] is not None and diag[5] is not None:
        diag = diag[4][diag[5]]
        diag = diag.strip()
    else:
        diag = ""
    testline(ok, diag)
def testdone():
    global _ntests
    print "1..%s" % _ntests
    _ntests = 0

def uniqname():
    return "%08x" % random.randint(1, 2 ** 32)

_fstest_exe = os.path.splitext(os.path.realpath(__file__))[0] + ".exe"
_field_re = re.compile(r'(?:[^\s"]|"[^"]*")+')
class _fstest_task(object):
    def __init__(self, tsk, cmd, exp):
        self.tsk = tsk
        self.cmd = cmd
        self.exp = exp
        self.out = None
        self.err = None
        self.res = None
        arg = cmd.split() if hasattr(cmd, "split") else list(cmd)
        if self.tsk:
            arg.insert(0, "-w")
        self.prc = subprocess.Popen([_fstest_exe] + arg,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.thr = threading.Thread(target=self._readthread)
        self.thr.start()
    def __enter__(self):
        self.thr.join()
        self.err, self.res = self._fstest_res()
        if self.exp is not None:
            self._expect(self.cmd, self.exp, self.err, self.res)
        return self
    def __exit__(self, type, value, traceback):
        if self.tsk:
            try:
                self.prc.stdin.write("\n")
            except IOError:
                pass
        self.prc.stdin.close()
        self.prc.wait()
        ret = self.prc.poll()
        if ret:
            raise subprocess.CalledProcessError(ret, self.cmd)
    def _readthread(self):
        self.out = self.prc.stdout.read()
        self.out = self.out.replace("\r\n", "\n").replace("\r", "\n")
    def _fstest_res(self):
        out = self.out.splitlines()
        res = []
        for l in out[1:]:
            if not l:
                continue
            d = {}
            res.append(d)
            for p in _field_re.findall(l):
                k, v = p.split("=", 2)
                if v.startswith('"') and v.endswith('"') and len(v) >= 2:
                    v = v[1:-1]
                else:
                    try:
                        v = int(v, 0)
                    except:
                        pass
                d[k] = v
        return out[0], res
    def _expect(self, cmd, exp, err, res):
        s = "expect" if not self.tsk else "expect_task"
        if isinstance(exp, types.FunctionType): # function, lambda
            if "0" == err:
                testline(exp(res), "%s \"%s\" %s - result %s" % (s, cmd, exp.__name__, res))
            else:
                testline(0, "%s \"%s\" %s - got %s" % (s, cmd, 0, err))
        else:
            if err is None:
                testline(1, "%s \"%s\" %s" % (s, cmd, exp))
            elif str(exp) == err:
                testline(1, "%s \"%s\" %s" % (s, cmd, exp))
            else:
                testline(0, "%s \"%s\" %s - got %s" % (s, cmd, exp, err))

def fstest(cmd):
    with _fstest_task(False, cmd, None) as task:
        pass
    return task.err, task.res
def expect(cmd, exp):
    with _fstest_task(False, cmd, exp) as task:
        pass
    return task.err, task.res
def fstest_task(cmd):
    return _fstest_task(True, cmd, None)
def expect_task(cmd, exp):
    if isinstance(exp, types.FunctionType): # function, lambda
        print "# expect_task \"%s\" %s" % (cmd, exp.__name__)
    else:
        print "# expect_task \"%s\" %s" % (cmd, exp)
    return _fstest_task(True, cmd, exp)
