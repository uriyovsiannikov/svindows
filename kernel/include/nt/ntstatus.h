/*
 * nt/ntstatus.h - NTSTATUS result codes.
 *
 * NTSTATUS is a 32-bit value laid out as:
 *   bits 31..30  Severity   (0 success, 1 informational, 2 warning, 3 error)
 *   bit  29      Customer
 *   bit  28      Reserved
 *   bits 27..16  Facility
 *   bits 15..0   Code
 *
 * A value is "successful" (NT_SUCCESS) when it is non-negative, i.e. the top
 * severity bit is clear. Only a small, growing subset is defined here.
 */
#ifndef _NT_NTSTATUS_H_
#define _NT_NTSTATUS_H_

#include <nt/ntdef.h>

#define NT_SUCCESS(Status)        (((NTSTATUS)(Status)) >= 0)
#define NT_INFORMATION(Status)    ((((ULONG)(Status)) >> 30) == 1)
#define NT_WARNING(Status)        ((((ULONG)(Status)) >> 30) == 2)
#define NT_ERROR(Status)          ((((ULONG)(Status)) >> 30) == 3)

/* --- Severities ---------------------------------------------------- */
#define STATUS_SEVERITY_SUCCESS       0x0
#define STATUS_SEVERITY_INFORMATIONAL 0x1
#define STATUS_SEVERITY_WARNING       0x2
#define STATUS_SEVERITY_ERROR         0x3

/* --- Success ------------------------------------------------------- */
#define STATUS_SUCCESS                 ((NTSTATUS)0x00000000L)
#define STATUS_WAIT_0                  ((NTSTATUS)0x00000000L)
#define STATUS_TIMEOUT                 ((NTSTATUS)0x00000102L)
#define STATUS_PENDING                 ((NTSTATUS)0x00000103L)

/* --- Errors -------------------------------------------------------- */
#define STATUS_UNSUCCESSFUL            ((NTSTATUS)0xC0000001L)
#define STATUS_NOT_IMPLEMENTED         ((NTSTATUS)0xC0000002L)
#define STATUS_INVALID_INFO_CLASS      ((NTSTATUS)0xC0000003L)
#define STATUS_INFO_LENGTH_MISMATCH    ((NTSTATUS)0xC0000004L)
#define STATUS_ACCESS_VIOLATION        ((NTSTATUS)0xC0000005L)
#define STATUS_INVALID_HANDLE          ((NTSTATUS)0xC0000008L)
#define STATUS_INVALID_PARAMETER       ((NTSTATUS)0xC000000DL)
#define STATUS_ACCESS_DENIED           ((NTSTATUS)0xC0000022L)
#define STATUS_NO_SUCH_DEVICE          ((NTSTATUS)0xC000000EL)
#define STATUS_NO_SUCH_FILE            ((NTSTATUS)0xC000000FL)
#define STATUS_END_OF_FILE             ((NTSTATUS)0xC0000011L)
#define STATUS_NO_MEMORY               ((NTSTATUS)0xC0000017L)
#define STATUS_CONFLICTING_ADDRESSES   ((NTSTATUS)0xC0000018L)
#define STATUS_UNABLE_TO_FREE_VM       ((NTSTATUS)0xC000001AL)
#define STATUS_INVALID_PAGE_PROTECTION ((NTSTATUS)0xC0000045L)
#define STATUS_BUFFER_TOO_SMALL        ((NTSTATUS)0xC0000023L)
#define STATUS_OBJECT_TYPE_MISMATCH    ((NTSTATUS)0xC0000024L)
#define STATUS_OBJECT_NAME_INVALID     ((NTSTATUS)0xC0000033L)
#define STATUS_OBJECT_NAME_NOT_FOUND   ((NTSTATUS)0xC0000034L)
#define STATUS_OBJECT_NAME_COLLISION   ((NTSTATUS)0xC0000035L)
#define STATUS_OBJECT_PATH_NOT_FOUND   ((NTSTATUS)0xC000003AL)
#define STATUS_NOT_SUPPORTED           ((NTSTATUS)0xC00000BBL)
#define STATUS_INVALID_IMAGE_FORMAT    ((NTSTATUS)0xC000007BL)
#define STATUS_ENTRYPOINT_NOT_FOUND    ((NTSTATUS)0xC0000139L)
#define STATUS_INSUFFICIENT_RESOURCES  ((NTSTATUS)0xC000009AL)
#define STATUS_DEVICE_NOT_READY        ((NTSTATUS)0xC00000A3L)
#define STATUS_INTERNAL_ERROR          ((NTSTATUS)0xC00000E5L)
#define STATUS_FATAL_APP_EXIT          ((NTSTATUS)0x40000015L)

#endif /* _NT_NTSTATUS_H_ */
