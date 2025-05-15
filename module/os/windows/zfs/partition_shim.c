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

// partition_shim.c
// Proof-of-concept dynamic disk filter for openzfs.sys

#include <sys/proc.h>

#include <ntddk.h>
#include <ntdddisk.h>
#include <initguid.h>
#include <ntddstor.h>    // GUID_DEVINTERFACE_DISK
#include <wdmguid.h>

#include <sys/efi_partition.h>

#define	dprintf(...) \
	KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, __VA_ARGS__))


// ----------------------------------------------------------------------
// Globals
// ----------------------------------------------------------------------
static PDRIVER_OBJECT g_ShimDriverObject;
static PVOID g_PnpNotifyHandle;

// ----------------------------------------------------------------------
// Forward declarations
// ----------------------------------------------------------------------
struct shim_extension {
    PDEVICE_OBJECT LowerDevice;
    PDRIVE_LAYOUT_INFORMATION_EX layout;
    ULONG layout_size;
    ULONG sector_size;
    WORK_QUEUE_ITEM RemoveWorkItem;
};
typedef struct shim_extension shim_extension_t;

typedef struct _SHIM_WORK {
    WORK_QUEUE_ITEM WorkItem;
    UNICODE_STRING LinkName;
} SHIM_WORK, *PSHIM_WORK;

void ShimAttachWorker(PVOID Context);

DRIVER_UNLOAD ShimUnload;
DRIVER_INITIALIZE ShimDriverEntry;
NTSTATUS ShimDispatchPnp(PDEVICE_OBJECT, PIRP);
NTSTATUS ShimDispatchIoctl(PDEVICE_OBJECT, PIRP);
void ShimDiskInterfaceChange(
    PDEVICE_INTERFACE_CHANGE_NOTIFICATION,
    PVOID);
BOOLEAN ShouldShimDisk(PDEVICE_OBJECT, PFILE_OBJECT, shim_extension_t *);

NTSTATUS NTAPI IoCreateDriver(
    _In_opt_ PUNICODE_STRING 	DriverName,
    _In_ PDRIVER_INITIALIZE 	InitializationFunction);


// ----------------------------------------------------------------------
// Exported from openzfs.sys to kick this shim up
// ----------------------------------------------------------------------

void
InitializePartitionShim(void)
{
	UNICODE_STRING drvName = RTL_CONSTANT_STRING(
	    L"\\Driver\\OpenZFSPartitionShim");
	// Create a second driver object and call ShimDriverEntry on it:
	IoCreateDriver(&drvName, ShimDriverEntry);
	dprintf("Started Shim\n");
}

//
// Simple passthrough dispatch: skip this stack frame and forward.
// Use this for all IRP_MJ_xxx you don't want to special-case.
//
NTSTATUS
ShimPassThrough(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp)
{
	shim_extension_t *shimext =
	    (shim_extension_t *)DeviceObject->DeviceExtension;

	IoSkipCurrentIrpStackLocation(Irp);
	return (IoCallDriver(
	    shimext->LowerDevice,
	    Irp));
}


// ----------------------------------------------------------------------
// ShimDriverEntry: runs once for the shim driver
// ----------------------------------------------------------------------

NTSTATUS
ShimDriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
	UNREFERENCED_PARAMETER(RegistryPath);

	g_ShimDriverObject = DriverObject;
	DriverObject->DriverUnload = ShimUnload;

	// Register for existing & future disks:
	IoRegisterPlugPlayNotification(
	    EventCategoryDeviceInterfaceChange,
	    PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
	    (PVOID)&GUID_DEVINTERFACE_DISK,
	    DriverObject,
	    (PDRIVER_NOTIFICATION_CALLBACK_ROUTINE)ShimDiskInterfaceChange,
	    NULL,
	    &g_PnpNotifyHandle);

	// Default-passthrough for all IRPs:
	for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
		DriverObject->MajorFunction[i] = ShimPassThrough;

	// Hook only PnP and DeviceControl:
	DriverObject->MajorFunction[IRP_MJ_PNP] = ShimDispatchPnp;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = ShimDispatchIoctl;

	return (STATUS_SUCCESS);
}

