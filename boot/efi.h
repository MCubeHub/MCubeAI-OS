#pragma once
/* Minimal, self-contained UEFI 2.x definitions for the MCubeAIOS loader.
 * Only the pieces the loader touches are declared, but the layout of
 * EFI_BOOT_SERVICES / EFI_SYSTEM_TABLE follows the specification exactly. */

#include <stdint.h>
#include <stddef.h>

#define EFIAPI __attribute__((ms_abi))
#define PACKED __attribute__((packed))

typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef int64_t   INT64;
typedef uint64_t  UINTN;
typedef uint64_t  EFI_STATUS;
typedef void     *EFI_HANDLE;
typedef void     *EFI_EVENT;
typedef uint64_t  EFI_PHYSICAL_ADDRESS;
typedef uint64_t  EFI_VIRTUAL_ADDRESS;
typedef uint64_t  EFI_TPL;
typedef uint16_t  CHAR16;
typedef void      VOID;
typedef uint8_t   BOOLEAN;

#define EFIERR(a) (0x8000000000000000ULL | (a))
#define EFI_ERROR(s) (((INT64)(s)) < 0)
#define EFI_SUCCESS 0ULL
#define EFI_INVALID_PARAMETER EFIERR(2)
#define EFI_BUFFER_TOO_SMALL  EFIERR(5)
#define EFI_NOT_FOUND         EFIERR(14)

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

typedef struct {
    UINT32 Type;
    UINT32 Pad;
    UINT64 PhysicalStart;
    UINT64 VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef enum {
    EfiReservedMemoryType = 0,
    EfiLoaderCode = 1,
    EfiLoaderData = 2,
    EfiBootServicesCode = 3,
    EfiBootServicesData = 4,
    EfiRuntimeServicesCode = 5,
    EfiRuntimeServicesData = 6,
    EfiConventionalMemory = 7,
    EfiUnusableMemory = 8,
    EfiACPIReclaimMemory = 9,
    EfiACPIMemoryNVS = 10,
    EfiMemoryMappedIO = 11,
    EfiMemoryMappedIOPortSpace = 12,
    EfiPalCode = 13,
    EfiPersistentMemory = 14,
    EfiUnacceptedMemoryType = 15,
    EfiMaxMemoryType = 15
} EFI_MEMORY_TYPE;

typedef enum {
    AllocateAnyPages = 0,
    AllocateMaxAddress = 1,
    AllocateAddress = 2,
    MaxAllocateType = 3
} EFI_ALLOCATE_TYPE;

typedef enum {
    EfiResetCold = 0,
    EfiResetWarm = 1,
    EfiResetShutdown = 2,
} EFI_RESET_TYPE;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);
    EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, CHAR16 *);
    EFI_STATUS (EFIAPI *TestString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, CHAR16 *);
    EFI_STATUS (EFIAPI *QueryMode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN, UINTN *, UINTN *);
    EFI_STATUS (EFIAPI *SetMode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN);
    EFI_STATUS (EFIAPI *SetAttribute)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN);
    EFI_STATUS (EFIAPI *ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *);
    EFI_STATUS (EFIAPI *SetCursorPosition)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN, UINTN);
    EFI_STATUS (EFIAPI *EnableCursor)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);
    void *Mode;
};

typedef struct {
    UINT32 MaxMode;
    UINT32 Mode;
    UINT32 Attribute;
    UINT32 CursorColumn;
    UINT32 CursorRow;
    BOOLEAN CursorVisible;
} SIMPLE_TEXT_OUTPUT_MODE;

typedef struct EFI_BOOT_SERVICES EFI_BOOT_SERVICES;
typedef EFI_STATUS (EFIAPI *EFI_EVENT_NOTIFY)(EFI_EVENT, void *);

struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;

    EFI_TPL    (EFIAPI *RaiseTPL)(EFI_TPL);
    void       (EFIAPI *RestoreTPL)(EFI_TPL);
    EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE, UINTN, EFI_PHYSICAL_ADDRESS *);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS, UINTN);
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *, EFI_MEMORY_DESCRIPTOR *, UINTN *, UINTN *, UINT32 *);
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE, UINTN, void **);
    EFI_STATUS (EFIAPI *FreePool)(void *);
    EFI_STATUS (EFIAPI *CreateEvent)(UINT32, EFI_TPL, EFI_EVENT_NOTIFY, void *, EFI_EVENT *);
    EFI_STATUS (EFIAPI *SetTimer)(EFI_EVENT, UINT32, UINT64);
    EFI_STATUS (EFIAPI *WaitForEvent)(UINTN, EFI_EVENT *, UINTN *);
    EFI_STATUS (EFIAPI *SignalEvent)(EFI_EVENT);
    EFI_STATUS (EFIAPI *CloseEvent)(EFI_EVENT);
    EFI_STATUS (EFIAPI *CheckEvent)(EFI_EVENT);
    EFI_STATUS (EFIAPI *InstallProtocolInterface)(EFI_HANDLE *, EFI_GUID *, UINTN, void *);
    EFI_STATUS (EFIAPI *ReinstallProtocolInterface)(EFI_HANDLE, EFI_GUID *, void *, void *);
    EFI_STATUS (EFIAPI *UninstallProtocolInterface)(EFI_HANDLE, EFI_GUID *, void *);
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, EFI_GUID *, void **);
    void       *Reserved;
    EFI_STATUS (EFIAPI *RegisterProtocolNotify)(EFI_GUID *, EFI_EVENT, void **);
    EFI_STATUS (EFIAPI *LocateHandle)(UINTN, EFI_GUID *, void *, UINTN *, EFI_HANDLE *);
    EFI_STATUS (EFIAPI *LocateDevicePath)(EFI_GUID *, EFI_HANDLE *, EFI_HANDLE *);
    EFI_STATUS (EFIAPI *InstallConfigurationTable)(EFI_GUID *, void *);
    EFI_STATUS (EFIAPI *LoadImage)(BOOLEAN, EFI_HANDLE, void *, void *, UINTN, EFI_HANDLE *);
    EFI_STATUS (EFIAPI *StartImage)(EFI_HANDLE, UINTN *, CHAR16 **);
    EFI_STATUS (EFIAPI *Exit)(EFI_HANDLE, EFI_STATUS, UINTN, CHAR16 *);
    EFI_STATUS (EFIAPI *UnloadImage)(EFI_HANDLE);
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE, UINTN);
    EFI_STATUS (EFIAPI *GetNextMonotonicCount)(UINT64 *);
    EFI_STATUS (EFIAPI *Stall)(UINTN);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN, UINT64, UINTN, CHAR16 *);
    EFI_STATUS (EFIAPI *ConnectController)(EFI_HANDLE, EFI_HANDLE *, void *, BOOLEAN);
    EFI_STATUS (EFIAPI *DisconnectController)(EFI_HANDLE, EFI_HANDLE, EFI_HANDLE);
    EFI_STATUS (EFIAPI *OpenProtocol)(EFI_HANDLE, EFI_GUID *, void **, EFI_HANDLE, EFI_HANDLE, UINT32);
    EFI_STATUS (EFIAPI *CloseProtocol)(EFI_HANDLE, EFI_GUID *, EFI_HANDLE, EFI_HANDLE);
    EFI_STATUS (EFIAPI *OpenProtocolInformation)(EFI_HANDLE, EFI_GUID *, void ***, UINTN *);
    EFI_STATUS (EFIAPI *ProtocolsPerHandle)(EFI_HANDLE, EFI_GUID ***, UINTN *);
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(UINTN, EFI_GUID *, void *, UINTN *, EFI_HANDLE **);
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *, void *, void **);
    EFI_STATUS (EFIAPI *InstallMultipleProtocolInterfaces)(EFI_HANDLE *, ...);
    EFI_STATUS (EFIAPI *UninstallMultipleProtocolInterfaces)(EFI_HANDLE, ...);
    EFI_STATUS (EFIAPI *CalculateCrc32)(void *, UINTN, UINT32 *);
    void       (EFIAPI *CopyMem)(void *, void *, UINTN);
    void       (EFIAPI *SetMem)(void *, UINTN, UINT8);
    EFI_STATUS (EFIAPI *CreateEventEx)(UINT32, EFI_TPL, EFI_EVENT_NOTIFY, const void *, EFI_EVENT *);
};

typedef struct {
    EFI_GUID VendorGuid;
    void    *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32  FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void    *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    void    *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

typedef struct {
    UINT32 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(void *, void **);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *, EFI_FILE_PROTOCOL **, CHAR16 *, UINT64, UINT64);
    EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *);
    EFI_STATUS (EFIAPI *Delete)(EFI_FILE_PROTOCOL *);
    EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *, UINTN *, void *);
    EFI_STATUS (EFIAPI *Write)(EFI_FILE_PROTOCOL *, UINTN *, void *);
    EFI_STATUS (EFIAPI *GetPosition)(EFI_FILE_PROTOCOL *, UINT64 *);
    EFI_STATUS (EFIAPI *SetPosition)(EFI_FILE_PROTOCOL *, UINT64);
    EFI_STATUS (EFIAPI *GetInfo)(EFI_FILE_PROTOCOL *, EFI_GUID *, UINTN *, void *);
    EFI_STATUS (EFIAPI *SetInfo)(EFI_FILE_PROTOCOL *, EFI_GUID *, UINTN, void *);
    EFI_STATUS (EFIAPI *Flush)(EFI_FILE_PROTOCOL *);
};

typedef struct {
    UINT32 Red;
    UINT32 Green;
    UINT32 Blue;
    UINT32 Reserved;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    UINT32 PixelFormat; /* 0 RGBA, 1 BGRA, 2 BitMask, 3 BltOnly */
    EFI_PIXEL_BITMASK PixelInformation;
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    EFI_STATUS (EFIAPI *QueryMode)(void *, UINT32, UINTN *, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
    EFI_STATUS (EFIAPI *SetMode)(void *, UINT32);
    EFI_STATUS (EFIAPI *Blt)(void *, void *, UINT32, UINTN, UINTN, UINTN, UINTN, UINTN, UINTN, UINT32);
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath;
    void *Reserved;
    UINT32 LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    UINT64 ImageSize;
    UINT32 ImageCodeType;
    UINT32 ImageDataType;
    EFI_STATUS (EFIAPI *Unload)(EFI_HANDLE);
} EFI_LOADED_IMAGE_PROTOCOL;

/* GUIDs used by the loader. */
extern EFI_GUID gEfiLoadedImageProtocolGuid;
extern EFI_GUID gEfiSimpleFileSystemProtocolGuid;
extern EFI_GUID gEfiGraphicsOutputProtocolGuid;
extern EFI_GUID gEfiAcpi20TableGuid;

#define EFI_FILE_MODE_READ  0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE 0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL

/* ACPI RSDP (only the fields the kernel currently reads). */
typedef struct {
    char Signature[8];
    UINT8 Checksum;
    char OemId[6];
    UINT8 Revision;
    UINT32 RsdtAddress;
    UINT32 Length;
    UINT64 XsdtAddress;
    UINT8 ExtendedChecksum;
    UINT8 Reserved[3];
} PACKED ACPI_RSDP;
