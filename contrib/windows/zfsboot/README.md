# Booting Windows from a ZFS root (experimental)

Windows 10 21H2 (19044.1288) has been booted to a desktop from a ZFS root on
VMware Workstation (UEFI). It is a proof of concept, not something to put real
data on.

How it works:

1. The firmware starts [Quibble](https://github.com/maharmstone/quibble), a
   replacement for the Windows boot loader.
2. Quibble loads `zfs.efi` (a read-only ZFS driver for UEFI, in the
   `zfs/` directory of the Quibble fork on the `zfs-driver` branch:
   <https://github.com/openzfsonwindows/quibble>). That makes the pool's
   `bootfs` dataset a `SimpleFileSystem`, and Quibble reads `ntoskrnl.exe`, the
   `SYSTEM` hive, the boot-start drivers and `OpenZFS.sys` from it.
3. The kernel starts. `OpenZFS.sys` is a boot-start driver
   (`Group="Boot File System"`). With the `boot_import` tunable set to 2 it
   finds the pool on the partition that was booted and mounts the root dataset
   on that partition's volume (`zfs_rootpool.c`), so the kernel finds
   `\SystemRoot`.
4. The ordinary mount path takes over once the system is up.

Quibble itself needed a fix for hypervisors that present Hyper-V's interface
(it never gave the kernel a hypercall page): maharmstone/quibble#143.

## Making a pool that can be booted

Only what `zfs.efi` understands can be on the pool:

    zpool create -f -d \
      -o feature@lz4_compress=enabled -o feature@embedded_data=enabled \
      -o feature@hole_birth=enabled -o feature@extensible_dataset=enabled \
      -o feature@large_blocks=enabled -o ashift=12 \
      -O casesensitivity=insensitive -O compression=lz4 -O atime=off \
      -O mountpoint=none rpool PhysicalDrive0
    zfs create -o driveletter=Y rpool/root
    zpool set bootfs=rpool/root rpool

* Compression: only `lz4` has been run. The reader also knows `lzjb` and `zle`,
  but that is untested. `gzip` is a stub in `zfs.efi`, and `zstd` and the
  `blake3` checksum are not wired in.
* No encryption, no features beyond the list above, and one device.
* On Windows, give the device as `PhysicalDrive0`; `\\.\PhysicalDrive0` was
  rejected by `zpool create` with "invalid argument".
* Use `casesensitivity=insensitive`. Quibble and the kernel look files up in
  whatever case the registry and the INF files use (`system32`, `System32`),
  and `zfs.efi` folds ASCII for lookups in such datasets.

### The partition type

`zpool create` tags the partition as a Solaris `/usr` partition. Windows still
makes a volume for it, but the Mount Manager only registers volumes of types it
recognises. The boot volume is then never found, and the phase 1 thread waits
for ever in `mountmgr!MountMgrBootDriveLetterAssignment`. Change the type to
Basic Data with the pool exported:

    Set-Partition -DiskNumber 0 -PartitionNumber 1 `
        -GptType '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'

`retag-gpt.py` does the same to a disk image file.

### Making `AppX` happy

Set `com.apple.mimic=ntfs` on the dataset. Without it the volume reports a file
system called "ZFS", the AppX deployment service refuses to deploy to it
("Windows cannot deploy to path WindowsApps of file system type ZFS",
0x80073CFD), and the Start menu, Search and Settings do not work.

## Putting Windows on it

Copy an installed Windows to the dataset. What worked, run as SYSTEM:

    robocopy C:\Windows Y:\Windows /E /COPY:DAT /DCOPY:DAT /B /R:1 /W:1 /XJ /MT:8 ^
        /XD C:\Windows\Temp C:\Windows\System32\config

and the same for `Program Files`, `Program Files (x86)`, `ProgramData` and
`Users`. Use `/COPY:DAT` and not `/COPYALL`; see the known problems.

The registry hives are locked, so save them with `reg save` into
`Windows\System32\config` (`SYSTEM`, `SOFTWARE`, `SAM`, `SECURITY`, `DEFAULT`),
copy the ones that are not loaded (`COMPONENTS`, `DRIVERS`) as files, and
`reg save` the logged on user's `NTUSER.DAT` and `UsrClass.dat` (or the user
gets a temporary profile). Then run `prepare-root-hive.ps1` on the `SYSTEM`
hive copy.

Files that the loader and the kernel ask for that are easy to miss when
copying by hand: `Windows\inf\errata.inf` (`EmInitSystem` fails the boot with
`PHASE1_INITIALIZATION_FAILED` without it), `System32\CodeIntegrity\driver.stl`
(loading `ntdll.dll` fails), `System32\catroot`, the drivers behind the kernel's
API sets (`tm.sys`, `clipsp.sys`, `cmimcext.sys`, `ntosext.sys`,
`werkernel.sys`), the crash dump drivers, `Windows\Fonts\arial.ttf` and
`cour.ttf`, and the imports of the boot drivers (`storport.sys`, `ataport.sys`,
`netio.sys`, ...). Copying the whole tree avoids chasing them.

## Quibble

`freeldr.ini`, with an ARC name for the partition (`rdisk` is Quibble's
numbering of the disks):

    [Windows_zfs]
    SystemPath=multi(0)disk(0)rdisk(1)partition(1)\Windows
    Options=/DEBUG /DEBUGPORT=COM1 /BAUDRATE=115200 /BREAK

`zfs.efi` goes in Quibble's `drivers` directory with `ntfs.efi` and `btrfs.efi`.
`/BREAK` stops the kernel at its first instruction so that WinDbg can attach
before anything fails; leave it out for a normal boot. The kernel only probes
for a debugger for a few seconds at boot, so a late WinDbg gets no answer.

A test-signed `OpenZFS.sys` needs `/TESTSIGNING`; the kernel ignores a
catalog-less EV signature on its own.

## Known problems

* **Read-only import.** `zpool import -o readonly=on` does not make the file
  system read-only. A cache flush then reaches `zfs_log_write`, and
  `zilog_dirty` asserts that the pool is writable: `spl_panic`, bugcheck
  `0x00ffffff`.
* **Backup and restore privileges** are not honoured on create, so
  `robocopy /COPYALL` fails creating directories whose copied ACL only lets
  TrustedInstaller write. Without the original ACLs the system boots, but
  servicing and `sfc` have nothing to compare against.
* **After an unclean shutdown** of a ZFS-booted system, one read-write import
  left the root dataset unmounted and the pool busy (export failed) until a
  reboot. A second import mounted it. The cause is not known.
* `zfs mount` and `zfs get mounted` do not know about the boot-time mount, and
  the `driveletter` property still holds whatever it was created with while
  the volume has `C:`.
* `zfs.efi` handles one device, has no timestamps or volume size, and has only
  been run on VMware. Windows 11 has not been tried with it.
* There is no flush for the borrowed vdev (`vdev_disk.c`), so a hard reset can
  lose recent writes.
