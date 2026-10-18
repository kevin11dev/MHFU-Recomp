// Adds the EBOOT's .bss, and overlay images (from tools/extract_overlays.py or
// tools/ppst_overlays.py) as memory blocks at their load addresses, so they are analyzed together
// with the EBOOT that calls into them.
// Run as a Ghidra headless pre-script: -preScript AddOverlayBlocks.java <overlay dir>
//
// Each overlay is split using its MWo3 header so only the code is executable:
//   <name>.header  the 0x40-byte header
//   <name>         code
//   <name>.data    initialized data, then bss (zeroed, or what the game had stored there if
//                  the image came from a save state)
//@category PSP

import java.io.File;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.util.Arrays;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.MemoryBlock;

public class AddOverlayBlocks extends GhidraScript {

	private static final int HEADER_SIZE = 0x40;

	// The EBOOT's .bss (zeroed at start, not in the file) runs from the end of .data up to where
	// the overlays are loaded. Without a block there, the game's globals have no address.
	private static final long BSS_START = 0x089CBA90L, BSS_END = 0x09A5F200L;

	@Override
	protected void run() throws Exception {
		if (currentProgram.getMemory().getBlock(toAddr(BSS_START)) == null) {
			MemoryBlock bss = currentProgram.getMemory().createUninitializedBlock(".bss",
				toAddr(BSS_START), BSS_END - BSS_START, false);
			bss.setPermissions(true, true, false);
		}
		File[] files = new File(getScriptArgs()[0]).listFiles((dir, n) -> n.endsWith(".ovl.bin"));
		Arrays.sort(files);
		for (File file : files) {
			byte[] bytes = Files.readAllBytes(file.toPath());
			String name = file.getName().replaceFirst("\\.bin$", "");
			// MWo3 header: magic, overlay ID, load address, code size, data size, bss size, ...
			ByteBuffer header = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
			long addr = header.getInt(8) & 0xFFFFFFFFL;
			int code = header.getInt(12), data = header.getInt(16), bss = header.getInt(20);
			if (HEADER_SIZE + code + data == bytes.length) {
				// From DATA.BIN (tools/extract_overlays.py): no bss, which starts zeroed anyway
				bytes = Arrays.copyOf(bytes, bytes.length + bss);
			}
			if (HEADER_SIZE + code + data + bss != bytes.length) {
				printerr(name + ": header sizes don't add up, adding it as one executable block");
				add(name, addr, bytes, 0, bytes.length, true);
				continue;
			}
			add(name + ".header", addr, bytes, 0, HEADER_SIZE, false);
			add(name, addr, bytes, HEADER_SIZE, code, true);
			add(name + ".data", addr, bytes, HEADER_SIZE + code, data + bss, false);
			println(String.format("Added %s at 0x%08X: %d bytes code, %d data, %d bss", name, addr,
				code, data, bss));
		}
	}

	private void add(String name, long base, byte[] bytes, int offset, int length,
			boolean executable) throws Exception {
		Address start = toAddr(base + offset);
		MemoryBlock block = createMemoryBlock(name, start,
			Arrays.copyOfRange(bytes, offset, offset + length), false);
		block.setPermissions(true, true, executable);
	}
}
