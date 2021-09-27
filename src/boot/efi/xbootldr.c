/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <efi.h>
#include <efigpt.h>
#include <efilib.h>

#include "xbootldr.h"
#include "util.h"

union GptHeaderBuffer {
        EFI_PARTITION_TABLE_HEADER gpt_header;
        uint8_t space[((sizeof(EFI_PARTITION_TABLE_HEADER) + 511) / 512) * 512];
};

static EFI_DEVICE_PATH *path_parent(EFI_DEVICE_PATH *path, EFI_DEVICE_PATH *node) {
        EFI_DEVICE_PATH *parent;
        UINTN len;

        assert(path);
        assert(node);

        len = (UINT8*) NextDevicePathNode(node) - (UINT8*) path;
        parent = (EFI_DEVICE_PATH*) AllocatePool(len + sizeof(EFI_DEVICE_PATH));
        if (!parent)
                return NULL;

        CopyMem(parent, path, len);
        CopyMem((UINT8*) parent + len, EndDevicePath, sizeof(EFI_DEVICE_PATH));

        return parent;
}

static BOOLEAN verify_gpt(union GptHeaderBuffer *gpt_header_buffer, EFI_LBA lba_expected) {
        EFI_PARTITION_TABLE_HEADER *h;
        UINT32 crc32, crc32_saved;
        EFI_STATUS err;

        assert(gpt_header_buffer);

        h = &gpt_header_buffer->gpt_header;

        /* Some superficial validation of the GPT header */
        if(CompareMem(&h->Header.Signature, "EFI PART", sizeof(h->Header.Signature) != 0))
                return FALSE;

        if (h->Header.HeaderSize < 92 || h->Header.HeaderSize > 512)
                return FALSE;

        if (h->Header.Revision != 0x00010000U)
                return FALSE;

        /* Calculate CRC check */
        crc32_saved = h->Header.CRC32;
        h->Header.CRC32 = 0;
        err = BS->CalculateCrc32(gpt_header_buffer, h->Header.HeaderSize, &crc32);
        h->Header.CRC32 = crc32_saved;
        if (EFI_ERROR(err) || crc32 != crc32_saved)
                return FALSE;

        if (h->MyLBA != lba_expected)
                return FALSE;

        if (h->SizeOfPartitionEntry < sizeof(EFI_PARTITION_ENTRY))
                return FALSE;

        if (h->NumberOfPartitionEntries <= 0 || h->NumberOfPartitionEntries > 1024)
                return FALSE;

        /* overflow check */
        if (h->SizeOfPartitionEntry > UINTN_MAX / h->NumberOfPartitionEntries)
                return FALSE;

        return TRUE;
}

static EFI_STATUS try_gpt(
                EFI_BLOCK_IO *block_io,
                EFI_LBA lba,
                UINT32 *ret_part_number,
                UINT64 *ret_part_start,
                UINT64 *ret_part_size,
                EFI_GUID *ret_part_uuid) {

        _cleanup_freepool_ EFI_PARTITION_ENTRY *entries = NULL;
        union GptHeaderBuffer gpt;
        EFI_STATUS err;
        UINT32 crc32;
        UINTN size;

        assert(block_io);
        assert(ret_part_number);
        assert(ret_part_start);
        assert(ret_part_size);
        assert(ret_part_uuid);

        /* Read the GPT header */
        err = uefi_call_wrapper(
                        block_io->ReadBlocks, 5,
                        block_io,
                        block_io->Media->MediaId,
                        lba,
                        sizeof(gpt), &gpt);
        if (EFI_ERROR(err))
                return err;

        if (!verify_gpt(&gpt, lba))
                return EFI_DEVICE_ERROR;

        /* Now load the GPT entry table */
        size = ALIGN_TO((UINTN) gpt.gpt_header.SizeOfPartitionEntry * (UINTN) gpt.gpt_header.NumberOfPartitionEntries, 512);
        entries = AllocatePool(size);
        if (!entries)
                return EFI_OUT_OF_RESOURCES;

        err = uefi_call_wrapper(
                        block_io->ReadBlocks, 5,
                        block_io,
                        block_io->Media->MediaId,
                        gpt.gpt_header.PartitionEntryLBA,
                        size, entries);
        if (EFI_ERROR(err))
                return err;

        /* Calculate CRC of entries array, too */
        err = BS->CalculateCrc32(entries, size, &crc32);
        if (EFI_ERROR(err) || crc32 != gpt.gpt_header.PartitionEntryArrayCRC32)
                return err;

        /* Now we can finally look for xbootloader partitions. */
        for (UINTN i = 0; i < gpt.gpt_header.NumberOfPartitionEntries; i++) {
                EFI_PARTITION_ENTRY *entry;
                EFI_LBA start, end;

                entry = (EFI_PARTITION_ENTRY*) ((UINT8*) entries + gpt.gpt_header.SizeOfPartitionEntry * i);

                if (CompareMem(&entry->PartitionTypeGUID, XBOOTLDR_GUID, sizeof(entry->PartitionTypeGUID)) != 0)
                        continue;

                /* Let's use memcpy(), in case the structs are not aligned (they really should be though) */
                CopyMem(&start, &entry->StartingLBA, sizeof(start));
                CopyMem(&end, &entry->EndingLBA, sizeof(end));

                if (end < start) /* Bogus? */
                        continue;

                *ret_part_number = i + 1;
                *ret_part_start = start;
                *ret_part_size = end - start + 1;
                CopyMem(ret_part_uuid, &entry->UniquePartitionGUID, sizeof(*ret_part_uuid));

                return EFI_SUCCESS;
        }

        /* This GPT was fully valid, but we didn't find what we are looking for. This
         * means there's no reason to check the second copy of the GPT header */
        return EFI_NOT_FOUND;
}

