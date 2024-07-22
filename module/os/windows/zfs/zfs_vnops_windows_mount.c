/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2019 Jorgen Lundman <lundman@lundman.net>
 */
#define	INITGUID
#include <Ntifs.h>
#include <intsafe.h>
#include <ntddvol.h>
#include <ntdddisk.h>
#include <mountmgr.h>
#include <sys/cred.h>
#include <sys/vnode.h>
#include <sys/zfs_dir.h>
#include <sys/zfs_ioctl.h>
#include <sys/fs/zfs.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/spa.h>
#include <sys/txg.h>
#include <sys/dbuf.h>
#include <sys/zap.h>
#include <sys/sa.h>
#include <sys/zfs_vnops.h>
#include <sys/stat.h>

#include <sys/unistd.h>
#include <sys/uuid.h>

#include <sys/types.h>
#include <sys/zfs_mount.h>

#include <sys/zfs_windows.h>
#include <sys/driver_extension.h>

#undef _NTDDK_

#include <wdmsec.h>
#pragma comment(lib, "wdmsec.lib")


extern int getzfsvfs(const char *dsname, zfsvfs_t **zfvp);

uint64_t zfs_disable_removablemedia = 0;
ZFS_MODULE_RAW(zfs, disable_removablemedia, zfs_disable_removablemedia,
    U64, ZMOD_RW, 0, "Disable Removable Media");

extern kmem_cache_t *znode_cache;

/*
 * Jump through the hoops needed to make a mount happen.
 *
 * Create a new Volume name
 * Register a new unknown device
 * Assign volume name
 * Register device as disk
 * fill in disk information
 * broadcast information
 */

NTSTATUS
mountmgr_add_drive_letter(PDEVICE_OBJECT mountmgr, PUNICODE_STRING devpath)
{
	NTSTATUS Status;
	ULONG mmdltsize;
	MOUNTMGR_DRIVE_LETTER_TARGET* mmdlt;
	MOUNTMGR_DRIVE_LETTER_INFORMATION mmdli;

	mmdltsize = offsetof(MOUNTMGR_DRIVE_LETTER_TARGET, DeviceName[0])
	    + devpath->Length;

	mmdlt = kmem_alloc(mmdltsize, KM_SLEEP);

	mmdlt->DeviceNameLength = devpath->Length;
	RtlCopyMemory(&mmdlt->DeviceName, devpath->Buffer, devpath->Length);
	dprintf("mmdlt = %.*S\n",
	    (int)(mmdlt->DeviceNameLength / sizeof (WCHAR)), mmdlt->DeviceName);

	Status = dev_ioctl(mountmgr, IOCTL_MOUNTMGR_NEXT_DRIVE_LETTER,
	    mmdlt, mmdltsize, &mmdli,
	    sizeof (MOUNTMGR_DRIVE_LETTER_INFORMATION), FALSE, NULL);

	if (!NT_SUCCESS(Status))
		dprintf("IOCTL_MOUNTMGR_NEXT_DRIVE_LETTER returned %08lx\n",
		    Status);
	else
		dprintf("DriveLetterWasAssigned = %u, "
		    "CurrentDriveLetter = %c\n",
		    mmdli.DriveLetterWasAssigned, mmdli.CurrentDriveLetter);

	kmem_free(mmdlt, mmdltsize);

	return (Status);
}

/*
 * check if valid mountpoint, like \DosDevices\X:
 */
BOOLEAN
MOUNTMGR_IS_DRIVE_LETTER_A(char *mountpoint)
{
	UNICODE_STRING wc_mpt;
	wchar_t buf[PATH_MAX];
	mbstowcs(buf, mountpoint, sizeof (buf));
	RtlInitUnicodeString(&wc_mpt, buf);
	return (MOUNTMGR_IS_DRIVE_LETTER(&wc_mpt));
}

/*
 * check if valid mountpoint, like \??\Volume{abc}
 */
BOOLEAN
MOUNTMGR_IS_VOLUME_NAME_A(char *mountpoint)
{
	UNICODE_STRING wc_mpt;
	wchar_t buf[PATH_MAX];
	mbstowcs(buf, mountpoint, sizeof (buf));
	RtlInitUnicodeString(&wc_mpt, buf);
	return (MOUNTMGR_IS_VOLUME_NAME(&wc_mpt));
}

/*
 * Returns the last mountpoint for the device (devpath) (unfiltered)
 * This is either \DosDevices\X: or \??\Volume{abc} in most cases
 * If only_driveletter or only_volume_name is set TRUE,
 * every mountpoint will be checked with MOUNTMGR_IS_DRIVE_LETTER or
 * MOUNTMGR_IS_VOLUME_NAME and discarded if not valid
 * only_driveletter and only_volume_name are mutual exclusive
 */
NTSTATUS
mountmgr_get_mountpoint(PDEVICE_OBJECT mountmgr,
    PUNICODE_STRING devpath, PUNICODE_STRING savename, BOOLEAN only_driveletter,
    BOOLEAN only_volume_name)
{
	MOUNTMGR_MOUNT_POINT point = { 0 };
	MOUNTMGR_MOUNT_POINTS points;
	PMOUNTMGR_MOUNT_POINTS ppoints = NULL;
	int len;
	NTSTATUS Status;

	if (only_driveletter && only_volume_name)
		return (STATUS_INVALID_PARAMETER);

	ppoints = &points;
	Status = dev_ioctl(mountmgr, IOCTL_MOUNTMGR_QUERY_POINTS,
	    &point, sizeof (MOUNTMGR_MOUNT_POINT), ppoints,
	    sizeof (MOUNTMGR_MOUNT_POINTS), FALSE, NULL);

	if (Status == STATUS_BUFFER_OVERFLOW) {
		len = points.Size;
		ppoints = kmem_alloc(len, KM_SLEEP);
		Status = dev_ioctl(mountmgr, IOCTL_MOUNTMGR_QUERY_POINTS,
		    &point, sizeof (MOUNTMGR_MOUNT_POINT),
		    ppoints, len, FALSE, NULL);

	}
	dprintf("IOCTL_MOUNTMGR_QUERY_POINTS return %lx - looking for '%wZ'\n",
	    Status, devpath);
	if (Status == STATUS_SUCCESS) {
		for (int Index = 0;
		    Index < ppoints->NumberOfMountPoints;
		    Index++) {
			PMOUNTMGR_MOUNT_POINT ipoint =
			    ppoints->MountPoints + Index;
			PWCHAR DeviceName =
			    (PWCHAR)((PUCHAR)ppoints +
			    ipoint->DeviceNameOffset);
			PWCHAR SymbolicLinkName =
			    (PWCHAR)((PUCHAR)ppoints +
			    ipoint->SymbolicLinkNameOffset);

			// Why is this hackery needed, we should be able
			// to lookup the drive letter from volume name
			dprintf("   point %d: '%.*S' '%.*S'\n", Index,
			    (int)(ipoint->DeviceNameLength / sizeof (WCHAR)),
			    DeviceName,
			    (int)(ipoint->SymbolicLinkNameLength /
			    sizeof (WCHAR)),
			    SymbolicLinkName);
			if (wcsncmp(DeviceName, devpath->Buffer,
			    ipoint->DeviceNameLength / sizeof (WCHAR)) == 0) {

				RtlUnicodeStringCbCopyStringN(savename,
				    SymbolicLinkName,
				    ipoint->SymbolicLinkNameLength);
				// Might as well null terminate.
				savename->Buffer[
				    ipoint->SymbolicLinkNameLength /
				    sizeof (WCHAR)] = 0;

				if (only_driveletter &&
				    !MOUNTMGR_IS_DRIVE_LETTER(savename))
					savename->Length = 0;
				else if (only_volume_name &&
				    !MOUNTMGR_IS_VOLUME_NAME(savename))
					savename->Length = 0;

				if (MOUNTMGR_IS_DRIVE_LETTER(savename) ||
				    MOUNTMGR_IS_VOLUME_NAME(savename))
					break;
			}
		}
	}

	if (ppoints != NULL) kmem_free(ppoints, len);
	return (STATUS_SUCCESS);
}

#define MOUNTMGR_IS_DOSDEVICES(s, l) (   \
    (l) >= 26 &&                \
    (s)[0] == '\\' &&           \
    (s)[1] == 'D' &&            \
    (s)[2] == 'o' &&            \
    (s)[3] == 's' &&            \
    (s)[4] == 'D' &&            \
    (s)[5] == 'e' &&            \
    (s)[6] == 'v' &&            \
    (s)[7] == 'i' &&            \
    (s)[8] == 'c' &&            \
    (s)[9] == 'e' &&            \
    (s)[10] == 's' &&           \
    (s)[11] == '\\' &&          \
    (s)[12] >= 'A' &&           \
    (s)[12] <= 'Z' &&           \
    (s)[13] == ':')

NTSTATUS
mountmgr_get_mountpoint2(PDEVICE_OBJECT mountmgr,
    PUNICODE_STRING devpath,
    PUNICODE_STRING symbolicname,
    PUNICODE_STRING mountpoint,
    BOOLEAN stop_when_found)
{
	MOUNTMGR_MOUNT_POINT point = { 0 };
	MOUNTMGR_MOUNT_POINTS points;
	PMOUNTMGR_MOUNT_POINTS ppoints = NULL;
	int len;
	NTSTATUS Status;
	int found = 0;

	ppoints = &points;
	Status = dev_ioctl(mountmgr, IOCTL_MOUNTMGR_QUERY_POINTS,
	    &point, sizeof (MOUNTMGR_MOUNT_POINT), ppoints,
	    sizeof (MOUNTMGR_MOUNT_POINTS), FALSE, NULL);

	if (Status == STATUS_BUFFER_OVERFLOW) {
		len = points.Size;
		ppoints = kmem_alloc(len, KM_SLEEP);
		Status = dev_ioctl(mountmgr, IOCTL_MOUNTMGR_QUERY_POINTS,
		    &point, sizeof (MOUNTMGR_MOUNT_POINT),
		    ppoints, len, FALSE, NULL);

	}
	dprintf("IOCTL_MOUNTMGR_QUERY_POINTS return %lx - looking for '%wZ'\n",
	    Status, devpath);
	if (Status == STATUS_SUCCESS) {
		for (int Index = 0;
		    Index < ppoints->NumberOfMountPoints;
		    Index++) {
			PMOUNTMGR_MOUNT_POINT ipoint =
			    ppoints->MountPoints + Index;
			PWCHAR DeviceName =
			    (PWCHAR)((PUCHAR)ppoints +
			    ipoint->DeviceNameOffset);
			PWCHAR SymbolicLinkName =
			    (PWCHAR)((PUCHAR)ppoints +
			    ipoint->SymbolicLinkNameOffset);
			// UniqueId is a binary blob.

			// Why is this hackery needed, we should be able
			// to lookup the drive letter from volume name
			dprintf("   point %d: '%.*S' '%.*S'\n", Index,
			    (int)(ipoint->DeviceNameLength / sizeof (WCHAR)),
			    DeviceName,
			    (int)(ipoint->SymbolicLinkNameLength /
			    sizeof (WCHAR)),
			    SymbolicLinkName);

			if (wcsncmp(DeviceName, devpath->Buffer,
			    ipoint->DeviceNameLength / sizeof (WCHAR)) == 0) {

				if (mountpoint != NULL &&
				    MOUNTMGR_IS_DOSDEVICES(SymbolicLinkName, ipoint->SymbolicLinkNameLength)) {
					// Mountpoint
					RtlUnicodeStringCbCopyStringN(mountpoint,
					    SymbolicLinkName,
					    ipoint->SymbolicLinkNameLength);
					// Might as well null terminate.
					mountpoint->Buffer[
					    ipoint->SymbolicLinkNameLength /
					    sizeof (WCHAR)] = 0;
					found++;
				} else if (symbolicname != NULL) {
					// SymbolicLinkName
					RtlUnicodeStringCbCopyStringN(symbolicname,
					    SymbolicLinkName,
					    ipoint->SymbolicLinkNameLength);
					// Might as well null terminate.
					symbolicname->Buffer[
					    ipoint->SymbolicLinkNameLength /
					    sizeof (WCHAR)] = 0;
					found++;
				}

				if (stop_when_found && found == 2)
					break;
			} // DeviceName match
		} // for
	}

	if (ppoints != NULL) kmem_free(ppoints, len);
	return (STATUS_SUCCESS);
}

