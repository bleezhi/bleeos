/* Minimal UEFI (x86_64) definitions for the BleeOS loader.
 * No gnu-efi needed: the structs below cover exactly what loader.c
 * uses. UEFI firmware uses the Microsoft ABI, so every firmware call
 * goes through an EFIAPI (ms_abi) function pointer. */
#ifndef UEFI_EFI_H
#define UEFI_EFI_H

#define EFIAPI __attribute__((ms_abi))

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef u64 UINTN;
typedef u64 EFI_STATUS;
typedef void *EFI_HANDLE;
typedef u16 CHAR16;   /* compile with -fshort-wchar */

#define EFI_SUCCESS 0
#define EFI_ERROR_MASK 0x8000000000000000ull
#define EFI_BUFFER_TOO_SMALL (EFI_ERROR_MASK | 5)

typedef struct {
    u32 Data1;
    u16 Data2;
    u16 Data3;
    u8 Data4[8];
} EFI_GUID;

/* ---------- console ---------- */
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, const CHAR16 *String);
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_TEXT_STRING OutputString;
    void *TestString;
    void *QueryMode;
    void *SetMode;
    void *SetAttribute;
    void *ClearScreen;
    void *SetCursorPosition;
    void *EnableCursor;
    void *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* ---------- memory map ---------- */
typedef struct {
    u32 Type;
    u32 Pad;
    u64 PhysicalStart;
    u64 VirtualStart;
    u64 NumberOfPages;
    u64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ---------- boot services (full order; unused entries stay void *) ---------- */
struct EFI_BOOT_SERVICES;
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    int Type, int MemoryType, UINTN Pages, u64 *Memory);
typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap,
    UINTN *MapKey, UINTN *DescriptorSize, u32 *DescriptorVersion);
typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(
    EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    EFI_GUID *Protocol, void *Registration, void **Interface);
typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
    EFI_HANDLE ImageHandle, UINTN MapKey);
typedef EFI_STATUS (EFIAPI *EFI_STALL)(UINTN Microseconds);
typedef EFI_STATUS (EFIAPI *EFI_SET_WATCHDOG_TIMER)(
    UINTN Timeout, u64 WatchdogCode, UINTN DataSize, CHAR16 *WatchdogData);

typedef struct EFI_BOOT_SERVICES {
    void *Hdr[3];              /* EFI_TABLE_HEADER: u64 + u32 + u32 + u32 + u32 */
    void *RaiseTPL;
    void *RestoreTPL;
    EFI_ALLOCATE_PAGES AllocatePages;
    void *FreePages;
    EFI_GET_MEMORY_MAP GetMemoryMap;
    void *AllocatePool;
    void *FreePool;
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL HandleProtocol;
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_EXIT_BOOT_SERVICES ExitBootServices;
    void *GetNextMonotonicCount;
    EFI_STALL Stall;
    EFI_SET_WATCHDOG_TIMER SetWatchdogTimer;
    void *ConnectController;
    void *DisconnectController;
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL LocateProtocol;
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32;
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
} EFI_BOOT_SERVICES;

/* AllocatePages Type / MemoryType we use */
#define AllocateAddress 2
#define AllocateAnyPages 0
#define EfiLoaderData 2

/* ---------- system table (prefix up to BootServices) ---------- */
typedef struct {
    void *Hdr[3];
    CHAR16 *FirmwareVendor;
    u32 FirmwareRevision;
    u32 __pad;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
} EFI_SYSTEM_TABLE;

/* ---------- loaded image (prefix up to DeviceHandle) ---------- */
typedef struct {
    u32 Revision;
    u32 __pad;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
} EFI_LOADED_IMAGE_PROTOCOL;

/* ---------- filesystem ---------- */
struct EFI_FILE_PROTOCOL;
struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_VOLUME_OPEN)(
    struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
    struct EFI_FILE_PROTOCOL **Root);
typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    u64 Revision;
    EFI_VOLUME_OPEN OpenVolume;
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(
    struct EFI_FILE_PROTOCOL *This, struct EFI_FILE_PROTOCOL **NewHandle,
    const CHAR16 *FileName, u64 OpenMode, u64 Attributes);
typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(struct EFI_FILE_PROTOCOL *This);
typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(
    struct EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FILE_GETINFO)(
    struct EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType,
    UINTN *BufferSize, void *Buffer);
typedef struct EFI_FILE_PROTOCOL {
    u64 Revision;
    EFI_FILE_OPEN Open;
    EFI_FILE_CLOSE Close;
    void *Delete;
    EFI_FILE_READ Read;
    void *Write;
    void *GetPosition;
    void *SetPosition;
    EFI_FILE_GETINFO GetInfo;
    void *SetInfo;
    void *Flush;
} EFI_FILE_PROTOCOL;

#define EFI_FILE_MODE_READ 1

/* ---------- graphics output ---------- */
typedef struct {
    u32 Version;
    u32 HorizontalResolution;
    u32 VerticalResolution;
    int PixelFormat;
    /* EFI_PIXEL_BITMASK: 4 x u32 inline (not a pointer) */
    u32 PixelRedMask;
    u32 PixelGreenMask;
    u32 PixelBlueMask;
    u32 PixelReserved;
    u32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;
/* PixelFormat: 0 RGBReserved8, 1 BGRReserved8 (both 32bpp direct
 * color), 2 BitMask, 3 BltOnly (no direct framebuffer) */
struct EFI_GRAPHICS_OUTPUT_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_GOP_QUERY_MODE)(
    struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This, u32 ModeNumber,
    UINTN *SizeOfInfo, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
typedef EFI_STATUS (EFIAPI *EFI_GOP_SET_MODE)(
    struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This, u32 ModeNumber);
typedef struct {
    u32 MaxMode;
    u32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    u64 FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;
typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GOP_QUERY_MODE QueryMode;
    EFI_GOP_SET_MODE SetMode;
    void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ---------- GUIDs ---------- */
static EFI_GUID LoadedImageGuid = {
    0x5B1B31A1, 0x9562, 0x11D2,
    { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }
};
static EFI_GUID FileSystemGuid = {
    0x0964E5B22, 0x6459, 0x11D2,
    { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }
};
static EFI_GUID GopGuid = {
    0x9042A9DE, 0x23DC, 0x4A38,
    { 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A }
};
static EFI_GUID FileInfoGuid = {
    0x09576E92, 0x6D3F, 0x11D2,
    { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }
};
/* EFI_FILE_INFO prefix: Size u64, FileSize u64, ... */
typedef struct {
    u64 Size;
    u64 FileSize;
} EFI_FILE_INFO_PREFIX;

#endif
