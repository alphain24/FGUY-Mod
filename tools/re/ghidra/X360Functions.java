// Research (Ghidra headless): the Xbox 360 build's functions, from its exception table (.pdata). The image is the XEX's
// decrypted memory image (tools/re/xex.ps1) imported raw at 0x82000000 as PowerPC:BE:64:A2ALT-32addr; .pdata holds one
// 8-byte entry per function: its address, then (bits 8-29) its length in instructions.
//   analyzeHeadless <project dir> X360 -import x360_default.exe.bin -loader BinaryLoader -loader-baseAddr 0x82000000
//                   -processor PowerPC:BE:64:A2ALT-32addr -scriptPath tools/re/ghidra -preScript X360Functions.java <pdata hex> <size hex>
// @category FGUY
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;

public class X360Functions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        long pdata = Long.parseLong(args[0].replace("0x", ""), 16), size = Long.parseLong(args[1].replace("0x", ""), 16);
        int made = 0;
        for (long p = pdata; p + 8 <= pdata + size; p += 8) {
            long begin = getInt(toAddr(p)) & 0xFFFFFFFFL, info = getInt(toAddr(p + 4)) & 0xFFFFFFFFL;
            long len = (info >> 8) & 0x3FFFFF;
            if (begin == 0 || len == 0) continue;
            Address a = toAddr(begin);
            AddressSet body = new AddressSet(a, a.add(len * 4 - 1));
            new DisassembleCommand(a, body, true).applyTo(currentProgram, monitor);
            if (getFunctionAt(a) == null && createFunction(a, null) != null) made++;
        }
        println("X360Functions: " + made + " functions from .pdata");
    }
}