// ----------------------------------------------------------------------
// ShimUnload: cleanup notifications, never called with IoCreateDriver
// ----------------------------------------------------------------------
void
ShimUnload(_In_ PDRIVER_OBJECT DriverObject)
{
	if (g_PnpNotifyHandle) {
		IoUnregisterPlugPlayNotification(g_PnpNotifyHandle);
		g_PnpNotifyHandle = NULL;
	}
	dprintf("Ending Shim\n");
}

// Manual unload of Shim driver, called from driver.c
void
UnInitializePartitionShim(void)
{
	ShimUnload(NULL);
}

// ----------------------------------------------------------------------
// ShimDiskInterfaceChange: attach per-disk filter if ShouldShim()
// ----------------------------------------------------------------------

void
ShimDiskInterfaceChange(
    PDEVICE_INTERFACE_CHANGE_NOTIFICATION not,
    PVOID context)
{
	UNREFERENCED_PARAMETER(context);

	// We care only about arrivals.
	if (RtlCompareMemory(&not->Event,
	    &GUID_DEVICE_INTERFACE_ARRIVAL,
	    sizeof (GUID)) != sizeof (GUID))
		return;

	dprintf("%s arrival: %wZ\n", __func__, not->SymbolicLinkName);

	// Can't do much here directly, so punt it to a worker thread.
	PSHIM_WORK w = ExAllocatePoolWithTag(
	    NonPagedPool,
	    sizeof (*w),
	    'zfsS');
	if (!w)
		return;

	// 2) Copy the symbolic name into nonpaged memory
	w->LinkName.Length = not->SymbolicLinkName->Length;
	w->LinkName.MaximumLength = not->SymbolicLinkName->Length;
	w->LinkName.Buffer = ExAllocatePoolWithTag(
	    NonPagedPool,
	    w->LinkName.Length,
	    'zfsL');
	if (!w->LinkName.Buffer) {
		ExFreePool(w);
		return;
	}

	RtlCopyMemory(
	    w->LinkName.Buffer,
	    not->SymbolicLinkName->Buffer,
	    w->LinkName.Length);

	// 3) Queue the work item at PASSIVE_LEVEL
	ExInitializeWorkItem(&w->WorkItem,
	    ShimAttachWorker,
	    w);
	ExQueueWorkItem(&w->WorkItem, DelayedWorkQueue);
}

// Disk has arrived, go check it out
void
ShimAttachWorker(PVOID Context)
{
	PSHIM_WORK w = Context;
	BOOLEAN shim_disk = FALSE;
	PFILE_OBJECT fileObj = NULL;
	PDEVICE_OBJECT topPDO = NULL;
	PDEVICE_OBJECT bottomPDO = NULL;
	PDEVICE_OBJECT diskPDO = NULL;
	NTSTATUS status;

	// Grab the file and device object
	status = IoGetDeviceObjectPointer(
	    &w->LinkName,
	    FILE_READ_DATA,
	    &fileObj,
	    &topPDO);
	if (!NT_SUCCESS(status))
		return;

	// Typically, topPDO here is FltMgr.sys
	// Bottom is Disk.sys, which we can
	// READ from.
	bottomPDO = IoGetDeviceAttachmentBaseRef(topPDO);

	// We also need to Attach to the right place:
	diskPDO = fileObj->DeviceObject;
	ObReferenceObject(diskPDO);

	// Now go READ the partition to see if it needs Shimming.
	// tmo_shimext->layout will hold the Partition Label to use.
	shim_extension_t tmp_shimext;
	shim_disk = ShouldShimDisk(bottomPDO, fileObj, &tmp_shimext);

	// Release everything except diskPDO, if we attach.
	ObDereferenceObject(fileObj);
	ObDereferenceObject(bottomPDO);

	// No Shim layer needed, exit
	if (!shim_disk)
		goto out;

	dprintf("ShimDisk %wZ\n", &w->LinkName);

	// Create an anonymous filter DO and attach above diskPDO:
	PDEVICE_OBJECT filterDO;
	status = IoCreateDevice(
	    g_ShimDriverObject,
	    sizeof (shim_extension_t),
	    NULL,
	    FILE_DEVICE_DISK,
	    0,
	    FALSE,
	    &filterDO);
	if (!NT_SUCCESS(status)) {
		ExFreePool(tmp_shimext.layout);
		goto out;
	}

	// We created a new Shim device, assign the real shimext to it.
	shim_extension_t *shimext;
	shimext = (shim_extension_t *)filterDO->DeviceExtension;
	memcpy(shimext, &tmp_shimext, sizeof (shim_extension_t));

	// Copy over buffering flags so reads/writes route the same way:
	filterDO->Flags |= diskPDO->Flags &
	    (DO_BUFFERED_IO | DO_DIRECT_IO);

	// Attach us to intercept partition label reads.
	shimext->LowerDevice = IoAttachDeviceToDeviceStack(filterDO,
	    fileObj->DeviceObject);
	filterDO->Flags &= ~DO_DEVICE_INITIALIZING;

out:
	ObDereferenceObject(diskPDO);
	ExFreePool(w->LinkName.Buffer);
	ExFreePool(w);

	dprintf("%s return %d\n", __func__, status);
}

