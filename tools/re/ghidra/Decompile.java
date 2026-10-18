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

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        d = new DecompInterface();
        d.openProgram(currentProgram);
        out = new PrintWriter(args[0], "UTF-8");
        try {
            for (int i = 1; i < args.length; i++) {
                boolean callers = args[i].startsWith("callers:");
                String hex = (callers ? args[i].substring(8) : args[i]).replace("0x", "");
                Address a = toAddr(Long.parseLong(hex, 16));
                Function f = getFunctionContaining(a);
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
