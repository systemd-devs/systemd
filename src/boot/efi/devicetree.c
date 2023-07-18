/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "devicetree.h"
#include "proto/dt-fixup.h"
#include "util.h"

#define FDT_V1_SIZE (7*4)
#define be32toh(x) __builtin_bswap32(x)

static EFI_STATUS devicetree_allocate(struct devicetree_state *state, size_t size) {
        size_t pages = DIV_ROUND_UP(size, EFI_PAGE_SIZE);
        EFI_STATUS err;

        assert(state);

        err = BS->AllocatePages(AllocateAnyPages, EfiACPIReclaimMemory, pages, &state->addr);
        if (err != EFI_SUCCESS)
                return err;

        state->pages = pages;
        return err;
}

static size_t devicetree_allocated(const struct devicetree_state *state) {
        assert(state);
        return state->pages * EFI_PAGE_SIZE;
}

static EFI_STATUS devicetree_fixup(struct devicetree_state *state, size_t len) {
        EFI_DT_FIXUP_PROTOCOL *fixup;
        size_t size;
        EFI_STATUS err;

        assert(state);

        err = BS->LocateProtocol(MAKE_GUID_PTR(EFI_DT_FIXUP_PROTOCOL), NULL, (void **) &fixup);
        if (err != EFI_SUCCESS)
                return log_error_status(EFI_SUCCESS, "Could not locate device tree fixup protocol, skipping.");

        size = devicetree_allocated(state);
        err = fixup->Fixup(fixup, PHYSICAL_ADDRESS_TO_POINTER(state->addr), &size,
                           EFI_DT_APPLY_FIXUPS | EFI_DT_RESERVE_MEMORY);
        if (err == EFI_BUFFER_TOO_SMALL) {
                EFI_PHYSICAL_ADDRESS oldaddr = state->addr;
                size_t oldpages = state->pages;
                void *oldptr = PHYSICAL_ADDRESS_TO_POINTER(state->addr);

                err = devicetree_allocate(state, size);
                if (err != EFI_SUCCESS)
                        return err;

                memcpy(PHYSICAL_ADDRESS_TO_POINTER(state->addr), oldptr, len);
                err = BS->FreePages(oldaddr, oldpages);
                if (err != EFI_SUCCESS)
                        return err;

                size = devicetree_allocated(state);
                err = fixup->Fixup(fixup, PHYSICAL_ADDRESS_TO_POINTER(state->addr), &size,
                                   EFI_DT_APPLY_FIXUPS | EFI_DT_RESERVE_MEMORY);
        }

        return err;
}

EFI_STATUS devicetree_install(struct devicetree_state *state, EFI_FILE *root_dir, char16_t *name) {
        _cleanup_(file_closep) EFI_FILE *handle = NULL;
        _cleanup_free_ EFI_FILE_INFO *info = NULL;
        size_t len;
        EFI_STATUS err;

        assert(state);
        assert(root_dir);
        assert(name);

        state->orig = find_configuration_table(MAKE_GUID_PTR(EFI_DTB_TABLE));
        if (!state->orig)
                return EFI_UNSUPPORTED;

        err = root_dir->Open(root_dir, &handle, name, EFI_FILE_MODE_READ, EFI_FILE_READ_ONLY);
        if (err != EFI_SUCCESS)
                return err;

        err = get_file_info(handle, &info, NULL);
        if (err != EFI_SUCCESS)
                return err;
        if (info->FileSize < FDT_V1_SIZE || info->FileSize > 32 * 1024 * 1024)
                /* 32MB device tree blob doesn't seem right */
                return EFI_INVALID_PARAMETER;

        len = info->FileSize;

        err = devicetree_allocate(state, len);
        if (err != EFI_SUCCESS)
                return err;

        err = handle->Read(handle, &len, PHYSICAL_ADDRESS_TO_POINTER(state->addr));
        if (err != EFI_SUCCESS)
                return err;

        err = devicetree_fixup(state, len);
        if (err != EFI_SUCCESS)
                return err;

        return BS->InstallConfigurationTable(
                        MAKE_GUID_PTR(EFI_DTB_TABLE), PHYSICAL_ADDRESS_TO_POINTER(state->addr));
}