// ----------------------------------------------------------------------
// ShimDispatchPnp: passthrough all PnP IRPs
// ----------------------------------------------------------------------

NTSTATUS
ShimDispatchPnp(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp)
{
	shim_extension_t *shimext =
	    (shim_extension_t *)DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION  sp = IoGetCurrentIrpStackLocation(Irp);

	switch (sp->MinorFunction) {

	case IRP_MN_REMOVE_DEVICE:
		IoSkipCurrentIrpStackLocation(Irp);

		NTSTATUS ret = IoCallDriver(shimext->LowerDevice, Irp);

		dprintf("Removing Shim\n");
		if (DeviceObject->AttachedDevice)
			IoDetachDevice(DeviceObject);
		IoDeleteDevice(DeviceObject);
		return (ret);
	}

	IoSkipCurrentIrpStackLocation(Irp);
	return (IoCallDriver(shimext->LowerDevice, Irp));
}


//
// Build a new DRIVE_LAYOUT_INFORMATION_EX based on the on-disk EFI data.
// Returns NULL on failure.
//
PDRIVE_LAYOUT_INFORMATION_EX
BuildGptDriveLayout(
    _In_reads_bytes_(sizeof (efi_gpt_t))
    efi_gpt_t *gptHeaderBuf,
    _In_reads_bytes_(gptHeaderBuf->NumberOfPartitionEntries *
	gptHeaderBuf->SizeOfPartitionEntry)
    efi_gpe_t *gptArrayBuf,
    ULONG SectorSize)
{
	USHORT realCount =
	    (USHORT) gptHeaderBuf->efi_gpt_NumberOfPartitionEntries;
	ULONG slotCount = 0;
	ULONG layoutSize =
	    sizeof (DRIVE_LAYOUT_INFORMATION_EX) +
	    (slotCount - 1) * sizeof (PARTITION_INFORMATION_EX);

	// Allocate & zero
	PDRIVE_LAYOUT_INFORMATION_EX layout =
	    ExAllocatePoolWithTag(NonPagedPoolNx, layoutSize, 'shim');
	if (!layout)
		return (NULL);
	RtlZeroMemory(layout, layoutSize);

	// 1) Top‐level fields
	layout->PartitionStyle = PARTITION_STYLE_GPT;
	layout->PartitionCount = slotCount;

	// Copy the EFI header into the embed
	// layout->Gpt.DiskId = gptHeaderBuf->efi_gpt_DiskGUID;
	memcpy(&layout->Gpt.DiskId,
	    &gptHeaderBuf->efi_gpt_DiskGUID,
	    sizeof (layout->Gpt.DiskId));

	// layout->Gpt.Header = *gptHeaderBuf;
	int outPart = 0;

	// 2) Populate the real entries
	for (USHORT i = 0; i < realCount && i < slotCount; i++) {
		PARTITION_INFORMATION_EX *out =
		    &layout->PartitionEntry[outPart];
		efi_gpe_t *in = &gptArrayBuf[i];

		if (in->efi_gpe_StartingLBA == 0 &&
		    in->efi_gpe_EndingLBA == 0)
			continue;

		out->PartitionStyle = PARTITION_STYLE_GPT;
		out->StartingOffset.QuadPart =
		    (LONGLONG)in->efi_gpe_StartingLBA *
		    (LONGLONG)SectorSize;
		out->PartitionLength.QuadPart =
		    ((LONGLONG)in->efi_gpe_EndingLBA -
		    (LONGLONG)in->efi_gpe_StartingLBA + 1) *
		    (LONGLONG)SectorSize;
		out->PartitionNumber = (USHORT)(i + 1);
		out->RewritePartition = FALSE;

		// GPT‐specific fields
		memcpy(&out->Gpt.PartitionType,
		    &in->efi_gpe_PartitionTypeGUID,
		    sizeof (out->Gpt.PartitionType));

		memcpy(&out->Gpt.PartitionId,
		    &in->efi_gpe_UniquePartitionGUID,
		    sizeof (out->Gpt.PartitionId));

		out->Gpt.Attributes = in->efi_gpe_Attributes.PartitionAttrs;

		RtlCopyMemory(
		    out->Gpt.Name,
		    in->efi_gpe_PartitionName,
		    sizeof (out->Gpt.Name));

		outPart++;
	}

	// 3) The rest of the slots [realCount..slotCount) remain zeroed
	layout->PartitionCount = outPart;

	dprintf("BuildGptDriveLayout: %u slots, %u real\n", outPart, realCount);

	return (layout);
}

