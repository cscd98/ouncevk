// Copyright © 2026 OunceVK contributors
// SPDX-License-Identifier: MIT

//! Removes copies from uniform to warp registers that legalization inserts.
//!
//! Some sources, such as the data of a store, must be warp registers, so
//! legalize copies a uniform value used there into one. Two cases make the
//! copy avoidable:
//!
//! - The uniform value came from a warp register: `%u = r2ur %x`. Uses of
//!   the copy read `%x` instead.
//! - The uniform value is computed by one instruction and only ever copied
//!   to warp registers. The instruction then writes a warp register itself,
//!   which NVIDIA's compiler also does for values only stores read.
//!
//! Warp ALU instructions likewise read `%x` rather than `%u = r2ur %x`: a
//! uniform register is not accepted in every source, and legalize would
//! copy it back. They do so only where `%x` is used later anyway, in the
//! blocks' dominance order, or within a few instructions of the r2ur: a
//! uniform value costs no GPR, and keeping `%x` live much longer would
//! raise register use and lower occupancy. The same limit applies to the
//! copies above.
//!
//! Legalize also copies a constant or uniform value into a warp register
//! separately for each instruction that needs it there, such as the handle
//! of every image load. A copy of the same value a few instructions after
//! the previous copy of it in the block reuses that copy.
//!
//! The pass runs on SSA after legalize, and legalize runs again after it,
//! since an instruction moved to the warp datapath may need its own sources
//! legalized.

use crate::ir::*;
use crate::opt_instr_sched_common::{side_effect_type, SideEffect};

use rustc_hash::FxHashMap;

/// Where an instruction is: its block and index, and its position in the
/// blocks' order.
#[derive(Clone, Copy)]
struct DefLoc {
    block: usize,
    ip: usize,
    pos: usize,
}

/// If `instr` copies a uniform GPR value into a warp GPR, returns the
/// destination and the uniform source.
fn uniform_to_warp_copy(instr: &Instr) -> Option<(SSAValue, SSAValue)> {
    let Op::Copy(copy) = &instr.op else {
        return None;
    };
    if !instr.pred.is_true() || !copy.src.is_unmodified() {
        return None;
    }
    let dst = copy.dst.as_ssa().filter(|d| d.comps() == 1)?[0];
    let SrcRef::SSA(src) = &copy.src.src_ref else {
        return None;
    };
    if src.comps() != 1 || dst.file() != RegFile::GPR {
        return None;
    }
    (src[0].file() == RegFile::UGPR).then_some((dst, src[0]))
}

/// Whether `instr`, which defines a uniform GPR value, can write a warp GPR
/// instead: a real, unpredicated instruction defining that one value
/// (not a vector), without predicate or carry sources, whose warp form
/// legalize can complete. R2UR and REDUX write only uniform registers.
fn can_move_to_warp(instr: &Instr) -> bool {
    let mut defs = 0;
    instr.for_each_ssa_def(|_| defs += 1);
    if instr.op.is_virtual()
        || !instr.pred.is_true()
        || matches!(instr.op, Op::R2UR(_) | Op::Redux(_))
        || defs != 1
    {
        return false;
    }
    let mut ok = true;
    instr.for_each_ssa_use(|ssa| {
        if matches!(ssa.file(), RegFile::UPred | RegFile::Carry | RegFile::Pred)
        {
            ok = false;
        }
    });
    ok
}

/// How far, in instructions of one block, a warp value may be kept live
/// past its last use so that an instruction reads it instead of its
/// uniform copy.
const SHORT_EXTENSION: usize = 16;

/// A uniform value copied from a warp value: `%u = r2ur %x`.
struct R2UR {
    warp: SSAValue,
    block: usize,
    pos: usize,
}

/// Program positions and uses, in the blocks' dominance order.
struct Positions {
    last_use: FxHashMap<SSAValue, usize>,
    r2ur: FxHashMap<SSAValue, R2UR>,
}

impl Positions {
    /// Whether an instruction at `pos` in `block` may read the warp value
    /// behind `uniform` instead of `uniform`: the warp value is used there
    /// or later anyway, or only briefly needs to live longer.
    fn warp_source(
        &self,
        uniform: SSAValue,
        block: usize,
        pos: usize,
    ) -> Option<SSAValue> {
        let r2ur = self.r2ur.get(&uniform)?;
        let live_anyway =
            self.last_use.get(&r2ur.warp).is_some_and(|last| *last >= pos);
        let brief = r2ur.block == block
            && pos.saturating_sub(r2ur.pos) <= SHORT_EXTENSION;
        (live_anyway || brief).then_some(r2ur.warp)
    }
}

