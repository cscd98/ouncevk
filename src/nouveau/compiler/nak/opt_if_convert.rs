// Copyright © 2026 OunceVK contributors
// SPDX-License-Identifier: MIT

//! If-conversion of short divergent `if`s.
//!
//! A divergent `if` reaches NAK as a head, a then block, an else block and a
//! join:
//!
//! ```text
//! head:  b = bclear; b' = bssy b L; @!p bra else
//! then:  ...; bra join
//! else:  ...; bra join          (empty when the if has no else)
//! join:  phis; bsync b'; nop L; ...
//! ```
//!
//! or with the head branching straight to the join. The reconvergence
//! barrier and the branches cost about five instructions and a taken
//! branch. For short bodies, NVIDIA's compiler predicates the bodies
//! instead. This pass does the same:
//!
//! - ALU instructions and special-register reads run unconditionally: they
//!   have no side effects and cannot fault.
//! - Memory, texture and surface instructions run under the predicate of
//!   their side. A global load uses its own predicate source, which returns
//!   0 when false; the others use the instruction predicate, so their
//!   results are undefined when false. Such a result is then only used by
//!   instructions of the same side or by a select that discards it.
//! - The join's phis become selects on the predicate.
//!
//! An `if` that nothing after it needs to reconverge from has no
//! `bssy`/`bsync`, and is converted the same way.
//!
//! In compute shaders, an `if` without an `else` that is too long to
//! predicate, after which the shader only exits, becomes a predicated exit
//! of the threads that skip it, as NVIDIA's compiler does:
//!
//! ```text
//! head:  ...; @!p exit
//! then:  ...
//! ```
//!
//! Fragment shaders keep the branch: an exited thread can no longer act as
//! a helper for its quad's derivatives.
//!
//! The join then has only the head as predecessor and is merged into it
//! when possible, so an enclosing `if` whose body held this one can be
//! converted in turn. The pass runs on SSA, after opt_bar_prop has removed
//! the barrier moves, and matches SM70 reconvergence (`bssy`/`bsync`).

use crate::ir::*;
use crate::opt_instr_sched_common::{side_effect_type, SideEffect};
use crate::opt_jump_thread::{opt_fall_through, rewrite_cfg};

use rustc_hash::FxHashMap;

/// The most instructions, both sides together, that are predicated
/// instead of branched over.
const MAX_PREDICATED_INSTRS: usize = 8;

/// How one instruction of a body is converted.
enum Conversion {
    /// Runs unconditionally.
    Unconditional,
    /// A global load whose own predicate source is free.
    LoadPredicate,
    /// Runs under the instruction predicate.
    InstrPredicate,
}

fn classify(instr: &Instr) -> Option<Conversion> {
    if !instr.pred.is_true() {
        return None;
    }
    match &instr.op {
        Op::Ld(ld) => match ld.access.space {
            MemSpace::Global(_) if ld.pred.is_true() => {
                Some(Conversion::LoadPredicate)
            }
            MemSpace::Global(_) | MemSpace::Shared => {
                Some(Conversion::InstrPredicate)
            }
            _ => None,
        },
        Op::St(st) => match st.access.space {
            MemSpace::Global(_) | MemSpace::Shared => {
                Some(Conversion::InstrPredicate)
            }
            _ => None,
        },
        Op::Atom(atom) => match atom.mem_space {
            MemSpace::Global(_) | MemSpace::Shared => {
                Some(Conversion::InstrPredicate)
            }
            _ => None,
        },
        Op::Tex(_)
        | Op::Tld(_)
        | Op::Tld4(_)
        | Op::Tmml(_)
        | Op::Txd(_)
        | Op::Txq(_)
        | Op::SuLd(_)
        | Op::SuSt(_)
        | Op::SuAtom(_)
        | Op::Ldc(_)
        | Op::Ipa(_) => Some(Conversion::InstrPredicate),
        Op::S2R(_) | Op::CS2R(_) | Op::Copy(_) => {
            Some(Conversion::Unconditional)
        }
        op if op.is_virtual() => None,
        op if is_pure_alu(op) => Some(Conversion::Unconditional),
        _ => None,
    }
}

