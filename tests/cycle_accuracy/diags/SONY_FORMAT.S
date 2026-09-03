; POMIIGS — Apple IIgs emulator
; VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
;
; SONY_FORMAT.S — SmartPort FORMAT / WRITE / READ / verify on the slot-5
; 3.5" drive through the genuine ROM firmware (Sony LLE under POMIIGS).
; Boots as a ProDOS 8 SYSTEM file (built by tools/build_pom_diags.sh with
; Merlin32 + cp2, like the MiSTer diagnostics). Text page report:
;   row 0  SONY FORMAT TEST
;   row 2  STATUS:xx  FORMAT:xx  WRITE:xx  READ:xx  (xx = error code, 00 = ok)
;   row 4  VERIFY:P / VERIFY:F
;   row 6  FORMAT TEST PASSED / FORMAT TEST FAILED
        ORG   $2000
        TYP   $06
        DSK   SONYFMT.SYSTEM

SCREEN  EQU   $0400
ROW2    EQU   $0500
ROW4    EQU   $0600
ROW6    EQU   $0700
BUF1    EQU   $3000          ; write buffer (512 bytes)
BUF2    EQU   $3200          ; read buffer
DISPAT  EQU   $F0            ; zero page: SmartPort dispatch address (2 bytes)
ERRS    EQU   $F2            ; four error bytes: status, format, write, read

START   LDA   #$A0
        LDX   #$00
:CLR    STA   SCREEN,X
        STA   SCREEN+$80,X
        STA   SCREEN+$100,X
        STA   SCREEN+$180,X
        INX
        BNE   :CLR
        LDX   #$00
:HDR    LDA   TITLE,X
        BEQ   :HDRDN
        STA   SCREEN,X
        INX
        BNE   :HDR
:HDRDN
        LDA   #$FF           ; "not run" markers
        STA   ERRS
        STA   ERRS+1
        STA   ERRS+2
        STA   ERRS+3

; SmartPort dispatch = $C500 + ($C5FF) + 3
        LDA   $C5FF
        CLC
        ADC   #$03
        STA   DISPAT
        LDA   #$C5
        ADC   #$00
        STA   DISPAT+1
        LDA   #$4C           ; JMP (DISPAT) trampoline at $2FF0: JMP abs to the entry
        STA   $2FF0
        LDA   DISPAT
        STA   $2FF1
        LDA   DISPAT+1
        STA   $2FF2

; STATUS unit 1, code 0 → carry clear expected
        JSR   $2FF0
        DFB   $00
        DA    PL_STAT
        BCC   :STOK
        STA   ERRS
        JMP   :FMT
:STOK   LDA   #$00
        STA   ERRS

; FORMAT unit 1
:FMT    JSR   $2FF0
        DFB   $03
        DA    PL_FMT
        BCC   :FMOK
        STA   ERRS+1
        JMP   REPORT
:FMOK   LDA   #$00
        STA   ERRS+1

; fill BUF1 with a pattern (index XOR $A5), clear BUF2
        LDX   #$00
:FILL   TXA
        EOR   #$A5
        STA   BUF1,X
        STA   BUF1+$100,X
        LDA   #$00
        STA   BUF2,X
        STA   BUF2+$100,X
        INX
        BNE   :FILL

; WRITE block $0064 from BUF1
        JSR   $2FF0
        DFB   $02
        DA    PL_WRITE
        BCC   :WROK
        STA   ERRS+2
        JMP   REPORT
:WROK   LDA   #$00
        STA   ERRS+2

; READ block $0064 into BUF2
        JSR   $2FF0
        DFB   $01
        DA    PL_READ
        BCC   :RDOK
        STA   ERRS+3
        JMP   REPORT
:RDOK   LDA   #$00
        STA   ERRS+3

REPORT
; row 2: STATUS:xx  FORMAT:xx  WRITE:xx  READ:xx
        LDX   #$00
:R2     LDA   ROW2MSG,X
        BEQ   :R2DN
        STA   ROW2,X
        INX
        BNE   :R2
:R2DN   LDA   ERRS
        LDY   #$07
        JSR   PUTHEX
        LDA   ERRS+1
        LDY   #$12
        JSR   PUTHEX
        LDA   ERRS+2
        LDY   #$1C
        JSR   PUTHEX
        LDA   ERRS+3
        LDY   #$25
        JSR   PUTHEX

; verify only when every call succeeded
        LDA   ERRS
        ORA   ERRS+1
        ORA   ERRS+2
        ORA   ERRS+3
        BNE   FAILED
        LDX   #$00
:CMP    LDA   BUF1,X
        CMP   BUF2,X
        BNE   :BAD
        LDA   BUF1+$100,X
        CMP   BUF2+$100,X
        BNE   :BAD
        INX
        BNE   :CMP
        LDX   #$00
:V1     LDA   VOKMSG,X
        BEQ   :V1DN
        STA   ROW4,X
        INX
        BNE   :V1
:V1DN   LDX   #$00
:P1     LDA   PASSMSG,X
        BEQ   HALT
        STA   ROW6,X
        INX
        BNE   :P1
:BAD    LDX   #$00
:V2     LDA   VBADMSG,X
        BEQ   FAILED
        STA   ROW4,X
        INX
        BNE   :V2
FAILED  LDX   #$00
:F1     LDA   FAILMSG,X
        BEQ   HALT
        STA   ROW6,X
        INX
        BNE   :F1
HALT    JMP   HALT

; A = byte, Y = column on row 2 → two hex digits
PUTHEX  PHA
        LSR
        LSR
        LSR
        LSR
        JSR   :DIG
        STA   ROW2,Y
        INY
        PLA
        AND   #$0F
        JSR   :DIG
        STA   ROW2,Y
        RTS
:DIG    CMP   #$0A
        BCC   :NUM
        CLC
        ADC   #$07
:NUM    CLC
        ADC   #$B0           ; '0' | $80
        RTS

; SmartPort parameter lists
PL_STAT DFB   $03            ; count
        DFB   $01            ; unit 1
        DA    STATBUF
        DFB   $00            ; status code 0
PL_FMT  DFB   $01
        DFB   $01
PL_WRITE DFB  $03
        DFB   $01
        DA    BUF1
        DFB   $64,$00,$00    ; block $000064
PL_READ DFB   $03
        DFB   $01
        DA    BUF2
        DFB   $64,$00,$00
STATBUF DS    8

TITLE   ASC   "SONY FORMAT TEST"
        DFB   $00
ROW2MSG ASC   "STATUS:    FORMAT:    WRITE:    READ:   "
        DFB   $00
VOKMSG  ASC   "VERIFY:P"
        DFB   $00
VBADMSG ASC   "VERIFY:F"
        DFB   $00
PASSMSG ASC   "FORMAT TEST PASSED"
        DFB   $00
FAILMSG ASC   "FORMAT TEST FAILED"
        DFB   $00