/// Whether a warp ALU instruction's direct SSA sources may be rewritten.
/// The uniform address of a memory access and a bindless constant buffer
/// handle stay uniform, so only pure ALU instructions qualify.
fn reads_warp_sources(instr: &Instr) -> bool {
    !instr.is_uniform()
        && !instr.op.is_virtual()
        && !matches!(instr.op, Op::R2UR(_))
        && side_effect_type(&instr.op) == SideEffect::None
}

/// If `instr` copies a constant buffer or uniform value into a warp GPR,
/// returns the destination and the source.
fn constant_to_warp_copy(instr: &Instr) -> Option<(SSAValue, &Src)> {
    let Op::Copy(copy) = &instr.op else {
        return None;
    };
    if !instr.pred.is_true() || !copy.src.is_unmodified() {
        return None;
    }
    let dst = copy.dst.as_ssa().filter(|d| d.comps() == 1)?[0];
    if dst.file() != RegFile::GPR {
        return None;
    }
    let constant = match &copy.src.src_ref {
        SrcRef::CBuf(cb) => !matches!(cb.buf, CBuf::BindlessSSA(_)),
        SrcRef::SSA(src) => src.comps() == 1 && src[0].is_uniform(),
        _ => false,
    };
    constant.then_some((dst, &copy.src))
}

impl Function {
    /// Makes a copy of a constant or uniform value use the earlier copy of
    /// the same value in its block, when the previous copy of that value
    /// (kept or replaced) is at most SHORT_EXTENSION instructions before
    /// it.
    fn reuse_constant_copies(&mut self) -> bool {
        let mut rename: FxHashMap<SSAValue, SSAValue> = Default::default();
        for block in &mut self.blocks.iter_mut() {
            // Earlier copies: the source, the destination, and the
            // position of the last copy of that source.
            let mut earlier: Vec<(Src, SSAValue, usize)> = Vec::new();
            let mut removed: Vec<usize> = Vec::new();
            for (ip, instr) in block.instrs.iter().enumerate() {
                let Some((dst, src)) = constant_to_warp_copy(instr) else {
                    continue;
                };
                let reuse = earlier.iter_mut().find(|(earlier_src, _, last)| {
                    earlier_src == src && ip - *last <= SHORT_EXTENSION
                });
                if let Some((_, earlier_dst, last)) = reuse {
                    rename.insert(dst, *earlier_dst);
                    *last = ip;
                    removed.push(ip);
                } else {
                    earlier.retain(|(earlier_src, _, _)| earlier_src != src);
                    earlier.push((src.clone(), dst, ip));
                }
            }
            for ip in removed.into_iter().rev() {
                block.instrs.remove(ip);
            }
        }
        if rename.is_empty() {
            return false;
        }
        for block in &mut self.blocks.iter_mut() {
            for instr in &mut block.instrs {
                instr.for_each_ssa_use_mut(|ssa| {
                    if let Some(renamed) = rename.get(ssa) {
                        *ssa = *renamed;
                    }
                });
            }
        }
        true
    }