/// ALU instructions that only compute their destinations from their
/// sources. Subgroup operations are excluded: their result depends on
/// which threads are active.
fn is_pure_alu(op: &Op) -> bool {
    side_effect_type(op) == SideEffect::None
        && !matches!(
            op,
            Op::Shfl(_)
                | Op::Vote(_)
                | Op::Match(_)
                | Op::Redux(_)
                | Op::PixLd(_)
        )
}

fn is_bookkeeping(instr: &Instr) -> bool {
    matches!(instr.op, Op::PhiSrcs(_) | Op::Annotate(_))
}

/// Counts the body instructions of `block`, or returns None if any of them
/// cannot be converted.
fn count_body(block: &BasicBlock) -> Option<usize> {
    let mut count = 0;
    for instr in &block.instrs {
        if is_bookkeeping(instr) || instr.is_branch() {
            continue;
        }
        classify(instr)?;
        count += 1;
    }
    Some(count)
}

/// The reconvergence instructions of one `if`.
struct Reconvergence {
    bclear_ip: usize,
    bssy_ip: usize,
    /// In the join block.
    bsync_ip: usize,
    nop_ip: usize,
}

fn single_ssa(src: &Src) -> Option<SSAValue> {
    match &src.src_ref {
        SrcRef::SSA(ssa) if ssa.comps() == 1 => Some(ssa[0]),
        _ => None,
    }
}

fn single_dst_ssa(dst: &Dst) -> Option<SSAValue> {
    match dst.as_ssa() {
        Some(ssa) if ssa.comps() == 1 => Some(ssa[0]),
        _ => None,
    }
}

fn count_ssa_uses(func: &Function, value: SSAValue) -> usize {
    let mut uses = 0;
    for block in func.blocks.iter() {
        for instr in &block.instrs {
            for src in instr.srcs() {
                uses += src.src_ref.iter_ssa().filter(|s| **s == value).count();
            }
            uses += instr.pred.iter_ssa().filter(|s| **s == value).count();
        }
    }
    uses
}

/// Finds `b = bclear; b' = bssy b L` in `head` and `bsync b'; nop L` at the
/// start of `join`, each barrier value used only there.
fn find_reconvergence(
    func: &Function,
    head: usize,
    join: usize,
) -> Option<Reconvergence> {
    let head_instrs = &func.blocks[head].instrs;
    let bssy_ip = head_instrs
        .iter()
        .rposition(|instr| matches!(instr.op, Op::BSSy(_)))?;
    let Op::BSSy(bssy) = &head_instrs[bssy_ip].op else {
        unreachable!();
    };
    if !bssy.cond.is_true() || !head_instrs[bssy_ip].pred.is_true() {
        return None;
    }
    let bar_in = single_ssa(&bssy.bar_in)?;
    let bar_out = single_dst_ssa(&bssy.bar_out)?;
    let bclear_ip = head_instrs.iter().position(|instr| match &instr.op {
        Op::BClear(bclear) => single_dst_ssa(&bclear.dst) == Some(bar_in),
        _ => false,
    })?;

    let join_instrs = &func.blocks[join].instrs;
    let bsync_ip = join_instrs.iter().position(|instr| {
        !matches!(instr.op, Op::PhiDsts(_) | Op::Annotate(_))
    })?;
    let Op::BSync(bsync) = &join_instrs[bsync_ip].op else {
        return None;
    };
    if !bsync.cond.is_true()
        || !join_instrs[bsync_ip].pred.is_true()
        || single_ssa(&bsync.bar) != Some(bar_out)
    {
        return None;
    }
    let nop_ip = bsync_ip + 1;
    match join_instrs.get(nop_ip).map(|instr| &instr.op) {
        Some(Op::Nop(nop)) if nop.label == Some(bssy.target) => (),
        _ => return None,
    }

    if count_ssa_uses(func, bar_in) != 1 || count_ssa_uses(func, bar_out) != 1
    {
        return None;
    }
    Some(Reconvergence {
        bclear_ip,
        bssy_ip,
        bsync_ip,
        nop_ip,
    })
}

