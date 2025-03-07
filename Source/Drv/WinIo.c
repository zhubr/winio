// ---------------------------------------------------- //
//                      WinIo v3.0                      //
//					Direct Hardware Access for Windows	//
//           Copyright 1998-2010 Yariv Kaplan           //
//               http://www.internals.com               //
// ---------------------------------------------------- //

#include <ntddk.h>
#include "winio_nt.h"

#define IOPM_SIZE 0x2000
typedef char IOPM[IOPM_SIZE];

// Function definition section
// -----------------------------------------------------------------
NTSTATUS WinIoDispatch(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp);
void WinIoUnload(IN PDRIVER_OBJECT DriverObject);
NTSTATUS UnmapPhysicalMemory(HANDLE PhysicalMemoryHandle, PVOID pPhysMemLin, PVOID PhysSection);
NTSTATUS MapPhysicalMemoryToLinearSpace(PVOID pPhysAddress,
										SIZE_T PhysMemSizeInBytes,
										PVOID *ppPhysMemLin,
										HANDLE *pPhysicalMemoryHandle,
										PVOID *ppPhysSection);

#ifndef AMD64
void Ke386SetIoAccessMap(int, IOPM *);
void Ke386QueryIoAccessMap(int, IOPM *);
void Ke386IoSetAccessProcess(PEPROCESS, int);
#endif

// -----------------------------------------------------------------

struct tagAllocStruct curr_blocks[WINIO_ALLOC_LIMIT];


void AllocContigMemory(int blk_idx, struct tagAllocStruct *blk) {

	PHYSICAL_ADDRESS   tmpPhys, tmpZero;

	tmpZero.QuadPart = 0;
	tmpPhys.QuadPart = blk->pvPhysMask;
	blk->pvKrnlAddress = MmAllocateContiguousMemorySpecifyCache(blk->dwPhysMemSizeInBytes, tmpZero, tmpPhys, tmpZero, blk->MappingFlags);
        if (blk->pvKrnlAddress) {
		blk->AllocIndex = blk_idx;
		tmpPhys = MmGetPhysicalAddress((PVOID)blk->pvKrnlAddress);
		blk->pvPhysAddress = tmpPhys.QuadPart;
        }
}

void FreeContigMemory(int blk_idx, struct tagAllocStruct *blk) {

	if (blk->pvKrnlAddress && !blk->pvMdl) {
		blk->AllocIndex = 0;
		MmFreeContiguousMemory((PVOID)blk->pvKrnlAddress);
		blk->pvKrnlAddress = 0;
		blk->pvPhysAddress = 0;
	}

}

void MapContigMemory(int blk_idx, struct tagAllocStruct *blk, PFILE_OBJECT FileObject) {

        if (blk->AllocIndex && blk->pvKrnlAddress && !blk->pvMdl && !blk->pvPhysMemLin) {
		blk->pvMdl = (DWORD64)IoAllocateMdl((PVOID)blk->pvKrnlAddress, blk->dwPhysMemSizeInBytes, FALSE, FALSE, NULL);
		if (blk->pvMdl) {
			blk->pvFileObj = (DWORD64)FileObject;
			MmBuildMdlForNonPagedPool((PMDL)blk->pvMdl);
			blk->pvPhysMemLin = (DWORD64)MmMapLockedPagesSpecifyCache((PMDL)blk->pvMdl, UserMode, blk->MappingFlags, NULL, FALSE, NormalPagePriority);
		}
        }
}

void UnmapContigMemory(int blk_idx, struct tagAllocStruct *blk) {

        if (blk->pvPhysMemLin) {

		MmUnmapLockedPages((PVOID)blk->pvPhysMemLin, (PMDL)blk->pvMdl);
		blk->pvPhysMemLin = 0;

        }

	if (blk->pvMdl) {
		IoFreeMdl((PMDL)blk->pvMdl);
		blk->pvMdl = 0;
	}

	blk->pvFileObj = 0;
}

void UnmapContigMemoryAll(PFILE_OBJECT FileObject) {

        int blk_num;

	for (blk_num = 0; blk_num < WINIO_ALLOC_LIMIT; blk_num++) if (curr_blocks[blk_num].AllocIndex != 0)
		if (curr_blocks[blk_num].pvFileObj == (DWORD64)FileObject)
			UnmapContigMemory(blk_num+1, &curr_blocks[blk_num]);
}