NTSTATUS
shim_change_partitionlabel(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context)
{
	shim_extension_t *shimext =
	    (shim_extension_t *)DeviceObject->DeviceExtension;
	UNREFERENCED_PARAMETER(Context);

	// Grab the returned layout
	PDRIVE_LAYOUT_INFORMATION_EX layout =
	    (PDRIVE_LAYOUT_INFORMATION_EX)Irp->AssociatedIrp.SystemBuffer;

	dprintf("Changing partition label status %lx PartitionCount %u\n",
	    Irp->IoStatus.Status, layout ? layout->PartitionCount : 0);
	if (shimext->layout) {

		dprintf("Before %p of length %u\n",
		    Irp->AssociatedIrp.SystemBuffer, Irp->IoStatus.Information);

		PDRIVE_LAYOUT_INFORMATION_EX newLayout =
		    ExAllocatePoolWithTag(
		    NonPagedPool,
		    shimext->layout_size,
		    'zfsS');

		if (newLayout) {
			// Free the original buffer the I/O manager gave us
			ExFreePool(Irp->AssociatedIrp.SystemBuffer);

			// Swap in ours
			memcpy(newLayout, shimext->layout,
			    shimext->layout_size);
			Irp->AssociatedIrp.SystemBuffer = newLayout;
			Irp->IoStatus.Information = shimext->layout_size;
			dprintf("After %p of length %u\n",
			    Irp->AssociatedIrp.SystemBuffer,
			    Irp->IoStatus.Information);
		}
	}

	// Let the I/O manager finish completing the IRP
	return (STATUS_SUCCESS);
}

// ----------------------------------------------------------------------
// ShimDispatchIoctl: passthrough all IOCTLs
// (you'll hook IOCTL_DISK_GET_DRIVE_LAYOUT_EX here later)
// ----------------------------------------------------------------------