/*
 * Returns the last valid mountpoint of the device according
 * to MOUNTMGR_IS_DRIVE_LETTER()
 */
NTSTATUS
mountmgr_get_drive_letter(DEVICE_OBJECT *mountmgr,
    PUNICODE_STRING devpath, PUNICODE_STRING savename)
{
	return (mountmgr_get_mountpoint(mountmgr, devpath, savename,
	    TRUE, FALSE));
}

/*
 * Returns the last valid mountpoint of the device according
 * to MOUNTMGR_IS_VOLUME_NAME()
 */
NTSTATUS
mountmgr_get_volume_name_mountpoint(PDEVICE_OBJECT mountmgr,
    PUNICODE_STRING devpath, PUNICODE_STRING savename)
{
	return (mountmgr_get_mountpoint(mountmgr, devpath, savename,
	    FALSE, TRUE));
}

NTSTATUS
SendIoctlToMountManager(__in ULONG IoControlCode, __in PVOID InputBuffer,
    __in ULONG Length, __out PVOID OutputBuffer,
    __in ULONG OutputLength)
{
	NTSTATUS status;
	UNICODE_STRING mountManagerName;
	PFILE_OBJECT mountFileObject;
	PDEVICE_OBJECT mountDeviceObject;
	PIRP irp;
	KEVENT driverEvent;
	IO_STATUS_BLOCK iosb;

	RtlInitUnicodeString(&mountManagerName, MOUNTMGR_DEVICE_NAME);

	status = IoGetDeviceObjectPointer(&mountManagerName,
	    FILE_READ_ATTRIBUTES,
	    &mountFileObject, &mountDeviceObject);

	if (!NT_SUCCESS(status)) {
		dprintf("  IoGetDeviceObjectPointer failed: 0x%lx\n", status);
		return (status);
	}

	KeInitializeEvent(&driverEvent, NotificationEvent, FALSE);

	irp = IoBuildDeviceIoControlRequest(IoControlCode, mountDeviceObject,
	    InputBuffer, Length, OutputBuffer,
	    OutputLength, FALSE, &driverEvent, &iosb);

	if (irp == NULL) {
		dprintf("  IoBuildDeviceIoControlRequest failed\n");
		ObDereferenceObject(mountFileObject);
		return (STATUS_INSUFFICIENT_RESOURCES);
	}

	status = IoCallDriver(mountDeviceObject, irp);

	if (status == STATUS_PENDING) {
		KeWaitForSingleObject(&driverEvent, Executive, KernelMode,
		    FALSE, NULL);
	}
	status = iosb.Status;

	ObDereferenceObject(mountFileObject);
	// Don't dereference mountDeviceObject, mountFileObject is enough

	if (NT_SUCCESS(status)) {
		dprintf("  IoCallDriver success\n");
	} else {
		dprintf("  IoCallDriver failed: 0x%lx\n", status);
	}

	return (status);
}

NTSTATUS
MountMgrChangeNotify(void)
{
	NTSTATUS status;
	ULONG length;
	MOUNTMGR_CHANGE_NOTIFY_INFO chinfo_in;
	MOUNTMGR_CHANGE_NOTIFY_INFO chinfo_out;

	dprintf("=> MountMgrChangeNotify\n");

	length = sizeof (MOUNTMGR_CHANGE_NOTIFY_INFO);

	status = SendIoctlToMountManager(IOCTL_MOUNTMGR_CHANGE_NOTIFY,
	    &chinfo_in, length, &chinfo_out, length);

	if (NT_SUCCESS(status))
		dprintf("  IoCallDriver success\n");
	else
		dprintf("  IoCallDriver failed: 0x%lx\n", status);

	dprintf("<= MountMgrChangeNotify\n");

	return (status);
}

NTSTATUS
SendVolumeArrivalNotification(PUNICODE_STRING DeviceName)
{
	NTSTATUS status;
	PMOUNTMGR_TARGET_NAME targetName;
	ULONG length;

	dprintf("=> SendVolumeArrivalNotification: '%wZ'\n", DeviceName);

	length = sizeof (MOUNTMGR_TARGET_NAME) + DeviceName->Length - 1;
	targetName = ExAllocatePoolWithTag(PagedPool, length, 'ZFSV');

	if (targetName == NULL) {
		dprintf("  can't allocate MOUNTMGR_TARGET_NAME\n");
		return (STATUS_INSUFFICIENT_RESOURCES);
	}

	RtlZeroMemory(targetName, length);

	targetName->DeviceNameLength = DeviceName->Length;
	RtlCopyMemory(targetName->DeviceName, DeviceName->Buffer,
	    DeviceName->Length);

	status = SendIoctlToMountManager(
	    IOCTL_MOUNTMGR_VOLUME_ARRIVAL_NOTIFICATION,
	    targetName, length, NULL, 0);

	if (NT_SUCCESS(status)) {
		dprintf("  IoCallDriver success\n");
	} else {
		dprintf("  IoCallDriver failed: 0x%lx\n", status);
	}

	ExFreePool(targetName);

	dprintf("<= SendVolumeArrivalNotification\n");

	return (status);
}

NTSTATUS
SendVolumeRemovalNotification(PUNICODE_STRING DeviceName)
{
	NTSTATUS status;
	PMOUNTMGR_TARGET_NAME targetName;
	ULONG length;

	dprintf("=> SendVolumeRemovalNotification: '%wZ'\n", DeviceName);

	length = sizeof (MOUNTMGR_TARGET_NAME) + DeviceName->Length - 1;
	targetName = ExAllocatePoolWithTag(PagedPool, length, 'ZFSV');

	if (targetName == NULL) {
		dprintf("  can't allocate MOUNTMGR_TARGET_NAME\n");
		return (STATUS_INSUFFICIENT_RESOURCES);
	}

	RtlZeroMemory(targetName, length);

	targetName->DeviceNameLength = DeviceName->Length;
	RtlCopyMemory(targetName->DeviceName, DeviceName->Buffer,
	    DeviceName->Length);

#ifndef IOCTL_MOUNTMGR_VOLUME_REMOVAL_NOTIFICATION
#define	IOCTL_MOUNTMGR_VOLUME_REMOVAL_NOTIFICATION  \
	CTL_CODE(MOUNTMGRCONTROLTYPE, 22, METHOD_BUFFERED, FILE_READ_ACCESS)
#endif

	status = SendIoctlToMountManager(
	    IOCTL_MOUNTMGR_VOLUME_REMOVAL_NOTIFICATION,
	    targetName, length, NULL, 0);

	if (NT_SUCCESS(status)) {
		dprintf("  IoCallDriver success\n");
	} else {
		dprintf("  IoCallDriver failed: 0x%lx\n", status);
	}

	ExFreePool(targetName);

	dprintf("<= SendVolumeArrivalNotification\n");

	return (status);
}

NTSTATUS
RegisterDeviceInterface(__in PDRIVER_OBJECT DriverObject,
    __in PDEVICE_OBJECT DeviceObject, __in mount_t *Dcb)
{
	PDEVICE_OBJECT	pnpDeviceObject = NULL;
	NTSTATUS		status;

	/* This creates the PDO - PhysicalDeviceObject */
	status = IoReportDetectedDevice(
	    DriverObject,
	    InterfaceTypeUndefined,
	    0xFFFFFFFF, // 0
	    0xFFFFFFFF, // 0
	    NULL,
	    NULL,
	    FALSE,
	    &pnpDeviceObject);

	if (NT_SUCCESS(status)) {
		dprintf("  IoReportDetectedDevice success\n");
	} else {
		dprintf("  IoReportDetectedDevice failed: 0x%lx\n", status);
		return (status);
	}

	Dcb->PhysicalDeviceObject = pnpDeviceObject;

	Dcb->AttachedDevice = IoAttachDeviceToDeviceStack(DeviceObject, 
	    pnpDeviceObject);

	if (Dcb->AttachedDevice != NULL) {
		dprintf("  IoAttachDeviceToDeviceStack success\n");
	} else {
		dprintf("  IoAttachDeviceToDeviceStack failed\n");
	}

	status = IoRegisterDeviceInterface(
	    pnpDeviceObject,
//	    &GUID_DEVINTERFACE_DISK,
	    &GUID_DEVINTERFACE_VOLUME,
	    NULL,
	    &Dcb->deviceInterfaceName);  // device_name checks out

	if (NT_SUCCESS(status)) {
		dprintf("  IoRegisterDeviceInterface success: %wZ\n",
		    &Dcb->deviceInterfaceName);
	} else {
		dprintf("  IoRegisterDeviceInterface failed: 0x%lx\n", status);
		return (status);
	}

	status = IoSetDeviceInterfaceState(&Dcb->deviceInterfaceName, TRUE);

	if (NT_SUCCESS(status)) {
		dprintf("  IoSetDeviceInterfaceState success\n");
	} else {
		dprintf("  IoSetDeviceInterfaceState failed: 0x%lx\n", status);
		return (status);
	}

	status = IoRegisterDeviceInterface(
	    pnpDeviceObject,
	    &MOUNTDEV_MOUNTED_DEVICE_GUID,
	    NULL,
	    &Dcb->fsInterfaceName);

	if (NT_SUCCESS(status)) {
		dprintf("  IoRegisterDeviceInterface success: %wZ\n",
		    &Dcb->fsInterfaceName);
	} else {
		dprintf("  IoRegisterDeviceInterface failed: 0x%lx\n", status);
		return (status);
	}

	status = IoSetDeviceInterfaceState(&Dcb->fsInterfaceName, TRUE);

	if (NT_SUCCESS(status)) {
		dprintf("  IoSetDeviceInterfaceState success\n");
	} else {
		dprintf("  IoSetDeviceInterfaceState failed: 0x%lx\n", status);
		return (status);
	}

#if 0
	status = IoRegisterDeviceInterface(
	    pnpDeviceObject,
	    &GUID_DEVINTERFACE_VOLUME,
	    NULL,
	    &Dcb->volumeInterfaceName
	);

	if (!NT_SUCCESS(status)) {
		dprintf("Failed to register GUID_DEVINTERFACE_VOLUME: %08x\n", status);
	}

	// Enable the device interface
	status = IoSetDeviceInterfaceState(&Dcb->volumeInterfaceName, TRUE);
	if (!NT_SUCCESS(status)) {
		KdPrint(("Failed to enable GUID_DEVINTERFACE_VOLUME: %08x\n", status));
	}
#endif

	return (status);
}

NTSTATUS
SendVolumeCreatePoint(__in PUNICODE_STRING DeviceName,
    __in PUNICODE_STRING MountPoint)
{
	NTSTATUS status;
	PMOUNTMGR_CREATE_POINT_INPUT point;
	ULONG length;

	dprintf("=> SendVolumeCreatePoint\n");

	length = sizeof (MOUNTMGR_CREATE_POINT_INPUT) + MountPoint->Length +
	    DeviceName->Length;
	point = ExAllocatePoolWithTag(PagedPool, length, 'ZFSV');

	if (point == NULL) {
		dprintf("  can't allocate MOUNTMGR_CREATE_POINT_INPUT\n");
		return (STATUS_INSUFFICIENT_RESOURCES);
	}

	RtlZeroMemory(point, length);

	dprintf("  DeviceName: %wZ\n", DeviceName);
	point->DeviceNameOffset = sizeof (MOUNTMGR_CREATE_POINT_INPUT);
	point->DeviceNameLength = DeviceName->Length;
	RtlCopyMemory((PCHAR)point + point->DeviceNameOffset,
	    DeviceName->Buffer,
	    DeviceName->Length);

	dprintf("  MountPoint: %wZ\n", MountPoint);
	point->SymbolicLinkNameOffset =
	    point->DeviceNameOffset + point->DeviceNameLength;
	point->SymbolicLinkNameLength = MountPoint->Length;
	RtlCopyMemory((PCHAR)point + point->SymbolicLinkNameOffset,
	    MountPoint->Buffer, MountPoint->Length);

	status = SendIoctlToMountManager(IOCTL_MOUNTMGR_CREATE_POINT, point,
	    length, NULL, 0);

	if (NT_SUCCESS(status)) {
		dprintf("  IoCallDriver success\n");
	} else {
		dprintf("  IoCallDriver failed: 0x%lx\n", status);
	}

	ExFreePool(point);

	dprintf("<= SendVolumeCreatePoint\n");

	return (status);
}

