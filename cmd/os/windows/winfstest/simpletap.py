#!/usr/bin/python

# simpletap.py
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

import re

_re_pl = re.compile(r"^(\d+)..(\d+)")
_re_ok = re.compile(r"^ok (\d+)(.*)")
_re_ko = re.compile(r"^not ok (\d+)(.*)")
def parse(iter):
    pl = None
    ok = set()
    ko = set()
    for line in iter:
        m = _re_pl.search(line)
        if m:
            pl = (int(m.group(1)), int(m.group(2)))
            yield ("PL", pl)
            continue
        m = _re_ok.search(line)
        if m:
            tu = (int(m.group(1)), m.group(2))
            ok.add(tu[0])
            yield ("OK", tu)
            continue
        m = _re_ko.search(line)
        if m:
            tu = (int(m.group(1)), m.group(2))
            ko.add(tu[0])
            yield ("KO", tu)
            continue
        yield ("VV", line)
    if pl is None:
        yield ("RR", "?", ok, ko)
    else:
        yield ("RR", set(xrange(pl[0], pl[1])) - ok - ko, ok, ko)

if "__main__" == __name__:
    import os, subprocess, sys

    def walktree(path):
        if os.path.isdir(path):
            return os.walk(path)
        else:
            return [(os.path.dirname(path), [], [os.path.basename(path)])]
    def write(s):
        global newline
        sys.stdout.write(s)
        newline = False
    def writenl(s):
        global newline
        if not newline and s:
            sys.stdout.write("\n")
        sys.stdout.write(s + "\n")
        newline = True
    def writehead(path):
        write((path + " ").ljust(39, ".") + " ")
    def main():
        verbose = False
        newline = False
        colors = hasattr(sys.stdout, "isatty") and sys.stdout.isatty() and os.getenv("TERM") == "xterm"
        okstr = "\x1B[32mok\x1B[0m" if colors else "ok"
        kostr = "\x1B[31mnot ok\x1B[0m" if colors else "not ok"
        totals = [0, 0]
        argv = sys.argv[1:]
        if argv and "--run" == argv[0]:
            argv = [os.path.join(os.path.dirname(sys.argv[0]), "t", arg) for arg in argv[1:]]
            if not argv:
                argv = [os.path.join(os.path.dirname(sys.argv[0]), "t")]
        for arg in argv:
            for dirpath, dirnames, filenames in walktree(arg):
                for filename in filenames:
                    if filename.endswith(".t"):
                        filename = os.path.join(dirpath, filename)
                        writehead(filename)
                        out = subprocess.check_output([sys.executable, filename],
                            stderr=subprocess.STDOUT, universal_newlines=True)
                        for i in parse(out.splitlines()):
                            if "RR" == i[0]:
                                if newline:
                                    writehead(filename)
                                if not i[1] and not i[3]:
                                    write(okstr)
                                else:
                                    write(kostr)
                                    if i[3]:
                                        write(" %s/%s" % (len(i[3]), len(i[2]) + len(i[3])))
                                    if "?" == i[1]:
                                        write(" - test plan missing")
                            elif "PL" == i[0]:
                                if verbose:
                                    writenl("%s..%s" % i[1])
                            elif "OK" == i[0]:
                                totals[0] += 1
                                if verbose:
                                    writenl(okstr + " %s%s" % i[1])
                            elif "KO" == i[0]:
                                totals[1] += 1
                                writenl(kostr + " %s%s" % i[1])
                            elif "VV" == i[0]:
                                if verbose:
                                    writenl("%s" % i[1])
                            else:
                                assert False
                        writenl("")
        if totals[0] + totals[1]:
            writenl("")
            writehead("total")
            if totals[0]:
                write("%s %s/%s" % (okstr, totals[0], totals[0] + totals[1]))
            if totals[1]:
                write("%s%s %s/%s" % (" - " if totals[0] else "", kostr, totals[1], totals[0] + totals[1]))
            writenl("")
        sys.exit(1 if totals[1] else 0)

    try:
        main()
    except subprocess.CalledProcessError, ex:
        print >>sys.stderr
        print >>sys.stderr, ex
        print >>sys.stderr, ex.output
        sys.exit(ex.returncode)
