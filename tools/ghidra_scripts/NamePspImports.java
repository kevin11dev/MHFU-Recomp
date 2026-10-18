// Names the PSP firmware import stubs listed in a TSV written by tools/psp_imports.py.
// Run as a Ghidra headless pre-script: -preScript NamePspImports.java <imports.tsv>
//@category PSP

import java.io.File;
import java.nio.file.Files;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class NamePspImports extends GhidraScript {

	@Override
	protected void run() throws Exception {
		File tsv = new File(getScriptArgs()[0]);
		int named = 0;
		for (String line : Files.readAllLines(tsv.toPath())) {
			String[] cols = line.split("\t");
			Address addr = toAddr(Long.parseLong(cols[0], 16));
			String lib = cols[1], nid = cols[2], name = cols[3];

			disassemble(addr);
			Function f = getFunctionAt(addr);
			if (f == null) {
				f = createFunction(addr, name);
			}
			if (f == null) {
				printerr("Could not create import stub at " + addr);
				continue;
			}
			f.setName(name, SourceType.IMPORTED);
			setPlateComment(addr, "Import: " + lib + " NID 0x" + nid);
			named++;
		}
		println("Named " + named + " PSP import stubs");
	}
}
