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
 * Copyright (c) 2024 by Jorgen Lundman <lundman@lundman.net>.
 */

#ifndef SYS_DRIVER_EXTENSION_H
#define	SYS_DRIVER_EXTENSION_H

struct OpenZFS_Driver_Extension_s {
	PDEVICE_OBJECT PhysicalDeviceObject;
	PDEVICE_OBJECT LowerDeviceObject;
	PDEVICE_OBJECT FunctionalDeviceObject; // AddDevice unknown
	PDEVICE_OBJECT AddDeviceObject;    // Passed along when mounting
	PDEVICE_OBJECT ChildDeviceObject;  // Passed along when creating
	PDEVICE_OBJECT ioctlDeviceObject;  // /dev/zfs pdo
	PDEVICE_OBJECT fsDiskDeviceObject; // /dev/zfs vdo
	PDEVICE_OBJECT StorportDeviceObject;

	PDRIVER_UNLOAD STOR_DriverUnload;
	PDRIVER_ADD_DEVICE STOR_AddDevice;
	PDRIVER_DISPATCH STOR_MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];
};

typedef struct OpenZFS_Driver_Extension_s OpenZFS_Driver_Extension;

#define	ZFS_DRIVER_EXTENSION(DO, V) \
    OpenZFS_Driver_Extension *(V) = \
    (OpenZFS_Driver_Extension *)IoGetDriverObjectExtension((DO), (DO));

extern int
zfs_init_driver_extension(PDRIVER_OBJECT);

#define INITGUID
#include <guiddef.h>
DEFINE_GUID(BtrfsBusInterface, 0x4d414874, 0x6865, 0x6761, 0x6d, 0x65, 0x83, 0x69, 0x17, 0x9a, 0x7d, 0x1d);

#endif
