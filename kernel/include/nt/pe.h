/*
 * nt/pe.h - PE/COFF (Portable Executable) image structures.
 *
 * The subset needed to load a PE32+ (x86-64) executable: the MZ/PE headers, the
 * 64-bit optional header, section headers, and base relocations. Field layouts
 * match the Windows definitions exactly so real images parse correctly.
 */
#ifndef _NT_PE_H_
#define _NT_PE_H_

#include <nt/ntdef.h>

#define IMAGE_DOS_SIGNATURE            0x5A4D      /* "MZ" */
#define IMAGE_NT_SIGNATURE             0x00004550  /* "PE\0\0" */
#define IMAGE_FILE_MACHINE_AMD64       0x8664
#define IMAGE_NT_OPTIONAL_HDR64_MAGIC  0x020B

#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES 16
#define IMAGE_DIRECTORY_ENTRY_EXPORT     0
#define IMAGE_DIRECTORY_ENTRY_IMPORT     1
#define IMAGE_DIRECTORY_ENTRY_BASERELOC  5

#define IMAGE_ORDINAL_FLAG64 0x8000000000000000ULL

/* Section characteristics. */
#define IMAGE_SCN_CNT_CODE     0x00000020
#define IMAGE_SCN_MEM_EXECUTE  0x20000000
#define IMAGE_SCN_MEM_READ     0x40000000
#define IMAGE_SCN_MEM_WRITE    0x80000000

/* Base relocation types. */
#define IMAGE_REL_BASED_ABSOLUTE 0
#define IMAGE_REL_BASED_DIR64    10

typedef struct PACKED _IMAGE_DOS_HEADER {
    UINT16 e_magic;
    UINT16 e_cblp;
    UINT16 e_cp;
    UINT16 e_crlc;
    UINT16 e_cparhdr;
    UINT16 e_minalloc;
    UINT16 e_maxalloc;
    UINT16 e_ss;
    UINT16 e_sp;
    UINT16 e_csum;
    UINT16 e_ip;
    UINT16 e_cs;
    UINT16 e_lfarlc;
    UINT16 e_ovno;
    UINT16 e_res[4];
    UINT16 e_oemid;
    UINT16 e_oeminfo;
    UINT16 e_res2[10];
    UINT32 e_lfanew; /* file offset of the NT headers */
} IMAGE_DOS_HEADER, *PIMAGE_DOS_HEADER;

typedef struct PACKED _IMAGE_FILE_HEADER {
    UINT16 Machine;
    UINT16 NumberOfSections;
    UINT32 TimeDateStamp;
    UINT32 PointerToSymbolTable;
    UINT32 NumberOfSymbols;
    UINT16 SizeOfOptionalHeader;
    UINT16 Characteristics;
} IMAGE_FILE_HEADER, *PIMAGE_FILE_HEADER;

typedef struct PACKED _IMAGE_DATA_DIRECTORY {
    UINT32 VirtualAddress;
    UINT32 Size;
} IMAGE_DATA_DIRECTORY;

typedef struct PACKED _IMAGE_OPTIONAL_HEADER64 {
    UINT16 Magic;
    UINT8  MajorLinkerVersion;
    UINT8  MinorLinkerVersion;
    UINT32 SizeOfCode;
    UINT32 SizeOfInitializedData;
    UINT32 SizeOfUninitializedData;
    UINT32 AddressOfEntryPoint;
    UINT32 BaseOfCode;
    UINT64 ImageBase;
    UINT32 SectionAlignment;
    UINT32 FileAlignment;
    UINT16 MajorOperatingSystemVersion;
    UINT16 MinorOperatingSystemVersion;
    UINT16 MajorImageVersion;
    UINT16 MinorImageVersion;
    UINT16 MajorSubsystemVersion;
    UINT16 MinorSubsystemVersion;
    UINT32 Win32VersionValue;
    UINT32 SizeOfImage;
    UINT32 SizeOfHeaders;
    UINT32 CheckSum;
    UINT16 Subsystem;
    UINT16 DllCharacteristics;
    UINT64 SizeOfStackReserve;
    UINT64 SizeOfStackCommit;
    UINT64 SizeOfHeapReserve;
    UINT64 SizeOfHeapCommit;
    UINT32 LoaderFlags;
    UINT32 NumberOfRvaAndSizes;
    IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER64, *PIMAGE_OPTIONAL_HEADER64;

typedef struct PACKED _IMAGE_NT_HEADERS64 {
    UINT32                  Signature;
    IMAGE_FILE_HEADER       FileHeader;
    IMAGE_OPTIONAL_HEADER64 OptionalHeader;
} IMAGE_NT_HEADERS64, *PIMAGE_NT_HEADERS64;

typedef struct PACKED _IMAGE_SECTION_HEADER {
    UINT8  Name[8];
    UINT32 VirtualSize;        /* Misc.VirtualSize */
    UINT32 VirtualAddress;
    UINT32 SizeOfRawData;
    UINT32 PointerToRawData;
    UINT32 PointerToRelocations;
    UINT32 PointerToLinenumbers;
    UINT16 NumberOfRelocations;
    UINT16 NumberOfLinenumbers;
    UINT32 Characteristics;
} IMAGE_SECTION_HEADER, *PIMAGE_SECTION_HEADER;

typedef struct PACKED _IMAGE_BASE_RELOCATION {
    UINT32 VirtualAddress;
    UINT32 SizeOfBlock;
    /* followed by SizeOfBlock-8 bytes of UINT16 relocation entries */
} IMAGE_BASE_RELOCATION, *PIMAGE_BASE_RELOCATION;

/* --- Imports --- */

typedef struct PACKED _IMAGE_IMPORT_DESCRIPTOR {
    UINT32 OriginalFirstThunk; /* RVA of the import name table (INT) */
    UINT32 TimeDateStamp;
    UINT32 ForwarderChain;
    UINT32 Name;               /* RVA of the imported DLL's name */
    UINT32 FirstThunk;         /* RVA of the import address table (IAT) */
} IMAGE_IMPORT_DESCRIPTOR, *PIMAGE_IMPORT_DESCRIPTOR;

typedef struct PACKED _IMAGE_IMPORT_BY_NAME {
    UINT16 Hint;
    CHAR   Name[1]; /* NUL-terminated */
} IMAGE_IMPORT_BY_NAME, *PIMAGE_IMPORT_BY_NAME;

/* --- Exports --- */

typedef struct PACKED _IMAGE_EXPORT_DIRECTORY {
    UINT32 Characteristics;
    UINT32 TimeDateStamp;
    UINT16 MajorVersion;
    UINT16 MinorVersion;
    UINT32 Name;                  /* RVA of the DLL name */
    UINT32 Base;                  /* starting ordinal number */
    UINT32 NumberOfFunctions;
    UINT32 NumberOfNames;
    UINT32 AddressOfFunctions;    /* RVA -> DWORD[NumberOfFunctions] of func RVAs */
    UINT32 AddressOfNames;        /* RVA -> DWORD[NumberOfNames] of name RVAs     */
    UINT32 AddressOfNameOrdinals; /* RVA -> WORD[NumberOfNames] of ordinals       */
} IMAGE_EXPORT_DIRECTORY, *PIMAGE_EXPORT_DIRECTORY;

#endif /* _NT_PE_H_ */
