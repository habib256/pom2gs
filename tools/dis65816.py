#!/usr/bin/env python3
"""Minimal 65C816 disassembler for ROM archaeology (self-test triage).

    tools/dis65816.py <rom-file> <bank:addr> [count] [--m8|--m16] [--x8|--x16]

The ROM file maps to banks $FC-$FF (256 KB) or $FE-$FF (128 KB). M/X widths
only affect immediate operand sizes; pass them explicitly for native code.
"""
import sys

OPS = {}
def op(code, mn, mode): OPS[code] = (mn, mode)
# mode: number of operand bytes or special tag
for base, mn in [(0x00,'ORA'),(0x20,'AND'),(0x40,'EOR'),(0x60,'ADC'),(0x80,'STA'),(0xA0,'LDA'),(0xC0,'CMP'),(0xE0,'SBC')]:
    op(base|0x01, mn, '(dp,X)'); op(base|0x03, mn, 'sr,S'); op(base|0x05, mn, 'dp'); op(base|0x07, mn, '[dp]')
    op(base|0x09, mn, '#M'); op(base|0x0D, mn, 'abs'); op(base|0x0F, mn, 'long')
    op(base|0x11, mn, '(dp),Y'); op(base|0x12, mn, '(dp)'); op(base|0x13, mn, '(sr,S),Y'); op(base|0x15, mn, 'dp,X')
    op(base|0x17, mn, '[dp],Y'); op(base|0x19, mn, 'abs,Y'); op(base|0x1D, mn, 'abs,X'); op(base|0x1F, mn, 'long,X')
for base, mn in [(0x00,'ASL'),(0x20,'ROL'),(0x40,'LSR'),(0x60,'ROR')]:
    op(base|0x06, mn, 'dp'); op(base|0x0A, mn, 'A'); op(base|0x0E, mn, 'abs'); op(base|0x16, mn, 'dp,X'); op(base|0x1E, mn, 'abs,X')
for c,mn,md in [(0xE6,'INC','dp'),(0xEE,'INC','abs'),(0xF6,'INC','dp,X'),(0xFE,'INC','abs,X'),(0x1A,'INC','A'),
                (0xC6,'DEC','dp'),(0xCE,'DEC','abs'),(0xD6,'DEC','dp,X'),(0xDE,'DEC','abs,X'),(0x3A,'DEC','A'),
                (0xA2,'LDX','#X'),(0xA6,'LDX','dp'),(0xAE,'LDX','abs'),(0xB6,'LDX','dp,Y'),(0xBE,'LDX','abs,Y'),
                (0xA0,'LDY','#X'),(0xA4,'LDY','dp'),(0xAC,'LDY','abs'),(0xB4,'LDY','dp,X'),(0xBC,'LDY','abs,X'),
                (0x86,'STX','dp'),(0x8E,'STX','abs'),(0x96,'STX','dp,Y'),(0x84,'STY','dp'),(0x8C,'STY','abs'),(0x94,'STY','dp,X'),
                (0x64,'STZ','dp'),(0x74,'STZ','dp,X'),(0x9C,'STZ','abs'),(0x9E,'STZ','abs,X'),
                (0xE0,'CPX','#X'),(0xE4,'CPX','dp'),(0xEC,'CPX','abs'),(0xC0,'CPY','#X'),(0xC4,'CPY','dp'),(0xCC,'CPY','abs'),
                (0x24,'BIT','dp'),(0x2C,'BIT','abs'),(0x34,'BIT','dp,X'),(0x3C,'BIT','abs,X'),(0x89,'BIT','#M'),
                (0x04,'TSB','dp'),(0x0C,'TSB','abs'),(0x14,'TRB','dp'),(0x1C,'TRB','abs'),
                (0x4C,'JMP','abs'),(0x6C,'JMP','(abs)'),(0x7C,'JMP','(abs,X)'),(0x5C,'JML','long'),(0xDC,'JML','[abs]'),
                (0x20,'JSR','abs'),(0xFC,'JSR','(abs,X)'),(0x22,'JSL','long'),(0x60,'RTS','imp'),(0x6B,'RTL','imp'),(0x40,'RTI','imp'),
                (0x10,'BPL','rel'),(0x30,'BMI','rel'),(0x50,'BVC','rel'),(0x70,'BVS','rel'),(0x90,'BCC','rel'),(0xB0,'BCS','rel'),
                (0xD0,'BNE','rel'),(0xF0,'BEQ','rel'),(0x80,'BRA','rel'),(0x82,'BRL','rel16'),
                (0x00,'BRK','#8'),(0x02,'COP','#8'),(0x42,'WDM','#8'),(0xEA,'NOP','imp'),(0xCB,'WAI','imp'),(0xDB,'STP','imp'),
                (0x18,'CLC','imp'),(0x38,'SEC','imp'),(0x58,'CLI','imp'),(0x78,'SEI','imp'),(0xB8,'CLV','imp'),(0xD8,'CLD','imp'),(0xF8,'SED','imp'),
                (0xC2,'REP','#8'),(0xE2,'SEP','#8'),(0xFB,'XCE','imp'),(0xEB,'XBA','imp'),
                (0xAA,'TAX','imp'),(0xA8,'TAY','imp'),(0x8A,'TXA','imp'),(0x98,'TYA','imp'),(0x9A,'TXS','imp'),(0xBA,'TSX','imp'),
                (0x9B,'TXY','imp'),(0xBB,'TYX','imp'),(0x5B,'TCD','imp'),(0x7B,'TDC','imp'),(0x1B,'TCS','imp'),(0x3B,'TSC','imp'),
                (0xE8,'INX','imp'),(0xC8,'INY','imp'),(0xCA,'DEX','imp'),(0x88,'DEY','imp'),
                (0x48,'PHA','imp'),(0x68,'PLA','imp'),(0xDA,'PHX','imp'),(0xFA,'PLX','imp'),(0x5A,'PHY','imp'),(0x7A,'PLY','imp'),
                (0x08,'PHP','imp'),(0x28,'PLP','imp'),(0x8B,'PHB','imp'),(0xAB,'PLB','imp'),(0x0B,'PHD','imp'),(0x2B,'PLD','imp'),(0x4B,'PHK','imp'),
                (0xF4,'PEA','abs'),(0xD4,'PEI','(dp)'),(0x62,'PER','rel16'),(0x54,'MVN','mv'),(0x44,'MVP','mv')]:
    op(c,mn,md)

