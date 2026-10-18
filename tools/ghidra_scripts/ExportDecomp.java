// Decompiles functions and writes the C output in address-ordered chunks, plus an index of
// functions and a list of defined strings, one directory per unit: "eboot" for the EBOOT itself,
// or the overlay name (e.g. "game_task") for memory blocks added by AddOverlayBlocks.java.
// Run as a Ghidra headless post-script: -postScript ExportDecomp.java <output root> eboot|overlays
//@category PSP

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.parallel.DecompileConfigurer;
import ghidra.app.decompiler.parallel.DecompilerCallback;
import ghidra.app.decompiler.parallel.ParallelDecompiler;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.util.task.TaskMonitor;

public class ExportDecomp extends GhidraScript {

	private static final long CHUNK_BYTES = 0x4000;
	private static final String OVERLAY_SUFFIX = ".ovl";

	private record Result(long entry, String name, long size, String code, String error) {}

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File outRoot = new File(args[0]);
		boolean overlays = args[1].equals("overlays");

		Map<String, List<Function>> unitFunctions = new TreeMap<>();
		currentProgram.getFunctionManager().getFunctions(true).forEach(f -> {
			String unit = unitOf(f.getEntryPoint());
			// The EBOOT calls into whichever overlay is meant to be loaded at an address, so with a
			// different overlay there, calls can land in its data; skip "functions" created there.
			MemoryBlock block = currentProgram.getMemory().getBlock(f.getEntryPoint());
			if (!f.isExternal() && !f.isThunk() && block != null && block.isExecute() &&
				isOverlay(unit) == overlays) {
				unitFunctions.computeIfAbsent(unit, u -> new ArrayList<>()).add(f);
			}
		});
		for (var entry : unitFunctions.entrySet()) {
			exportUnit(entry.getKey(), entry.getValue(), new File(outRoot, dirName(entry.getKey())));
		}
	}

	/** The overlay name (e.g. "game_task.ovl") for overlay blocks, or "eboot" for the rest. */
	private String unitOf(Address addr) {
		MemoryBlock block = currentProgram.getMemory().getBlock(addr);
		String name = block == null ? "" : block.getName().replaceFirst("\\.(header|data)$", "");
		return name.endsWith(OVERLAY_SUFFIX) ? name : "eboot";
	}

	private static boolean isOverlay(String unit) {
		return unit.endsWith(OVERLAY_SUFFIX);
	}

	private static String dirName(String unit) {
		return isOverlay(unit) ? unit.substring(0, unit.length() - OVERLAY_SUFFIX.length()) : unit;
	}

	private void exportUnit(String unit, List<Function> functions, File outDir) throws Exception {
		outDir.mkdirs();
		// What an earlier export wrote goes (another scene may have found other functions).
		File[] old = outDir.listFiles((dir, name) -> name.endsWith(".c") || name.endsWith(".tsv"));
		for (File file : old == null ? new File[0] : old) {
			file.delete();
		}
		String label = isOverlay(unit) ? unit : currentProgram.getName();
		println("Decompiling " + functions.size() + " functions in " + label);

		DecompileConfigurer configurer = decompiler -> {
			DecompileOptions options = new DecompileOptions();
			options.grabFromProgram(currentProgram);
			decompiler.setOptions(options);
			decompiler.toggleCCode(true);
			decompiler.toggleSyntaxTree(false);
			decompiler.setSimplificationStyle("decompile");
		};
		DecompilerCallback<Result> callback =
			new DecompilerCallback<>(currentProgram, configurer) {
				@Override
				public Result process(DecompileResults results, TaskMonitor m) {
					Function f = results.getFunction();
					long entry = f.getEntryPoint().getOffset();
					long size = f.getBody().getNumAddresses();
					if (results.decompileCompleted() && results.getDecompiledFunction() != null) {
						return new Result(entry, f.getName(), size,
							results.getDecompiledFunction().getC(), null);
					}
					return new Result(entry, f.getName(), size, null, results.getErrorMessage());
				}
			};
		callback.setTimeout(120);
		List<Result> results;
		try {
			results = new ArrayList<>(
				ParallelDecompiler.decompileFunctions(callback, functions, monitor));
		}
		finally {
			callback.dispose();
		}
		results.sort(Comparator.comparingLong(Result::entry));

		int failed = 0;
		PrintWriter chunk = null;
		long chunkStart = -1;
		try (PrintWriter index = new PrintWriter(new File(outDir, "functions.tsv"), "UTF-8")) {
			index.println("address\tsize\tname\tfile\tstatus");
			for (Result r : results) {
				long start = r.entry - (r.entry % CHUNK_BYTES);
				String file = String.format("%08X.c", start);
				if (start != chunkStart) {
					if (chunk != null) {
						chunk.close();
					}
					chunkStart = start;
					chunk = new PrintWriter(new File(outDir, file), "UTF-8");
					chunk.printf("// %s: functions at 0x%08X-0x%08X%n", label, start,
						start + CHUNK_BYTES - 1);
					chunk.println("// Ghidra decompiler output, with the names and types in re/ (FUN_ names: not named yet).");
					chunk.println();
				}
				chunk.printf("// 0x%08X  %s  (%d bytes)%n", r.entry, r.name, r.size);
				if (r.code != null) {
					chunk.println(r.code.strip());
				}
				else {
					failed++;
					chunk.println("// DECOMPILATION FAILED: " + r.error);
				}
				chunk.println();
				index.printf("%08X\t%d\t%s\t%s\t%s%n", r.entry, r.size, r.name, file,
					r.code != null ? "ok" : "failed");
			}
		}
		finally {
			if (chunk != null) {
				chunk.close();
			}
		}

		int strings = 0;
		try (PrintWriter out = new PrintWriter(new File(outDir, "strings.tsv"), "UTF-8")) {
			out.println("address\tstring");
			DataIterator it = currentProgram.getListing().getDefinedData(true);
			while (it.hasNext()) {
				Data d = it.next();
				Address addr = d.getAddress();
				if (addr.isLoadedMemoryAddress() && unitOf(addr).equals(unit) &&
					StringDataInstance.isString(d)) {
					String s = StringDataInstance.getStringDataInstance(d).getStringValue();
					if (s != null) {
						out.printf("%08X\t%s%n", addr.getOffset(),
							s.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")
								.replace("\r", "\\r"));
						strings++;
					}
				}
			}
		}
		println(String.format("Exported %d functions (%d failed) and %d strings to %s",
			results.size(), failed, strings, outDir));
	}
}
