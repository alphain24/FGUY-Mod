// Research (Ghidra headless): where the code uses given float constants - every 4-byte copy of each value in memory,
// and the code referring to that copy (x86: an fld / movss / fmul of [address]), with the function holding it.
//   analyzeHeadless <project dir> <project> -process <file> -noanalysis -readOnly -scriptPath tools/re/ghidra
//                   -postScript FloatRefs.java <out.txt> <float> [<float> ...]
// @category FGUY
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.io.PrintWriter;

public class FloatRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        boolean big = currentProgram.getLanguage().isBigEndian();
        try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
            for (int i = 1; i < args.length; i++) {
                int bits = Float.floatToIntBits(Float.parseFloat(args[i]));
                byte[] want = big ? new byte[] {(byte) (bits >> 24), (byte) (bits >> 16), (byte) (bits >> 8), (byte) bits}
                                  : new byte[] {(byte) bits, (byte) (bits >> 8), (byte) (bits >> 16), (byte) (bits >> 24)};
                Address at = currentProgram.getMinAddress();
                int copies = 0;
                while (copies < 200) {
                    Address hit = find(at, want);
                    if (hit == null) break;
                    copies++;
                    Reference[] refs = getReferencesTo(hit);
                    if (refs.length > 0) {
                        out.println(args[i] + " at " + hit);
                        for (Reference r : refs) {
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