NTSTATUS
SendVolumeCreatePointX(__in PUNICODE_STRING DeviceName,
    __in PUNICODE_STRING MountPoint)
{
	NTSTATUS status;
	PMOUNTMGR_VOLUME_MOUNT_POINT point;
	ULONG length;

	dprintf("=> SendVolumeCreatePointX\n");

	length = sizeof (MOUNTMGR_VOLUME_MOUNT_POINT) + MountPoint->Length +
	    DeviceName->Length;
	point = ExAllocatePoolWithTag(PagedPool, length, 'ZFSV');

	if (point == NULL) {
		dprintf("  can't allocate MOUNTMGR_VOLUME_MOUNT_POINT\n");
		return (STATUS_INSUFFICIENT_RESOURCES);
	}

	RtlZeroMemory(point, length);

	dprintf("  DeviceName: %wZ\n", DeviceName);
	point->TargetVolumeNameOffset = sizeof (MOUNTMGR_VOLUME_MOUNT_POINT);
	point->TargetVolumeNameLength = DeviceName->Length;
	RtlCopyMemory((PCHAR)point + point->TargetVolumeNameOffset,
	    DeviceName->Buffer,
	    DeviceName->Length);

	dprintf("  MountPoint: %wZ\n", MountPoint);
	point->SourceVolumeNameOffset =
	    point->TargetVolumeNameOffset + point->TargetVolumeNameLength;
	point->SourceVolumeNameLength = MountPoint->Length;
	RtlCopyMemory((PCHAR)point + point->SourceVolumeNameOffset,
	    MountPoint->Buffer, MountPoint->Length);

	status = SendIoctlToMountManager(
	    IOCTL_MOUNTMGR_VOLUME_MOUNT_POINT_CREATED,
	    point, length, NULL, 0);

	if (NT_SUCCESS(status)) {
		dprintf("  IoCallDriver success\n");
	} else {
		dprintf("  IoCallDriver failed: 0x%lx\n", status);
	}

	ExFreePool(point);

	dprintf("<= SendVolumeCreatePointX\n");

	return (status);
}

NTSTATUS
SendVolumeDeletePoints(__in PUNICODE_STRING MountPoint,
    __in PUNICODE_STRING DeviceName)
{
	NTSTATUS status;
	PMOUNTMGR_MOUNT_POINT point;
	PMOUNTMGR_MOUNT_POINTS deletedPoints;
	ULONG length;
	ULONG olength;

	dprintf("=> SendVolumeDeletePoints: '%wZ'\n", DeviceName);

	if (_wcsnicmp(L"\\DosDevices\\", MountPoint->Buffer, 12)) {
		dprintf("Not a drive letter, skipping\n");
		return (STATUS_SUCCESS);
	}

	length = sizeof (MOUNTMGR_MOUNT_POINT) + MountPoint->Length;
	if (DeviceName != NULL) {
		length += DeviceName->Length;
	}
	point = kmem_alloc(length, KM_SLEEP);

	if (point == NULL) {
		dprintf("  can't allocate MOUNTMGR_CREATE_POINT_INPUT\n");
		return (STATUS_INSUFFICIENT_RESOURCES);
	}

	olength = sizeof (MOUNTMGR_MOUNT_POINTS) + 1024;
	deletedPoints = kmem_alloc(olength, KM_SLEEP);
	if (deletedPoints == NULL) {
		dprintf("  can't allocate PMOUNTMGR_MOUNT_POINTS\n");
		kmem_free(point, length);
		return (STATUS_INSUFFICIENT_RESOURCES);
	}

	RtlZeroMemory(point, length); // kmem_zalloc
	RtlZeroMemory(deletedPoints, olength);

	dprintf("  MountPoint: %wZ\n", MountPoint);
	point->SymbolicLinkNameOffset = sizeof (MOUNTMGR_MOUNT_POINT);
	point->SymbolicLinkNameLength = MountPoint->Length;
	RtlCopyMemory((PCHAR)point + point->SymbolicLinkNameOffset,
	    MountPoint->Buffer, MountPoint->Length);
	if (DeviceName != NULL) {
		dprintf("  DeviceName: %wZ\n", DeviceName);
		point->DeviceNameOffset =
		    point->SymbolicLinkNameOffset +
		    point->SymbolicLinkNameLength;
		point->DeviceNameLength = DeviceName->Length;
		RtlCopyMemory((PCHAR)point + point->DeviceNameOffset,
		    DeviceName->Buffer, DeviceName->Length);
	}

	// Only symbolic link can be deleted with IOCTL_MOUNTMGR_DELETE_POINTS.
	// If any other entry is specified, the mount manager will ignore
	// subsequent IOCTL_MOUNTMGR_VOLUME_ARRIVAL_NOTIFICATION for the
	// same volume ID.
	status = SendIoctlToMountManager(IOCTL_MOUNTMGR_DELETE_POINTS, point,
	    length, deletedPoints, olength);

	if (NT_SUCCESS(status)) {
		dprintf("  IoCallDriver success, %ld mount points deleted.\n",
		    deletedPoints->NumberOfMountPoints);
	} else {
		dprintf("  IoCallDriver failed: 0x%lx\n", status);
	}

	kmem_free(point, length);
	kmem_free(deletedPoints, olength);

	dprintf("<= SendVolumeDeletePoints\n");

	return (status);
}

void
zfs_release_mount(mount_t *zmo)
{
	FreeUnicodeString(&zmo->symlink_name);
	FreeUnicodeString(&zmo->device_name);
	FreeUnicodeString(&zmo->fs_name);
	FreeUnicodeString(&zmo->uuid);
	FreeUnicodeString(&zmo->mountpoint);
	FreeUnicodeString(&zmo->deviceInterfaceName);
	FreeUnicodeString(&zmo->fsInterfaceName);
	FreeUnicodeString(&zmo->volumeInterfaceName);

	if (zmo->vpb) {
		zmo->vpb->DeviceObject = NULL;
		zmo->vpb->RealDevice = NULL;
		zmo->vpb->Flags = 0;
	}
}


static void
io_verify_volume_impl(void *arg)
{
	IoVerifyVolume(arg, FALSE);
}