void FreeContigMemoryAll() {

        int blk_num;

	for (blk_num = 0; blk_num < WINIO_ALLOC_LIMIT; blk_num++) if (curr_blocks[blk_num].AllocIndex != 0) {
		UnmapContigMemory(blk_num+1, &curr_blocks[blk_num]);
		FreeContigMemory(blk_num+1, &curr_blocks[blk_num]);
        }
}


// Installable driver initialization entry point.
// This entry point is called directly by the I/O system.

NTSTATUS DriverEntry (IN PDRIVER_OBJECT DriverObject,
					  IN PUNICODE_STRING RegistryPath)
{
	UNICODE_STRING  DeviceNameUnicodeString;
	UNICODE_STRING  DeviceLinkUnicodeString;
	NTSTATUS        ntStatus;
	PDEVICE_OBJECT  DeviceObject = NULL;

	KdPrint(("Entering DriverEntry"));

	memset(curr_blocks, 0, sizeof(curr_blocks));
	RtlInitUnicodeString (&DeviceNameUnicodeString, L"\\Device\\WinIo");

	// Create a device object 

	ntStatus = IoCreateDevice (DriverObject,
		0,
		&DeviceNameUnicodeString,
		FILE_DEVICE_WINIO,
		0,
		FALSE,
		&DeviceObject);

	if (NT_SUCCESS(ntStatus))
	{
		// Create dispatch points for device control, create, close.

		DriverObject->MajorFunction[IRP_MJ_CREATE]		   =
		DriverObject->MajorFunction[IRP_MJ_CLOSE]		   =
		DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = WinIoDispatch;
		DriverObject->DriverUnload                         = WinIoUnload;

		// Create a symbolic link, e.g. a name that a Win32 app can specify
		// to open the device.

		RtlInitUnicodeString (&DeviceLinkUnicodeString, L"\\DosDevices\\WinIo");

		ntStatus = IoCreateSymbolicLink (&DeviceLinkUnicodeString,
			&DeviceNameUnicodeString);

		if (!NT_SUCCESS(ntStatus))
		{
			// Symbolic link creation failed- note this & then delete the
			// device object (it's useless if a Win32 app can't get at it).

			KdPrint(("ERROR: IoCreateSymbolicLink failed"));

			IoDeleteDevice (DeviceObject);
		}

	}
	else
	{
		KdPrint(("ERROR: IoCreateDevice failed"));
	}

	KdPrint(("Leaving DriverEntry"));

	return ntStatus;
}


// Process the IRPs sent to this device

