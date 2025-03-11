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
 * Copyright (c) 2025 Jorgen Lundman <lundman@lundman.net>
 */

/*
 * It is undesirable for /FileSystem/OpenZFS and the StorPort miniport for ZVOL
 * to be in the same driver. Not only as they both need to set DriverObject
 * MajorFunction table, AddDevice and DriverUnload function pointers, but they
 * need to attach at different places in the driver stack, as well as, to be
 * installed as different device types from the .INF file.
 *
 * So OpenZFS.sys driver, and the StorPort miniport driver for ZVOLs
 * (OpenZVOL.sys) are now separate, with two .INF files, OpenZFS.inf and
 * OpenZVOL.inf.
 *
 * We consider OpenZFS.sys to be the main driver, and can function without
 * OpenZVOL.sys, although without ZVOL device support. Datasets can still be
 * used and mounted, just not ZVOL Volumes.
 *
 * OpenZVOL.sys's DriverLoad() calls StorPortInit() to get ready, and uses
 * IoCreateDriver() to have a second set of MajorFunctions for the creation of
 * \Device\OpenZVOL device node. This is used for IRP communication with
 * OpenZVOL.sys driver.
 *
 * When OpenZFS.sys is loaded, it will start a taskq to poll for OpenZVOL.sys
 * to load (including if it is already loaded) by polling for the existence of
 * \Device\OpenZVOL. Once the OpenZVOL.sys driver has loaded and is detected,
 * OpenZFS.sys will register itself. OpenZFS.sys will keep the FileObject to
 * OpenZVOL.sys held, until deregister time. Both for performance, and to
 * ensure the driver will not unload.
 *
 * zvol_os_register_module(zfs_api_t *): Issue IRP [OPENZVOL_REGISTER] with
 *         function pointers for IO; namely zvol_os_read_zv(),
 *         zvol_os_write_zv() and zvol_os_unmap().
 *         OpenZVOL when receiving IRP will save the function pointers, and
 *         keep a reference on DriverObject.
 *
 * zvol_os_deregister_module(void): Issue IRP [OPENZVOL_DEREGISTER].
 * Closes FileObject
 *         to \Device\OpenZVOL.
 *         OpenZVOL will clear the function pointers, and release DriverObject.
 *
 * zvol_os_assign_targetid(zvol_state_t *): Issue IRP [OPENZVOL_ASSIGN_TARGETID]
 *         Passing over a pointer to "zv" zvol_state_t.
 *         OpenZVOL looks up available (lun, tag) and assigns to "zv".
 *         After completion "zv" is not used by OpenZVOL, just the address to
 *         map (lun, id) and "zv".
 *
 * zvol_os_clear_targetid(zvol_state_t *): Issue IRP [OPENZVOL_CLEAR_TARGETID]
 *         Passing pointer to "zv" zvol_state_t.
 *         OpenZVOL will release (lun, id) mapping to "zv" and not refer
 *         to it again.
 *
 * zvol_os_announce_buschange(void): Issue IRP [OPENZVOL_ANNOUNCE_BUSCHANGE]
 *         OpenZVOL will issue buschanged notification, for drive added/removed.
 *
 * It could be considered hacky to pass the IO functions (zvol_os_read_zv(),
 * zvol_os_write_zv() and zvol_os_unmap()) to OpenZVOL.sys, and call them
 * directly. It hopefully means there is no performance penalty to separate
 * the two drivers, but it could be explored to instead issue IRPs for IO
 * as well. Comments welcome.
 * Certainly direct calling functions would not work in Unix, so it is
 * surprising that it does in Windows.
 *
 *
 */


// Get "_daylight: has bad storage class" in time.h
#define	_INC_TIME

#include <sys/types.h>
#include <sys/mod_os.h>
#include <ntddk.h>
#include <ntddstor.h>
#include <storport.h>

#include <sys/zfs_context.h>
#include <sys/wzvol.h>

#include <sys/spa.h>
#include <sys/zfs_rlock.h>
#include <sys/dataset_kstats.h>
#include <sys/zil.h>
#include <sys/zvol.h>
#include <sys/zvol_impl.h>
#include <sys/zvol_os.h>

#include <sys/openzvol.h>


DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD OpenZFS_Fini;
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, OpenZFS_Fini)

int zfs_flags = 0;

#ifdef __clang__
// #error "This file should be compiled with MSVC not Clang"
#endif