SIZE = {'imp':0,'A':0,'#8':1,'dp':1,'dp,X':1,'dp,Y':1,'(dp,X)':1,'(dp),Y':1,'(dp)':1,'[dp]':1,'[dp],Y':1,'sr,S':1,'(sr,S),Y':1,'rel':1,
        'abs':2,'abs,X':2,'abs,Y':2,'(abs)':2,'(abs,X)':2,'[abs]':2,'rel16':2,'mv':2,'long':3,'long,X':3}

def dis(rom, base_bank, bank, addr, count, m8=True, x8=True):
    lines=[]
    off=(bank-base_bank)*0x10000+addr
    for _ in range(count):
        if off>=len(rom): break
        c=rom[off]; mn,md=OPS.get(c,('???','imp'))
        n = 1 if md=='#M' and m8 else 2 if md=='#M' else 1 if md=='#X' and x8 else 2 if md=='#X' else SIZE[md]
        ops=rom[off+1:off+1+n]; pc=addr
        if md in('#M','#X','#8'): text='#$'+''.join('%02X'%b for b in reversed(ops))
        elif md=='rel': text='$%04X'%((addr+2+(ops[0]-256 if ops[0]>127 else ops[0]))&0xFFFF)
        elif md=='rel16': text='$%04X'%((addr+3+int.from_bytes(ops,'little',signed=True))&0xFFFF)
        elif md=='mv': text='$%02X,$%02X'%(ops[1],ops[0])
        elif md in('imp','A'): text=''
        else:
            v=int.from_bytes(ops,'little'); h='$%0*X'%(n*2,v)
            text=md.replace('dp',h).replace('abs',h).replace('long',h).replace('sr',h)
        raw=' '.join('%02X'%b for b in rom[off:off+1+n])
        lines.append('$%02X:%04X  %-12s %s %s'%(bank,pc,raw,mn,text))
        if mn=='REP' and ops[0]&0x20: m8=False
        if mn=='REP' and ops[0]&0x10: x8=False
        if mn=='SEP' and ops[0]&0x20: m8=True
        if mn=='SEP' and ops[0]&0x10: x8=True
        off+=1+n; addr=(addr+1+n)&0xFFFF
    return lines

if __name__=='__main__':
    rom=open(sys.argv[1],'rb').read(); base=0x100-len(rom)//0x10000
    bank,addr=sys.argv[2].split(':'); bank=int(bank,16); addr=int(addr,16)
    count=int(sys.argv[3]) if len(sys.argv)>3 and not sys.argv[3].startswith('--') else 40
    m8 = '--m16' not in sys.argv; x8 = '--x16' not in sys.argv
    print('\n'.join(dis(rom,base,bank,addr,count,m8,x8)))