    pub fn opt_uniform_copies(&mut self) -> bool {
        let reused = self.reuse_constant_copies();
        // Scan in the blocks' order, which preserves dominance.
        let mut defs: FxHashMap<SSAValue, DefLoc> = Default::default();
        let mut uses: FxHashMap<SSAValue, usize> = Default::default();
        let mut positions = Positions {
            last_use: Default::default(),
            r2ur: Default::default(),
        };
        let mut copies: Vec<(DefLoc, SSAValue, SSAValue)> = Vec::new();
        let mut pos = 0;
        for block in 0..self.blocks.len() {
            for (ip, instr) in self.blocks[block].instrs.iter().enumerate() {
                let loc = DefLoc { block, ip, pos };
                instr.for_each_ssa_def(|ssa| {
                    defs.insert(*ssa, loc);
                });
                instr.for_each_ssa_use(|ssa| {
                    *uses.entry(*ssa).or_default() += 1;
                });
                if let Op::R2UR(r2ur) = &instr.op {
                    if let (Some(dst), SrcRef::SSA(x)) =
                        (r2ur.dst.as_ssa(), &r2ur.src.src_ref)
                    {
                        if dst.comps() == 1
                            && x.comps() == 1
                            && instr.pred.is_true()
                        {
                            positions
                                .r2ur
                                .insert(dst[0], R2UR { warp: x[0], block, pos });
                        }
                    }
                } else {
                    instr.for_each_ssa_use(|ssa| {
                        positions.last_use.insert(*ssa, pos);
                    });
                }
                if let Some((dst, src)) = uniform_to_warp_copy(instr) {
                    copies.push((loc, dst, src));
                }
                pos += 1;
            }
        }

        // Copies of each uniform value, to decide whether all its uses are
        // copies.
        let mut copies_of: FxHashMap<SSAValue, usize> = Default::default();
        for (_, _, src) in &copies {
            *copies_of.entry(*src).or_default() += 1;
        }

        // Decide on the original positions, then change the code.
        let mut rename: FxHashMap<SSAValue, SSAValue> = Default::default();
        let mut removed: Vec<DefLoc> = Vec::new();
        let mut moved_to_warp: FxHashMap<SSAValue, SSAValue> = Default::default();
        for (loc, dst, src) in copies {
            if let Some(warp) = positions.warp_source(src, loc.block, loc.pos) {
                rename.insert(dst, warp);
                removed.push(loc);
                continue;
            }
            let Some(def) = defs.get(&src).copied() else {
                continue;
            };
            let def_instr = &self.blocks[def.block].instrs[def.ip];
            let only_copied = uses.get(&src) == copies_of.get(&src);
            if !only_copied || !can_move_to_warp(def_instr) {
                continue;
            }
            // As a warp instruction it may read at most one uniform register,
            // and must not read uniform copies of warp values that legalize
            // would copy back: either would bring back a copy.
            let mut round_trip = false;
            let mut uniform_srcs = 0;
            def_instr.for_each_ssa_use(|ssa| {
                if positions.r2ur.contains_key(ssa) {
                    if positions.warp_source(*ssa, def.block, def.pos).is_none() {
                        round_trip = true;
                    }
                } else if ssa.is_uniform() {
                    uniform_srcs += 1;
                }
            });
            if round_trip || uniform_srcs > 1 {
                continue;
            }
            let warp = *moved_to_warp
                .entry(src)
                .or_insert_with(|| self.ssa_alloc.alloc(RegFile::GPR));
            rename.insert(dst, warp);
            removed.push(loc);
        }

        for (uniform, warp) in &moved_to_warp {
            let def = defs[uniform];
            let instr = &mut self.blocks[def.block].instrs[def.ip];
            instr.for_each_ssa_def_mut(|ssa| {
                if ssa == uniform {
                    *ssa = *warp;
                }
            });
        }

        let mut progress = reused || !rename.is_empty();
        let mut pos = 0;
        for block in 0..self.blocks.len() {
            for instr in &mut self.blocks[block].instrs {
                if reads_warp_sources(instr) {
                    for src in instr.srcs_mut() {
                        let SrcRef::SSA(vec) = &mut src.src_ref else {
                            continue;
                        };
                        for ssa in &mut vec[..] {
                            if let Some(warp) =
                                positions.warp_source(*ssa, block, pos)
                            {
                                *ssa = warp;
                                progress = true;
                            }
                        }
                    }
                }
                pos += 1;
            }
        }

        removed.sort_by_key(|loc| (loc.block, std::cmp::Reverse(loc.ip)));
        for loc in removed {
            self.blocks[loc.block].instrs.remove(loc.ip);
        }
        for block in 0..self.blocks.len() {
            for instr in &mut self.blocks[block].instrs {
                instr.for_each_ssa_use_mut(|ssa| {
                    if let Some(renamed) = rename.get(ssa) {
                        *ssa = *renamed;
                    }
                });
            }
        }
        progress
    }
}

impl Shader<'_> {
    /// Returns whether anything changed, in which case the shader needs
    /// legalizing again.
    pub fn opt_uniform_copies(&mut self) -> bool {
        let mut progress = false;
        for f in &mut self.functions {
            progress |= f.opt_uniform_copies();
        }
        progress
    }
}
