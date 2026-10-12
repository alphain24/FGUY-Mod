// Ghidra headless script: decompile the functions containing the given addresses (hex) into one text file.
//   decompile.ps1 <addr> [<addr> ...]       (see decompile.ps1; output: build/decomp.txt)
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.FileWriter;
import java.io.PrintWriter;

public class Decompile extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(args[0]));
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (int i = 1; i < args.length; i++) {
            Address a = toAddr(Long.parseLong(args[i].replace("0x", ""), 16));
            Function f = getFunctionContaining(a);
            if (f == null) {   // not found by the analysis: start it after the padding (int3s) before it
                Address p = a;
                for (int k = 0; k < 0x8000; k++, p = p.subtract(1)) {
                    if ((p.getOffset() & 0xf) == 0 && (getByte(p.subtract(1)) & 0xff) == 0xcc) break;
                }
                disassemble(p);
                f = createFunction(p, null);
                if (f == null) f = getFunctionContaining(a);
                if (f == null) { out.println("// " + args[i] + ": no function (tried " + p + ")"); continue; }
            }
            DecompileResults r = d.decompileFunction(f, 120, monitor);
            out.println("// ===== " + f.getName() + " @ " + f.getEntryPoint() + " (asked: " + args[i] + ")");
            out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
        }
        out.close();
    }
}
