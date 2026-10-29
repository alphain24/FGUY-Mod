// Research (Ghidra headless): decompile the functions holding the given addresses into a text file.
//   analyzeHeadless <project dir> FGUY -process FGUY.exe -noanalysis -readOnly -scriptPath tools/re/ghidra
//                   -postScript Decompile.java <out.txt> <hex address> [<hex address> ...]
// An address written "callers:<hex>" decompiles every function that calls the function holding it instead.
// @category FGUY
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

public class Decompile extends GhidraScript {
    DecompInterface d;
    PrintWriter out;

    void dump(String label, Function f) {
        DecompileResults r = d.decompileFunction(f, 120, monitor);
        out.println("// ===== " + label + " in " + f.getName() + " @ " + f.getEntryPoint());
        out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
    }

    // Xbox 360 code saves and restores registers through small shared helpers (__savegprlr_N / __restgprlr_N) that the
    // analysis takes for functions that never return, so every function ends at its first call. Those flags off, and
    // each function's body as its .pdata entry gives it (address, then length in instructions at bits 8-29).
    void fixXbox360() throws Exception {
        if (!currentProgram.getLanguageID().getIdAsString().startsWith("PowerPC")) return;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true))
            if (f.hasNoReturn() && f.getBody().getNumAddresses() < 0x200) f.setNoReturn(false);
        Address pdata = toAddr(0x820e7200L);
        for (long p = 0; p < 0x30310; p += 8) {
            long begin = getInt(pdata.add(p)) & 0xFFFFFFFFL, len = ((getInt(pdata.add(p + 4)) & 0xFFFFFFFFL) >> 8) & 0x3FFFFF;
            if (begin != 0 && len != 0) xboxBodies.put(begin, len);
        }
    }
    java.util.TreeMap<Long, Long> xboxBodies = new java.util.TreeMap<>();

    Function xboxFunction(Address a) throws Exception {
        java.util.Map.Entry<Long, Long> e = xboxBodies.floorEntry(a.getOffset());
        if (e == null || a.getOffset() >= e.getKey() + e.getValue() * 4) return null;
        Address start = toAddr(e.getKey());
        ghidra.program.model.address.AddressSet body = new ghidra.program.model.address.AddressSet(start, start.add(e.getValue() * 4 - 1));
        currentProgram.getListing().clearCodeUnits(start, start.add(e.getValue() * 4 - 1), false);
        new ghidra.app.cmd.disassemble.DisassembleCommand(body, body, true).applyTo(currentProgram, monitor);
        Function f = getFunctionAt(start);
        if (f == null) f = createFunction(start, null);
        if (f != null) f.setBody(body);
        return f;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        fixXbox360();
        d = new DecompInterface();
        d.openProgram(currentProgram);
        out = new PrintWriter(args[0], "UTF-8");
        try {
            for (int i = 1; i < args.length; i++) {
                boolean callers = args[i].startsWith("callers:");
                String hex = (callers ? args[i].substring(8) : args[i]).replace("0x", "");
                Address a = toAddr(Long.parseLong(hex, 16));
                Function f = xboxBodies.isEmpty() ? getFunctionContaining(a) : xboxFunction(a);
                if (f == null && !callers) {   // (not found by the analysis - e.g. Xbox 360 code: given its start, made here)
                    disassemble(a);
                    f = createFunction(a, null);
                }
                if (f == null) { out.println("// " + args[i] + ": no function"); continue; }
                if (!callers) { dump(args[i], f); continue; }
                Set<Function> seen = new LinkedHashSet<>();
                for (Reference ref : getReferencesTo(f.getEntryPoint())) {
                    Function c = getFunctionContaining(ref.getFromAddress());
                    out.println("// " + f.getName() + " referenced from " + ref.getFromAddress() + " (" + ref.getReferenceType() + ")"
                                + (c != null ? " in " + c.getName() : ""));
                    if (c != null) seen.add(c);
                }
                for (Function c : seen) dump("caller of " + f.getName(), c);
            }
        } finally {
            out.close();
        }
    }
}
