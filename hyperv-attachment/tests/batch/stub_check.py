"""Emulate the built production MASM stub, never issue a host hypercall."""
import argparse
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn.x86_const import (
    UC_X86_REG_RAX, UC_X86_REG_RBX, UC_X86_REG_RCX, UC_X86_REG_RDX,
    UC_X86_REG_R8, UC_X86_REG_R9, UC_X86_REG_R10, UC_X86_REG_R11,
    UC_X86_REG_RIP, UC_X86_REG_RSP,
)


def verify(path):
    pe = pefile.PE(str(path))
    base = pe.OPTIONAL_HEADER.ImageBase
    entry = next(e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name == b"send_hypercall")
    runtime = next(e for e in pe.DIRECTORY_ENTRY_EXCEPTION if e.struct.BeginAddress == entry)
    unwind = pe.get_data(runtime.struct.UnwindData, 8)
    # UNWIND_INFO v1, one prolog byte (PUSH RBX), one UWOP_PUSH_NONVOL RBX.
    assert unwind[:6] == bytes([1, 1, 1, 0, 1, 0x30]), unwind.hex()
    image = pe.get_memory_mapped_image()
    scenarios = [0, 1, 0x1337, 0x8000000000000000, 0xFEDCBA9876543210]
    for response in scenarios:
        uc = Uc(UC_ARCH_X86, UC_MODE_64)
        uc.mem_map(base, (len(image) + 4095) & ~4095)
        uc.mem_write(base, image)
        scratch = 0x20000000
        uc.mem_map(scratch, 0x20000)
        params, stack, stop = scratch, scratch + 0x10008, scratch + 0x1F000
        args = [0x8A3B55C7927000B, 0x12345000, 0x112233445566, 64, 0x77889900, 0]
        uc.mem_write(params, struct.pack("<6Q", *args))
        uc.mem_write(stack, struct.pack("<Q", stop))
        uc.reg_write(UC_X86_REG_RSP, stack)
        uc.reg_write(UC_X86_REG_RCX, params)
        original_rbx = 0x123456789ABCDEF0
        uc.reg_write(UC_X86_REG_RBX, original_rbx)
        seen = []

        def hook(machine, address, size, _):
            if bytes(machine.mem_read(address, size)) != b"\x0f\xa2":
                return
            registers = [UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8,
                         UC_X86_REG_R9, UC_X86_REG_R10, UC_X86_REG_R11]
            assert [machine.reg_read(r) for r in registers] == args
            assert machine.reg_read(UC_X86_REG_RAX) == 0
            seen.append(address)
            machine.reg_write(UC_X86_REG_RAX, response)
            machine.reg_write(UC_X86_REG_RBX, 0xBADBADBAD)
            machine.reg_write(UC_X86_REG_RCX, 0xBADCAFE)
            machine.reg_write(UC_X86_REG_RDX, 0xBEEF)
            machine.reg_write(UC_X86_REG_RIP, address + size)

        uc.hook_add(UC_HOOK_CODE, hook)
        uc.emu_start(base + entry, stop, count=30)
        assert len(seen) == 1
        assert uc.reg_read(UC_X86_REG_RBX) == original_rbx
        assert uc.reg_read(UC_X86_REG_RAX) == response
        assert uc.reg_read(UC_X86_REG_RSP) == stack + 8
    return {"state": "PASS", "scenarios": len(scenarios), "unwind": "PUSH RBX, v1", "host_hypercalls": 0}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("model", type=Path)
    options = parser.parse_args()
    print(json.dumps(verify(options.model)))