NTSTATUS
ShimDispatchIoctl(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp)
{
	PIO_STACK_LOCATION  sp = IoGetCurrentIrpStackLocation(Irp);
	shim_extension_t *shimext =
	    (shim_extension_t *)DeviceObject->DeviceExtension;

	// dprintf("Relay %u\n", sp->Parameters.DeviceIoControl.IoControlCode);
	if (sp->Parameters.DeviceIoControl.IoControlCode ==
	    IOCTL_DISK_GET_DRIVE_LAYOUT_EX) {

		IoCopyCurrentIrpStackLocationToNext(Irp);

		IoSetCompletionRoutine(
		    Irp,
		    shim_change_partitionlabel,
		    NULL,
		    TRUE, TRUE, TRUE);
		dprintf("Got IOCTL_DISK_GET_DRIVE_LAYOUT_EX:"
		    " added completion.\n");

		return (IoCallDriver(
		    shimext->LowerDevice,
		    Irp));
	}

	IoSkipCurrentIrpStackLocation(Irp);
	return (IoCallDriver(
	    shimext->LowerDevice,
	    Irp));
}

unsigned char *
shim_alloc_and_read(PDEVICE_OBJECT diskPDO,
    uint64_t offset,
    uint64_t length,
    uint64_t sectorsize)
{
	KEVENT event;
	IO_STATUS_BLOCK iosb;
	PIRP irp;
	NTSTATUS status;
	unsigned char *buffer;
	unsigned char *ptr;

	// We can only read sectorsize here, so roundup the length
	length = P2ROUNDUP(length, sectorsize);

	buffer = ExAllocatePoolWithTag(
	    NonPagedPool,
	    length,
	    'zfsS');
	if (!buffer)
		return (NULL);

	LARGE_INTEGER byteOffset;

	dprintf("attempting to read offset %llx length %llx\n",
	    offset, length);

	ptr = buffer;

	while (length > 0) {

		int amount = MIN(sectorsize, length);

		byteOffset.QuadPart = offset;

		// Initialize an event for the IRP to signal on
		KeInitializeEvent(&event, NotificationEvent, FALSE);

		irp = IoBuildSynchronousFsdRequest(
		    IRP_MJ_READ,
		    diskPDO,
		    ptr,
		    amount,
		    &byteOffset,
		    &event,
		    &iosb);
		if (!irp)
			return (NULL);

		// 6) Send it directly to disk.sys
		status = IoCallDriver(diskPDO, irp);
		if (status == STATUS_PENDING) {
			KeWaitForSingleObject(&event,
			    Executive,
			    KernelMode,
			    FALSE,
			    NULL);
			status = iosb.Status;
		}

		if (!NT_SUCCESS(status)) {
			ExFreePool(buffer);
			return (NULL);
		}

		ptr += amount;
		offset += amount;
		length -= amount;
	}

	dprintf("Read OK\n");
	return (buffer);
}

// ----------------------------------------------------------------------
// ShouldShimDisk: YOUR hook to call libefi and detect a 9-entry GPT
// ----------------------------------------------------------------------

