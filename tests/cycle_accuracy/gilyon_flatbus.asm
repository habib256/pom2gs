; POMIIGS — Apple IIgs emulator
; VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
;
; Flat-bus driver for the gilyon 65C816 test generator (snes-tests/cputest,
; MIT). The generated tests-full.inc is pure CPU code: each test calls
; `init_test` (far), runs, saves registers through bankN_save_results /
; `save_results` (far), compares against the expected values and jumps to
; `fail` on a mismatch; the last test jumps to `success`. The original driver
; reports on the SNES PPU; this one writes a status byte and STPs so
; tests/gilyon_test.cpp can read the verdict and the failing test number from
; memory. Layout (lorom.cfg) and the zero-page result block are kept
; identical so the generated code links unchanged.
;
;   ca65 gilyon_flatbus.asm -I <cputest> -o cputest_pom.o
;   ld65 -C lorom.cfg -o cputest_pom.sfc -Ln cputest_pom.lbl cputest_pom.o

.p816
.i16
.a8

.segment "HEADER"
	.byte "65C816 TEST POMIIGS  "
.segment "ROMINFO"
	.byte $30, 0, $08, 0, 0, 0, 0
	.word $0000, $FFFF

native_brk_handler = $1000
native_cop_handler = $1004
emulation_brk_handler = $1008
emulation_cop_handler = $100C

.segment "VECTORS"
	.word 0, 0, native_cop_handler, native_brk_handler, 0, 0, 0, 0
	.word 0, 0, emulation_cop_handler, 0, 0, 0, main, emulation_brk_handler

.segment "ZEROPAGE"
.res $10
test_num: .word 0
result_a: .word 0
result_x: .word 0
result_y: .word 0
result_p: .word 0
result_s: .word 0
result_d: .word 0
result_dbr: .byte 0
retaddr: .word 0
status: .byte 0          ; 0 running, 1 success, 2 failed, 3 invalid test order
.export status, test_num, main

.segment "CODE"

main:
	clc
	xce
	sei
	rep #$18  ; 16 bit X/Y
	sep #$20  ; 8 bit A
	ldx #$01EF
	txs
	; STP opcodes in the BRK/COP handler slots, as the original driver does
	lda #$DB
	sta $1000
	sta $1004
	sta $1008
	sta $100C
	stz status
	ldx #$ffff
	stx test_num
	jmp start_tests

; x = new test num (far call from every test)
init_test:
	dex
	cpx test_num
	beq @ok
	clc
	xce
	sei
	rep #$18
	sep #$20
	ldx #$01EF
	txs
	lda #$03
	sta status
	stp
@ok:
	inx
	stx test_num
	rtl

; Save the register values, and reset state (D, DBR, etc.) — verbatim from
; the original driver (pure CPU).
save_results:
	sei
	rep #$38
	.a16
	.i16
	phd
	pha
	lda #$0000
	tcd
	pla
	sta result_a
	stx result_x
	sty result_y
	plx  ; d register
	stx result_d
	tsc  ; original S value minus 3 (due to jsl).
	inc a
	inc a
	inc a
	sta result_s
	sep #$20
	.a8
	phb
	pla
	sta result_dbr
	lda #$00
	pha
	plb
	rtl

success:
	lda #$01
	sta status
	stp

fail:
	ldx #$1ef
	txs  ; in case s is invalid
	lda #$02
	sta status
	stp

	.include "tests-full.inc"

.segment "RODATA"
tests_table:
	.include "tests_table.inc"
	.faraddr success

.segment "TEST_DATA"  ; At address FFA0. Used by some tests (verbatim from the original driver)
test_addr:      ; $FFA0
	.word $1212
test_target:    ; $FFA2
	.word $8000
test_target24:  ; $FFA4
	.word $8000
	.byte $7E
