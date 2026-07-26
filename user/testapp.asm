; ============================================================================
; user/testapp.asm - a native NTOS test program demonstrating multithreading.
;
; The main thread creates an auto-reset event and a worker thread (passing the
; event handle as the argument), then waits on the event. The worker runs, sets
; the event to wake main, and exits. Main then waits on the worker's thread
; handle (signaled on exit) before finishing - the classic create/signal/join.
;
; Windows x64 ABI: args in RCX/RDX/R8/R9; RAX returns; 32-byte shadow space;
; RBX/R12-R15 are non-volatile, so handles kept there survive the syscalls.
; ============================================================================
bits 64
default rel

extern NtCreateEvent
extern NtSetEvent
extern NtWaitForSingleObject
extern NtCreateThread
extern NtDisplayString
extern NtTerminateThread

section .text
global Start

; ---- main thread ----------------------------------------------------------
Start:
    and     rsp, -16
    sub     rsp, 32

    xor     ecx, ecx              ; notification = 0 (auto-reset event)
    xor     edx, edx              ; initial state = 0 (not signaled)
    call    NtCreateEvent
    mov     rbx, rax              ; rbx = event handle

    lea     rcx, [msg_created]
    call    NtDisplayString

    lea     rcx, [WorkerEntry]    ; thread = NtCreateThread(WorkerEntry, event)
    mov     rdx, rbx              ; argument = event handle
    call    NtCreateThread
    mov     r12, rax              ; r12 = worker thread handle

    lea     rcx, [msg_waiting]
    call    NtDisplayString

    mov     rcx, rbx              ; wait for the worker to signal the event
    call    NtWaitForSingleObject

    lea     rcx, [msg_woke]
    call    NtDisplayString

    mov     rcx, r12              ; join: wait for the worker thread to exit
    call    NtWaitForSingleObject

    lea     rcx, [msg_joined]
    call    NtDisplayString

    call    NtTerminateThread
.hang_main:
    jmp     .hang_main

; ---- worker thread (RCX = event handle) -----------------------------------
WorkerEntry:
    and     rsp, -16
    sub     rsp, 32
    mov     rbx, rcx              ; rbx = event handle

    lea     rcx, [msg_worker_run]
    call    NtDisplayString

    mov     r13, 0                ; burn some cycles so the interleaving shows
.work:
    inc     r13
    cmp     r13, 6000000
    jb      .work

    mov     rcx, rbx              ; signal the event -> wakes main
    call    NtSetEvent

    lea     rcx, [msg_worker_set]
    call    NtDisplayString

    call    NtTerminateThread
.hang_worker:
    jmp     .hang_worker

section .rdata
msg_created:    db "main: created an event and a worker thread", 0
msg_waiting:    db "main: waiting for the worker to signal the event...", 0
msg_woke:       db "main: woke up - the worker signaled the event", 0
msg_joined:     db "main: worker thread has exited; joining complete", 0
msg_worker_run: db "  worker: running, about to signal the event", 0
msg_worker_set: db "  worker: signaled the event, now exiting", 0