static EFI_DEVICE_PATH *find_device(
                EFI_HANDLE *device,
                UINT32 *ret_part_number,
                UINT64 *ret_part_start,
                UINT64 *ret_part_size,
                EFI_GUID *ret_part_uuid) {

        EFI_DEVICE_PATH *partition_path;
        EFI_STATUS err;

        assert(device);
        assert(ret_part_number);
        assert(ret_part_start);
        assert(ret_part_size);
        assert(ret_part_uuid);

        partition_path = DevicePathFromHandle(device);
        if (!partition_path)
                return NULL;

        for (EFI_DEVICE_PATH *node = partition_path; !IsDevicePathEnd(node); node = NextDevicePathNode(node)) {
                _cleanup_freepool_ EFI_DEVICE_PATH *disk_path = NULL;
                EFI_HANDLE disk_handle;
                EFI_BLOCK_IO *block_io;
                EFI_DEVICE_PATH *p;

                /* First, Let's look for the SCSI/SATA/USB/… device path node, i.e. one above the media
                 * devices */
                if (DevicePathType(node) != MESSAGING_DEVICE_PATH)
                        continue;

                /* Determine the device path one level up */
                disk_path = p = path_parent(partition_path, node);
                if (!disk_path)
                        continue;

                err = uefi_call_wrapper(BS->LocateDevicePath, 3, &BlockIoProtocol, &p, &disk_handle);
                if (EFI_ERROR(err))
                        continue;

                err = uefi_call_wrapper(BS->HandleProtocol, 3, disk_handle, &BlockIoProtocol, (VOID **)&block_io);
                if (EFI_ERROR(err))
                        continue;

                /* Filter out some block devices early. (We only care about block devices that aren't
                 * partitions themselves — we look for GPT partition tables to parse after all —, and only
                 * those which contain a medium and have at least 2 blocks.) */
                if (block_io->Media->LogicalPartition ||
                    !block_io->Media->MediaPresent ||
                    block_io->Media->LastBlock <= 1)
                        continue;

                /* Try both copies of the GPT header, in case one is corrupted */
                for (UINTN nr = 0; nr < 2; nr++) {
                        /* Read the first copy at LBA 1 and then try backup GPT header at the very last
                         * LBA of this block device if it was corrupted. */
                        EFI_LBA lbas[] = { 1, block_io->Media->LastBlock };

                        err = try_gpt(
                                block_io, lbas[nr],
                                ret_part_number,
                                ret_part_start,
                                ret_part_size,
                                ret_part_uuid);
                        if (!EFI_ERROR(err))
                                return DuplicateDevicePath(partition_path);

                        /* GPT was valid but no XBOOT loader partition found. */
                        if (err == EFI_NOT_FOUND)
                                break;
                }
        }

        /* No xbootloader partition found */
        return NULL;
}

VOID xbootldr_open(EFI_HANDLE *device, EFI_HANDLE *ret_device, EFI_FILE **ret_root_dir) {
        _cleanup_freepool_ EFI_DEVICE_PATH *partition_path = NULL;
        UINT32 part_number;
        UINT64 part_start, part_size;
        EFI_GUID part_uuid;
        EFI_STATUS err;

        assert(device);
        assert(ret_device);
        assert(ret_root_dir);

        partition_path = find_device(device, &part_number, &part_start, &part_size, &part_uuid);
        if (!partition_path)
                return;

        /* Patch in the data we found */
        for (EFI_DEVICE_PATH *node = partition_path; !IsDevicePathEnd(node); node = NextDevicePathNode(node)) {
                HARDDRIVE_DEVICE_PATH *hd;

                if (DevicePathType(node) != MEDIA_DEVICE_PATH)
                        continue;

                if (DevicePathSubType(node) != MEDIA_HARDDRIVE_DP)
                        continue;

                hd = (HARDDRIVE_DEVICE_PATH*) node;
                hd->PartitionNumber = part_number;
                hd->PartitionStart = part_start;
                hd->PartitionSize = part_size;
                CopyMem(hd->Signature, &part_uuid, sizeof(hd->Signature));
                hd->MBRType = MBR_TYPE_EFI_PARTITION_TABLE_HEADER;
                hd->SignatureType = SIGNATURE_TYPE_GUID;
        }

        err = uefi_call_wrapper(BS->LocateDevicePath, 3, &BlockIoProtocol, &partition_path, ret_device);
        if (EFI_ERROR(err))
                return;

        *ret_root_dir = LibOpenRoot(*ret_device);
}
