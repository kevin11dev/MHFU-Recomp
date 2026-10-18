// Applies the reverse engineering database in re/ to the program: creates its structures, names
// the functions and sets their signatures and notes, and labels and types the globals. Functions
// of an overlay are only applied when that overlay was added (AddOverlayBlocks.java), as overlays
// share addresses.
// Run as a Ghidra headless script, after analysis: -postScript ApplySymbols.java <re directory>
//@category PSP

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.ArrayDataType;
import ghidra.program.model.data.CategoryPath;
import ghidra.program.model.data.CharDataType;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeConflictHandler;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.FloatDataType;
import ghidra.program.model.data.IntegerDataType;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.ShortDataType;
import ghidra.program.model.data.SignedCharDataType;
import ghidra.program.model.data.Structure;
import ghidra.program.model.data.StructureDataType;
import ghidra.program.model.data.UnsignedCharDataType;
import ghidra.program.model.data.UnsignedIntegerDataType;
import ghidra.program.model.data.UnsignedShortDataType;
import ghidra.program.model.data.VoidDataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Function.FunctionUpdateType;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class ApplySymbols extends GhidraScript {

	private static final CategoryPath CATEGORY = new CategoryPath("/mhfu");

	private record Field(long offset, String type, String name, String notes) {}

	private DataTypeManager dtm;
	private final Map<String, Structure> structs = new LinkedHashMap<>();

	@Override
	protected void run() throws Exception {
		File dir = new File(getScriptArgs()[0]);
		dtm = currentProgram.getDataTypeManager();
		applyStructs(new File(dir, "structs.tsv"));
		int functions = applyFunctions(new File(dir, "functions.tsv"));
		int globals = applyGlobals(new File(dir, "globals.tsv"));
		println(String.format("Applied %d structures, %d functions, %d globals", structs.size(),
			functions, globals));
	}

	/** Lines of a TSV file, without comments and blank lines, split on tabs. */
	private static List<String[]> rows(File file) throws Exception {
		List<String[]> rows = new ArrayList<>();
		for (String line : Files.readAllLines(file.toPath(), StandardCharsets.UTF_8)) {
			if (line.isBlank() || line.startsWith("#")) {
				continue;
			}
			rows.add(line.split("\t", -1));
		}
		return rows;
	}

	private static String column(String[] row, int i) {
		return i < row.length ? row[i].trim() : "";
	}

	private static long number(String text) {
		return Long.decode(text.trim());
	}

	private void applyStructs(File file) throws Exception {
		Map<String, List<Field>> fields = new LinkedHashMap<>();
		Map<String, Long> sizes = new LinkedHashMap<>();
		for (String[] row : rows(file)) {
			String struct = column(row, 0);
			long offset = number(column(row, 1));
			String type = column(row, 2);
			fields.computeIfAbsent(struct, s -> new ArrayList<>());
			if (type.equals("size")) {
				sizes.put(struct, offset);
			}
			else {
				fields.get(struct).add(new Field(offset, type, column(row, 3), column(row, 4)));
			}
		}
		// A "base" field gives a structure all of another's fields.
		Map<String, List<Field>> flat = new LinkedHashMap<>();
		for (String struct : fields.keySet()) {
			flat.put(struct, flatten(struct, fields, 0));
		}
		// The shells first, so fields can be of any structure.
		Structure vec3 = new StructureDataType(CATEGORY, "vec3", 0, dtm);
		vec3.add(FloatDataType.dataType, 4, "x", null);
		vec3.add(FloatDataType.dataType, 4, "y", null);
		vec3.add(FloatDataType.dataType, 4, "z", null);
		structs.put("vec3", (Structure) dtm.addDataType(vec3, DataTypeConflictHandler.REPLACE_HANDLER));
		for (String struct : flat.keySet()) {
			long size = sizes.getOrDefault(struct, 0L);
			for (String s = struct; size == 0 && s != null; s = baseOf(s, fields)) {
				size = sizes.getOrDefault(s, 0L);
			}
			Structure shell = new StructureDataType(CATEGORY, struct, (int) Math.max(size, 1), dtm);
			structs.put(struct,
				(Structure) dtm.addDataType(shell, DataTypeConflictHandler.REPLACE_HANDLER));
		}
		for (var entry : flat.entrySet()) {
			Structure struct = structs.get(entry.getKey());
			for (Field field : entry.getValue()) {
				DataType type = type(field.type);
				long end = field.offset + type.getLength();
				if (end > struct.getLength()) {
					struct.growStructure((int) (end - struct.getLength()));
				}
				struct.replaceAtOffset((int) field.offset, type, type.getLength(), field.name,
					field.notes.isEmpty() ? null : field.notes);
			}
		}
	}

	private static String baseOf(String struct, Map<String, List<Field>> fields) {
		for (Field f : fields.getOrDefault(struct, List.of())) {
			if (f.name.equals("base")) {
				return f.type;
			}
		}
		return null;
	}

	private List<Field> flatten(String struct, Map<String, List<Field>> fields, int depth) {
		if (depth > 8) {
			throw new IllegalArgumentException("structures inherit in a loop at " + struct);
		}
		List<Field> out = new ArrayList<>();
		for (Field f : fields.getOrDefault(struct, List.of())) {
			if (f.name.equals("base")) {
				for (Field b : flatten(f.type, fields, depth + 1)) {
					out.add(new Field(f.offset + b.offset, b.type, b.name, b.notes));
				}
			}
			else {
				out.add(f);
			}
		}
		return out;
	}

	/** A type written as in re/README.md. */
	private DataType type(String text) {
		text = text.trim();
		if (text.endsWith("*")) {
			return new PointerDataType(type(text.substring(0, text.length() - 1)), 4, dtm);
		}
		if (text.endsWith("]")) {
			int open = text.lastIndexOf('[');
			DataType element = type(text.substring(0, open));
			int count = Integer.decode(text.substring(open + 1, text.length() - 1).trim());
			return new ArrayDataType(element, count, element.getLength(), dtm);
		}
		switch (text) {
			case "u8": return UnsignedCharDataType.dataType;
			case "s8": return SignedCharDataType.dataType;
			case "char": return CharDataType.dataType;
			case "u16": return UnsignedShortDataType.dataType;
			case "s16": return ShortDataType.dataType;
			case "u32": return UnsignedIntegerDataType.dataType;
			case "s32": return IntegerDataType.dataType;
			case "f32": return FloatDataType.dataType;
			case "void": return VoidDataType.dataType;
			default:
				Structure struct = structs.get(text);
				if (struct == null) {
					throw new IllegalArgumentException("unknown type " + text);
				}
				return struct;
		}
	}

	/** Whether the address is in the module ("eboot" or an overlay's name). */
	private boolean inModule(Address addr, String module) {
		MemoryBlock block = currentProgram.getMemory().getBlock(addr);
		if (block == null) {
			return false;
		}
		String name = block.getName();
		boolean overlay = name.endsWith(".ovl");
		return module.equals("eboot") ? !overlay && !name.contains(".ovl.") : name.equals(module + ".ovl");
	}

	private int applyFunctions(File file) throws Exception {
		int applied = 0;
		for (String[] row : rows(file)) {
			String module = column(row, 0);
			Address addr = toAddr(number(column(row, 1)));
			String name = column(row, 2), signature = column(row, 3), notes = column(row, 4);
			if (!inModule(addr, module)) {
				continue;
			}
			Function f = getFunctionAt(addr);
			if (f == null) {
				disassemble(addr);
				f = createFunction(addr, name);
				if (f == null) {
					printerr("Couldn't make a function of " + name + " at " + addr);
					continue;
				}
			}
			f.setName(name, SourceType.USER_DEFINED);
			if (!notes.isEmpty()) {
				f.setComment(notes);
			}
			if (!signature.isEmpty()) {
				applySignature(f, signature);
			}
			applied++;
		}
		return applied;
	}

	/** "ret name(type a, type b)". */
	private void applySignature(Function f, String signature) throws Exception {
		int open = signature.indexOf('('), close = signature.lastIndexOf(')');
		String head = signature.substring(0, open).trim();
		String returnType = head.substring(0, head.lastIndexOf(' ') + 1).trim();
		if (head.endsWith("*") || returnType.isEmpty()) {
			throw new IllegalArgumentException("can't read " + signature);
		}
		f.setReturnType(type(returnType), SourceType.USER_DEFINED);
		List<Parameter> params = new ArrayList<>();
		String list = signature.substring(open + 1, close).trim();
		if (!list.isEmpty() && !list.equals("void")) {
			for (String param : list.split(",")) {
				param = param.trim().replace("*", "* ").replaceAll("\\s+", " ").trim();
				int space = param.lastIndexOf(' ');
				String type = param.substring(0, space).replace(" ", "");
				params.add(new ParameterImpl(param.substring(space + 1), type(type), currentProgram));
			}
		}
		f.replaceParameters(params, FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS, true,
			SourceType.USER_DEFINED);
	}

	private int applyGlobals(File file) throws Exception {
		int applied = 0;
		for (String[] row : rows(file)) {
			Address addr = toAddr(number(column(row, 0)));
			String name = column(row, 1), notes = column(row, 3);
			if (currentProgram.getMemory().getBlock(addr) == null) {
				continue;
			}
			createLabel(addr, name, true, SourceType.USER_DEFINED);
			DataType type = type(column(row, 2));
			clearListing(addr, addr.add(type.getLength() - 1));
			createData(addr, type);
			if (!notes.isEmpty()) {
				setEOLComment(addr, notes);
			}
			applied++;
		}
		return applied;
	}
}