int
zfs_windows_mount(zfs_cmd_t *zc)
{
	dprintf("%s: '%s' '%s'\n", __func__, zc->zc_name, zc->zc_value);
	NTSTATUS status;
	uuid_t uuid;
	char uuid_a[UUID_PRINTABLE_STRING_LENGTH];
	// PDEVICE_OBJECT pdo = NULL;
	PDEVICE_OBJECT diskDeviceObject = NULL;
	// PDEVICE_OBJECT fsDeviceObject = NULL;
	OpenZFS_Driver_Extension *DriverExtension =
	    (OpenZFS_Driver_Extension *)IoGetDriverObjectExtension(WIN_DriverObject, WIN_DriverObject);

	/*
	 * We expect mountpath (zv_value) to be already sanitised, ie, Windows
	 * translated paths. So it should be on this style:
	 * "\\??\\c:"  mount as drive letter C:
	 * "\\??\\?:"  mount as first available drive letter
	 * "\\??\\c:\\BOOM"  mount as drive letter C:\BOOM
	 */
	int mplen = strlen(zc->zc_value);
	if ((mplen < 6) ||
	    strncmp("\\??\\", zc->zc_value, 4)) {
		dprintf("%s: mountpoint '%s' does not start with \\??\\x:",
		    __func__, zc->zc_value);
		return (EINVAL);
	}

	zfs_vfs_uuid_gen(zc->zc_name, uuid);
	zfs_vfs_uuid_unparse(uuid, uuid_a);

	char buf[PATH_MAX];
	// snprintf(buf, sizeof (buf), "\\Device\\ZFS{%s}", uuid_a);
	// L"\\Device\\Volume"
	// WCHAR diskDeviceNameBuf[MAXIMUM_FILENAME_LENGTH];
	// WCHAR fsDeviceNameBuf[MAXIMUM_FILENAME_LENGTH]; // L"\\Device\\ZFS"
	// L"\\DosDevices\\Global\\Volume"
	// WCHAR symbolicLinkNameBuf[MAXIMUM_FILENAME_LENGTH];
	UNICODE_STRING diskDeviceName;
	UNICODE_STRING fsDeviceName;
	UNICODE_STRING symbolicLinkTarget;
	UNICODE_STRING arcLinkTarget;

	ANSI_STRING pants;
	ULONG deviceCharacteristics;
	deviceCharacteristics = 0; // FILE_DEVICE_IS_MOUNTED;
	/* Allow $recycle.bin - don't set removable. */
	if (!zfs_disable_removablemedia)
		deviceCharacteristics |= FILE_REMOVABLE_MEDIA;

	snprintf(buf, sizeof (buf), "\\Device\\Volume{%s}", uuid_a);

	pants.Buffer = buf;
	pants.Length = strlen(buf);
	pants.MaximumLength = PATH_MAX;
	status = RtlAnsiStringToUnicodeString(&diskDeviceName, &pants, TRUE);
	dprintf("%s: new devstring '%wZ'\n", __func__, &diskDeviceName);
#if 1

	/* This creates the FDO - Functional Device Object */
	status = IoCreateDeviceSecure(WIN_DriverObject,
	    sizeof (mount_t),
	    &diskDeviceName,
	    FILE_DEVICE_DISK,
	    deviceCharacteristics | FILE_DEVICE_IS_MOUNTED,
	    FALSE,
	    &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R,
	    NULL,
	    &diskDeviceObject);

	if (status != STATUS_SUCCESS) {
		dprintf("IoCreateDeviceSecure returned %08lx\n", status);
		return (status);
	}

#else
	status = IoCreateDevice(WIN_DriverObject, sizeof (mount_t),
	    NULL, FILE_DEVICE_DISK,
	    FILE_AUTOGENERATED_DEVICE_NAME | FILE_DEVICE_SECURE_OPEN, FALSE,
	    &diskDeviceObject);
	xprintf("Created ChildDeviceObject %p\n", diskDeviceObject);
#endif
	if (status != STATUS_SUCCESS) {
		dprintf("IoCreateDeviceSecure returned %08lx\n", status);
		return (status);
	}

	mount_t *zmo_dcb = diskDeviceObject->DeviceExtension;

	zmo_dcb->type = MOUNT_TYPE_DCB;
	zmo_dcb->size = sizeof (mount_t);

	zfs_vfs_uuid_gen(zc->zc_name, zmo_dcb->rawuuid);

	vfs_setfsprivate(zmo_dcb, NULL);
	dprintf("%s: created dcb at %p asked for size %llu\n",
	    __func__, zmo_dcb, sizeof (mount_t));
	AsciiStringToUnicodeString(uuid_a, &zmo_dcb->uuid);
	// Should we keep the name with slashes like "BOOM/lower" or
	// just "lower". Turns out the name in Explorer only
	// works for 4 chars or lower. Why?
	AsciiStringToUnicodeString(zc->zc_name, &zmo_dcb->name);
	RtlDuplicateUnicodeString(0, &diskDeviceName, &zmo_dcb->device_name);

	// strlcpy(zc->zc_value, buf, sizeof (zc->zc_value));
	zmo_dcb->FunctionalDeviceObject = diskDeviceObject;
	// So DeviceRelations can return it
	DriverExtension->ChildDeviceObject = diskDeviceObject;
	// So AddDevice can match us
	DriverExtension->AddDeviceObject = diskDeviceObject;

	dprintf("New device %p has extension %p\n",
	    diskDeviceObject, zmo_dcb);

	snprintf(buf, sizeof (buf), "\\DosDevices\\Global\\Volume{%s}", uuid_a);
	//snprintf(buf, sizeof (buf), "\\DosDevices\\Global\\E:", uuid_a);
	pants.Buffer = buf;
	pants.Length = strlen(buf);
	pants.MaximumLength = PATH_MAX;
	status = RtlAnsiStringToUnicodeString(&symbolicLinkTarget, &pants,
	    TRUE);
	dprintf("%s: new symlink '%wZ'\n", __func__, &symbolicLinkTarget);
	AsciiStringToUnicodeString(buf, &zmo_dcb->symlink_name);

	snprintf(buf, sizeof (buf), "\\ArcName\\OpenZFS(%s)", uuid_a);
	//snprintf(buf, sizeof (buf), "\\DosDevices\\Global\\E:", uuid_a);
	pants.Buffer = buf;
	pants.Length = strlen(buf);
	pants.MaximumLength = PATH_MAX;
	status = RtlAnsiStringToUnicodeString(&arcLinkTarget, &pants,
	    TRUE);
	dprintf("%s: new symlink '%wZ'\n", __func__, &arcLinkTarget);
	AsciiStringToUnicodeString(buf, &zmo_dcb->arc_name);

	//snprintf(buf, sizeof (buf), "\\Device\\ZFS{%s}", uuid_a);
	snprintf(buf, sizeof (buf), "\\??\\{%s}", uuid_a);
	pants.Buffer = buf;
	pants.Length = strlen(buf);
	pants.MaximumLength = PATH_MAX;
	status = RtlAnsiStringToUnicodeString(&fsDeviceName, &pants, TRUE);
	dprintf("%s: new fsname '%wZ'\n", __func__, &fsDeviceName);
	AsciiStringToUnicodeString(buf, &zmo_dcb->fs_name);

	diskDeviceObject->Flags |= DO_DIRECT_IO;
	diskDeviceObject->Flags |= DO_BUS_ENUMERATED_DEVICE;

	DbgBreakPoint();

	// diskDeviceObject->Flags |= DO_POWER_PAGABLE;
	diskDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

#if 1
	// zmo_dcb->PhysicalDeviceObject = DriverExtension->PhysicalDeviceObject;
//	zmo_dcb->PhysicalDeviceObject = diskDeviceObject;
	// zmo_dcb->FunctionalDeviceObject = DriverExtension->FunctionalDeviceObject;

//	zmo_dcb->LowerDeviceObject = IoAttachDeviceToDeviceStack(diskDeviceObject,
//	    DriverExtension->PhysicalDeviceObject);

//	status = IoReportDetectedDevice(
//	    WIN_DriverObject,
//	    InterfaceTypeUndefined,
//	    -1,
//	    -1,
//	    NULL,
//	    NULL,
//	    FALSE,
//	    &WIN_PhysicalDeviceObject
//	);

//	status = IoRegisterPlugPlayNotification(EventCategoryTargetDeviceChange, 0, FileObject,
//		drvobj, pnp_removal, pdode, &vc->notification_entry);
#if 0
	status = IoRegisterDeviceInterface(
	    diskDeviceObject,
	    &GUID_DEVINTERFACE_DISK,
	    NULL,
	    &zmo_dcb->deviceInterfaceName
	);

	status = IoSetDeviceInterfaceState(&zmo_dcb->deviceInterfaceName, TRUE);
#endif

	zmo_dcb->AttachedDevice = IoAttachDeviceToDeviceStack(diskDeviceObject,
	    DriverExtension->PhysicalDeviceObject);
	// zmo_dcb->AttachedDevice = IoAttachDeviceToDeviceStack(diskDeviceObject,
	//    DriverExtension->FunctionalDeviceObject);

	// zmo->VolumeDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
	IoInvalidateDeviceRelations(DriverExtension->PhysicalDeviceObject, BusRelations);

#else
	status = IoReportDetectedDevice(
	    WIN_DriverObject,
	    InterfaceTypeUndefined,
	    -1,
	    -1,
	    NULL,
	    NULL,
	    FALSE,
	    &diskDeviceObject
	);
#endif
	// Call ZFS and have it setup a mount "zfsvfs"
	// we don't have the vcb yet, but we want to find out mount
	// problems early.
	struct zfs_mount_args mnt_args;
	mnt_args.struct_size = sizeof (struct zfs_mount_args);
	mnt_args.optlen = 0;
	mnt_args.mflag = 0; // Set flags
	mnt_args.fspec = zc->zc_name;

	// zc_cleanup_fd carrier mount flags for now.
	if (zc->zc_cleanup_fd & MNT_RDONLY)
		vfs_setrdonly(zmo_dcb);

#if 0
	if (zc->zc_value[0] != 0) {
		PUNICODE_STRING tmp;
		tmp = &(zmo_dcb->mountpoint.Length);
		AsciiStringToUnicodeString(zc->zc_value, tmp);
	}
#endif
	// Mount will temporarily be pointing to "dcb" until the
	// zfs_vnop_mount() below corrects it to "vcb".
	status = zfs_vfs_mount(zmo_dcb, NULL, (user_addr_t)&mnt_args, NULL);
	dprintf("%s: zfs_vfs_mount() returns %ld\n", __func__, status);

	if (status) {
		IoDeleteDevice(diskDeviceObject);
		return (status);
	}

#if 0
	status = IoRegisterPlugPlayNotification(EventCategoryTargetDeviceChange, 0, FileObject,
	    WIN_DriverObject, pnp_removal, pdode, &zmo_dcb->notification_entry);
	if (!NT_SUCCESS(status))
		dprintf("IoRegisterPlugPlayNotification returned %08lx\n", status);
// #else

	status = RegisterDeviceInterface(WIN_DriverObject,
	    diskDeviceObject, zmo_dcb);

	status = IoCreateSymbolicLink(&symbolicLinkTarget, &diskDeviceName);
	// status = IoCreateSymbolicLink(&zmo_dcb->deviceInterfaceName, &diskDeviceName);

	if (!NT_SUCCESS(status)) {
		IoDeleteDevice(diskDeviceObject);
		dprintf("  IoCreateSymbolicLink returned 0x%lx\n", status);
		return (status);
	}
#else

#endif
	// Check if we are to mount with driveletter, or path
	// We already check that path is "\\??\\" above, and
	// at least 6 chars. Seventh char can be zero, or "/"
	// then zero, for drive only mount.
	if ((zc->zc_value[6] == 0) ||
	    ((zc->zc_value[6] == '/') &&
	    (zc->zc_value[7] == 0))) {
		zmo_dcb->justDriveLetter = B_TRUE;
	} else {
		zmo_dcb->justDriveLetter = B_FALSE;
	}

	// Remember mountpoint path
	// if (zmo_dcb->mountpoint.Buffer)
	//	FreeUnicodeString(&zmo_dcb->mountpoint);

	AsciiStringToUnicodeString(zc->zc_value, &zmo_dcb->mountpoint);

	dprintf("%s: driveletter %d '%wZ'\n",
	    __func__, zmo_dcb->justDriveLetter, &zmo_dcb->mountpoint);

	// Return volume name to userland
	snprintf(zc->zc_value, sizeof (zc->zc_value),
	    "\\DosDevices\\Global\\Volume{%s}", uuid_a);

	// Mark devices as initialized
	// diskDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
	// ObReferenceObject(diskDeviceObject);

	/*
	 * IoVerifyVolume() kicks off quite a large amount of work
	 * which can grow the stack very large. Usually it goes
	 * something like:
	 * IoVerifyVolume()
	 * NtIoCallDriver()
	 * zfs_vnop_mount()
	 * CreateReparsePoint()
	 * NtIoCallDriver()
	 * zfs_vnop_lookup()
	 * zfs_mkdir()
	 * dnode_allocate()
	 * dbuf_dirty()
	 * so any cal to kmem_alloc() which might trigger a
	 * magazine alloc would tip us over.
	 */
	dprintf("Verify Volume\n");
	if (taskq_dispatch(system_taskq, io_verify_volume_impl,
	    diskDeviceObject, TQ_SLEEP) == 0)
		IoVerifyVolume(diskDeviceObject, FALSE);
	
	// Free diskDeviceName
	FreeUnicodeString(&diskDeviceName);

	status = STATUS_SUCCESS;
	return (status);
}

void
InitVpb(__in PVPB Vpb, __in PDEVICE_OBJECT VolumeDevice)
{
	if (Vpb != NULL) {
		Vpb->DeviceObject = VolumeDevice;
		Vpb->VolumeLabelLength =
		    (USHORT)wcslen(VOLUME_LABEL) * sizeof (WCHAR);
		RtlStringCchCopyW(Vpb->VolumeLabel,
		    sizeof (Vpb->VolumeLabel) / sizeof (WCHAR), VOLUME_LABEL);
		Vpb->SerialNumber = 0x19831116;
		Vpb->Flags |= VPB_MOUNTED;
	}
}

NTSTATUS
CreateReparsePoint(POBJECT_ATTRIBUTES poa, PCUNICODE_STRING SubstituteName,
    PCUNICODE_STRING PrintName)
{
	HANDLE hFile;
	IO_STATUS_BLOCK iosb;
	NTSTATUS status;

	dprintf("%s: \n", __func__);

	// this is stalled forever waiting for event of deletion -
	// possibly ZFS doesnt send event?
	status = ZwDeleteFile(poa);
	if (status != STATUS_SUCCESS)
		dprintf("pre-rmdir failed 0x%lx\n", status);
	status = ZwCreateFile(&hFile, FILE_ALL_ACCESS, poa, &iosb, 0, 0, 0,
	    FILE_CREATE, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
	    0, 0);
	if (0 > status)
		return (status);
	dprintf("%s: create ok\n", __func__);

	USHORT cb = 2 * sizeof (WCHAR) +
	    FIELD_OFFSET(REPARSE_DATA_BUFFER,
	    MountPointReparseBuffer.PathBuffer) +
	    SubstituteName->Length + PrintName->Length;
	PREPARSE_DATA_BUFFER prdb =
	    (PREPARSE_DATA_BUFFER) ExAllocatePoolWithTag(PagedPool, cb, 'ZFSM');
	RtlZeroMemory(prdb, cb);
	prdb->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
	prdb->ReparseDataLength = cb - REPARSE_DATA_BUFFER_HEADER_SIZE;
	prdb->MountPointReparseBuffer.SubstituteNameLength =
	    SubstituteName->Length;
	prdb->MountPointReparseBuffer.PrintNameLength = PrintName->Length;
	prdb->MountPointReparseBuffer.PrintNameOffset =
	    SubstituteName->Length + sizeof (WCHAR);
	memcpy(prdb->MountPointReparseBuffer.PathBuffer,
	    SubstituteName->Buffer, SubstituteName->Length);
	memcpy(RtlOffsetToPointer(prdb->MountPointReparseBuffer.PathBuffer,
	    SubstituteName->Length + sizeof (WCHAR)),
	    PrintName->Buffer, PrintName->Length);
	status = ZwFsControlFile(hFile, 0, 0, 0, &iosb,
	    FSCTL_SET_REPARSE_POINT, prdb, cb, 0, 0);
	dprintf("%s: ControlFile %ld / 0x%lx\n", __func__, status, status);

	if (0 > status) {
		static FILE_DISPOSITION_INFORMATION fdi = { TRUE };
		ZwSetInformationFile(hFile, &iosb, &fdi,
		    sizeof (fdi), FileDispositionInformation);
	}
	ZwClose(hFile);
	return (status);
}

