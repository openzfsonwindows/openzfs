import os
import argparse

import subprocess

from pathlib import Path, PurePosixPath, PureWindowsPath, WindowsPath

from pprint import pprint

import time


import json

import logging

logging.basicConfig(level=logging.DEBUG)


print("Printed immediately.")
def parse_arguments():
    parser = argparse.ArgumentParser(description='Process command line arguments.')
    parser.add_argument('-path', type=dir_path, required=True)
    return parser.parse_args()
def dir_path(path):
    if os.path.isdir(path):
        return path
    else:
        raise argparse.ArgumentTypeError(f"readable_dir:{path} is not a valid path")



def get_DeviceId():
    magic_number_process = subprocess.run(
        ["wmic", "diskdrive", "get", "DeviceId"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )

    #https://github.com/sir-ragna/dddddd
    #get DeviceId

    a=magic_number_process.stdout.decode(encoding='UTF-8',errors='strict').replace("\r\r\n", "\r\n")

    c = a.splitlines()

    d = [x.split() for x in c]

    e = [x[0] for x in d if len(x) > 0 and x[0] not in "DeviceID"]

    e.sort()

    #print(e)

    #print([x.encode(encoding='UTF-8') for x in e])

    #import csv

    #with open('csv_file.csv', 'w', encoding='UTF8') as f:
    #    writer = csv.writer(f, dialect='excel', quoting=csv.QUOTE_ALL)
    #
    #    for row in e:
    #        writer.writerow([row])

    return e

def allocate_file(name, size):
    with open(name, 'wb') as f:
        f.seek(size)
        f.write(b'0')

def delete_file(name):
    if os.path.exists(name):
        os.remove(name)
    else:
        print("The file does not exist")


def get_driveletters():
    magic_number_process = subprocess.run(
        ["C:\\Program Files\\OpenZFS On Windows\\zfs.exe", "mount"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )

    #b'test01                          H:\\ \r\ntest02                          I:\\ \r\n'


    a=magic_number_process.stdout.decode(encoding='UTF-8',errors='strict')

    c = a.splitlines()

    logging.debug("get_driveletters() {}".format(c))

    #print("get_driveletters() debug",c)

    d = [x.split() for x in c]

    logging.debug("get_driveletters() {}".format(d))

    return d

#      run: '& "C:\Program Files\OpenZFS On Windows\zfs.exe" mount'



def create_pool(name, file):
    magic_number_process = subprocess.run(
        ["C:\\Program Files\\OpenZFS On Windows\\zpool.exe", "create", "-f", name, file],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )



def destroy_pool(name):
    magic_number_process = subprocess.run(
        ["C:\\Program Files\\OpenZFS On Windows\\zpool.exe", "destroy", "-f", name],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )

def zpool(*args):
    magic_number_process = subprocess.run(
        ["C:\\Program Files\\OpenZFS On Windows\\zpool.exe", *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )

    return magic_number_process


def zfs(*args):
    magic_number_process = subprocess.run(
        ["C:\\Program Files\\OpenZFS On Windows\\zfs.exe", *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )

    return magic_number_process


def run(args):
    d = {"zfs": "C:\\Program Files\\OpenZFS On Windows\\zfs.exe", "zpool": "C:\\Program Files\\OpenZFS On Windows\\zpool.exe"}
    l = list(args)
    cmd = d[l[0]]
    result = subprocess.run(
        [cmd, *l[1:]],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )

    return result



def tounc(name):
    q = "\\\\?\\" + str(name)
    return q





def runWithPrint(cmd):
    print(" ".join(cmd))
    ret = run(cmd)
    logging.debug(str("args={}".format(" ".join(ret.args))))
    logging.debug(str("returncode={}".format(ret.returncode)))
    logging.debug(str("stdout={}".format(ret.stdout)))
    logging.debug(str("stderr={}".format(ret.stderr)))


    return ret

def preTest(testName = None):
    print("=" * 20)
    if testName is not None:
        print("Name:", testName)

    get_driveletters()

def postTest():
    get_driveletters()
    print("=" * 20)

def main():
    parsed_args = parse_arguments()

    print("Path:", parsed_args.path)

    p = PureWindowsPath(parsed_args.path)

    print("Path object:", p)

    print("Physical devices", get_DeviceId())

    if p.is_absolute():

        f1 = PureWindowsPath(p, "test01.dat")
        allocate_file(f1, 1024*1024*1024)
        f2 = PureWindowsPath(p, "test02.dat")
        allocate_file(f2, 1024*1024*1024)
        f3 = PureWindowsPath(p, "test03.dat")
        allocate_file(f3, 1024*1024*1024)







        preTest()
        ret = runWithPrint(["zpool", "create", "-f", "test01", tounc(f1)])
        time.sleep(10)
        if ret.returncode != 0:
            print("FAIL")
        print("Drive letters after pool create:", get_driveletters())
        runWithPrint(["zpool", "destroy", "-f", "test01"])
        time.sleep(10)
        postTest()





        preTest()
        ret = runWithPrint(["zpool", "create", "-f", "test02", tounc(f1), tounc(f2)])
        if ret.returncode != 0:
            print("FAIL")
        time.sleep(10)
        print("Drive letters after pool create:", get_driveletters())
        runWithPrint(["zpool", "destroy", "-f", "test02"])
        time.sleep(10)
        postTest()





        preTest()
        ret = runWithPrint(["zpool", "create", "-f", "test03", tounc(f1), tounc(f2), tounc(f3)])
        if ret.returncode != 0:
            print("FAIL")
        time.sleep(10)
        print("Drive letters after pool create:", get_driveletters())
        runWithPrint(["zpool", "destroy", "-f", "test03"])
        time.sleep(10)
        postTest()





        preTest()
        ret = runWithPrint(["zpool", "create", "-f", "test04", "mirror", tounc(f1), tounc(f2)])
        if ret.returncode != 0:
            print("FAIL")
        time.sleep(10)
        print("Drive letters after pool create:", get_driveletters())
        runWithPrint(["zpool", "destroy", "-f", "test04"])
        time.sleep(10)
        postTest()





        preTest()
        ret = runWithPrint(["zpool", "create", "-f", "test05", "mirror", tounc(f1), tounc(f2), tounc(f3)])
        time.sleep(10)
        if ret.returncode != 0:
            print("FAIL")
        print("Drive letters after pool create:", get_driveletters())
        runWithPrint(["zpool", "destroy", "-f", "test05"])
        time.sleep(10)
        postTest()





        preTest()
        ret = runWithPrint(["zpool", "create", "-f", "test06", "raidz", tounc(f1), tounc(f2), tounc(f3)])
        if ret.returncode != 0:
            print("FAIL")
        time.sleep(10)
        print("Drive letters after pool create:", get_driveletters())
        runWithPrint(["zpool", "destroy", "-f", "test06"])
        time.sleep(10)
        postTest()





        preTest()
        ret = runWithPrint(["zpool", "create", "-f", "test07", "raidz1", tounc(f1), tounc(f2), tounc(f3)])
        if ret.returncode != 0:
            print("FAIL")
        time.sleep(10)
        print("Drive letters after pool create:", get_driveletters())
        runWithPrint(["zpool", "destroy", "-f", "test07"])
        time.sleep(10)
        postTest()


















        preTest("snapshot no hang:")

        ret = runWithPrint(["zpool", "create", "-f", "testsn01", tounc(f1)])
        if ret.returncode != 0:
            print("FAIL")
        time.sleep(10)
        print("Drive letters after pool create:", get_driveletters())

        f = PureWindowsPath(get_driveletters()[0][1], "test01.file")
        allocate_file(f, 1024)

        ret = runWithPrint(["zfs", "snapshot", "testsn01@friday"])
        if ret.returncode != 0:
            print("FAIL")

        f = PureWindowsPath(get_driveletters()[0][1], "test02.file")
        allocate_file(f, 1024)

        ret = runWithPrint(["zpool", "export", "-a"])
        if ret.returncode != 0:
            print("FAIL")
        time.sleep(10)

        runWithPrint(["zpool", "destroy", "-f", "testsn01"])

        time.sleep(10)
        postTest()






        # preTest("snapshot hang")
        #
        # ret = runWithPrint(["zpool", "create", "-f", "testsn02", tounc(f1)])
        # if ret.returncode != 0:
        #     print("FAIL")
        # time.sleep(10)
        # print("Drive letters after pool create:", get_driveletters())
        #
        # f = PureWindowsPath(get_driveletters()[0][1], "test01.file")
        # allocate_file(f, 1024)
        #
        # ret = runWithPrint(["zfs", "snapshot", "testsn02@friday"])
        # if ret.returncode != 0:
        #     print("FAIL")
        #
        #
        # f = PureWindowsPath(get_driveletters()[0][1], "test02.file")
        # allocate_file(f, 1024)
        #
        # ret = runWithPrint(["zfs", "mount", "testsn02@friday"])
        # if ret.returncode != 0:
        #     print("FAIL")
        #
        #
        # ret = runWithPrint(["zpool", "export", "-a"])
        # if ret.returncode != 0:
        #     print("FAIL")
        # time.sleep(10)
        #
        # runWithPrint(["zpool", "destroy", "-f", "testsn02"])
        # time.sleep(10)
        # postTest()






































        preTest("regex for key file")

        random_bytearray = bytearray(os.urandom(32))

        key01 = PureWindowsPath(p, "key01.key")

        with open(key01, 'wb') as f:
            f.write(random_bytearray)

        nx ="file://" + tounc(key01).replace("\\", "/")
        print(nx)


        ret = runWithPrint(["zpool", "create", "-f", "-O", "encryption=aes-256-ccm", "-O", "keylocation=" + nx, "-O", "keyformat=raw", "tank", tounc(f1)])
        if ret.returncode != 0:
            print("FAIL")
        time.sleep(10)
        print("Drive letters after pool create:", get_driveletters())

        ret = runWithPrint(["zfs", "get", "keylocation", "tank"])
        if ret.returncode != 0:
            print("FAIL")

        ret = runWithPrint(["zpool", "export", "tank"])
        if ret.returncode != 0:
            print("FAIL")
        time.sleep(10)







        print("Drive letters before pool create:", get_driveletters())
        ret = runWithPrint(["zpool", "import", "-f", "-l", "-O", "encryption=aes-256-ccm", "-O", "keylocation=" + nx, "-O", "keyformat=raw", tounc(f1)])
        if ret.returncode != 0:
            print("FAIL")
        time.sleep(10)


        print("Drive letters after pool create:", get_driveletters())
        runWithPrint(["zpool", "destroy", "-f", "tank"])
        time.sleep(10)
        postTest()













        preTest("run out of drive letters")

        for i in range(1, 26):
            ret = runWithPrint(["zpool", "create", "-f", "tank" + str(i), tounc(f1)])
            if ret.returncode != 0:
                print("FAIL")
            time.sleep(10)

            print("Drive letters after pool create:", get_driveletters())

            f = PureWindowsPath(get_driveletters()[0][1], "test01.file")
            try:
                allocate_file(f, 1024)
            except:
                print("FAIL")

            runWithPrint(["zpool", "destroy", "-f", "tank" + str(i)])
            time.sleep(10)


        postTest()








        delete_file(f1)
        delete_file(f2)
        delete_file(f3)




if __name__ == "__main__":
    main()





