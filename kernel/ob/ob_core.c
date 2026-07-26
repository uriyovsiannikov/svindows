/*
 * ob/ob_core.c - object types, object lifetime, reference counting.
 */
#include <ntos/ob.h>
#include <ntos/ex.h>
#include <ntos/ke.h>
#include <ntos/rtl.h>
#include "obp.h"

static LIST_ENTRY g_type_list;

void ObInitialize(void)
{
    InitializeListHead(&g_type_list);
    ObpInitializeNamespace();
    ObInitializeHandleTable();
    KeLog("[ob]   object manager initialized\n");
}

POBJECT_TYPE ObCreateObjectType(const char *name, OB_DELETE_METHOD del)
{
    POBJECT_TYPE type = ExAllocatePoolWithTag(NonPagedPool, sizeof(OBJECT_TYPE),
                                              'epyT');
    if (!type)
        return NULL;

    memset(type, 0, sizeof(*type));
    type->Name = name;
    type->DeleteProcedure = del;
    InsertTailList(&g_type_list, &type->TypeListEntry);

    KeLog("[ob]   registered object type '%s'\n", name);
    return type;
}

NTSTATUS ObCreateObject(POBJECT_TYPE type, SIZE_T body_size, POBJECT *out_object)
{
    if (!type || !out_object)
        return STATUS_INVALID_PARAMETER;

    SIZE_T total = sizeof(OBJECT_HEADER) + body_size;
    POBJECT_HEADER header = ExAllocatePoolWithTag(NonPagedPool, total, 'jbO');
    if (!header)
        return STATUS_NO_MEMORY;

    memset(header, 0, total);
    header->PointerCount = 1;
    header->HandleCount = 0;
    header->Type = type;
    header->Name = NULL;
    header->ParentDirectory = NULL;

    type->TotalObjects++;

    *out_object = ObObjectFromHeader(header);
    return STATUS_SUCCESS;
}

void ObReferenceObject(POBJECT object)
{
    POBJECT_HEADER header = ObHeaderFromObject(object);
    InterlockedIncrement(&header->PointerCount);
}

static void ObpDeleteObject(POBJECT_HEADER header)
{
    POBJECT_TYPE type = header->Type;

    /* Detach from the namespace if it is still linked (defensive; normally the
     * namespace holds a reference, so an inserted object never reaches zero). */
    if (header->ParentDirectory) {
        RemoveEntryList(&header->NamespaceEntry);
        header->ParentDirectory = NULL;
    }

    if (type->DeleteProcedure)
        type->DeleteProcedure(ObObjectFromHeader(header));

    if (type->TotalObjects)
        type->TotalObjects--;

    ExFreePool(header);
}

void ObDereferenceObject(POBJECT object)
{
    POBJECT_HEADER header = ObHeaderFromObject(object);
    LONG remaining = InterlockedDecrement(&header->PointerCount);
    if (remaining < 0)
        KeBugCheck(0xC0000000u, "ObDereferenceObject: reference count underflow");
    if (remaining == 0)
        ObpDeleteObject(header);
}

LONG ObGetReferenceCount(POBJECT object)
{
    return ObHeaderFromObject(object)->PointerCount;
}