NTSTATUS
DeleteReparsePoint(POBJECT_ATTRIBUTES poa)
{
	HANDLE hFile;
	IO_STATUS_BLOCK iosb;
	NTSTATUS status;
	REPARSE_DATA_BUFFER ReparseData;

	dprintf("%s: \n", __func__);

	status = ZwCreateFile(&hFile, FILE_ALL_ACCESS, poa, &iosb, 0, 0, 0,
	    FILE_OPEN_IF, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
	    FILE_OPEN_FOR_BACKUP_INTENT | FILE_OPEN_REPARSE_POINT,
	    0, 0);
	if (0 > status)
		return (status);
	dprintf("%s: create ok\n", __func__);

	memset(&ReparseData, 0, REPARSE_DATA_BUFFER_HEADER_SIZE);
	ReparseData.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;

	status = ZwFsControlFile(hFile, 0, 0, 0, &iosb,
	    FSCTL_DELETE_REPARSE_POINT, &ReparseData,
	    REPARSE_DATA_BUFFER_HEADER_SIZE, NULL, 0);

	ZwClose(hFile);
	return (status);
}

/*
 * go through all mointpoints (IOCTL_MOUNTMGR_QUERY_POINTS)
 * and check if our driveletter is in the list
 * return 1 if yes, otherwise 0
 */
NTSTATUS
mountmgr_is_driveletter_assigned(PDEVICE_OBJECT mountmgr,
    wchar_t driveletter, BOOLEAN *ret)
{
	MOUNTMGR_MOUNT_POINT point = { 0 };
	MOUNTMGR_MOUNT_POINTS points;
	PMOUNTMGR_MOUNT_POINTS ppoints = NULL;
	int len;
	*ret = 0;
	NTSTATUS Status;

	ppoints = &points;
	Status = dev_ioctl(mountmgr, IOCTL_MOUNTMGR_QUERY_POINTS, &point,
	    sizeof (MOUNTMGR_MOUNT_POINT), ppoints,
	    sizeof (MOUNTMGR_MOUNT_POINTS), FALSE, NULL);

	if (Status == STATUS_BUFFER_OVERFLOW) {
		len = points.Size;
		ppoints = kmem_alloc(len, KM_SLEEP);
		Status = dev_ioctl(mountmgr, IOCTL_MOUNTMGR_QUERY_POINTS,
		    &point, sizeof (MOUNTMGR_MOUNT_POINT), ppoints,
		    len, FALSE, NULL);
	}
	dprintf("IOCTL_MOUNTMGR_QUERY_POINTS return %lx - "
	    "looking for driveletter '%c'\n",
	    Status, driveletter);
	if (Status == STATUS_SUCCESS) {
		char mpt_name[PATH_MAX] = { 0 };
		for (int Index = 0;
		    Index < ppoints->NumberOfMountPoints;
		    Index++) {
			PMOUNTMGR_MOUNT_POINT ipoint =
			    ppoints->MountPoints + Index;
			PWCHAR DeviceName =
			    (PWCHAR)((PUCHAR)ppoints +
			    ipoint->DeviceNameOffset);
			PWCHAR SymbolicLinkName =
			    (PWCHAR)((PUCHAR)ppoints +
			    ipoint->SymbolicLinkNameOffset);

			dprintf("   point %d: '%.*S' '%.*S'\n", Index,
			    (int)(ipoint->DeviceNameLength / sizeof (WCHAR)),
			    DeviceName,
			    (int)(ipoint->SymbolicLinkNameLength /
			    sizeof (WCHAR)),
			    SymbolicLinkName);

			ULONG length = 0;
			RtlUnicodeToUTF8N(mpt_name, MAXPATHLEN - 1, &length,
			    SymbolicLinkName,
			    ipoint->SymbolicLinkNameLength);
			mpt_name[length] = 0;
			char c_driveletter;
			wctomb(&c_driveletter, driveletter);
			if (MOUNTMGR_IS_DRIVE_LETTER_A(mpt_name) &&
			    mpt_name[12] == c_driveletter) {
				*ret = 1;
				if (ppoints != NULL) kmem_free(ppoints, len);
				return (STATUS_SUCCESS);
			}
		}
	}

	if (ppoints != NULL) kmem_free(ppoints, len);
	return (Status);
}

/*
 * assign driveletter with IOCTL_MOUNTMGR_CREATE_POINT
 */
NTSTATUS
mountmgr_assign_driveletter(PUNICODE_STRING device_name,
    wchar_t driveletter)
{
	DECLARE_UNICODE_STRING_SIZE(mpt, 16);
	RtlUnicodeStringPrintf(&mpt, L"\\DosDevices\\%c:", driveletter);
	return (SendVolumeCreatePoint(device_name, &mpt));
}


/*
 * assign next free driveletter (D..Z) if mountmgr is offended
 * and refuses to do it
 */
NTSTATUS
SetNextDriveletterManually(PDEVICE_OBJECT mountmgr,
    PUNICODE_STRING device_name)
{
	NTSTATUS status;
	for (wchar_t c = 'D'; c <= 'Z'; c++) {
		BOOLEAN ret;
		status = mountmgr_is_driveletter_assigned(mountmgr, c, &ret);
		if (status == STATUS_SUCCESS && ret == 0) {
			status = mountmgr_assign_driveletter(device_name, c);

			if (status == STATUS_SUCCESS) {
				// prove it
				status =
				    mountmgr_is_driveletter_assigned(mountmgr,
				    c, &ret);
				if (status == STATUS_SUCCESS) {
					if (ret == 1)
						return (STATUS_SUCCESS);
					else
						return
						    (STATUS_VOLUME_DISMOUNTED);
				} else {
					return (status);
				}
			}
		}
	}
	return (status);
}



void
generateGUID(char *pguid)
{
	char *uuid_format = "xxxxxxxx-xxxx-4xxx-Nxxx-xxxxxxxxxxxx";
	char *szHex = "0123456789ABCDEF-";
	int len = strlen(uuid_format);

	for (int i = 0; i < len + 1; i++) {
		int r = rand() % 16;
		char c = ' ';

		switch (uuid_format[i])	{
		case 'x': { c = szHex[r]; }
			break;
		case 'N': { c = szHex[(r & 0x03) | 0x08]; }
			break;
		case '-': { c = '-'; }
			break;
		case '4': { c = '4'; }
			break;
		}

		pguid[i] = (i < len) ? c : 0x00;
	}
}

void
generateVolumeNameMountpoint(wchar_t *vol_mpt)
{
	char GUID[50];
	wchar_t wc_guid[50];
	generateGUID(&GUID);
	mbstowcs(&wc_guid, GUID, 50);
	_snwprintf(vol_mpt, 50, L"\\??\\Volume{%s}\\", wc_guid);
}
NTSTATUS
CreateMountPoint(PUNICODE_STRING sourceVolumeName, PUNICODE_STRING targetVolumeName)
{
	NTSTATUS status;

	// Calculate lengths including null terminators
	ULONG sourceNameLength = sourceVolumeName->Length + sizeof(WCHAR);
	ULONG targetNameLength = targetVolumeName->Length + sizeof(WCHAR);
	ULONG totalLength = sizeof (MOUNTMGR_VOLUME_MOUNT_POINT) + sourceNameLength + targetNameLength;

	// Allocate memory for the MOUNTMGR_VOLUME_MOUNT_POINT structure
	PMOUNTMGR_VOLUME_MOUNT_POINT mountPoint =
	    (PMOUNTMGR_VOLUME_MOUNT_POINT)ExAllocatePoolWithTag(NonPagedPoolNx, totalLength, 'mntp');

	if (!mountPoint)
		return (STATUS_INSUFFICIENT_RESOURCES);

	RtlZeroMemory(mountPoint, totalLength);

	// Set the offsets and lengths
	mountPoint->SourceVolumeNameOffset = sizeof(MOUNTMGR_VOLUME_MOUNT_POINT);
	mountPoint->SourceVolumeNameLength = sourceNameLength;
	mountPoint->TargetVolumeNameOffset = mountPoint->SourceVolumeNameOffset + sourceNameLength;
	mountPoint->TargetVolumeNameLength = targetNameLength;

	// Copy the source volume name and add a null terminator
	RtlCopyMemory(((PUCHAR)mountPoint) + mountPoint->SourceVolumeNameOffset,
	    sourceVolumeName->Buffer, sourceVolumeName->Length);
	*((WCHAR*)(((PUCHAR)mountPoint) + mountPoint->SourceVolumeNameOffset + sourceVolumeName->Length)) = L'\0';

	// Copy the target volume name and add a null terminator
	RtlCopyMemory(((PUCHAR)mountPoint) + mountPoint->TargetVolumeNameOffset,
	    targetVolumeName->Buffer, targetVolumeName->Length);
	*((WCHAR*)(((PUCHAR)mountPoint) + mountPoint->TargetVolumeNameOffset + targetVolumeName->Length)) = L'\0';

	status = SendIoctlToMountManager(
	    IOCTL_MOUNTMGR_VOLUME_MOUNT_POINT_CREATED,
	    mountPoint, totalLength, NULL, 0);

	if (NT_SUCCESS(status)) {
		dprintf("  IoCallDriver success\n");
	} else {
		dprintf("  IoCallDriver failed: 0x%lx\n", status);
	}
	 ExFreePoolWithTag(mountPoint, 'mntp');
	 return (status);
}

typedef struct _MOUNTMGR_CREATE_POINT {
	USHORT SymbolicLinkNameOffset;
	USHORT SymbolicLinkNameLength;
	USHORT DeviceNameOffset;
	USHORT DeviceNameLength;
} MOUNTMGR_CREATE_POINT, *PMOUNTMGR_CREATE_POINT;

NTSTATUS
GPTCreateMountPoint(PUNICODE_STRING SymbolicLinkName, PUNICODE_STRING DeviceName)
{
	NTSTATUS status;
	PMOUNTMGR_CREATE_POINT inputBuffer;
	ULONG inputSize;

	// Calculate the size of the input buffer
	inputSize = sizeof(MOUNTMGR_CREATE_POINT) +
	    SymbolicLinkName->Length + sizeof(WCHAR) +  // +sizeof(WCHAR) for NULL terminator
	    DeviceName->Length + sizeof(WCHAR);         // +sizeof(WCHAR) for NULL terminator

	// Allocate the input buffer
	inputBuffer = (PMOUNTMGR_CREATE_POINT)ExAllocatePoolWithTag(NonPagedPoolNx, inputSize, 'mntp');
	if (!inputBuffer)
		return (STATUS_INSUFFICIENT_RESOURCES);

	// Fill in the input buffer
	inputBuffer->SymbolicLinkNameOffset = sizeof(MOUNTMGR_CREATE_POINT);
	inputBuffer->SymbolicLinkNameLength = SymbolicLinkName->Length;

	inputBuffer->DeviceNameOffset = inputBuffer->SymbolicLinkNameOffset + SymbolicLinkName->Length + sizeof(WCHAR);
	inputBuffer->DeviceNameLength = DeviceName->Length;

	// Copy the symbolic link name and device name into the buffer
	RtlCopyMemory(((UCHAR*)inputBuffer) + inputBuffer->SymbolicLinkNameOffset, SymbolicLinkName->Buffer, SymbolicLinkName->Length);
	*((WCHAR*)(((UCHAR*)inputBuffer) + inputBuffer->SymbolicLinkNameOffset + SymbolicLinkName->Length)) = L'\0';

	RtlCopyMemory(((UCHAR*)inputBuffer) + inputBuffer->DeviceNameOffset,
	    DeviceName->Buffer, DeviceName->Length);
	*((WCHAR*)(((UCHAR*)inputBuffer) + inputBuffer->DeviceNameOffset +
	    DeviceName->Length)) = UNICODE_NULL;

	status = SendIoctlToMountManager(
	    IOCTL_MOUNTMGR_CREATE_POINT,
	    inputBuffer, inputSize, NULL, 0);

	if (NT_SUCCESS(status)) {
		dprintf("  IoCallDriver success\n");
	} else {
		dprintf("  IoCallDriver failed: 0x%lx\n", status);
	}
	 ExFreePoolWithTag(inputBuffer, 'mntp');
	 return (status);

}

