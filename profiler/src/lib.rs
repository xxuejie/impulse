//! Guest-program profiler: turns a stream of guest PCs (via per-instruction
//! callback) plus the guest ELF into a folded-stack profile inspired by CKB's
//! `ckb-vm-pprof` debugger. The folded text format is directly consumable by:
//!   - `inferno-flamegraph` (SVG flamegraph)
//!   - CKB's `ckb-vm-pprof-converter`  (gzip pprof protobuf)
//!   - CKB's `ckb-vm-samply-converter` (Gecko JSON for `samply load`)
//!
//! One line per unique call path, root-first: `frame0; frame1; ...; frameN <count>`.
//!
//! Usage from the runner:
//! ```ignore
//! let mut prof = FoldedProfile::new(&elf_bytes);
//! let state = instance.execute_with_hook(inputs, Box::new(|pc| prof.on_instruction(pc)))?;
//! prof.write(&mut std::fs::File::create("folded.txt")?)?;
//! ```

use object::{File, Object, ObjectSymbol, SymbolKind};
use std::collections::HashMap;
use std::io::Write;

/// A function from the guest ELF: address range + folded-frame label.
struct Func {
    start: u64,
    end: u64,
    label: String,
}

/// Streaming folded-stack profiler. Processes one PC at a time via
/// [`on_instruction`], maintaining a shadow call stack and a count trie.
/// Memory is O(unique call paths × depth) — typically KB, not GB.
pub struct FoldedProfile {
    funcs: Vec<Func>,
    start_map: HashMap<u64, usize>,
    /// Shadow call stack of trie node IDs. Node 0 = root.
    stack: Vec<usize>,
    /// Trie nodes: (parent, func_idx, count). func_idx = usize::MAX for root.
    nodes: Vec<(Option<usize>, usize, u64)>,
    /// Child lookup: (parent_node, func_idx) → child_node.
    child: HashMap<(usize, usize), usize>,
}

impl FoldedProfile {
    /// Parse the ELF symbol table and initialize the profiler.
    pub fn new(elf: &[u8]) -> Result<Self, Box<dyn std::error::Error>> {
        let obj = File::parse(elf)?;
        let mut funcs = Vec::new();
        let mut start_map = HashMap::new();
        for sym in obj.symbols() {
            if sym.kind() != SymbolKind::Text || sym.size() == 0 {
                continue;
            }
            let start = sym.address();
            let mangled = sym.name().unwrap_or("??");
            let label = demangle(mangled);
            let idx = funcs.len();
            funcs.push(Func {
                start,
                end: start + sym.size(),
                label,
            });
            start_map.insert(start, idx);
        }
        let nil = usize::MAX;
        Ok(Self {
            funcs,
            start_map,
            stack: vec![0],
            nodes: vec![(None, nil, 0)],
            child: HashMap::new(),
        })
    }

    /// Called once per executed guest instruction. O(1) amortized — folds
    /// into the count trie without per-PC allocation.
    pub fn on_instruction(&mut self, pc: u32) {
        let pc = pc as u64;
        let nil = usize::MAX;
        let top_func = self.nodes[*self.stack.last().unwrap()].1;
        let in_top = top_func != nil && pc > self.funcs[top_func].start && pc <= self.funcs[top_func].end;

        if in_top {
            // stay in the current function
        } else if let Some(&f) = self.start_map.get(&pc) {
            // pc is exactly a function entry → call (flatten recursion)
            let already = self.stack.iter().any(|&n| self.nodes[n].1 == f);
            if !already {
                let parent = *self.stack.last().unwrap();
                let next = *self.child.entry((parent, f)).or_insert_with(|| {
                    self.nodes.push((Some(parent), f, 0));
                    self.nodes.len() - 1
                });
                self.stack.push(next);
            }
        } else {
            // unwind to the deepest frame whose range contains pc
            let mut found = None;
            for (d, &n) in self.stack.iter().enumerate().rev() {
                let f = self.nodes[n].1;
                if f != nil && pc > self.funcs[f].start && pc <= self.funcs[f].end {
                    found = Some(d);
                    break;
                }
            }
            if let Some(d) = found {
                self.stack.truncate(d + 1);
            }
        }

        self.nodes[*self.stack.last().unwrap()].2 += 1;
    }

    /// Write the folded-stack profile. Call after execution completes.
    pub fn write(&self, writer: &mut impl Write) -> Result<(), Box<dyn std::error::Error>> {
        for (id, (_, _, count)) in self.nodes.iter().enumerate() {
            if id == 0 || *count == 0 {
                continue;
            }
            let mut path: Vec<usize> = Vec::new();
            let mut cur = id;
            while let Some(parent) = self.nodes[cur].0 {
                path.push(self.nodes[cur].1);
                cur = parent;
            }
            path.reverse();
            let frames: Vec<&str> = path.iter().map(|&i| self.funcs[i].label.as_str()).collect();
            writeln!(writer, "{} {}", frames.join("; "), count)?;
        }
        writer.flush()?;
        Ok(())
    }
}

fn demangle(name: &str) -> String {
    if let Ok(d) = rustc_demangle::try_demangle(name) {
        return format!("{d:#}");
    }
    if let Ok(d) = cpp_demangle::Symbol::new(name) {
        return format!("{d:#}");
    }
    name.to_string()
}