static char* devicetree_get_compatible(const void *dtb) {
        assert(dtb);
        const struct fdt_header *dt_header = dtb;
        if (!IS_ALIGNED64(dt_header) || be32toh(dt_header->Magic) != UINT32_C(0xd00dfeed))
                return NULL;

        uint32_t dt_size = be32toh(dt_header->TotalSize);
        uint32_t struct_off = be32toh(dt_header->OffDTStruct);
        uint32_t struct_size = be32toh(dt_header->SizeDTStruct);
        uint32_t strings_off = be32toh(dt_header->OffDTStrings);
        uint32_t strings_size = be32toh(dt_header->SizeDTStrings);

        if (struct_off % sizeof(uint32_t) || struct_size % sizeof(uint32_t) ||
            strings_off + strings_size > dt_size || struct_off + struct_size > strings_off)
                return NULL;

        uint32_t *cursor = (uint32_t *) ((uint8_t *) dt_header + struct_off);
        char *strings_block = (char *) ((uint8_t *) dt_header + strings_off);

        for (uint32_t i = 0; i < struct_size - 2; i++) {
                switch (be32toh(cursor[i])) {
                case FDT_BEGIN_NODE:
                        if (cursor[++i] != 0)
                                return NULL;
                        break;
                case FDT_NOP:
                        break;
                case FDT_PROP:
                        uint32_t len = be32toh(cursor[++i]);
                        uint32_t name_off = be32toh(cursor[++i]);

                        if (name_off + strlen8("compatible") < strings_size) {
                                if (streq8(strings_block + name_off, "compatible")) {
                                        char *c = (char *) &cursor[++i];

                                        if (len == 0 || i + len > struct_size ||
                                            c[len - 1] != '\0')
                                                c = NULL;

                                        return c;
                                }
                        }

                        i += DIV_ROUND_UP(len, 4);
                        break;
                default:
                        return NULL;

                }
        }

        return NULL;
}

EFI_STATUS devicetree_match(const void *dtb_buffer, size_t dtb_length) {
        assert(dtb_buffer);

        const char *cursor = dtb_buffer;
        size_t len = dtb_length;

        const void *fw_dtb = find_configuration_table(MAKE_GUID_PTR(EFI_DTB_TABLE));
        if (!fw_dtb)
                return EFI_UNSUPPORTED;

        const char *fw_compat = devicetree_get_compatible(fw_dtb);

        if (len >= sizeof(struct fdt_header)) {
                struct fdt_header *dt_header = (struct fdt_header *)cursor;
                size_t size = be32toh(dt_header->TotalSize);

                if (size > len)
                        return EFI_INVALID_PARAMETER;

                const char *compat = devicetree_get_compatible(cursor);
                if (!compat)
                        return EFI_INVALID_PARAMETER;

                /* Only matches the first compatible string from each DT */
                if (streq8(compat, fw_compat))
                        return EFI_SUCCESS;
        }

        return EFI_NOT_FOUND;
}

EFI_STATUS devicetree_install_from_memory(
                struct devicetree_state *state, const void *dtb_buffer, size_t dtb_length) {

        EFI_STATUS err;

        assert(state);
        assert(dtb_buffer && dtb_length > 0);

        state->orig = find_configuration_table(MAKE_GUID_PTR(EFI_DTB_TABLE));
        if (!state->orig)
                return EFI_UNSUPPORTED;

        err = devicetree_allocate(state, dtb_length);
        if (err != EFI_SUCCESS)
                return err;

        memcpy(PHYSICAL_ADDRESS_TO_POINTER(state->addr), dtb_buffer, dtb_length);

        err = devicetree_fixup(state, dtb_length);
        if (err != EFI_SUCCESS)
                return err;

        return BS->InstallConfigurationTable(
                        MAKE_GUID_PTR(EFI_DTB_TABLE), PHYSICAL_ADDRESS_TO_POINTER(state->addr));
}

void devicetree_cleanup(struct devicetree_state *state) {
        EFI_STATUS err;

        if (!state->pages)
                return;

        err = BS->InstallConfigurationTable(MAKE_GUID_PTR(EFI_DTB_TABLE), state->orig);
        /* don't free the current device tree if we can't reinstate the old one */
        if (err != EFI_SUCCESS)
                return;

        BS->FreePages(state->addr, state->pages);
        state->pages = 0;
}