BOOLEAN
ShouldShimDisk(PDEVICE_OBJECT diskPDO, PFILE_OBJECT FileObject,
    shim_extension_t *shimext)
{

	// Lookup SectorSize!
	efi_gpt_t *gptHeaderBuf = NULL;
	efi_gpe_t *gptPartBuf = NULL;
	efi_gpt_t *gptBackupBuf = NULL;

	KEVENT event;
	IO_STATUS_BLOCK iosb;
	PIRP irp;
	NTSTATUS status;
	DISK_GEOMETRY_EX geom;
	ULONG SectorSize = 512; // sane default
	unsigned char *buffer;
	int i;

	// init an event for the IRP completion
	KeInitializeEvent(&event, NotificationEvent, FALSE);

	// build the IOCTL_DISK_GET_DRIVE_GEOMETRY_EX request
	irp = IoBuildDeviceIoControlRequest(
	    IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
	    diskPDO,
	    NULL, 0,
	    &geom, sizeof (geom),
	    FALSE,
	    &event,
	    &iosb);
	if (!irp)
		return (FALSE);

	// send it down
	status = IoCallDriver(diskPDO, irp);
	if (status == STATUS_PENDING) {
		KeWaitForSingleObject(&event,
		    Executive,
		    KernelMode,
		    FALSE,
		    NULL);
		status = iosb.Status;
	}

	if (NT_SUCCESS(status)) {
		SectorSize = geom.Geometry.BytesPerSector;
	}

	dprintf("SectorSize = %u\n", SectorSize);

	// Read Sector 1
	buffer = shim_alloc_and_read(diskPDO, 1 * SectorSize,
	    SectorSize, SectorSize);
	if (!buffer) {
		dprintf("failed to read sector 1\n");
		return (FALSE);
	}

	gptHeaderBuf = (efi_gpt_t *)buffer;

	dprintf("Read Said %lx. This disk has %u partitions\n", status,
	    status ? 0 : gptHeaderBuf->efi_gpt_NumberOfPartitionEntries);

	BOOLEAN ret = FALSE;

	if (gptHeaderBuf->efi_gpt_NumberOfPartitionEntries == 9) {

		uint64_t length =
		    gptHeaderBuf->efi_gpt_NumberOfPartitionEntries *
		    gptHeaderBuf->efi_gpt_SizeOfPartitionEntry;

		buffer = shim_alloc_and_read(diskPDO,
		    gptHeaderBuf->efi_gpt_PartitionEntryLBA * SectorSize,
		    length,
		    SectorSize);
		if (!buffer) {
			dprintf("failed to read sector array\n");
			ExFreePool(gptHeaderBuf);
			gptHeaderBuf = NULL;
			return (FALSE);
		}

		// Is it all zero? Windows has a habit to zero out the
		// partition table if it doesn't like it.
		// Replace this with CRC check.
		for (i = 0; i < length; i++) {
			if (buffer[i] != 0)
				break;
		}

		// Try backup then?
		if (i == length) {
			dprintf("Trying backup label\n");

			buffer = shim_alloc_and_read(diskPDO,
			    gptHeaderBuf->efi_gpt_AlternateLBA * SectorSize,
			    SectorSize,
			    SectorSize);

			if (!buffer) {
				dprintf("failed to read backup label\n");
				ExFreePool(gptHeaderBuf);
				gptHeaderBuf = NULL;
				return (FALSE);
			}

			gptBackupBuf = (efi_gpt_t *)buffer;

			buffer = shim_alloc_and_read(diskPDO,
			    gptBackupBuf->efi_gpt_PartitionEntryLBA *
			    SectorSize,
			    length,
			    SectorSize);
			if (!buffer) {
				dprintf("failed to read backup sector array\n");
				ExFreePool(gptHeaderBuf);
				ExFreePool(gptBackupBuf);
				gptHeaderBuf = NULL;
				return (FALSE);
			}

			// We should repair the label here.
			for (i = 0; i < length; i++) {
				if (buffer[i] != 0)
					break;
			}

			// Both failed, not much to do
			if (i == length) {
				dprintf("Primary and Backup failed.\n");
				ExFreePool(gptHeaderBuf);
				ExFreePool(gptBackupBuf);
				gptHeaderBuf = NULL;
				return (FALSE);
			}
		}

		gptPartBuf = (efi_gpe_t *)buffer;

		// If we get here, we have valid gptHeaderBuf and
		// valid gptPartBuf

		PDRIVE_LAYOUT_INFORMATION_EX tmplayout =
		    BuildGptDriveLayout(gptHeaderBuf, gptPartBuf, SectorSize);

		if (tmplayout) {
			shimext->layout = tmplayout;
			shimext->layout_size =
			    sizeof (DRIVE_LAYOUT_INFORMATION_EX)
			    + (tmplayout->PartitionCount - 1) *
			    sizeof (PARTITION_INFORMATION_EX);
			shimext->sector_size = SectorSize;

			ret = TRUE;
		}

		ExFreePool(gptHeaderBuf);
		ExFreePool(gptBackupBuf);

	}

	return (ret);
}
