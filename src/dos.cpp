// INT 21h / BIOS service dispatch.
#include "dos.h"
#include <cstdio>
#include <vector>

namespace dosemu {

void Dos::terminate(int code) {
    cpu_.exit_code = code;
    cpu_.halted = true;
}

bool Dos::handle(uint8_t n) {
    switch (n) {
        case 0x20: terminate(0); return true;           // terminate program
        case 0x21: return int21();
        case 0x10:                                       // BIOS video — accept and ignore
        case 0x1A:                                       // BIOS time
            return true;
        default:
            return int21_default(n);
    }
}

bool Dos::int21_default(uint8_t) { return true; }   // unknown INTs: no-op for now

bool Dos::int21() {
    uint8_t ah = cpu_.r[AX] >> 8;
    switch (ah) {
        case 0x00: terminate(0); return true;                       // terminate
        case 0x02: out(1, cpu_.r[DX] & 0xFF); return true;          // output char in DL
        case 0x04: return true;                                     // output to aux — ignore
        case 0x05: return true;                                     // output to printer — ignore
        case 0x06:                                                  // direct console I/O
            if ((cpu_.r[DX] & 0xFF) != 0xFF) { out(1, cpu_.r[DX] & 0xFF); }
            else { cpu_.sb(AX, 0); cpu_.flags |= ZF; }              // no input available
            return true;
        case 0x09: {                                                // output '$'-terminated string at DS:DX
            uint16_t seg = cpu_.sreg[DS], off = cpu_.r[DX];
            for (int i = 0; i < 65535; ++i) {
                char c = static_cast<char>(mem_.rb(seg, off + i));
                if (c == '$') break;
                out(1, c);
            }
            cpu_.sb(AX, '$');
            return true;
        }
        case 0x0B: cpu_.sb(AX, 0); return true;                     // check input status: none
        case 0x0C: return true;                                     // flush + input — ignore
        case 0x25: {                                                // set interrupt vector (AL=n) DS:DX
            uint8_t v = cpu_.r[AX] & 0xFF;
            mem_.w16(v * 4, cpu_.r[DX]);
            mem_.w16(v * 4 + 2, cpu_.sreg[DS]);
            return true;
        }
        case 0x30:                                                  // get DOS version: AL=major, AH=minor
            cpu_.r[AX] = (30 << 8) | 3;   // 3.30
            cpu_.r[BX] = 0; cpu_.r[CX] = 0;
            return true;
        case 0x35: {                                                // get interrupt vector (AL=n) -> ES:BX
            uint8_t v = cpu_.r[AX] & 0xFF;
            cpu_.r[BX] = mem_.r16(v * 4);
            cpu_.sreg[ES] = mem_.r16(v * 4 + 2);
            return true;
        }
        case 0x2A:                                                  // get date -> CX=year DH=month DL=day AL=dow
            cpu_.r[CX] = 1993; cpu_.r[DX] = (8 << 8) | 19; cpu_.sb(AX, 4); return true;
        case 0x2C:                                                  // get time -> CH=hr CL=min DH=sec DL=1/100
            cpu_.r[CX] = (12 << 8) | 0; cpu_.r[DX] = 0; return true;
        case 0x43: {                                                // get/set file attributes (AL=0 get, 1 set)
            if ((cpu_.r[AX] & 0xFF) == 0) {                          // get: also how a program tests existence
                if (files_.exists(read_asciiz(cpu_.sreg[DS], cpu_.r[DX]))) { cpu_.r[CX] = 0x20; cpu_.flags &= ~CF; }
                else { cpu_.flags |= CF; cpu_.r[AX] = 2; }
            } else cpu_.flags &= ~CF;                                // set: accept
            return true;
        }
        case 0x3C: {                                                // create file (CX attr, DS:DX name) -> handle
            int h = files_.create(read_asciiz(cpu_.sreg[DS], cpu_.r[DX]), cpu_.r[CX]);
            if (h < 0) { cpu_.flags |= CF; cpu_.r[AX] = -h; } else { cpu_.flags &= ~CF; cpu_.r[AX] = h; }
            return true;
        }
        case 0x3D: {                                                // open file (AL access, DS:DX name) -> handle
            int h = files_.open(read_asciiz(cpu_.sreg[DS], cpu_.r[DX]), cpu_.r[AX] & 0x03);
            if (h < 0) { cpu_.flags |= CF; cpu_.r[AX] = -h; } else { cpu_.flags &= ~CF; cpu_.r[AX] = h; }
            return true;
        }
        case 0x3E: {                                                // close handle
            int r = files_.close(cpu_.r[BX]);
            if (r < 0) { cpu_.flags |= CF; cpu_.r[AX] = -r; } else cpu_.flags &= ~CF;
            return true;
        }
        case 0x3F: {                                                // read (BX handle, CX bytes, DS:DX buf)
            uint16_t h = cpu_.r[BX], cnt = cpu_.r[CX], seg = cpu_.sreg[DS], off = cpu_.r[DX];
            int cfd;
            if (files_.is_console(h, cfd)) { cpu_.r[AX] = 0; cpu_.flags &= ~CF; return true; }  // stdin EOF for now
            std::vector<uint8_t> buf(cnt);
            int n = files_.read(h, buf.data(), cnt);
            if (n < 0) { cpu_.flags |= CF; cpu_.r[AX] = -n; return true; }
            for (int i = 0; i < n; ++i) mem_.wb(seg, off + i, buf[i]);
            cpu_.r[AX] = n; cpu_.flags &= ~CF; return true;
        }
        case 0x40: {                                                // write (BX handle, CX bytes, DS:DX buf)
            uint16_t h = cpu_.r[BX], cnt = cpu_.r[CX], seg = cpu_.sreg[DS], off = cpu_.r[DX];
            int cfd;
            if (files_.is_console(h, cfd)) {
                for (uint16_t i = 0; i < cnt; ++i) out(cfd == 2 ? 2 : 1, static_cast<char>(mem_.rb(seg, off + i)));
                cpu_.r[AX] = cnt; cpu_.flags &= ~CF; return true;
            }
            std::vector<uint8_t> buf(cnt);
            for (uint16_t i = 0; i < cnt; ++i) buf[i] = mem_.rb(seg, off + i);
            int n = files_.write(h, buf.data(), cnt);
            if (n < 0) { cpu_.flags |= CF; cpu_.r[AX] = -n; return true; }
            cpu_.r[AX] = n; cpu_.flags &= ~CF; return true;
        }
        case 0x41: {                                                // delete file (DS:DX name)
            int r = files_.remove(read_asciiz(cpu_.sreg[DS], cpu_.r[DX]));
            if (r < 0) { cpu_.flags |= CF; cpu_.r[AX] = -r; } else cpu_.flags &= ~CF;
            return true;
        }
        case 0x42: {                                                // lseek (AL method, BX handle, CX:DX offset) -> DX:AX
            long off = (static_cast<long>(cpu_.r[CX]) << 16) | cpu_.r[DX];
            long pos = files_.seek(cpu_.r[BX], off, cpu_.r[AX] & 0xFF);
            if (pos < 0) { cpu_.flags |= CF; cpu_.r[AX] = 6; return true; }
            cpu_.r[AX] = pos & 0xFFFF; cpu_.r[DX] = (pos >> 16) & 0xFFFF; cpu_.flags &= ~CF;
            return true;
        }
        case 0x45: {                                                // dup handle
            int h = files_.dup(cpu_.r[BX]);
            if (h < 0) { cpu_.flags |= CF; cpu_.r[AX] = -h; } else { cpu_.flags &= ~CF; cpu_.r[AX] = h; }
            return true;
        }
        case 0x46: {                                                // dup2 (force BX onto CX)
            int r = files_.dup2(cpu_.r[BX], cpu_.r[CX]);
            if (r < 0) { cpu_.flags |= CF; cpu_.r[AX] = -r; } else cpu_.flags &= ~CF;
            return true;
        }
        case 0x44:                                                  // IOCTL: report 0/1/2 as console devices
            if ((cpu_.r[AX] & 0xFF) == 0 && cpu_.r[BX] <= 2) {
                cpu_.r[DX] = 0x0080 | (cpu_.r[BX] == 0 ? 0x01 : 0x02) | 0x0010;  // char device, stdin/stdout, is-con
                cpu_.flags &= ~CF; return true;
            }
            cpu_.flags &= ~CF; cpu_.r[DX] = 0; return true;
        case 0x48: {                                                // allocate memory (BX paragraphs)
            uint16_t paras = cpu_.r[BX];
            if (heap_next_ + paras > heap_end_) {                   // not enough: report largest free
                cpu_.flags |= CF; cpu_.r[AX] = 8;                   // INSUFFICIENT MEMORY
                cpu_.r[BX] = heap_end_ - heap_next_;
                return true;
            }
            cpu_.r[AX] = heap_next_; heap_next_ += paras;
            cpu_.flags &= ~CF; return true;
        }
        case 0x49: cpu_.flags &= ~CF; return true;                  // free memory: accept (bump never frees)
        case 0x4A: {                                                // resize block (ES:block, BX paragraphs)
            // A startup shrink to free memory for children always succeeds; a grow
            // is granted up to the arena, which is all the guest needs here.
            cpu_.flags &= ~CF; return true;
        }
        case 0x4C: terminate(cpu_.r[AX] & 0xFF); return true;       // terminate with return code
        default:
            std::fprintf(stderr, "[dos] unimplemented INT 21h AH=%02Xh at %04X:%04X\n",
                         ah, cpu_.sreg[CS], cpu_.ip);
            cpu_.flags |= CF;   // signal error to the guest
            return true;
    }
}

}  // namespace dosemu
