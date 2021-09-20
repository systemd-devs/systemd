/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "cpio.h"
#include "measure.h"
#include "util.h"

#define EXTRA_DIR_SUFFIX L".extra.d"

static CHAR8* write_cpio_word(CHAR8 *p, UINT32 v) {
        static const char hex[] = "0123456789abcdef";

        /* Writes a CPIO header 8 character hex value */

        for (UINTN i = 0; i < 8; i++)
                p[7-i] = hex[(v >> (4 * i)) & 0xF];

        return p + 8;
}

static CHAR8* mangle_filename(CHAR8 *p, const CHAR16 *f) {
        CHAR8* w = p;

        /* Basically converts UTF-16 to plain ASCII (note that we filtered non-ASCII filenames beforehand, so
         * this operation is always safe) */

        for (; *f != 0; f++) {
                assert((INT32) *f >= 0 && (INT32) *f <= 0x7f);

                *(w++) = *f;
        }

        *w = 0;
        return w;
}

static CHAR8* pad4(CHAR8 *p, const CHAR8* start) {
        assert(p >= start);

        /* Appends NUL bytes to 'p', until the address is divisable by 4, when taken relative to 'start' */

        while ((p - start) % 4 != 0)
                *(p++) = 0;

        return p;
}

static EFI_STATUS pack_cpio_one(
                const CHAR16 *fname,
                const VOID *contents,
                UINTN contents_size,
                const CHAR8 *target_dir_prefix,
                UINT32 access_mode,
                UINT32 *inode_counter,
                VOID **cpio_buffer,
                UINTN *cpio_buffer_size) {

        UINTN l, target_dir_prefix_size, fname_size, q;
        CHAR8 *a;

        assert(inode_counter);
        assert(cpio_buffer);
        assert(cpio_buffer_size);

        /* Serializes one file in the cpio format understood by the kernel initrd logic.
         *
         * See: https://www.kernel.org/doc/Documentation/early-userspace/buffer-format.txt */

        if (contents_size > UINT32_MAX) /* cpio cannot deal with > 32bit file sizes */
                return EFI_LOAD_ERROR;

        if (*inode_counter == UINT32_MAX) /* more than 2^32-1 inodes? yikes. cpio doesn't support that either */
                return EFI_OUT_OF_RESOURCES;

        l = 6 + 13*8 + 1 + 1; /* Fixed CPIO header size, slash separator, and NUL byte after the file name*/

        target_dir_prefix_size = strlena(target_dir_prefix);
        if (l > UINTN_MAX - target_dir_prefix_size)
                return EFI_OUT_OF_RESOURCES;
        l += target_dir_prefix_size;

        fname_size = StrLen(fname);
        if (l > UINTN_MAX - fname_size)
                return EFI_OUT_OF_RESOURCES;
        l += fname_size; /* append space for file name */

        /* CPIO can't deal with fnames longer than 2^32-1 */
        if (target_dir_prefix_size + fname_size >= UINT32_MAX)
                return EFI_OUT_OF_RESOURCES;

        /* Align the whole header to 4 byte size */
        l = ALIGN_TO(l, 4);
        if (l == UINTN_MAX) /* overflow check */
                return EFI_OUT_OF_RESOURCES;

        /* Align the contents to 4 byte size */
        q = ALIGN_TO(contents_size, 4);
        if (q == UINTN_MAX) /* overflow check */
                return EFI_OUT_OF_RESOURCES;

        if (l > UINTN_MAX - q) /* overflow check */
                return EFI_OUT_OF_RESOURCES;
        l += q; /* Add contents to header */

        if (*cpio_buffer_size > UINTN_MAX - l) /* overflow check */
                return EFI_OUT_OF_RESOURCES;
        a = ReallocatePool(*cpio_buffer, *cpio_buffer_size, *cpio_buffer_size + l);
        if (!a)
                return EFI_OUT_OF_RESOURCES;

        *cpio_buffer = a;
        a = (CHAR8*) *cpio_buffer + *cpio_buffer_size;

        CopyMem(a, "070701", 6); /* magic ID */
        a += 6;

        a = write_cpio_word(a, (*inode_counter)++);                         /* inode */
        a = write_cpio_word(a, access_mode | 0100000 /* = S_IFREG */);      /* mode */
        a = write_cpio_word(a, 0);                                          /* uid */
        a = write_cpio_word(a, 0);                                          /* gid */
        a = write_cpio_word(a, 1);                                          /* nlink */
        a = write_cpio_word(a, 0);                                          /* mtime */
        a = write_cpio_word(a, contents_size);                              /* size */
        a = write_cpio_word(a, 0);                                          /* major(dev) */
        a = write_cpio_word(a, 0);                                          /* minor(dev) */
        a = write_cpio_word(a, 0);                                          /* major(rdev) */
        a = write_cpio_word(a, 0);                                          /* minor(rdev) */
        a = write_cpio_word(a, target_dir_prefix_size + fname_size + 1);    /* fname size */
        a = write_cpio_word(a, 0);                                          /* "crc" */

        CopyMem(a, target_dir_prefix, target_dir_prefix_size);
        a += target_dir_prefix_size;
        *(a++) = '/';
        a = mangle_filename(a, fname);

        /* Pad to next multiple of 4 */
        a = pad4(a, *cpio_buffer);

        CopyMem(a, contents, contents_size);
        a += contents_size;

        /* Pad to next multiple of 4 */
        a = pad4(a, *cpio_buffer);

        assert(a == (CHAR8*) *cpio_buffer + *cpio_buffer_size + l);
        *cpio_buffer_size += l;

        return EFI_SUCCESS;
}

