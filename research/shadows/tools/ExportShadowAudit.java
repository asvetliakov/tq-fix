// Headless Ghidra post-script. Exports a deterministic shadow-focused call
// graph closure, assembly listing, and decompilation from a fully analyzed PE.
// @category TitanQuest

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;

public class ExportShadowAudit extends GhidraScript {
    private static final int MAX_CALLER_DEPTH = 2;
    private static final int MAX_CALLEE_DEPTH = 3;

    private static class WalkItem {
        final Function function;
        final int direction;
        final int depth;

        WalkItem(Function function, int direction, int depth) {
            this.function = function;
            this.direction = direction;
            this.depth = depth;
        }
    }

    private static String quote(String value) {
        if (value == null) return "\"\"";
        return "\"" + value.replace("\"", "\"\"") + "\"";
    }

    private List<String> readSeeds(File file) throws Exception {
        List<String> seeds = new ArrayList<>();
        try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
            String line;
            while ((line = reader.readLine()) != null) {
                line = line.trim();
                if (!line.isEmpty() && !line.startsWith("#")) seeds.add(line);
            }
        }
        return seeds;
    }

    private String displayName(Function function) {
        String name = function.getName(true);
        return name == null ? function.getName() : name;
    }

    private boolean matchesSeed(Function function, String seed) {
        if (seed.startsWith("@")) {
            String expected = seed.substring(1).replace("0x", "");
            return function.getEntryPoint().toString().equalsIgnoreCase(expected);
        }
        return displayName(function).contains(seed);
    }

    private List<Function> sorted(Set<Function> functions) {
        List<Function> result = new ArrayList<>(functions);
        Collections.sort(result, Comparator.comparing(Function::getEntryPoint));
        return result;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 2) {
            throw new IllegalArgumentException(
                "ExportShadowAudit requires OUTPUT_DIR and SEEDS_FILE");
        }
        File output = new File(args[0]);
        File seedsFile = new File(args[1]);
        Files.createDirectories(output.toPath());
        List<String> seeds = readSeeds(seedsFile);

        Set<Function> roots = new LinkedHashSet<>();
        FunctionIterator all = currentProgram.getFunctionManager().getFunctions(true);
        while (all.hasNext()) {
            Function function = all.next();
            String name = displayName(function);
            for (String seed : seeds) {
                if (matchesSeed(function, seed)) {
                    roots.add(function);
                    break;
                }
            }
        }
        for (String seed : seeds) {
            if (!seed.startsWith("@")) continue;
            String value = seed.substring(1).replace("0x", "");
            Address address = currentProgram.getAddressFactory()
                .getDefaultAddressSpace().getAddress(value);
            Function function = currentProgram.getFunctionManager().getFunctionAt(address);
            if (function == null) {
                function = currentProgram.getFunctionManager().getFunctionContaining(address);
            }
            if (function == null && currentProgram.getMemory().contains(address)) {
                disassemble(address);
                createFunction(address, null);
                function = currentProgram.getFunctionManager().getFunctionAt(address);
            }
            if (function != null) roots.add(function);
        }

        Set<Function> closure = new LinkedHashSet<>();
        Map<Function, Integer> depth = new HashMap<>();
        Set<String> visitedWalks = new HashSet<>();
        ArrayDeque<WalkItem> queue = new ArrayDeque<>();
        for (Function root : roots) {
            closure.add(root);
            depth.put(root, 0);
            queue.add(new WalkItem(root, -1, 0));
            queue.add(new WalkItem(root, 1, 0));
        }
        while (!queue.isEmpty()) {
            monitor.checkCancelled();
            WalkItem item = queue.removeFirst();
            String visitKey = item.function.getEntryPoint() + ":" + item.direction;
            if (!visitedWalks.add(visitKey)) continue;
            int limit = item.direction < 0 ? MAX_CALLER_DEPTH : MAX_CALLEE_DEPTH;
            if (item.depth >= limit) continue;
            Set<Function> adjacent = item.direction < 0
                ? item.function.getCallingFunctions(monitor)
                : item.function.getCalledFunctions(monitor);
            for (Function function : adjacent) {
                if (function == null || function.isExternal()) continue;
                closure.add(function);
                int signedDepth = item.direction * (item.depth + 1);
                Integer oldDepth = depth.get(function);
                if (oldDepth == null || Math.abs(signedDepth) < Math.abs(oldDepth))
                    depth.put(function, signedDepth);
                queue.addLast(new WalkItem(function, item.direction, item.depth + 1));
            }
        }

        List<Function> functions = sorted(closure);
        try (PrintWriter out = new PrintWriter(
                new File(output, "functions.csv"), StandardCharsets.UTF_8)) {
            out.println("address,rva,size,depth,root,name");
            Address imageBase = currentProgram.getImageBase();
            for (Function function : functions) {
                long rva = function.getEntryPoint().subtract(imageBase);
                out.printf("%s,0x%08x,%d,%d,%s,%s%n",
                    function.getEntryPoint(), rva, function.getBody().getNumAddresses(),
                    depth.getOrDefault(function, -1), roots.contains(function),
                    quote(displayName(function)));
            }
        }

        try (PrintWriter out = new PrintWriter(
                new File(output, "callgraph.dot"), StandardCharsets.UTF_8)) {
            out.println("digraph shadow_pipeline {");
            out.println("  rankdir=LR;");
            for (Function function : functions) {
                String id = "n" + function.getEntryPoint().toString().replace(':', '_');
                String label = displayName(function).replace("\\", "\\\\")
                    .replace("\"", "\\\"");
                out.printf("  %s [label=\"%s\\n%s\"%s];%n", id, label,
                    function.getEntryPoint(), roots.contains(function)
                        ? ",shape=box,style=bold" : "");
            }
            Set<Function> included = new HashSet<>(functions);
            for (Function caller : functions) {
                for (Function callee : caller.getCalledFunctions(monitor)) {
                    if (!included.contains(callee)) continue;
                    String from = "n" + caller.getEntryPoint().toString().replace(':', '_');
                    String to = "n" + callee.getEntryPoint().toString().replace(':', '_');
                    out.printf("  %s -> %s;%n", from, to);
                }
            }
            out.println("}");
        }

        try (PrintWriter out = new PrintWriter(
                new File(output, "calls.csv"), StandardCharsets.UTF_8)) {
            out.println("caller_address,caller,callee_address,callee,external,in_closure");
            Set<Function> included = new HashSet<>(functions);
            for (Function caller : functions) {
                for (Function callee : caller.getCalledFunctions(monitor)) {
                    out.printf("%s,%s,%s,%s,%s,%s%n", caller.getEntryPoint(),
                        quote(displayName(caller)), callee.getEntryPoint(),
                        quote(displayName(callee)), callee.isExternal(),
                        included.contains(callee));
                }
            }
        }

        try (PrintWriter out = new PrintWriter(
                new File(output, "disassembly.asm"), StandardCharsets.UTF_8)) {
            for (Function function : functions) {
                monitor.checkCancelled();
                out.printf("%n; ============================================================================%n");
                out.printf("; %s @ %s, %d bytes, closure depth %d%n",
                    displayName(function), function.getEntryPoint(),
                    function.getBody().getNumAddresses(), depth.getOrDefault(function, -1));
                out.printf("; ============================================================================%n");
                InstructionIterator instructions = currentProgram.getListing()
                    .getInstructions(function.getBody(), true);
                while (instructions.hasNext()) {
                    Instruction instruction = instructions.next();
                    byte[] bytes = instruction.getBytes();
                    StringBuilder encoded = new StringBuilder();
                    for (byte value : bytes) encoded.append(String.format("%02x", value & 0xff));
                    out.printf("%s  %-24s  %s%n", instruction.getAddress(), encoded,
                        instruction.toString());
                }
            }
        }

        DecompInterface decompiler = new DecompInterface();
        decompiler.toggleCCode(true);
        decompiler.toggleSyntaxTree(true);
        decompiler.setSimplificationStyle("decompile");
        if (!decompiler.openProgram(currentProgram)) {
            throw new IllegalStateException("Decompiler could not open program");
        }
        try (PrintWriter out = new PrintWriter(
                new File(output, "decompiled.c"), StandardCharsets.UTF_8)) {
            for (Function function : functions) {
                monitor.checkCancelled();
                out.printf("%n/* ========================================================================== */%n");
                out.printf("/* %s @ %s, closure depth %d */%n",
                    displayName(function), function.getEntryPoint(),
                    depth.getOrDefault(function, -1));
                out.printf("/* ========================================================================== */%n");
                DecompileResults result = decompiler.decompileFunction(function, 120, monitor);
                if (result.decompileCompleted() && result.getDecompiledFunction() != null) {
                    out.println(result.getDecompiledFunction().getC());
                } else {
                    out.printf("/* DECOMPILATION FAILED: %s */%n", result.getErrorMessage());
                }
            }
        } finally {
            decompiler.dispose();
        }

        try (PrintWriter out = new PrintWriter(
                new File(output, "data-references.csv"), StandardCharsets.UTF_8)) {
            out.println("function,from,to,type");
            for (Function function : functions) {
                AddressIterator addresses = function.getBody().getAddresses(true);
                while (addresses.hasNext()) {
                    Address address = addresses.next();
                    Reference[] refs = currentProgram.getReferenceManager()
                        .getReferencesFrom(address);
                    for (Reference ref : refs) {
                        if (ref.getReferenceType().isData()) {
                            out.printf("%s,%s,%s,%s%n", quote(displayName(function)),
                                ref.getFromAddress(), ref.getToAddress(),
                                ref.getReferenceType());
                        }
                    }
                }
            }
        }

        println("Exported " + functions.size() + " shadow-audit functions ("
            + roots.size() + " roots) to " + output);
    }
}