#define	dprintf(...) \
	KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, __VA_ARGS__))

PDRIVER_OBJECT OPENZVOL_DriverObject = NULL;
extern int zvol_start(PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING pRegistryPath);

int (*pzvol_os_read_zv)(zvol_state_t *zv, zfs_uio_t *uio,
    int flags) = NULL;
int (*pzvol_os_write_zv)(zvol_state_t *zv, zfs_uio_t *uio,
    int flags) = NULL;
int (*pzvol_os_unmap)(zvol_state_t *zv, uint64_t off,
    uint64_t bytes) = NULL;

/*
 * These things are here cos we have cheated with the
 * SPL / ZFS barrier. SPL should never refer to ZFS
 * but here we are. Eventually we will clean up this
 * lazy code
 */

wchar_t zfs_vdev_protection_filter[ZFS_MODULE_STRMAX] = { L"\0" };
_Atomic uint64_t spl_lowest_vdev_disk_stack_remaining = 0;
_Atomic uint64_t spl_lowest_zvol_stack_remaining = 0;

void
printBuffer(const char *fmt, ...)
{
}

void
spl_set_arc_no_grow(int i)
{
}

void
zfs_inactive(void *vp, void *cr, void *ct)
{
}

int
zfs_vnop_reclaim(void *)
{
}

/* end of shoddy */

NTSTATUS
ControlDeviceIoctlHandler(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
	NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
	zvol_state_t *zv = NULL;
	void *addr;

	switch (irpSp->Parameters.DeviceIoControl.IoControlCode) {
	case OPENZVOL_REGISTER:
		dprintf("OPENZVOL_REGISTER\n");

		zvol_api_t *api = (zvol_api_t *)Irp->AssociatedIrp.SystemBuffer;

		if (irpSp->Parameters.DeviceIoControl.InputBufferLength !=
		    sizeof (zvol_api_t) || api == NULL) {
			dprintf("Received IOCTL with invalid size or buffer\n");
			status = STATUS_INVALID_PARAMETER;
			goto out;
		}

		// All functions must be set
		if (api->zvol_os_read_zv != NULL &&
		    api->zvol_os_write_zv != NULL &&
		    api->zvol_os_unmap != NULL) {
			pzvol_os_read_zv = api->zvol_os_read_zv;
			pzvol_os_write_zv = api->zvol_os_write_zv;
			pzvol_os_unmap = api->zvol_os_unmap;

			// Lock this driver until deregistered
			ObReferenceObject(OPENZVOL_DriverObject);
		}

		status = STATUS_SUCCESS;
		break;

	case OPENZVOL_DEREGISTER:
		dprintf("OPENZVOL_DEREGISTER\n");
		if (pzvol_os_read_zv != NULL) {
			pzvol_os_read_zv = NULL;
			pzvol_os_write_zv = NULL;
			pzvol_os_unmap = NULL;

			// Unlock this driver
			ObDereferenceObject(OPENZVOL_DriverObject);
		}
		status = STATUS_SUCCESS;
		break;

	case OPENZVOL_ASSIGN_TARGETID:
		dprintf("OPENZVOL_ASSIGN_TARGETID\n");
		addr = Irp->AssociatedIrp.SystemBuffer;
		if (irpSp->Parameters.DeviceIoControl.InputBufferLength !=
		    sizeof (zvol_state_t *) || addr == NULL) {
			dprintf("Received IOCTL with invalid size or buffer\n");
			status = STATUS_INVALID_PARAMETER;
			goto out;
		}
		// zv is a pointer to a pointer, so we need to dereference it
		zv = *((zvol_state_t **)addr);
		wzvol_assign_targetid(zv);
		status = STATUS_SUCCESS;
		break;
	case OPENZVOL_CLEAR_TARGETID:
		dprintf("OPENZVOL_CLEAR_TARGETID\n");
		addr = Irp->AssociatedIrp.SystemBuffer;
		if (irpSp->Parameters.DeviceIoControl.InputBufferLength !=
		    sizeof (zvol_state_t *) || addr == NULL) {
			dprintf("Received IOCTL with invalid size or buffer\n");
			status = STATUS_INVALID_PARAMETER;
			goto out;
		}
		// zv is a pointer to a pointer, so we need to dereference it
		zv = *((zvol_state_t **)addr);
		wzvol_clear_targetid(zv->zv_zso->zso_target_id,
		    zv->zv_zso->zso_lun_id, zv);
		status = STATUS_SUCCESS;
		break;
	case OPENZVOL_ANNOUNCE_BUSCHANGE:
		dprintf("OPENZVOL_ANNOUNCE_BUSCHANGE\n");
		wzvol_announce_buschange();
		status = STATUS_SUCCESS;
		break;

	default:
		dprintf("Received unknown IOCTL 0x%08x\n",
		    irpSp->Parameters.DeviceIoControl.IoControlCode);
	}
out:
	Irp->IoStatus.Status = status;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return (status);
}

