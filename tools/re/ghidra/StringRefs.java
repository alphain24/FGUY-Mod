// Research (Ghidra headless): where each given string (exact, 0-terminated) is used - every copy of it in memory and
// the code referring to it, with the function holding that code.
//   analyzeHeadless <project dir> <project> -process <file> -noanalysis -readOnly -scriptPath tools/re/ghidra
//                   -postScript StringRefs.java <out.txt> <string> [<string> ...]
// (Xbox 360 code builds addresses with lis + addi, which the analysis only sometimes ties to the string: see
// tools/re/ppcxref.py for those.)
// @category FGUY
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.io.PrintWriter;

public class StringRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
            for (int i = 1; i < args.length; i++) {
                byte[] want = (args[i] + "\0").getBytes("US-ASCII");
                Address at = currentProgram.getMinAddress();
                while (true) {
                    Address hit = find(at, want);
                    if (hit == null) break;
                    boolean whole = hit.getOffset() == 0 || getByte(hit.subtract(1)) == 0;
                    if (whole) {
                        out.println("\"" + args[i] + "\" at " + hit);
                        for (Reference r : getReferencesTo(hit)) {
                            Function f = getFunctionContaining(r.getFromAddress());
                            out.println("    from " + r.getFromAddress() + (f != null ? " in " + f.getName() + " @ " + f.getEntryPoint() : ""));
                        }
                    }
                    at = hit.add(1);
                }
            }
        }
    }
}