typedef struct _MOUNTMGR_VOLUME_MOUNT_POINT_INPUT {
	ULONG SymbolicLinkNameOffset;
	USHORT SymbolicLinkNameLength;
	ULONG UniqueIdOffset;
	USHORT UniqueIdLength;
	ULONG DeviceNameOffset;
	USHORT DeviceNameLength;
} MOUNTMGR_VOLUME_MOUNT_POINT_INPUT, *PMOUNTMGR_VOLUME_MOUNT_POINT_INPUT;

NTSTATUS
MountVolumeMountPoint(PUNICODE_STRING sourceVolumeName, PUNICODE_STRING volumeMountPoint)
{
	NTSTATUS status;

	PVOID inputBuffer = NULL;
	ULONG inputBufferLength = 0;

	// Calculate required buffer length
	inputBufferLength = sizeof(MOUNTMGR_VOLUME_MOUNT_POINT_INPUT) +
	    sourceVolumeName->Length +
	    volumeMountPoint->Length;

	// Allocate memory for input buffer
	inputBuffer = ExAllocatePoolWithTag(NonPagedPoolNx, inputBufferLength, 'volz');
	if (!inputBuffer)
		return (STATUS_INSUFFICIENT_RESOURCES);

	// Initialize input structure
	PMOUNTMGR_VOLUME_MOUNT_POINT_INPUT input = (PMOUNTMGR_VOLUME_MOUNT_POINT_INPUT)inputBuffer;
	RtlZeroMemory(input, sizeof(MOUNTMGR_VOLUME_MOUNT_POINT_INPUT));

	// Setup input structure fields
	input->SymbolicLinkNameLength = volumeMountPoint->Length;
	input->SymbolicLinkNameOffset = sizeof(MOUNTMGR_VOLUME_MOUNT_POINT_INPUT);

	input->DeviceNameLength = sourceVolumeName->Length;
	input->DeviceNameOffset = input->SymbolicLinkNameOffset + input->SymbolicLinkNameLength;

	// Copy the strings into the buffer
	RtlCopyMemory((PCHAR)input + input->SymbolicLinkNameOffset,
	    volumeMountPoint->Buffer,
	    input->SymbolicLinkNameLength);

	RtlCopyMemory((PCHAR)input + input->DeviceNameOffset,
	    sourceVolumeName->Buffer,
	    input->DeviceNameLength);


	status = SendIoctlToMountManager(
	    IOCTL_MOUNTMGR_VOLUME_MOUNT_POINT_CREATED,
	    input, inputBufferLength, NULL, 0);

	if (NT_SUCCESS(status)) {
		dprintf("  IoCallDriver success\n");
	} else {
		dprintf("  IoCallDriver failed: 0x%lx\n", status);
	}
	ExFreePoolWithTag(input, 'volz');
	return (status);
}


// Function to extract device name from symbolic link name
NTSTATUS
ExtractDeviceName(
    PUNICODE_STRING SymbolicLinkName, 
    PUNICODE_STRING DeviceName
)
{
	USHORT length = 0;
	USHORT i = 0;

	// Find the last backslash in the SymbolicLinkName
	for (i = (SymbolicLinkName->Length / sizeof(WCHAR))-1; i >= 0; i--) {
		if (SymbolicLinkName->Buffer[i] == L'\\') {
			break;
		}
	}

	// Calculate the length of the DeviceName
	length = (SymbolicLinkName->Length / sizeof(WCHAR)) - i;

	// Ensure the DeviceName buffer is large enough
	if (length * sizeof(WCHAR) > DeviceName->MaximumLength)
		return (STATUS_BUFFER_TOO_SMALL);

	// Copy the DeviceName from the SymbolicLinkName
	RtlCopyMemory(DeviceName->Buffer, &SymbolicLinkName->Buffer[i], length * sizeof(WCHAR));

	// Set the length and maximum length
	DeviceName->Length = length * sizeof(WCHAR);
	DeviceName->Buffer[length] = UNICODE_NULL; // Null-terminate the string

	return (STATUS_SUCCESS);
}


static void
mount_volume_impl(void *arg1)
{
	NTSTATUS status;
	mount_t *vcb = (mount_t *)arg1;
	mount_t *dcb = vcb->parent_device;

	DbgBreakPoint();

	dprintf("MOUNT starts here\n");
//	status = SendVolumeArrivalNotification(&dcb->device_name);
//	if (!NT_SUCCESS(status)) {
//		dprintf("  SendVolumeArrivalNotification failed: 0x%lx\n",
//		    status);
//	}

	UNICODE_STRING	name;
	PFILE_OBJECT	fileObject;
	PDEVICE_OBJECT	mountmgr;

	// Query MntMgr for points, just informative
	RtlInitUnicodeString(&name, MOUNTMGR_DEVICE_NAME);
	DECLARE_UNICODE_STRING_SIZE(symbolicname, PATH_MAX);
	DECLARE_UNICODE_STRING_SIZE(mountpath, PATH_MAX);

	/*
	 * So MountMgr assigns a SymbolicLinkName for our device
	 * which we need to use when talking to MountMgr. We also
	 * want to know if it was already assigned a driverletter
	 */
//	mountmgr_get_mountpoint2(mountmgr, &dcb->device_name,
//	    &symbolicname, &mountpath,
//	    TRUE);
	// Check if we are to mount as path or just drive letter
	if (dcb->justDriveLetter) {

	status = IoGetDeviceObjectPointer(&name, FILE_READ_ATTRIBUTES,
	    &fileObject, &mountmgr);
		status = mountmgr_get_drive_letter(mountmgr, &dcb->device_name,
		    &mountpath);
	ObDereferenceObject(fileObject);

		// If SendVolumeArrival was executed successfully we should
		// have two mountpoints
		// point 1: \Device\Volumes{abc}	\DosDevices\X:
		// point 2: \Device\Volumes{abc}	\??\Volume{xyz}
		// but if we are in remount and removed the mountpoints
		// for this volume manually before
		// they won't get assigned by mountmgr automatically anymore.
		// So at least check if we got them and if not, try to create.

		if (!MOUNTMGR_IS_DRIVE_LETTER(&mountpath)) {
			DECLARE_UNICODE_STRING_SIZE(mountpoint, PATH_MAX);
	status = IoGetDeviceObjectPointer(&name, FILE_READ_ATTRIBUTES,
	    &fileObject, &mountmgr);

			status = mountmgr_get_volume_name_mountpoint(mountmgr,
			    &dcb->device_name, &mountpoint);
				ObDereferenceObject(fileObject);

			if (!MOUNTMGR_IS_VOLUME_NAME(&mountpoint)) {
				// We have no volume name mountpoint for our
				// device, so generate a valid GUID and mount
				// the device
				UNICODE_STRING vol_mpt;
				wchar_t buf[50];
				generateVolumeNameMountpoint(&buf);
				RtlInitUnicodeString(&vol_mpt, buf);
				status =
				    SendVolumeCreatePoint(&dcb->device_name,
				    &vol_mpt);
			}

			// If driveletter was provided, try to add it
			// as mountpoint
			if (dcb && dcb->mountpoint.Length > 0 &&
			    dcb->mountpoint.Buffer[4] != '?') {
				// check if driveletter is unassigned
				BOOLEAN ret;
	status = IoGetDeviceObjectPointer(&name, FILE_READ_ATTRIBUTES,
	    &fileObject, &mountmgr);

				status = mountmgr_is_driveletter_assigned(
				    mountmgr,
				    dcb->mountpoint.Buffer[4], &ret);
	ObDereferenceObject(fileObject);

				if (status == STATUS_SUCCESS && ret == 0) {
					// driveletter is unassigned, try to
					// add mountpoint
	status = IoGetDeviceObjectPointer(&name, FILE_READ_ATTRIBUTES,
	    &fileObject, &mountmgr);
					status = mountmgr_assign_driveletter(
					    &dcb->device_name,
					    dcb->mountpoint.Buffer[4]);
	ObDereferenceObject(fileObject);
				} else {
					// driveletter already assigned,
					// find another one
					SetNextDriveletterManually(mountmgr,
					    &dcb->device_name);
				}
			} else {
				// user provided no driveletter,
				// find one on our own
				SetNextDriveletterManually(mountmgr,
				    &dcb->device_name);
			}
		} else { // !MOUNTMGR_IS_DRIVE_LETTER(&actualDriveletter)

			// mountpath is "/DosDevices/E:" but we need to
			// save that in dcb, for future query.
			FreeUnicodeString(&dcb->mountpoint);
			RtlDuplicateUnicodeString(0, &mountpath,
			    &dcb->mountpoint);



		}
	} else {
		OBJECT_ATTRIBUTES poa;
		// 36(uuid) + 6 (punct) + 6 (Volume)
		DECLARE_UNICODE_STRING_SIZE(volStr,
		    ZFS_MAX_DATASET_NAME_LEN);
		// "\??\Volume{0b1bb601-af0b-32e8-a1d2-54c167af6277}"
	status = IoGetDeviceObjectPointer(&name, FILE_READ_ATTRIBUTES,
	    &fileObject, &mountmgr);
		mountmgr_get_mountpoint2(mountmgr, &dcb->device_name,
		    &symbolicname, &mountpath,
		    TRUE);
	ObDereferenceObject(fileObject);

		DECLARE_UNICODE_STRING_SIZE(mountmgr_device_name,
		    ZFS_MAX_DATASET_NAME_LEN);
		// ExtractDeviceName(&symbolicname, &mountmgr_device_name);
		ExtractDeviceName(&dcb->device_name, &mountmgr_device_name);


		// We need to query the SymbolicName from MountMgr
//		mountmgr_get_mountpoint(mountmgr, &dcb->device_name, &mountpath,
//		    FALSE, TRUE);
		// mountpath="\??\Volume{df46d372-2857-11ef-82bd-58961d57db67}"

//		RtlUnicodeStringPrintf(&volStr,
//		    L"\\??\\Volume{%wZ}\\",
//		    vcb->uuid);

		// Annoyingly, the reparsepoints need trailing backslash
		RtlUnicodeStringPrintf(&volStr,
		    L"%wZ\\",
		    &symbolicname);
		UNICODE_STRING mountpoint;
		RtlInitUnicodeString(&mountpoint, L"\\??\\E:\\dataset");
		InitializeObjectAttributes(&poa,
		    &mountpoint, OBJ_KERNEL_HANDLE, NULL, NULL);
		    // &dcb->mountpoint, OBJ_KERNEL_HANDLE, NULL, NULL);
		dprintf("Creating reparse mountpoint on '%wZ' for "
		    "volume '%wZ'\n",
		    &dcb->mountpoint, &volStr);
		// 3rd arg is visible in DOS box
		// if (taskq_dispatch(system_taskq, io_verify_volume_impl,
		//     diskDeviceObject, TQ_SLEEP) == 0)
		//IoVerifyVolume(diskDeviceObject, FALSE);

		status = CreateReparsePoint(&poa, &volStr,
		    &volStr);
		    // &dcb->name);

		// Remove drive letter?
		// RtlUnicodeStringPrintf(&volStr, L"\\DosDevices\\E:");
		// RtlUnicodeStringPrintf(&volStr, L"%s", namex);
		// "\??\Volume{0b1bb601-af0b-32e8-a1d2-54c167af6277}"

		status = SendVolumeDeletePoints(&mountpath, &dcb->device_name);

		// Must start with "\DosDevices\X:"
		// mountpoint = "\\??\\x:"
		DECLARE_UNICODE_STRING_SIZE(mpoint, 128);

		status = RtlUnicodeStringPrintf(
		    &mpoint, L"\\DosDevices\\%ws", // name contains backslash
		    &dcb->mountpoint.Buffer[4]);
		UNICODE_STRING roger;
		RtlInitUnicodeString(&roger, L"\\DosDevices\\E:\\dataset");
				DbgBreakPoint();

		status =
		    GPTCreateMountPoint(
		    &roger, &dcb->device_name);
		    // &roger, &mountmgr_device_name);

//		status = CreateMountPoint(&volStr, &roger);
		//status = CreateMountPoint(&dcb->device_name, &roger); // mpoint

		// status = MountVolumeMountPoint(&volStr, &mpoint);
		DECLARE_UNICODE_STRING_SIZE(withslash, 128);
		status = RtlUnicodeStringPrintf(
		    &withslash, L"%wZ\\", // name contains backslash
		    &dcb->device_name);
		// status = MountVolumeMountPoint(&withslash, &roger);
		// status = MountVolumeMountPoint(&mountmgr_device_name, &roger);
		status = MountVolumeMountPoint(&dcb->device_name, &roger);

		 // = SendVolumeArrivalNotification(&dcb->device_name);
		 // status = SendVolumeArrivalNotification(&symbolicname);
		 // status = SendVolumeArrivalNotification(&roger);


		status = STATUS_SUCCESS;
	}

	RtlDuplicateUnicodeString(0, &dcb->mountpoint, &vcb->mountpoint);

	status = SendVolumeArrivalNotification(&dcb->device_name);
	if (!NT_SUCCESS(status)) {
		dprintf("  SendVolumeArrivalNotification failed: 0x%lx\n",
		    status);
	}

	if (dcb->root_file)
		status = FsRtlNotifyVolumeEvent(dcb->root_file, FSRTL_VOLUME_MOUNT);

	status = STATUS_SUCCESS;

	dprintf("Printing final result\n");
	status = IoGetDeviceObjectPointer(&name, FILE_READ_ATTRIBUTES,
	    &fileObject, &mountmgr);
	mountmgr_get_mountpoint2(mountmgr, &dcb->device_name,
	    NULL, NULL, TRUE);
	ObDereferenceObject(fileObject);
}