NTSTATUS
OpenZVOLCreateCloseCleanUp(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return (STATUS_SUCCESS);
}

NTSTATUS
OpenZVOLNoCall(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	DbgBreakPoint();
}

void
OpenZVOLUnloadRoutine(IN PDRIVER_OBJECT DriverObject)
{
	KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
	    "Deleting /Device/OpenZVOL\n"));

	UNICODE_STRING symbolicLinkName;
	RtlInitUnicodeString(&symbolicLinkName, SYMBOLIC_LINK_NAME);

	// Delete the symbolic link
	IoDeleteSymbolicLink(&symbolicLinkName);

	IoDeleteDevice(DriverObject->DeviceObject);
}

NTSTATUS
OpenZVOLDriverInitialize(IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath)
{
	UNREFERENCED_PARAMETER(RegistryPath);
	NTSTATUS status;
	PDEVICE_OBJECT PhysicalDeviceObject;
	UNICODE_STRING deviceName;
	UNICODE_STRING symbolicLinkName;
	RtlInitUnicodeString(&deviceName, DEVICE_NAME);
	RtlInitUnicodeString(&symbolicLinkName, SYMBOLIC_LINK_NAME);

	PDEVICE_OBJECT deviceObject;
	status = IoCreateDevice(DriverObject, 0, &deviceName,
	    FILE_DEVICE_UNKNOWN, 0, FALSE, &deviceObject);

	if (!NT_SUCCESS(status))
		return (status);

	status = IoCreateSymbolicLink(&symbolicLinkName, &deviceName);

	if (!NT_SUCCESS(status)) {
		IoDeleteDevice(deviceObject);
		return (status);
	}

#if 1
	for (int i = 0; i < IRP_MJ_MAXIMUM_FUNCTION; i++)
		DriverObject->MajorFunction[i] = OpenZVOLNoCall;
#endif

	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] =
	    ControlDeviceIoctlHandler;
	DriverObject->MajorFunction[IRP_MJ_CREATE] =
	    OpenZVOLCreateCloseCleanUp;
	DriverObject->MajorFunction[IRP_MJ_CLOSE] =
	    OpenZVOLCreateCloseCleanUp;
	DriverObject->MajorFunction[IRP_MJ_CLEANUP] =
	    OpenZVOLCreateCloseCleanUp;
	DriverObject->DriverUnload = OpenZVOLUnloadRoutine;
	deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

	KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
	    "Created /Device/OpenZVOL\n"));

	return (STATUS_SUCCESS);
}

NTSTATUS NTAPI IoCreateDriver(
    _In_opt_ PUNICODE_STRING 	DriverName,
    _In_ PDRIVER_INITIALIZE 	InitializationFunction
);

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING pRegistryPath)
{
	KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
	    "OpenZVOL: DriverEntry\n"));

	OPENZVOL_DriverObject = DriverObject;

	// Init storport
	zvol_start(DriverObject, pRegistryPath);

	// Init minimal device object for communication
	UNICODE_STRING driverName;
	RtlInitUnicodeString(&driverName, L"\\Driver\\OpenZVOL_Helper");

	NTSTATUS status = IoCreateDriver(&driverName,
	    &OpenZVOLDriverInitialize);
	if (!NT_SUCCESS(status)) {
		KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
		    "Failed to create OpenZVOL helper driver: %08X\n",
		    status));
	}

	KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
	    "OpenZVOL: Started\n"));

	return (STATUS_SUCCESS);
}

/* storport has its own Unload, so we can't */
void
OpenZVOL_Fini(PDRIVER_OBJECT DriverObject)
{
	(void) DriverObject;
	KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "OpenZVOL_Fini\n"));

	KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
	    "OpenZVOL: Goodbye.\n"));
}
