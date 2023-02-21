/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "efi.h"

#define EFI_TCG_PROTOCOL_GUID \
        GUID_DEF(0xf541796d, 0xa62e, 0x4954, 0xa7, 0x75, 0x95, 0x84, 0xf6, 0x1b, 0x9c, 0xdd)
#define EFI_TCG2_PROTOCOL_GUID \
        GUID_DEF(0x607f766c, 0x7455, 0x42be, 0x93, 0x0b, 0xe4, 0xd7, 0x6d, 0xb2, 0x72, 0x0f)

#define TCG_ALG_SHA 0x4
#define EFI_TCG2_EVENT_HEADER_VERSION 1
#define EV_IPL 13

typedef struct {
        uint8_t Major;
        uint8_t Minor;
        uint8_t RevMajor;
        uint8_t RevMinor;
} TCG_VERSION;

typedef struct {
        uint8_t Major;
        uint8_t Minor;
} EFI_TCG2_VERSION;

typedef struct {
        uint8_t Size;
        TCG_VERSION StructureVersion;
        TCG_VERSION ProtocolSpecVersion;
        uint8_t HashAlgorithmBitmap;
        bool TPMPresentFlag;
        bool TPMDeactivatedFlag;
} EFI_TCG_BOOT_SERVICE_CAPABILITY;

typedef struct {
        uint8_t Size;
        EFI_TCG2_VERSION StructureVersion;
        EFI_TCG2_VERSION ProtocolVersion;
        uint32_t HashAlgorithmBitmap;
        uint32_t SupportedEventLogs;
        bool TPMPresentFlag;
        uint16_t MaxCommandSize;
        uint16_t MaxResponseSize;
        uint32_t ManufacturerID;
        uint32_t NumberOfPCRBanks;
        uint32_t ActivePcrBanks;
} EFI_TCG2_BOOT_SERVICE_CAPABILITY;

typedef struct {
        uint32_t PCRIndex;
        uint32_t EventType;
        struct {
                uint8_t Digest[20];
        } Digest;
        uint32_t EventSize;
        uint8_t Event[0];
} _packed_ TCG_PCR_EVENT;

typedef struct {
        uint32_t HeaderSize;
        uint16_t HeaderVersion;
        uint32_t PCRIndex;
        uint32_t EventType;
} _packed_ EFI_TCG2_EVENT_HEADER;

typedef struct {
        uint32_t Size;
        EFI_TCG2_EVENT_HEADER Header;
        uint8_t Event[];
} _packed_ EFI_TCG2_EVENT;

typedef struct EFI_TCG_PROTOCOL EFI_TCG_PROTOCOL;
struct EFI_TCG_PROTOCOL {
        EFI_STATUS (EFIAPI *StatusCheck)(
                        EFI_TCG_PROTOCOL *This,
                        EFI_TCG_BOOT_SERVICE_CAPABILITY *ProtocolCapability,
                        uint32_t *TCGFeatureFlags,
                        EFI_PHYSICAL_ADDRESS *EventLogLocation,
                        EFI_PHYSICAL_ADDRESS *EventLogLastEntry);
        void *HashAll;
        void *LogEvent;
        void *PassThroughToTpm;
        EFI_STATUS (EFIAPI *HashLogExtendEvent)(
                        EFI_TCG_PROTOCOL *This,
                        EFI_PHYSICAL_ADDRESS HashData,
                        uint64_t HashDataLen,
                        uint32_t AlgorithmId,
                        TCG_PCR_EVENT *TCGLogData,
                        uint32_t *EventNumber,
                        EFI_PHYSICAL_ADDRESS *EventLogLastEntry);
};

typedef struct EFI_TCG2_PROTOCOL EFI_TCG2_PROTOCOL;
struct EFI_TCG2_PROTOCOL {
        EFI_STATUS (EFIAPI *GetCapability)(
                        EFI_TCG2_PROTOCOL *This,
                        EFI_TCG2_BOOT_SERVICE_CAPABILITY *ProtocolCapability);
        void *GetEventLog;
        EFI_STATUS (EFIAPI *HashLogExtendEvent)(
                        EFI_TCG2_PROTOCOL *This,
                        uint64_t Flags,
                        EFI_PHYSICAL_ADDRESS DataToHash,
                        uint64_t DataToHashLen,
                        EFI_TCG2_EVENT *EfiTcgEvent);
        void *SubmitCommand;
        void *GetActivePcrBanks;
        void *SetActivePcrBanks;
        void *GetResultOfSetActivePcrBanks;
};