NTSTATUS
matched_mount(PDEVICE_OBJECT DeviceObject, PDEVICE_OBJECT DeviceToMount, mount_t *dcb, PVPB vpb)
{
	zfsvfs_t *xzfsvfs = vfs_fsprivate(dcb);
	NTSTATUS status;
	PDEVICE_OBJECT volDeviceObject;

	if (xzfsvfs && xzfsvfs->z_unmounted) {
		dprintf("%s: Is a ZFS dataset -- unmounted. dcb %p ignoring: "
		    "type 0x%x != 0x%x, size %lu != %llu\n",
		    __func__, dcb,
		    dcb->type, MOUNT_TYPE_DCB, dcb->size, sizeof (mount_t));
		return (STATUS_UNRECOGNIZED_VOLUME);
	}

	xprintf("%s\n", __func__);

	// ZFS Dataset being mounted:
	// dprintf("%s: mounting '%wZ'\n", __func__, dcb->name);

	// We created a DISK before, now we create a VOLUME
	ULONG deviceCharacteristics;
	deviceCharacteristics = 0; // FILE_DEVICE_IS_MOUNTED;
	/* Allow $recycle.bin - don't set removable. */
	if (!zfs_disable_removablemedia)
		deviceCharacteristics |= FILE_REMOVABLE_MEDIA;

	if (dcb->mountflags & MNT_RDONLY)
		deviceCharacteristics |= FILE_READ_ONLY_DEVICE;
	DbgBreakPoint();
	/* This creates the VDO - VolumeDeviceObject */
	status = IoCreateDevice(WIN_DriverObject,
	    sizeof (mount_t),
	    NULL,
//	    FILE_DEVICE_DISK,
	    FILE_DEVICE_DISK_FILE_SYSTEM,
	    deviceCharacteristics|FILE_DEVICE_IS_MOUNTED,
	    FALSE,
	    &volDeviceObject);

	if (!NT_SUCCESS(status)) {
		dprintf("%s: IoCreateDevice failed: 0x%lx\n", __func__, status);
	}

	mount_t *vcb = volDeviceObject->DeviceExtension;
	vcb->type = MOUNT_TYPE_VCB;
	vcb->size = sizeof (mount_t);

	volDeviceObject->Flags |= DO_BUS_ENUMERATED_DEVICE;

	zfsvfs_t *zfsvfs = vfs_fsprivate(dcb);
	int giveup = 0;
	while (zfsvfs == NULL) {
		delay(hz / 10);
		dprintf("zfsvfs not resolved yet\n");
		zfsvfs = vfs_fsprivate(dcb);
		if (giveup++ > 50) {
			status = STATUS_UNRECOGNIZED_VOLUME;
			goto out;
		}
	}
	zfsvfs->z_vfs = vcb;
	vfs_setfsprivate(vcb, zfsvfs);
	// a bit hacky this bit, but we created some vnodes under
	// dcb during this mount hand over, make them be owned by
	// vcb
	vfs_changeowner(dcb, vcb);

	// Remember the parent device, so during unmount we can free both.
	vcb->parent_device = dcb;

	// vcb is the ptr used in unmount, so set both devices here.
	// vcb->diskDeviceObject = dcb->deviceObject;
	vcb->VolumeDeviceObject = volDeviceObject;

	RtlDuplicateUnicodeString(0, &dcb->fs_name, &vcb->fs_name);
	RtlDuplicateUnicodeString(0, &dcb->name, &vcb->name);
	RtlDuplicateUnicodeString(0, &dcb->device_name, &vcb->device_name);
	RtlDuplicateUnicodeString(0, &dcb->symlink_name, &vcb->symlink_name);
	RtlDuplicateUnicodeString(0, &dcb->uuid, &vcb->uuid);
//	RtlDuplicateUnicodeString(0, &dcb->mountpoint, &vcb->mountpoint);

	vcb->mountflags = dcb->mountflags;
	if (vfs_isrdonly(dcb))
		vfs_setrdonly(vcb);

	// Directory notification
	InitializeListHead(&vcb->DirNotifyList);
	FsRtlNotifyInitializeSync(&vcb->NotifySync);

	DbgBreakPoint();

	dcb->root_file = IoCreateStreamFileObject(NULL, DeviceToMount);
	if (dcb->root_file != NULL) {
		struct vnode *vp;
		struct znode *zp;
		zfs_ccb_t *zccb;

		dcb->root_file->Vpb = vpb;
		vnode_create(dcb, NULL, NULL, VDIR, VNODE_MARKROOT, &vp);
		dcb->root_file->FsContext = vp;
		vnode_ref(vp);
		vnode_couplefileobject(vp, dcb->root_file, 0);
		zp = kmem_cache_alloc(znode_cache, KM_SLEEP);
		zp->z_vnode = vp;
		vp->v_data = zp;
		zccb = kmem_zalloc(sizeof (zfs_ccb_t), KM_SLEEP);
		zccb->magic = ZFS_CCB_MAGIC;
		dcb->root_file->FsContext2 = zccb;
		zccb->z_name_len = 2;
		zccb->z_name_cache = kmem_zalloc(zccb->z_name_len, KM_SLEEP);
		zccb->z_name_cache[0] = '/';
		zccb->z_name_cache[1] = 0;
		zccb->z_name_offset = 0;
	}

	KIRQL OldIrql;
	IoAcquireVpbSpinLock(&OldIrql);
	InitVpb(vpb, volDeviceObject);
	vcb->vpb = vpb;
	vpb->ReferenceCount++;
	vpb->ReferenceCount++;
	vpb->ReferenceCount++;
	vpb->ReferenceCount+=50;
	xprintf("%p ReferenceCount is %d\n", vpb, vpb->ReferenceCount);
	//	dcb->vpb = vpb;
//	vpb->ReferenceCount++;
	IoReleaseVpbSpinLock(OldIrql);


	volDeviceObject->Flags |= DO_DIRECT_IO;
	volDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
	// SetLongFlag(vcb->Flags, VCB_MOUNTED);

#if 1
	PDEVICE_OBJECT attachedDevice;
	attachedDevice = IoAttachDeviceToDeviceStack(volDeviceObject,
	    DeviceObject); // DeviceToMount);
	if (attachedDevice == NULL) {
		IoDeleteDevice(volDeviceObject);
		status = STATUS_UNSUCCESSFUL;
		goto out;
	}
	vcb->AttachedDevice = attachedDevice;

#endif

	// ObReferenceObject(volDeviceObject);

	status = STATUS_SUCCESS;

//	if (NT_SUCCESS(status))
//		/* Sort out the mountpoint names */
//		taskq_dispatch(system_taskq, mount_volume_impl,
//		    vcb, TQ_SLEEP);

 out:
	return (status);
}

static void *fudgeptr = NULL;

int
zfs_vnop_mount(PDEVICE_OBJECT DiskDevice, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	PDRIVER_OBJECT DriverObject = DiskDevice->DriverObject;
	NTSTATUS status = STATUS_SUCCESS;
	PDEVICE_OBJECT DeviceToMount;

	if (IrpSp->Parameters.MountVolume.DeviceObject == NULL) {
		dprintf("%s: MountVolume is NULL\n", __func__);
		return (STATUS_UNRECOGNIZED_VOLUME);
	}

	DbgBreakPoint();

	OpenZFS_Driver_Extension *DriverExtension =
	    (OpenZFS_Driver_Extension *)IoGetDriverObjectExtension(WIN_DriverObject, WIN_DriverObject);
	if (DriverExtension == NULL) // Should never happen, but
		return (STATUS_INVALID_DEVICE_REQUEST);

	// The "/OpenZFS" FILE_DEVICE_DISK_FILE_SYSTEM "masterobj"
	if (DiskDevice != DriverExtension->fsDiskDeviceObject)
		return (STATUS_INVALID_DEVICE_REQUEST);

	DeviceToMount = IrpSp->Parameters.MountVolume.DeviceObject;
	 if (DeviceToMount->DeviceType != FILE_DEVICE_DISK) {
		// Not a disk device, pass it down
		IoSkipCurrentIrpStackLocation(Irp);
		return IoCallDriver(DeviceToMount, Irp);
	}

	// DeviceToMount must be released from here down
	mount_t *dcb = NULL;
	dcb = DeviceToMount->DeviceExtension;
	dcb = fudgeptr;

	 PDEVICE_OBJECT d2m2;
	d2m2 = IoGetDeviceAttachmentBaseRef(IrpSp->
	    Parameters.MountVolume.DeviceObject);

	ObDereferenceObject(DeviceToMount);

	if (dcb == NULL)
		return (STATUS_UNRECOGNIZED_VOLUME);

	 status = matched_mount(IrpSp->Parameters.MountVolume.DeviceObject,
	    DeviceToMount,
	    dcb, IrpSp->Parameters.MountVolume.Vpb);
	return (status);



	DeviceToMount = IoGetDeviceAttachmentBaseRef(IrpSp->
	    Parameters.MountVolume.DeviceObject);
	dprintf("*** mount request for %p : minor\n", DeviceToMount);

	if (DeviceToMount == NULL) {
		dprintf("%s: DeviceToMount is NULL\n", __func__);
		return (STATUS_UNRECOGNIZED_VOLUME);
	}

	if (DeviceToMount->DriverObject == WIN_DriverObject) {
		dprintf("*** The device belong to us\n");
		dcb = DeviceToMount->DeviceExtension;
	} else {
		dprintf("*** The device does NOT belong to us\n");
		status = STATUS_UNRECOGNIZED_VOLUME;
//		goto out;
	}

	// If it comes from "\Driver\PnpManager"
	if (DeviceToMount->AttachedDevice != NULL &&
	    DeviceToMount->AttachedDevice->DriverObject == WIN_DriverObject) {
		dprintf("*** The device belong to us after all\n");
		dcb = DeviceToMount->AttachedDevice->DeviceExtension;
		status = 0;
	}

	if (dcb == NULL) {
		dprintf("%s: Not a ZFS dataset -- ignoring\n", __func__);
		status = STATUS_UNRECOGNIZED_VOLUME;
		goto out;
	}

	if ((dcb->type != MOUNT_TYPE_DCB) ||
	    (dcb->size != sizeof (mount_t))) {
		dprintf("%s: Not a ZFS dataset -- dcb %p ignoring: "
		    "type 0x%x != 0x%x, size %lu != %llu\n",
		    __func__, dcb,
		    dcb->type, MOUNT_TYPE_DCB, dcb->size, sizeof (mount_t));
		status = STATUS_UNRECOGNIZED_VOLUME;
		goto out;
	}

	status = matched_mount(IrpSp->Parameters.MountVolume.DeviceObject,
	    DeviceToMount,
	    dcb, IrpSp->Parameters.MountVolume.Vpb);

 out:
	// match IoGetDeviceAttachmentBaseRef()
	ObDereferenceObject(DeviceToMount);

	dprintf("%s: exit: 0x%lx\n", __func__, status);
	return (status);
}