static EFI_STATUS pack_cpio_dir(
                const CHAR8 *path,
                UINT32 access_mode,
                UINT32 *inode_counter,
                VOID **cpio_buffer,
                UINTN *cpio_buffer_size) {

        UINTN l, path_size;
        CHAR8 *a;

        assert(path);
        assert(cpio_buffer);
        assert(cpio_buffer_size);

        /* Serializes one directory inode in cpio format. Note that cpio archives must first create the dirs
         * they want to place files in. */

        if (*inode_counter == UINT32_MAX)
                return EFI_OUT_OF_RESOURCES;

        l = 6 + 13*8 + 1; /* Fixed CPIO header size, and NUL byte after the file name*/

        path_size = strlena(path);
        if (l > UINTN_MAX - path_size)
                return EFI_OUT_OF_RESOURCES;
        l += path_size;

        /* Align the whole header to 4 byte size */
        l = ALIGN_TO(l, 4);
        if (l == UINTN_MAX) /* overflow check */
                return EFI_OUT_OF_RESOURCES;

        if (*cpio_buffer_size > UINTN_MAX - l) /* overflow check */
                return EFI_OUT_OF_RESOURCES;
        a = ReallocatePool(*cpio_buffer, *cpio_buffer_size, *cpio_buffer_size + l);
        if (!a)
                return EFI_OUT_OF_RESOURCES;

        *cpio_buffer = a;
        a = (CHAR8*) *cpio_buffer + *cpio_buffer_size;

        CopyMem(a, "070701", 6); /* magic ID */
        a += 6;

        a = write_cpio_word(a, (*inode_counter)++);                         /* inode */
        a = write_cpio_word(a, access_mode | 0040000 /* = S_IFDIR */);      /* mode */
        a = write_cpio_word(a, 0);                                          /* uid */
        a = write_cpio_word(a, 0);                                          /* gid */
        a = write_cpio_word(a, 1);                                          /* nlink */
        a = write_cpio_word(a, 0);                                          /* mtime */
        a = write_cpio_word(a, 0);                                          /* size */
        a = write_cpio_word(a, 0);                                          /* major(dev) */
        a = write_cpio_word(a, 0);                                          /* minor(dev) */
        a = write_cpio_word(a, 0);                                          /* major(rdev) */
        a = write_cpio_word(a, 0);                                          /* minor(rdev) */
        a = write_cpio_word(a, path_size + 1);                              /* fname size */
        a = write_cpio_word(a, 0);                                          /* "crc" */

        CopyMem(a, path, path_size + 1);
        a += path_size + 1;

        /* Pad to next multiple of 4 */
        a = pad4(a, *cpio_buffer);

        assert(a == (CHAR8*) *cpio_buffer + *cpio_buffer_size + l);

        *cpio_buffer_size += l;
        return EFI_SUCCESS;
}