/// If `head` ends with a conditional branch on a predicate, returns the
/// predicate under which the fall-through side runs and the branch target.
fn conditional_branch(func: &Function, head: usize) -> Option<(Pred, Label)> {
    let branch = func.blocks[head].instrs.last()?;
    let Op::Bra(bra) = &branch.op else {
        return None;
    };
    if !bra.cond.is_true() {
        return None;
    }
    match branch.pred.pred_ref {
        PredRef::SSA(ssa) if ssa.file() == RegFile::Pred => {
            Some((branch.pred.bnot(), bra.target))
        }
        _ => None,
    }
}

/// If `block` ends with an unconditional branch, returns its target.
fn jump_target(block: &BasicBlock) -> Option<Label> {
    let branch = block.branch()?;
    match &branch.op {
        Op::Bra(bra) if branch.pred.is_true() && bra.cond.is_true() => {
            Some(bra.target)
        }
        _ => None,
    }
}

/// Returns the block that `block` continues to: its unconditional branch
/// target or, without a branch, the next block.
fn continuation(func: &Function, block: usize) -> Option<usize> {
    match func.blocks[block].branch() {
        None if block + 1 < func.blocks.len() => Some(block + 1),
        None => None,
        Some(_) => block_index(func, jump_target(&func.blocks[block])?),
    }
}

fn block_index(func: &Function, label: Label) -> Option<usize> {
    (0..func.blocks.len()).find(|&i| func.blocks[i].label == label)
}

fn phi_srcs(block: &BasicBlock) -> FxHashMap<Phi, Src> {
    block
        .phi_srcs()
        .map(|phis| {
            phis.srcs.iter().map(|(phi, src)| (*phi, src.clone())).collect()
        })
        .unwrap_or_default()
}

/// Builds `dst = cond ? taken : not_taken`.
fn select(dst: Dst, cond: Pred, taken: Src, not_taken: Src) -> Instr {
    if taken == not_taken {
        return Instr::new(OpCopy {
            dst: dst,
            src: taken,
        });
    }
    if dst.as_ssa().unwrap().file() == RegFile::Pred {
        Instr::new(OpPLop3 {
            dsts: [dst, Dst::None],
            srcs: [cond.into(), taken, not_taken],
            ops: [
                LogicOp3::new_lut(&|c, x, y| (c & x) | (!c & y)),
                LogicOp3::new_const(false),
            ],
        })
    } else {
        Instr::new(OpSel {
            dst: dst,
            cond: cond.into(),
            srcs: [taken, not_taken],
        })
    }
}

/// Appends the body of `block` to `out`, predicated on `cond`.
fn append_predicated(out: &mut Vec<Instr>, block: &mut BasicBlock, cond: Pred) {
    for mut instr in std::mem::take(&mut block.instrs) {
        if matches!(instr.op, Op::PhiSrcs(_)) || instr.is_branch() {
            continue;
        }
        match classify(&instr) {
            Some(Conversion::LoadPredicate) => {
                let Op::Ld(ld) = &mut instr.op else {
                    unreachable!();
                };
                ld.pred = cond.into();
            }
            Some(Conversion::InstrPredicate) => instr.pred = cond,
            Some(Conversion::Unconditional) | None => (),
        }
        out.push(instr);
    }
}

/// The blocks of one convertible `if`.
struct IfShape {
    then_block: usize,
    /// The else block, if the head does not branch straight to the join.
    else_block: Option<usize>,
    join: usize,
    /// The predicate under which the then block runs.
    cond: Pred,
}

fn match_if(func: &Function, head: usize) -> Option<IfShape> {
    let (cond, target_label) = conditional_branch(func, head)?;
    let then_block = head + 1;
    if then_block >= func.blocks.len()
        || func.blocks.pred_indices(then_block) != [head]
    {
        return None;
    }
    let join = continuation(func, then_block)?;
    let target = block_index(func, target_label)?;
    let else_block = if target == join {
        None
    } else {
        if target == then_block
            || func.blocks.pred_indices(target) != [head]
            || continuation(func, target) != Some(join)
        {
            return None;
        }
        Some(target)
    };

    let skipped = else_block.unwrap_or(head);
    let mut join_preds = func.blocks.pred_indices(join).to_vec();
    join_preds.sort();
    let mut expected_preds = vec![then_block, skipped];
    expected_preds.sort();
    if join_preds != expected_preds {
        return None;
    }

    let mut count = count_body(&func.blocks[then_block])?;
    if let Some(else_block) = else_block {
        count += count_body(&func.blocks[else_block])?;
    }
    if count > MAX_PREDICATED_INSTRS {
        return None;
    }
    Some(IfShape {
        then_block,
        else_block,
        join,
        cond,
    })
}

