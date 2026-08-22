/*
 * ntos/trace.h - user-mode boundary tracing for genuine-DLL bring-up.
 *
 * The loader wraps selected USER32 imports in small trampolines that notify
 * the kernel (NtTraceCall) and tail-jump to the real function. The kernel
 * logs the call site and the first four arguments, which is the visibility we
 * need when a real Microsoft DLL silently changes behavior client-side.
 */
#ifndef NTOS_KERNEL_INCLUDE_NTOS_TRACE_H
#define NTOS_KERNEL_INCLUDE_NTOS_TRACE_H

/* Private syscall number used only by the loader-generated trampolines. */
#define NTOS_TRACE_SYSCALL 0xFB

/* Fixed user-mode address where a trampoline stores its tag before the
 * syscall; the kernel reads it from there. */
#define NTOS_TRACE_TAG_VA  0x000000000006FF80ULL

enum {
    NTOS_TRACE_RegisterClassW = 1,
    NTOS_TRACE_RegisterClassExW,
    NTOS_TRACE_CreateWindowExW,
    NTOS_TRACE_DestroyWindow,
    NTOS_TRACE_PostThreadMessageW,
    NTOS_TRACE_PostMessageW,
    NTOS_TRACE_DefWindowProcW,
    NTOS_TRACE_DispatchMessageW,
    NTOS_TRACE_RegisterClassWEntry,
    NTOS_TRACE_COUNT
};

#endif /* NTOS_KERNEL_INCLUDE_NTOS_TRACE_H */