static EFI_STATUS pack_cpio_prefix(
                const CHAR8 *path,
                UINT32 dir_mode,
                UINT32 *inode_counter,
                VOID **cpio_buffer,
                UINTN *cpio_buffer_size) {

        EFI_STATUS err;
        const CHAR8 *p = path;

        /* Serializes directory inodes of all prefix paths of the specified path in cpio format. Note that
         * (similar to mkdir -p behaviour) all leading paths are created with 0555 access mode, only the
         * final dir is created with the specified directory access mode. */

        for (;;) {
                const CHAR8 *e;

                e = strchra(p, '/');
                if (!e)
                        break;

                if (e > p) {
                        _cleanup_freepool_ CHAR8 *t = NULL;

                        t = strndup8(path, e - path);
                        if (!t)
                                return EFI_OUT_OF_RESOURCES;

                        err = pack_cpio_dir(t, 0555, inode_counter, cpio_buffer, cpio_buffer_size);
                        if (EFI_ERROR(err))
                                return err;
                }

                p = e + 1;
        }

        return pack_cpio_dir(path, dir_mode, inode_counter, cpio_buffer, cpio_buffer_size);
}


static EFI_STATUS pack_cpio_trailer(
                VOID **cpio_buffer,
                UINTN *cpio_buffer_size) {

        static const char trailer[] =
                "070701"
                "00000000"
                "00000000"
                "00000000"
                "00000000"
                "00000001"
                "00000000"
                "00000000"
                "00000000"
                "00000000"
                "00000000"
                "00000000"
                "0000000B"
                "00000000"
                "TRAILER!!!\0\0\0"; /* There's a fourth NUL byte appended here, because this is a string */

        VOID *a;

        /* Generates the cpio trailer record that indicates the end of our initrd cpio archive */

        assert(cpio_buffer);
        assert(cpio_buffer_size);
        assert_cc(sizeof(trailer) % 4 == 0);

        a = ReallocatePool(*cpio_buffer, *cpio_buffer_size, *cpio_buffer_size + sizeof(trailer));
        if (!a)
                return EFI_OUT_OF_RESOURCES;

        *cpio_buffer = a;
        CopyMem((UINT8*) *cpio_buffer + *cpio_buffer_size, trailer, sizeof(trailer));
        *cpio_buffer_size += sizeof(trailer);

        return EFI_SUCCESS;
}