void
mount_add_device(PDEVICE_OBJECT DriverObject,
    PDEVICE_OBJECT PhysicalDeviceObject, PDEVICE_OBJECT AddDeviceObject)
{
	NTSTATUS status;
	mount_t *zmo = (mount_t *)AddDeviceObject->DeviceExtension;

	xprintf("%s\n", __func__);
	DbgBreakPoint();

	// status = IoCreateDevice(DriverObject, sizeof(mount_t),
	//     &zmo->device_name, FILE_DEVICE_DISK,
	//    0, FALSE, &zmo->VolumeDeviceObject);

	zmo->PhysicalDeviceObject = PhysicalDeviceObject;

	status = IoCreateSymbolicLink(&zmo->symlink_name, &zmo->device_name);
	status = IoCreateSymbolicLink(&zmo->arc_name, &zmo->device_name);

	status = IoRegisterDeviceInterface(PhysicalDeviceObject, &GUID_DEVINTERFACE_VOLUME, NULL,
	   &zmo->deviceInterfaceName);
//	status = IoRegisterDeviceInterface(
//	    PhysicalDeviceObject,
//	    &MOUNTDEV_MOUNTED_DEVICE_GUID,
//	    NULL,
//	    &zmo->fsInterfaceName);

	zmo->AttachedDevice = IoAttachDeviceToDeviceStack(AddDeviceObject, PhysicalDeviceObject);

	// zmo->VolumeDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

	// status = IoSetDeviceInterfaceState(&zmo->deviceInterfaceName, TRUE);
	status = IoSetDeviceInterfaceState(&zmo->fsInterfaceName, TRUE);
	xprintf("FILE_DEVICE_DISK PhysicalDeviceObject %p = zmo %p\n",
	    PhysicalDeviceObject, zmo);
	fudgeptr = zmo;
}

int
zfs_remove_driveletter(mount_t *zmo)
{
	UNICODE_STRING name;
	PFILE_OBJECT fileObject;
	PDEVICE_OBJECT mountmgr;
	NTSTATUS Status;

	dprintf("%s: removing driveletter for '%wZ'\n", __func__, &zmo->name);

	// Query MntMgr for points, just informative
	RtlInitUnicodeString(&name, MOUNTMGR_DEVICE_NAME);
	Status = IoGetDeviceObjectPointer(&name, FILE_READ_ATTRIBUTES,
	    &fileObject, &mountmgr);

	MOUNTMGR_MOUNT_POINT *mmp = NULL;
	ULONG mmpsize;
	MOUNTMGR_MOUNT_POINTS mmps1, *mmps2 = NULL;

	mmpsize = sizeof (MOUNTMGR_MOUNT_POINT) + zmo->device_name.Length;

	mmp = kmem_zalloc(mmpsize, KM_SLEEP);

	mmp->DeviceNameOffset = sizeof (MOUNTMGR_MOUNT_POINT);
	mmp->DeviceNameLength = zmo->device_name.Length;
	RtlCopyMemory(&mmp[1], zmo->device_name.Buffer,
	    zmo->device_name.Length);

	Status = dev_ioctl(mountmgr, IOCTL_MOUNTMGR_DELETE_POINTS,
	    mmp, mmpsize, &mmps1, sizeof (MOUNTMGR_MOUNT_POINTS), FALSE, NULL);

	if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW) {
		goto out;
	}

	if (Status != STATUS_BUFFER_OVERFLOW || mmps1.Size == 0) {
		Status = STATUS_NOT_FOUND;
		goto out;
	}

	mmps2 = kmem_zalloc(mmps1.Size, KM_SLEEP);

	Status = dev_ioctl(mountmgr, IOCTL_MOUNTMGR_DELETE_POINTS,
	    mmp, mmpsize, mmps2, mmps1.Size, FALSE, NULL);

	// if (!NT_SUCCESS(Status))
	//	ERR("IOCTL_MOUNTMGR_DELETE_POINTS 2 returned %08x\n", Status);

out:
	dprintf("%s: removing driveletter returns 0x%lx\n", __func__, Status);

	if (mmps2)
		kmem_free(mmps2, mmps1.Size);
	if (mmp)
		kmem_free(mmp, mmpsize);

	ObDereferenceObject(fileObject);
	return (Status);
}

int
zfs_windows_unmount(zfs_cmd_t *zc)
{
	// IRP_MN_QUERY_REMOVE_DEVICE
	// IRP_MN_REMOVE_DEVICE
	// FsRtlNotifyVolumeEvent(, FSRTL_VOLUME_DISMOUNT);

	// Use name, lookup zfsvfs
	// use zfsvfs to get mount_t
	// mount_t has deviceObject, names etc.
	mount_t *zmo;
	mount_t *zmo_dcb = NULL;
	zfsvfs_t *zfsvfs;
	int error = EBUSY;
	// znode_t *zp;
	// int rdonly;

	if (getzfsvfs(zc->zc_name, &zfsvfs) == 0) {

		zmo = zfsvfs->z_vfs;
		NTSTATUS ntstatus;
		ASSERT(zmo->type == MOUNT_TYPE_VCB);

		// As part of unmount-preflight, we call vflush()
		// as it will indicate if we should return EBUSY.
		if (vnode_umount_preflight(zmo, NULL,
		    SKIPROOT|SKIPSYSTEM|SKIPSWAP)) {
			vfs_unbusy(zmo);
			return (SET_ERROR(EBUSY));
		}

		// getzfsvfs() grabs a READER lock,
		// convert it to WRITER, and wait for it.
		vfs_busy(zmo, LK_UPGRADE);
		// All readers are no punted until properly unmounted.

		// Try issuing DISMOUNT ... this wont work unless
		// "attached" in RegisterDeviceInterface()
//		FILE_OBJECT *root_file;
//		root_file = IoCreateStreamFileObject(NULL,
//		    zmo->VolumeDeviceObject);
		ntstatus = FsRtlNotifyVolumeEvent(zmo->root_file,
		    FSRTL_VOLUME_DISMOUNT);
//		ObDereferenceObject(root_file);

		UNICODE_STRING	name;
		PFILE_OBJECT	fileObject;
		PDEVICE_OBJECT	mountmgr;

		// Query MntMgr for points, just informative
		RtlInitUnicodeString(&name, MOUNTMGR_DEVICE_NAME);
		NTSTATUS status = IoGetDeviceObjectPointer(&name,
		    FILE_READ_ATTRIBUTES, &fileObject, &mountmgr);
		DECLARE_UNICODE_STRING_SIZE(mountpath, PATH_MAX);
		status = mountmgr_get_drive_letter(mountmgr,
		    &zmo->device_name, &mountpath);

		// Save the parent device
		zmo_dcb = zmo->parent_device;

		// Flush volume
		// rdonly = !spa_writeable(dmu_objset_spa(zfsvfs->z_os));

		// Delete mountpoints for our volume manually
		// Query the mountmgr for mountpoints and delete them
		// until no mountpoint is left Because we are not satisfied
		// with mountmgrs work, it gets offended and doesn't
		// automatically create mointpoints for our volume after we
		// deleted them manually But as long as we recheck that in
		// mount and create points manually (if necessary),
		// that should be ok hopefully

		// We used to loop here and keep deleting anything we find,
		// but we are only allowed to remove symlinks, anything
		// else and MountMgr ignores the device.
		ObDereferenceObject(fileObject);

		if (MOUNTMGR_IS_DRIVE_LETTER(&mountpath)) {

			zfs_remove_driveletter(zmo);

		} else {
			// If mount uses reparsepoint (not driveletter)
			OBJECT_ATTRIBUTES poa;

			InitializeObjectAttributes(&poa,
			    &zmo_dcb->mountpoint, OBJ_KERNEL_HANDLE, NULL,
			    NULL);
			dprintf("Deleting reparse mountpoint '%wZ'\n",
			    &zmo_dcb->mountpoint);
			DeleteReparsePoint(&poa);

			// Remove directory, only for !driveletter
			ZwDeleteFile(&poa);
		}

		KIRQL irql;
		IoAcquireVpbSpinLock(&irql);
		zmo->vpb->Flags &= ~VPB_MOUNTED;
		// zmo_dcb->vpb->Flags &= ~VPB_MOUNTED;
		zmo->vpb->Flags |= VPB_DIRECT_WRITES_ALLOWED;
		zmo->vpb->DeviceObject = NULL;
		IoReleaseVpbSpinLock(irql);

		// Release any notifications
#if (NTDDI_VERSION >= NTDDI_VISTA)
		FsRtlNotifyCleanupAll(zmo->NotifySync, &zmo->DirNotifyList);
#endif

		// This will make it try to mount again, so make sure we dont

		status = SendVolumeRemovalNotification(&zmo_dcb->device_name);

		/*
		 * We call mount on DCB, but shouldn't it be VCB? We
		 * match unmount on DCB here so vflush can compare.
		 * DCB and VCB do have almost the same information, but
		 * it is probably more correct to change mount to use VCB.
		 */
		error = zfs_vfs_unmount(zmo, 0, NULL);
		dprintf("%s: zfs_vfs_unmount %d\n", __func__, error);
		if (error)
			goto out_unlock;

		// Release devices
		IoDeleteSymbolicLink(&zmo->symlink_name);
		RtlFreeUnicodeString(&zmo->symlink_name);

		DbgBreakPoint();

		if (zmo->root_file) {
			struct vnode *vp = zmo->root_file->FsContext;
			if (vp) {
				kmem_cache_free(znode_cache, VTOZ(vp));
				vnode_rele(vp);
				vnode_decouplefileobject(zmo->root_file->FsContext, zmo->root_file);
				vnode_recycle(vp);
			}
			zfs_ccb_t *zccb = zmo->root_file->FsContext2;
			if (zccb) {
				if (zccb->z_name_cache != NULL) 
					kmem_free(zccb->z_name_cache, zccb->z_name_len);
				kmem_free(zccb, sizeof (zfs_ccb_t));
			}

			ObDereferenceObject(zmo->root_file);
			zmo->root_file = NULL;
		}

		// zmo has Volume, and Attached
		IoSetDeviceInterfaceState(&zmo_dcb->volumeInterfaceName, FALSE);
		IoSetDeviceInterfaceState(&zmo_dcb->fsInterfaceName, FALSE);
		IoSetDeviceInterfaceState(&zmo_dcb->deviceInterfaceName, FALSE);
		IoDetachDevice(zmo->AttachedDevice);
		zmo->AttachedDevice = NULL;

		// fsDeviceObject
		if (zmo->VolumeDeviceObject) {
			IoDeleteDevice(zmo->VolumeDeviceObject);
			zmo->VolumeDeviceObject = NULL;
		}

		zfs_release_mount(zmo);
		zmo = NULL;

		// dcb has physical and functional
		// There should also be a diskDevice above us to release.
		if (zmo_dcb != NULL) {
			if (zmo_dcb->PhysicalDeviceObject)
				IoDeleteDevice(zmo_dcb->PhysicalDeviceObject);
			zmo_dcb->PhysicalDeviceObject = NULL;

			if (zmo_dcb->FunctionalDeviceObject) {
				IoDetachDevice(zmo_dcb->AttachedDevice);
				IoDeleteDevice(zmo_dcb->FunctionalDeviceObject);
			}
			zmo_dcb->FunctionalDeviceObject = NULL;
			zfs_release_mount(zmo_dcb);
			zmo_dcb = NULL;
		}


		error = 0;

out_unlock:
		// counter to getzfvfs
		zfsvfs->z_vfs = NULL;
		vfs_unbusy(zmo);

	}
	return (error);
}