/// Builds the selects that replace the join's phis, or returns None if a
/// phi is not a GPR or predicate.
fn build_selects(func: &Function, head: usize, shape: &IfShape) -> Option<Vec<Instr>> {
    let skipped = shape.else_block.unwrap_or(head);
    let then_srcs = phi_srcs(&func.blocks[shape.then_block]);
    let skipped_srcs = phi_srcs(&func.blocks[skipped]);
    let mut selects = Vec::new();
    if let Some(phis) = func.blocks[shape.join].phi_dsts() {
        for (phi, dst) in phis.dsts.iter() {
            if !matches!(dst.as_ssa()?.file(), RegFile::GPR | RegFile::Pred) {
                return None;
            }
            let taken = then_srcs.get(phi)?;
            let not_taken = skipped_srcs.get(phi)?;
            selects.push(select(
                dst.clone(),
                shape.cond,
                taken.clone(),
                not_taken.clone(),
            ));
        }
    }
    Some(selects)
}

/// Whether `block` holds only `exit`, apart from the reconvergence
/// instructions at `skip`.
fn only_exits(block: &BasicBlock, skip: &[usize]) -> bool {
    let mut rest = block
        .instrs
        .iter()
        .enumerate()
        .filter(|(ip, instr)| {
            !skip.contains(ip) && !matches!(instr.op, Op::Annotate(_))
        })
        .map(|(_, instr)| instr);
    let is_exit = |instr: &Instr| {
        matches!(instr.op, Op::Exit(_)) && instr.pred.is_true()
    };
    rest.next().is_some_and(is_exit) && rest.next().is_none()
}

/// Turns the branch over an `if` without an `else`, after which the shader
/// only exits, into a predicated exit.
fn try_exit_early(func: &mut Function, head: usize) -> bool {
    let Some((_, target_label)) = conditional_branch(func, head) else {
        return false;
    };
    let then_block = head + 1;
    if then_block >= func.blocks.len()
        || func.blocks.pred_indices(then_block) != [head]
    {
        return false;
    }
    let Some(join) = continuation(func, then_block) else {
        return false;
    };
    let Some(target) = block_index(func, target_label) else {
        return false;
    };
    let else_block = if target == join {
        None
    } else if target != then_block
        && func.blocks.pred_indices(target) == [head]
        && continuation(func, target) == Some(join)
        && count_body(&func.blocks[target]) == Some(0)
    {
        Some(target)
    } else {
        return false;
    };
    let mut join_preds = func.blocks.pred_indices(join).to_vec();
    join_preds.sort();
    let mut expected_preds = vec![then_block, else_block.unwrap_or(head)];
    expected_preds.sort();
    if join_preds != expected_preds || func.blocks[join].phi_dsts().is_some() {
        return false;
    }
    let reconvergence = find_reconvergence(func, head, join);
    let join_skip: Vec<usize> = reconvergence
        .as_ref()
        .map(|r| vec![r.bsync_ip, r.nop_ip])
        .unwrap_or_default();
    if !only_exits(&func.blocks[join], &join_skip) {
        return false;
    }

    // The threads that took the branch over the body exit instead.
    let head_instrs = &mut func.blocks[head].instrs;
    let branch = head_instrs.pop().unwrap();
    if let Some(r) = &reconvergence {
        head_instrs.remove(r.bclear_ip.max(r.bssy_ip));
        head_instrs.remove(r.bclear_ip.min(r.bssy_ip));
    }
    let mut exit = Instr::new(OpExit {});
    exit.pred = branch.pred;
    head_instrs.push(exit);

    let join_instrs = &mut func.blocks[join].instrs;
    for ip in join_skip.iter().rev() {
        join_instrs.remove(*ip);
    }
    if let Some(else_block) = else_block {
        func.blocks[else_block].instrs.clear();
    }
    true
}