NTSTATUS WinIoDispatch(IN PDEVICE_OBJECT DeviceObject,
					   IN PIRP Irp)
{
	PIO_STACK_LOCATION IrpStack;
	ULONG              dwInputBufferLength;
	ULONG              dwOutputBufferLength;
	ULONG              dwIoControlCode;
	PVOID              pvIOBuffer;
	NTSTATUS           ntStatus;
	IOPM*			   pIOPM = NULL;
	struct             tagPhysStruct PhysStruct;
	struct             tagPortStruct PortStruct;
        struct             tagAllocStruct AllocStruct;
	struct             tagPhysStruct32 *pPhysStruct32 = NULL;

	KdPrint(("Entering WinIoDispatch"));

	// Init to default settings

	Irp->IoStatus.Status      = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;

	IrpStack = IoGetCurrentIrpStackLocation(Irp);

	// Get the pointer to the input/output buffer and it's length

	pvIOBuffer           = Irp->AssociatedIrp.SystemBuffer;
	dwInputBufferLength  = IrpStack->Parameters.DeviceIoControl.InputBufferLength;
	dwOutputBufferLength = IrpStack->Parameters.DeviceIoControl.OutputBufferLength;

	switch (IrpStack->MajorFunction)
	{
	case IRP_MJ_CREATE:

		KdPrint(("IRP_MJ_CREATE"));

		break;

	case IRP_MJ_CLOSE:

		KdPrint(("IRP_MJ_CLOSE"));
		UnmapContigMemoryAll(IrpStack->FileObject);

		break;

	case IRP_MJ_DEVICE_CONTROL:

		KdPrint(("IRP_MJ_DEVICE_CONTROL"));

		dwIoControlCode = IrpStack->Parameters.DeviceIoControl.IoControlCode;

		switch (dwIoControlCode)
		{
			// The Ke386 calls are no longer available on x64 Windows

#ifndef AMD64
	case IOCTL_WINIO_ENABLEDIRECTIO:

		KdPrint(("IOCTL_WINIO_ENABLEDIRECTIO"));

		pIOPM = MmAllocateNonCachedMemory(sizeof(IOPM));

		if (pIOPM)
		{
			RtlZeroMemory(pIOPM, sizeof(IOPM));

			Ke386IoSetAccessProcess(PsGetCurrentProcess(), 1);
			Ke386SetIoAccessMap(1, pIOPM);
			IrpStack->FileObject->FsContext2 = pIOPM;
		}
		else
			Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;

		break;

	case IOCTL_WINIO_DISABLEDIRECTIO:

		KdPrint(("IOCTL_WINIO_DISABLEDIRECTIO"));

		pIOPM = (IOPM *)IrpStack->FileObject->FsContext2;

		if (pIOPM)
		{
			Ke386IoSetAccessProcess(PsGetCurrentProcess(), 0);
			Ke386SetIoAccessMap(1, pIOPM);
			MmFreeNonCachedMemory(pIOPM, sizeof(IOPM));
		}

		break;
#endif

	case IOCTL_WINIO_WRITEPORT:

		KdPrint(("IOCTL_WINIO_WRITEPORT"));

		if (dwInputBufferLength)
		{
			memcpy (&PortStruct, pvIOBuffer, dwInputBufferLength);

			switch (PortStruct.bSize)
			{
			case 1:
				WRITE_PORT_UCHAR((PUCHAR)(USHORT)PortStruct.wPortAddr, (UCHAR)PortStruct.dwPortVal);
				break;

			case 2:
				WRITE_PORT_USHORT((PUSHORT)(USHORT)PortStruct.wPortAddr, (USHORT)PortStruct.dwPortVal);
				break;

			case 4:
				WRITE_PORT_ULONG((PULONG)(USHORT)PortStruct.wPortAddr, PortStruct.dwPortVal);
				break;
			}
		}
		else
			Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;

		break;

	case IOCTL_WINIO_READPORT:

		KdPrint(("IOCTL_WINIO_READPORT"));

		if (dwInputBufferLength)
		{
			memcpy (&PortStruct, pvIOBuffer, dwInputBufferLength);

			switch (PortStruct.bSize)
			{
			case 1:
				PortStruct.dwPortVal = (ULONG)READ_PORT_UCHAR((PUCHAR)(USHORT)PortStruct.wPortAddr);
				break;

			case 2:
				PortStruct.dwPortVal = (ULONG)READ_PORT_USHORT((PUSHORT)(USHORT)PortStruct.wPortAddr);
				break;

			case 4:
				PortStruct.dwPortVal = READ_PORT_ULONG((PULONG)(USHORT)PortStruct.wPortAddr);
				break;
			}

			memcpy (pvIOBuffer, &PortStruct.dwPortVal, sizeof(PortStruct.dwPortVal));
			Irp->IoStatus.Information = sizeof(PortStruct.dwPortVal);
		}
		else
			Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;

		break;

	case IOCTL_WINIO_MAPPHYSTOLIN:

		KdPrint(("IOCTL_WINIO_MAPPHYSTOLIN"));

		if (dwInputBufferLength)
		{
			memcpy (&PhysStruct, pvIOBuffer, dwInputBufferLength);

			ntStatus = MapPhysicalMemoryToLinearSpace((PVOID)PhysStruct.pvPhysAddress,
				(SIZE_T)PhysStruct.dwPhysMemSizeInBytes,
				(PVOID *)&PhysStruct.pvPhysMemLin,
				(HANDLE *)&PhysStruct.PhysicalMemoryHandle,
				(PVOID *)&PhysStruct.pvPhysSection);

			if (NT_SUCCESS(ntStatus))
			{
				memcpy (pvIOBuffer, &PhysStruct, dwInputBufferLength);
				Irp->IoStatus.Information = dwInputBufferLength;
			}

			Irp->IoStatus.Status = ntStatus;
		}
		else
			Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;

		break;

	case IOCTL_WINIO_UNMAPPHYSADDR:

		KdPrint(("IOCTL_WINIO_UNMAPPHYSADDR"));

		if (dwInputBufferLength)
		{
			memcpy (&PhysStruct, pvIOBuffer, dwInputBufferLength);

			ntStatus = UnmapPhysicalMemory((HANDLE)PhysStruct.PhysicalMemoryHandle, (PVOID)PhysStruct.pvPhysMemLin, (PVOID)PhysStruct.pvPhysSection);

			Irp->IoStatus.Status = ntStatus;
		}
		else
			Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;

		break;

	case IOCTL_WINIO_CNTGALLOC:

		KdPrint(("IOCTL_WINIO_CNTGALLOC"));

		if ((dwInputBufferLength >= 8*4) && (dwOutputBufferLength >= 8*6))
		{
			if (dwInputBufferLength > sizeof(AllocStruct)) dwInputBufferLength = sizeof(AllocStruct);
			if (dwOutputBufferLength > sizeof(AllocStruct)) dwOutputBufferLength = sizeof(AllocStruct);
			memset (&AllocStruct, 0, sizeof(AllocStruct));
			memcpy (&AllocStruct, pvIOBuffer, dwInputBufferLength);

			if ((AllocStruct.AllocIndex > 0) && (AllocStruct.AllocIndex <= WINIO_ALLOC_LIMIT)) {

				if ((0 == curr_blocks[AllocStruct.AllocIndex-1].AllocIndex) && (0 != AllocStruct.dwPhysMemSizeInBytes)) {
                                        memset (&curr_blocks[AllocStruct.AllocIndex-1], 0, sizeof(curr_blocks[0]));
					curr_blocks[AllocStruct.AllocIndex-1].dwPhysMemSizeInBytes = AllocStruct.dwPhysMemSizeInBytes;
					curr_blocks[AllocStruct.AllocIndex-1].pvPhysMask = AllocStruct.pvPhysMask;
					curr_blocks[AllocStruct.AllocIndex-1].MappingFlags = AllocStruct.MappingFlags;
					AllocContigMemory((int)AllocStruct.AllocIndex, &curr_blocks[AllocStruct.AllocIndex-1]);
				}
				if (AllocStruct.AllocIndex == curr_blocks[AllocStruct.AllocIndex-1].AllocIndex) {
					memcpy (pvIOBuffer, &curr_blocks[AllocStruct.AllocIndex-1], dwOutputBufferLength);
					Irp->IoStatus.Information = dwOutputBufferLength;
					Irp->IoStatus.Status = STATUS_SUCCESS;
				}

			} else {
                                AllocStruct.AllocIndex = WINIO_ALLOC_LIMIT;
				AllocStruct.pvPhysAddress = 0;
				//AllocStruct.pvPhysMask = 0;
				AllocStruct.pvKrnlAddress = 0;
				AllocStruct.pvPhysMemLin = 0;
				memcpy (pvIOBuffer, &AllocStruct, dwOutputBufferLength);
				Irp->IoStatus.Information = dwOutputBufferLength;
				Irp->IoStatus.Status = STATUS_SUCCESS;
			}
		}
		else
			Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;

		break;

	case IOCTL_WINIO_CNTGMAP:

		KdPrint(("IOCTL_WINIO_CNTGMAP"));

		if ((dwInputBufferLength >= 8*4) && (dwOutputBufferLength >= 8*6))
		{
			if (dwInputBufferLength > sizeof(AllocStruct)) dwInputBufferLength = sizeof(AllocStruct);
			if (dwOutputBufferLength > sizeof(AllocStruct)) dwOutputBufferLength = sizeof(AllocStruct);
			memset (&AllocStruct, 0, sizeof(AllocStruct));
			memcpy (&AllocStruct, pvIOBuffer, dwInputBufferLength);

			if ((AllocStruct.AllocIndex > 0) && (AllocStruct.AllocIndex <= WINIO_ALLOC_LIMIT)) {

				if (curr_blocks[AllocStruct.AllocIndex-1].AllocIndex) {
					UnmapContigMemory((int)AllocStruct.AllocIndex, &curr_blocks[AllocStruct.AllocIndex-1]);
					MapContigMemory((int)AllocStruct.AllocIndex, &curr_blocks[AllocStruct.AllocIndex-1], IrpStack->FileObject);
					memcpy (pvIOBuffer, &curr_blocks[AllocStruct.AllocIndex-1], dwOutputBufferLength);
					Irp->IoStatus.Information = dwOutputBufferLength;
					Irp->IoStatus.Status = STATUS_SUCCESS;
				} else
					Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;

			} else
				Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
		}
		else
			Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;

		break;

	case IOCTL_WINIO_CNTGUNMAP:

		KdPrint(("IOCTL_WINIO_CNTGUNMAP"));

		if ((dwInputBufferLength >= 8*4) && (dwOutputBufferLength >= 8*6))
		{
			if (dwInputBufferLength > sizeof(AllocStruct)) dwInputBufferLength = sizeof(AllocStruct);
			if (dwOutputBufferLength > sizeof(AllocStruct)) dwOutputBufferLength = sizeof(AllocStruct);
			memset (&AllocStruct, 0, sizeof(AllocStruct));
			memcpy (&AllocStruct, pvIOBuffer, dwInputBufferLength);

			if ((AllocStruct.AllocIndex > 0) && (AllocStruct.AllocIndex <= WINIO_ALLOC_LIMIT)) {

				if (curr_blocks[AllocStruct.AllocIndex-1].AllocIndex) {
					UnmapContigMemory((int)AllocStruct.AllocIndex, &curr_blocks[AllocStruct.AllocIndex-1]);
					memcpy (pvIOBuffer, &curr_blocks[AllocStruct.AllocIndex-1], dwOutputBufferLength);
					Irp->IoStatus.Information = dwOutputBufferLength;
					Irp->IoStatus.Status = STATUS_SUCCESS;
				} else
					Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;

			} else
				Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
		}
		else
			Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;

		break;

	case IOCTL_WINIO_CNTGFREE:

		KdPrint(("IOCTL_WINIO_CNTGFREE"));

		if ((dwInputBufferLength >= 8*4) && (dwOutputBufferLength >= 8*6))
		{
			if (dwInputBufferLength > sizeof(AllocStruct)) dwInputBufferLength = sizeof(AllocStruct);
			if (dwOutputBufferLength > sizeof(AllocStruct)) dwOutputBufferLength = sizeof(AllocStruct);
			memset (&AllocStruct, 0, sizeof(AllocStruct));
			memcpy (&AllocStruct, pvIOBuffer, dwInputBufferLength);

			if ((AllocStruct.AllocIndex > 0) && (AllocStruct.AllocIndex <= WINIO_ALLOC_LIMIT)) {

				UnmapContigMemory((int)AllocStruct.AllocIndex, &curr_blocks[AllocStruct.AllocIndex-1]);
				FreeContigMemory((int)AllocStruct.AllocIndex, &curr_blocks[AllocStruct.AllocIndex-1]);
				memcpy (pvIOBuffer, &curr_blocks[AllocStruct.AllocIndex-1], dwOutputBufferLength);
				Irp->IoStatus.Information = dwOutputBufferLength;
				Irp->IoStatus.Status = STATUS_SUCCESS;

			} else
				Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
		}
		else
			Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;

		break;

	default:

		KdPrint(("ERROR: Unknown IRP_MJ_DEVICE_CONTROL"));

		Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;

		break;
		}

		break;
	}

	// DON'T get cute and try to use the status field of the irp in the
	// return status.  That IRP IS GONE as soon as you call IoCompleteRequest.

	ntStatus = Irp->IoStatus.Status;

	IoCompleteRequest (Irp, IO_NO_INCREMENT);

	// We never have pending operation so always return the status code.

	KdPrint(("Leaving WinIoDispatch"));

	return ntStatus;
}

