; Original 68000 paravirtual .Sony driver for esp32_mac.
;
; This file is the readable source for kPvSonyDriver in pv_sony_driver.cpp.
; It is retained as source documentation and is not part of the ESP-IDF build.
; Regenerate from the repository root with:
;   tools\vasm68k\vasmm68k_mot.exe -m68000 -Fbin -o pv_sony_driver.bin src\pv_sony_driver.S

; The host patches the four zero address fields in the assembled payload with
; PV_FDD_GUEST_ADDR when installing the driver into ROM.
PV_FDD_ADDR   equ     $00000000
DISK_EVENT    equ     7
JIODONE       equ     $08fc
DRIVE_STATUS_SIZE equ  30
DRIVE_COUNT   equ     2
TM_TASK_SIZE  equ     22

driver:
        dc.w    $6f00                  ; RAM/read-write/periodic driver flags
        dc.w    30                     ; periodic Control call interval
        dc.w    0,0
        dc.w    open-driver
        dc.w    prime-driver
        dc.w    control-driver
        dc.w    status-driver
        dc.w    close-driver
        dc.b    5,".Sony"

open:
        move.l  a0,-(sp)
        move.l  a1,-(sp)
        moveq   #DRIVE_STATUS_SIZE*DRIVE_COUNT+TM_TASK_SIZE,d0
        dc.w    $a71e                  ; _NewPtrSysClear
        movea.l a0,a2
        movea.l (sp)+,a1
        movea.l (sp)+,a0
        move.l  a2,d0                  ; set Z without using 68020-only TST An
        beq.w   open_failed
        move.b  #0,PV_FDD_ADDR.l
        tst.w   d0
        bne.w   open_done

        ; System 6.0.8 expects the disk driver to leave at least one Time
        ; Manager task installed. The task remains inactive and its callback
        ; returns immediately.
        move.l  a0,-(sp)
        move.l  a1,-(sp)
        lea     DRIVE_STATUS_SIZE*DRIVE_COUNT(a2),a0
        lea     null_time_task(pc),a1
        move.l  a1,6(a0)
        dc.w    $a058                  ; _InsTime
        movea.l (sp)+,a1
        movea.l (sp)+,a0

        move.l  #$0001fffb,d0          ; drive 1, driver refnum -5
        lea     6(a2),a0
        dc.w    $a04e                  ; _AddDrive
        move.l  #$0002fffb,d0          ; drive 2, driver refnum -5
        lea     DRIVE_STATUS_SIZE+6(a2),a0
        dc.w    $a04e                  ; _AddDrive
open_done:
        rts
open_failed:
        moveq   #-23,d0                ; openErr
        rts

prime:
        move.b  #1,PV_FDD_ADDR.l
        bra.w   io_return

control:
        move.b  #2,PV_FDD_ADDR.l
        cmpi.w  #65,26(a0)             ; periodic accRun request
        bne.w   io_return
        tst.w   d0
        ble.w   io_return
        move.l  a0,-(sp)
        move.l  a1,-(sp)
        movea.w #DISK_EVENT,a0
        dc.w    $a02f                  ; _PostEvent; D0 is drive number
        movea.l (sp)+,a1
        movea.l (sp)+,a0
        moveq   #0,d0
        bra.w   io_return

status:
        move.b  #3,PV_FDD_ADDR.l
        bra.w   io_return

close:
        moveq   #-24,d0                ; closErr: driver remains installed
        rts

io_return:
        move.w  6(a0),d1
        btst    #9,d1                  ; noQueueBit
        beq.w   queued
        move.w  d0,16(a0)
        rts
queued:
        tst.w   d0
        ble.w   complete
        moveq   #0,d0                  ; positive means still in progress
        rts
complete:
        move.l  JIODONE.w,-(sp)        ; preserve A1: JIODone needs the DCE
        rts

null_time_task:
        rts