/// Converts the `if` whose head is block `head`, if it has the shape and
/// bodies described above.
fn try_convert(func: &mut Function, head: usize) -> bool {
    let Some(shape) = match_if(func, head) else {
        return false;
    };
    // An if that nothing after it needs to reconverge from has no barrier.
    // One that cannot be matched is left in place: without the branch, no
    // thread diverges and its bsync completes at once.
    let reconvergence = find_reconvergence(func, head, shape.join);
    let Some(mut selects) = build_selects(func, head, &shape) else {
        return false;
    };

    // The head keeps its own instructions, without the reconvergence, phi
    // sources and branch, then the predicated bodies and the selects.
    let mut instrs = std::mem::take(&mut func.blocks[head].instrs);
    instrs.pop();
    let mut instrs: Vec<Instr> = instrs
        .into_iter()
        .enumerate()
        .filter(|(ip, instr)| {
            let is_reconvergence = reconvergence
                .as_ref()
                .is_some_and(|r| *ip == r.bclear_ip || *ip == r.bssy_ip);
            !is_reconvergence && !matches!(instr.op, Op::PhiSrcs(_))
        })
        .map(|(_, instr)| instr)
        .collect();
    append_predicated(&mut instrs, &mut func.blocks[shape.then_block], shape.cond);
    if let Some(else_block) = shape.else_block {
        append_predicated(
            &mut instrs,
            &mut func.blocks[else_block],
            shape.cond.bnot(),
        );
    }
    instrs.append(&mut selects);

    // The join loses its phis and reconvergence and now has only the head
    // as predecessor. Merge it into the head, unless its uniform
    // instructions cannot move to a non-uniform head or it ends with a
    // conditional branch that must fall through to the next block.
    let join = shape.join;
    let join_instrs: Vec<Instr> = std::mem::take(&mut func.blocks[join].instrs)
        .into_iter()
        .enumerate()
        .filter(|(ip, instr)| {
            let is_reconvergence = reconvergence
                .as_ref()
                .is_some_and(|r| *ip == r.bsync_ip || *ip == r.nop_ip);
            !is_reconvergence && !matches!(instr.op, Op::PhiDsts(_))
        })
        .map(|(_, instr)| instr)
        .collect();
    let join_branch_is_conditional = join_instrs
        .last()
        .is_some_and(|instr| instr.is_branch() && !instr.is_branch_always_taken());
    let uniform_ok = func.blocks[head].uniform || !func.blocks[join].uniform;
    if uniform_ok && !join_branch_is_conditional {
        let falls_through = !join_instrs.last().is_some_and(|i| i.is_branch());
        instrs.extend(join_instrs);
        if falls_through && join + 1 < func.blocks.len() {
            instrs.push(Instr::new(OpBra {
                target: func.blocks[join + 1].label,
                cond: true.into(),
            }));
        }
    } else {
        func.blocks[join].instrs = join_instrs;
        instrs.push(Instr::new(OpBra {
            target: func.blocks[join].label,
            cond: true.into(),
        }));
    }
    func.blocks[head].instrs = instrs;
    true
}

impl Function {
    pub fn opt_if_convert(&mut self, exit_early: bool) {
        // Each conversion changes the CFG, which the matching reads, so
        // rebuild it and start over after each one. Converting an inner if
        // can make an enclosing one convertible.
        let mut progress = false;
        loop {
            // Predicating a short body saves more than an early exit.
            let converted = (0..self.blocks.len()).any(|head| {
                try_convert(self, head)
                    || (exit_early && try_exit_early(self, head))
            });
            if !converted {
                break;
            }
            rewrite_cfg(self);
            progress = true;
        }
        // A head whose join could not be merged jumps to it, and the join
        // now usually follows the head.
        if progress {
            opt_fall_through(self);
        }
    }
}

impl Shader<'_> {
    pub fn opt_if_convert(&mut self) {
        if self.sm.sm() < 70 {
            return;
        }
        let exit_early = matches!(self.info.stage, ShaderStageInfo::Compute(_));
        for f in &mut self.functions {
            f.opt_if_convert(exit_early);
        }
    }
}