EFI_STATUS pack_cpio(
                EFI_LOADED_IMAGE *loaded_image,
                const CHAR16 *match_suffix,
                const CHAR8 *target_dir_prefix,
                UINT32 dir_mode,
                UINT32 access_mode,
                UINTN tpm_pcr,
                const CHAR16 *tpm_description,
                VOID **ret_buffer,
                UINTN *ret_buffer_size) {

        _cleanup_(FileHandleClosep) EFI_FILE_HANDLE root = NULL, extra_dir = NULL;
        UINTN dirent_size = 0, buffer_size = 0, l, a,  n_items = 0, n_allocated = 0;
        _cleanup_freepool_ EFI_FILE_INFO *file_info = NULL, *dirent = NULL;
        _cleanup_freepool_ CHAR16 *loaded_image_path = NULL, *j = NULL;
        _cleanup_(strv_freep) CHAR16 **items = NULL;
        _cleanup_freepool_ VOID *buffer = NULL;
        UINT32 inode = 1; /* inode counter, so that each item gets a new inode */
        EFI_STATUS err;

        assert(loaded_image);
        assert(ret_buffer);
        assert(ret_buffer_size);

        root = LibOpenRoot(loaded_image->DeviceHandle);
        if (!root)
                return log_error_status_stall(EFI_LOAD_ERROR, L"Unable to open root directory.");

        loaded_image_path = DevicePathToStr(loaded_image->FilePath);
        if (!loaded_image_path)
                return log_oom();

        l = StrLen(loaded_image_path);
        a = StrLen(EXTRA_DIR_SUFFIX);
        if (l >= UINTN_MAX - a)
                return log_error_status_stall(EFI_OUT_OF_RESOURCES, L"Path length overflow.");

        j = AllocatePool(sizeof(CHAR16) * (l + a + 1));
        if (!j)
                return log_oom();

        StpCpy(StpCpy(j, loaded_image_path), EXTRA_DIR_SUFFIX);

        err = uefi_call_wrapper(root->Open, 5, root, &extra_dir, j, EFI_FILE_MODE_READ, 0ULL);
        if (err == EFI_NOT_FOUND) {
                /* No extra subdir, that's totally OK */
                *ret_buffer = NULL;
                *ret_buffer_size = 0;
                return EFI_SUCCESS;
        }
        if (EFI_ERROR(err))
                return log_error_status_stall(err, L"Failed to open extra directory of loaded image: %r", err);

        /* We opened the extra directory now, let's verify it actually *is* a directory */

        err = get_file_info_harder(extra_dir, &file_info);
        if (EFI_ERROR(err))
                return log_error_status_stall(err, L"Failed to get information about extra directory of loaded image: %r", err);
        if (!(file_info->Attribute & EFI_FILE_DIRECTORY))
                return log_error_status_stall(EFI_NOT_FOUND, L"Extra initrd directory is not actually a directory, refusing.");

        for (;;) {
                _cleanup_freepool_ CHAR16 *d = NULL;

                err = readdir_harder(extra_dir, &dirent, &dirent_size);
                if (EFI_ERROR(err))
                        return log_error_status_stall(err, L"Failed to read extra directory of loaded image: %r", err);
                if (!dirent) /* End of directory */
                        break;

                if (dirent->FileName[0] == '.')
                        continue;
                if (dirent->Attribute & EFI_FILE_DIRECTORY)
                        continue;
                if (match_suffix && !endswith_no_case(dirent->FileName, match_suffix))
                        continue;
                if (!is_ascii(dirent->FileName))
                        continue;
                if (StrLen(dirent->FileName) > 255) /* Max filename size on Linux */
                        continue;

                d = StrDuplicate(dirent->FileName);
                if (!d)
                        return log_oom();

                if (n_items >= n_allocated) {
                        UINTN m;

                        if (n_items > (UINTN_MAX / sizeof(UINT16)) - 16) /* Overflow check, just in case */
                                return log_oom();

                        m = n_items + 16;
                        items = ReallocatePool(&items, n_allocated * sizeof(UINT16*), m * sizeof(UINT16*));
                        if (!items)
                                return log_oom();

                        n_allocated = m;
                }

                items[n_items++] = TAKE_PTR(d);
                items[n_items] = NULL; /* Let's always NUL terminate, to make freeing via strv_free() easy */
        }

        if (n_items == 0) {
                /* Empty directory */
                *ret_buffer = NULL;
                *ret_buffer_size = 0;
                return EFI_SUCCESS;
        }

        /* Now, sort the files we found, to make this uniform and stable (and to ensure the TPM measurements
         * are not dependent on read order) */
        sort_pointer_array((VOID**) items, n_items, (compare_pointer_func_t) StrCmp);

        /* Generate the leading directory inodes right before adding the first files, to the
         * archive. Otherwise the cpio archive cannot be unpacked, since the leading dirs won't exist. */
        err = pack_cpio_prefix(target_dir_prefix, dir_mode, &inode, &buffer, &buffer_size);
        if (EFI_ERROR(err))
                return log_error_status_stall(err, L"Failed to pack cpio prefix: %r", err);

        for (UINTN i = 0; i < n_items; i++) {
                _cleanup_freepool_ CHAR8 *content = NULL;
                UINTN contentsize;

                err = file_read(extra_dir, items[i], 0, 0, &content, &contentsize);
                if (EFI_ERROR(err)) {
                        log_error_status_stall(err, L"Failed to read %s, ignoring: %r", items[i], err);
                        continue;
                }

                err = pack_cpio_one(
                                items[i],
                                content, contentsize,
                                target_dir_prefix,
                                access_mode,
                                &inode,
                                &buffer, &buffer_size);
                if (EFI_ERROR(err))
                        return log_error_status_stall(err, L"Failed to pack cpio file %s: %r", dirent->FileName, err);
        }

        err = pack_cpio_trailer(&buffer, &buffer_size);
        if (EFI_ERROR(err))
                return log_error_status_stall(err, L"Failed to pack cpio trailer: %r");

#if ENABLE_TPM
        err = tpm_log_event(
                        tpm_pcr,
                        (EFI_PHYSICAL_ADDRESS) (UINTN) buffer,
                        buffer_size,
                        tpm_description);
        if (EFI_ERROR(err))
                log_error_stall(L"Unable to add initrd TPM measurement, ignoring: %r", err);
#endif

        *ret_buffer = TAKE_PTR(buffer);
        *ret_buffer_size = buffer_size;

        return EFI_SUCCESS;
}