// Delete the associated device and return

void WinIoUnload(IN PDRIVER_OBJECT DriverObject)
{
	UNICODE_STRING DeviceLinkUnicodeString;
	NTSTATUS ntStatus;

	KdPrint(("Entering WinIoUnload"));

	FreeContigMemoryAll();

	RtlInitUnicodeString (&DeviceLinkUnicodeString, L"\\DosDevices\\WinIo");

	ntStatus = IoDeleteSymbolicLink (&DeviceLinkUnicodeString);

	if (NT_SUCCESS(ntStatus))
	{
		IoDeleteDevice (DriverObject->DeviceObject);
	}
	else
	{
		KdPrint(("ERROR: IoDeleteSymbolicLink"));
	}

	KdPrint(("Leaving WinIoUnload"));
}


NTSTATUS MapPhysicalMemoryToLinearSpace(PVOID pPhysAddress,
										SIZE_T PhysMemSizeInBytes,
										PVOID *ppPhysMemLin,
										HANDLE *pPhysicalMemoryHandle,
										PVOID *ppPhysSection)
{
	UNICODE_STRING     PhysicalMemoryUnicodeString;
	OBJECT_ATTRIBUTES  ObjectAttributes;
	PHYSICAL_ADDRESS   ViewBase;
	NTSTATUS           ntStatus;
	PHYSICAL_ADDRESS   pStartPhysAddress;
	PHYSICAL_ADDRESS   pEndPhysAddress;
	BOOLEAN            Result1, Result2;
	ULONG              IsIOSpace;
	unsigned char     *pbPhysMemLin = NULL;

	KdPrint(("Entering MapPhysicalMemoryToLinearSpace"));

	RtlInitUnicodeString (&PhysicalMemoryUnicodeString, L"\\Device\\PhysicalMemory");

	InitializeObjectAttributes (&ObjectAttributes,
		&PhysicalMemoryUnicodeString,
		OBJ_CASE_INSENSITIVE,
		(HANDLE) NULL,
		(PSECURITY_DESCRIPTOR) NULL);

	*pPhysicalMemoryHandle = NULL;
	*ppPhysSection = NULL;

	ntStatus = ZwOpenSection (pPhysicalMemoryHandle, SECTION_ALL_ACCESS, &ObjectAttributes);

	if (NT_SUCCESS(ntStatus))
	{

		ntStatus = ObReferenceObjectByHandle (*pPhysicalMemoryHandle,
			SECTION_ALL_ACCESS,
			(POBJECT_TYPE) NULL,
			KernelMode,
			ppPhysSection,
			(POBJECT_HANDLE_INFORMATION) NULL);

		if (NT_SUCCESS(ntStatus))
		{
			pStartPhysAddress.QuadPart = (ULONGLONG)(ULONG_PTR)pPhysAddress;

			pEndPhysAddress.QuadPart = pStartPhysAddress.QuadPart + PhysMemSizeInBytes;

			IsIOSpace = 0;

			Result1 = HalTranslateBusAddress (1, 0, pStartPhysAddress, &IsIOSpace, &pStartPhysAddress);

			IsIOSpace = 0;

			Result2 = HalTranslateBusAddress (1, 0, pEndPhysAddress, &IsIOSpace, &pEndPhysAddress);

			if (Result1 && Result2)
			{
				// Let ZwMapViewOfSection pick a linear address

				PhysMemSizeInBytes = (SIZE_T)pEndPhysAddress.QuadPart - (SIZE_T)pStartPhysAddress.QuadPart;

				ViewBase = pStartPhysAddress;

				ntStatus = ZwMapViewOfSection (*pPhysicalMemoryHandle,
					(HANDLE) -1,
					&pbPhysMemLin,
					0L,
					PhysMemSizeInBytes,
					&ViewBase,
					&PhysMemSizeInBytes,
					ViewShare,
					0,
					PAGE_READWRITE | PAGE_NOCACHE);

				// If the physical memory is already mapped with a different caching attribute, try again
				if (ntStatus == STATUS_CONFLICTING_ADDRESSES)
				{
					ntStatus = ZwMapViewOfSection (*pPhysicalMemoryHandle,
						(HANDLE) -1,
						&pbPhysMemLin,
						0L,
						PhysMemSizeInBytes,
						&ViewBase,
						&PhysMemSizeInBytes,
						ViewShare,
						0,
						PAGE_READWRITE);
				}


				if (!NT_SUCCESS(ntStatus))
					KdPrint(("ERROR: ZwMapViewOfSection failed"));
				else
				{
					pbPhysMemLin += pStartPhysAddress.QuadPart - ViewBase.QuadPart;
					*ppPhysMemLin = pbPhysMemLin;
				}  
			}
			else
				KdPrint(("ERROR: HalTranslateBusAddress failed"));
		}
		else
			KdPrint(("ERROR: ObReferenceObjectByHandle failed"));
	}
	else
		KdPrint(("ERROR: ZwOpenSection failed"));

	if (!NT_SUCCESS(ntStatus))
		ZwClose(*pPhysicalMemoryHandle);

	KdPrint(("Leaving MapPhysicalMemoryToLinearSpace"));

	return ntStatus;
}


NTSTATUS UnmapPhysicalMemory(HANDLE PhysicalMemoryHandle, PVOID pPhysMemLin, PVOID pPhysSection)
{
	NTSTATUS ntStatus;

	KdPrint(("Entering UnmapPhysicalMemory"));

	ntStatus = ZwUnmapViewOfSection((HANDLE)-1, pPhysMemLin);

	if (!NT_SUCCESS(ntStatus))
		KdPrint(("ERROR: UnmapViewOfSection failed"));

	if (pPhysSection)
		ObDereferenceObject(pPhysSection);

	ZwClose(PhysicalMemoryHandle);

	KdPrint(("Leaving UnmapPhysicalMemory"));

	return ntStatus;
}
