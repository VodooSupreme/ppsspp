// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.
#include "../../HLE/HLE.h"

#include "../MIPS.h"
#include "../MIPSCodeUtils.h"
#include "../MIPSAnalyst.h"
#include "../MIPSTables.h"

#include "ArmJit.h"
#include "ArmRegCache.h"
#include "ArmJitCache.h"
#include <ArmEmitter.h>

#define _RS ((op>>21) & 0x1F)
#define _RT ((op>>16) & 0x1F)
#define _RD ((op>>11) & 0x1F)
#define _FS ((op>>11) & 0x1F)
#define _FT ((op>>16) & 0x1F)
#define _FD ((op>>6 ) & 0x1F)
#define _POS  ((op>>6 ) & 0x1F)
#define _SIZE ((op>>11 ) & 0x1F)

#define LOOPOPTIMIZATION 0

using namespace MIPSAnalyst;

namespace MIPSComp
{

void Jit::BranchRSRTComp(u32 op, ArmGen::CCFlags cc, bool likely)
{
	if (js.inDelaySlot) {
		ERROR_LOG(JIT, "Branch in delay slot at %08x", js.compilerPC);
		return;
	}
	int offset = (signed short)(op&0xFFFF)<<2;
	int rt = _RT;
	int rs = _RS;
	u32 targetAddr = js.compilerPC + offset + 4;
		
	u32 delaySlotOp = Memory::ReadUnchecked_U32(js.compilerPC+4);

	//Compile the delay slot
	bool delaySlotIsNice = GetOutReg(delaySlotOp) != rt && GetOutReg(delaySlotOp) != rs;
  if (!delaySlotIsNice)
  {
    //ERROR_LOG(CPU, "Not nice delay slot in BranchRSRTComp :( %08x", js.compilerPC);
  }
  // The delay slot being nice doesn't really matter though...
	
	if (rt == 0)
  {
		gpr.MapReg(rs, MAP_INITVAL);
		CMP(gpr.R(rs), Operand2(0, TYPE_IMM));
  }
	else if (rs == 0 && (cc == CC_EQ || cc == CC_NEQ))  // only these are easily 'flippable'
	{
		gpr.MapReg(rt, MAP_INITVAL);
		CMP(gpr.R(rt), Operand2(0));
	}
	else 
	{
		gpr.SpillLock(rs, rt);
		gpr.MapReg(rs, MAP_INITVAL);
		gpr.MapReg(rt, MAP_INITVAL);
		gpr.ReleaseSpillLocks();
		CMP(gpr.R(rs), gpr.R(rt));
  }
  FlushAll();

  js.inDelaySlot = true;
  ArmGen::FixupBranch ptr;
  if (!likely)
  {
		// preserve flag around the delay slot! Maybe this is not always necessary on ARM where 
		// we can (mostly) control whether we set the flag or not. Of course, if someone puts an slt in to the
		// delay slot, we're screwed.
		MRS(R8);  // Save flags register. R8 is preserved through function calls and is not allocated.
    CompileAt(js.compilerPC + 4);
    FlushAll();
		_MSR(true, false, R8);  // Restore flags register

    ptr = B_CC(cc);
  }
  else
  {
    ptr = B_CC(cc);
    CompileAt(js.compilerPC + 4);
    FlushAll();
  }
  js.inDelaySlot = false;

  // Take the branch
  WriteExit(targetAddr, 0);

  SetJumpTarget(ptr);
  // Not taken
  WriteExit(js.compilerPC+8, 1);

  js.compiling = false;
}


void Jit::BranchRSZeroComp(u32 op, ArmGen::CCFlags cc, bool likely)
{
	if (js.inDelaySlot) {
		ERROR_LOG(JIT, "Branch in delay slot at %08x", js.compilerPC);
		return;
	}
	int offset = (signed short)(op&0xFFFF)<<2;
	int rs = _RS;
	u32 targetAddr = js.compilerPC + offset + 4;

	u32 delaySlotOp = Memory::ReadUnchecked_U32(js.compilerPC + 4);

	bool delaySlotIsNice = GetOutReg(delaySlotOp) != rs;
  if (!delaySlotIsNice)
  {
    // ERROR_LOG(CPU, "Not nice delay slot in BranchRSZeroComp :( %08x", js.compilerPC);
  }
	gpr.MapReg(rs, MAP_INITVAL);
  CMP(gpr.R(rs), Operand2(0, TYPE_IMM));
  FlushAll();

	ArmGen::FixupBranch ptr;
	js.inDelaySlot = true;
	if (!likely)
  {
		// preserve flag around the delay slot! Maybe this is not always necessary on ARM where 
		// we can (mostly) control whether we set the flag or not. Of course, if someone puts an slt in to the
		// delay slot, we're screwed.
		MRS(R8);  // Save flags register
    CompileAt(js.compilerPC + 4);
    FlushAll();
		_MSR(true, false, R8);  // Restore flags register
    ptr = B_CC(cc);
  }
  else
  {
    ptr = B_CC(cc);
    CompileAt(js.compilerPC + 4);
    FlushAll();
  }
  js.inDelaySlot = false;

  // Take the branch
  WriteExit(targetAddr, 0);

  SetJumpTarget(ptr);
  // Not taken
  WriteExit(js.compilerPC + 8, 1);
  js.compiling = false;
}


void Jit::Comp_RelBranch(u32 op)
{
	// The CC flags here should be opposite of the actual branch becuase they skip the branching action.
	switch (op>>26) 
	{
	case 4: BranchRSRTComp(op, CC_NEQ, false); break;//beq
	case 5: BranchRSRTComp(op, CC_EQ,  false); break;//bne

	case 6: BranchRSZeroComp(op, CC_GT, false); break;//blez
	case 7: BranchRSZeroComp(op, CC_LE, false); break;//bgtz

	case 20: BranchRSRTComp(op, CC_NEQ, true); break;//beql
	case 21: BranchRSRTComp(op, CC_EQ,  true); break;//bnel

	case 22: BranchRSZeroComp(op, CC_GT, true); break;//blezl
	case 23: BranchRSZeroComp(op, CC_LE, true); break;//bgtzl

	default:
		_dbg_assert_msg_(CPU,0,"Trying to compile instruction that can't be compiled");
		break;
	}
  js.compiling = false;
}

void Jit::Comp_RelBranchRI(u32 op)
{
	switch ((op >> 16) & 0x1F)
	{
	case 0: BranchRSZeroComp(op, CC_GE, false); break; //if ((s32)R(rs) <  0) DelayBranchTo(addr); else PC += 4; break;//bltz
	case 1: BranchRSZeroComp(op, CC_LT, false); break; //if ((s32)R(rs) >= 0) DelayBranchTo(addr); else PC += 4; break;//bgez
	case 2: BranchRSZeroComp(op, CC_GE, true);  break; //if ((s32)R(rs) <  0) DelayBranchTo(addr); else PC += 8; break;//bltzl
	case 3: BranchRSZeroComp(op, CC_LT, true);  break; //if ((s32)R(rs) >= 0) DelayBranchTo(addr); else PC += 8; break;//bgezl
	default:
		_dbg_assert_msg_(CPU,0,"Trying to compile instruction that can't be compiled");
		break;
	}
  js.compiling = false;
}

// If likely is set, discard the branch slot if NOT taken.
void Jit::BranchFPFlag(u32 op, ArmGen::CCFlags cc, bool likely)
{
	if (js.inDelaySlot) {
		ERROR_LOG(JIT, "Branch in delay slot at %08x", js.compilerPC);
		return;
	}
  int offset = (signed short)(op & 0xFFFF) << 2;
  u32 targetAddr = js.compilerPC + offset + 4;

  u32 delaySlotOp = Memory::ReadUnchecked_U32(js.compilerPC + 4);

  bool delaySlotIsNice = IsDelaySlotNice(op, delaySlotOp);
  if (!delaySlotIsNice)
  {
    //ERROR_LOG(CPU, "Not nice delay slot in BranchFPFlag :(");
  }
  FlushAll();

	LDR(R0, R10, offsetof(MIPSState, fpcond));
  TST(R0, Operand2(1, TYPE_IMM));
  ArmGen::FixupBranch ptr;
  js.inDelaySlot = true;
  if (!likely)
  {
		MRS(R8);  // Save flags register

    CompileAt(js.compilerPC + 4);
    FlushAll();

		_MSR(true, false, R8);  // Restore flags register
    ptr = B_CC(cc);
  }
  else
  {
    ptr = B_CC(cc);
    CompileAt(js.compilerPC + 4);
    FlushAll();
  }
  js.inDelaySlot = false;

  // Take the branch
  WriteExit(targetAddr, 0);

  SetJumpTarget(ptr);
  // Not taken
  WriteExit(js.compilerPC + 8, 1);
  js.compiling = false;
}

void Jit::Comp_FPUBranch(u32 op)
{
	switch((op >> 16) & 0x1f)
	{
	case 0:	BranchFPFlag(op, CC_NEQ, false); break;  // bc1f
	case 1: BranchFPFlag(op, CC_EQ,  false); break;  // bc1t
	case 2: BranchFPFlag(op, CC_NEQ, true);  break;  // bc1fl
	case 3: BranchFPFlag(op, CC_EQ,  true);  break;  // bc1tl
	default:
		_dbg_assert_msg_(CPU,0,"Trying to interpret instruction that can't be interpreted");
		break;
	}
  js.compiling = false;
}

// If likely is set, discard the branch slot if NOT taken.
void Jit::BranchVFPUFlag(u32 op, ArmGen::CCFlags cc, bool likely)
{
	int offset = (signed short)(op & 0xFFFF) << 2;
	u32 targetAddr = js.compilerPC + offset + 4;

	u32 delaySlotOp = Memory::ReadUnchecked_U32(js.compilerPC + 4);

	bool delaySlotIsNice = IsDelaySlotNice(op, delaySlotOp);
	if (!delaySlotIsNice)
	{
		//ERROR_LOG(CPU, "Not nice delay slot in BranchFPFlag :(");
	}
	FlushAll();

	ARMABI_MOVI2R(R0, (u32)&(mips_->vfpuCtrl[VFPU_CTRL_CC]));
	LDR(R0, R0, Operand2(0, TYPE_IMM));

	int imm3 = (op >> 18) & 7;
	TST(R0, Operand2(1 << imm3, TYPE_IMM));

	ArmGen::FixupBranch ptr;
	js.inDelaySlot = true;
	if (!likely)
	{
		MRS(R8);  // Save flags register

		CompileAt(js.compilerPC + 4);
		FlushAll();

		_MSR(true, false, R8);  // Restore flags register
		ptr = B_CC(cc);
	}
	else
	{
		ptr = B_CC(cc);
		CompileAt(js.compilerPC + 4);
		FlushAll();
	}
	js.inDelaySlot = false;

	// Take the branch
	WriteExit(targetAddr, 0);

	SetJumpTarget(ptr);
	// Not taken
	WriteExit(js.compilerPC + 8, 1);
	js.compiling = false;
}

void Jit::Comp_VBranch(u32 op)
{
  switch ((op >> 16) & 3)
  {
	case 0:	BranchVFPUFlag(op, CC_NEQ, false); break;  // bvf
	case 1: BranchVFPUFlag(op, CC_EQ,  false); break;  // bvt
	case 2: BranchVFPUFlag(op, CC_NEQ, true);  break;  // bvfl
	case 3: BranchVFPUFlag(op, CC_EQ,  true);  break;  // bvtl
	}
	js.compiling = false;
}

void PrintAtExit() {
	INFO_LOG(HLE, "at jump");
}

void Jit::Comp_Jump(u32 op)
{
	if (js.inDelaySlot) {
		ERROR_LOG(JIT, "Branch in delay slot at %08x", js.compilerPC);
		return;
	}
	u32 off = ((op & 0x03FFFFFF) << 2);
	u32 targetAddr = (js.compilerPC & 0xF0000000) | off;
	// Delay slot
	CompileAt(js.compilerPC + 4);
	FlushAll();

	switch (op >> 26) 
	{
	case 2: //j
    WriteExit(targetAddr, 0);
    break; 

	case 3: //jal
		ADD(R1, R10, MIPS_REG_RA * 4);  // compute address of RA in ram
		ARMABI_MOVI2R(R0, js.compilerPC + 8);
		STR(R1, R0);
    WriteExit(targetAddr, 0);
		break;

	default:
		_dbg_assert_msg_(CPU,0,"Trying to compile instruction that can't be compiled");
		break;
	}
	js.compiling = false;
}

void Jit::Comp_JumpReg(u32 op)
{
	if (js.inDelaySlot) {
		ERROR_LOG(JIT, "Branch in delay slot at %08x", js.compilerPC);
		return;
	}
	int rs = _RS;

	u32 delaySlotOp = Memory::ReadUnchecked_U32(js.compilerPC + 4);
	bool delaySlotIsNice = GetOutReg(delaySlotOp) != rs;
  // Do what with that information?
	delaySlotIsNice = false;

	gpr.MapReg(rs, MAP_INITVAL);

	if (delaySlotIsNice) {
		CompileAt(js.compilerPC + 4);
		MOV(R8, gpr.R(rs));  // Save the destination address through the delay slot. Could use isNice to avoid when the jit is fully implemented
		FlushAll();
		MovToPC(R8);  // For syscall to be able to return. Could be avoided with some checking.
	} else {
		// Delay slot
		MOV(R8, gpr.R(rs));  // Save the destination address through the delay slot. Could use isNice to avoid when the jit is fully implemented
		MovToPC(R8);  // For syscall to be able to return. Could be avoided with some checking.
		CompileAt(js.compilerPC + 4);
		FlushAll();
		if (!js.compiling) {
			// INFO_LOG(HLE, "Syscall in delay slot!");
			return;
		}
	}

	switch (op & 0x3f) 
	{
	case 8: //jr
		break;
	case 9: //jalr
		ADD(R1, R10, MIPS_REG_RA * 4);  // compute address of RA in ram
		ARMABI_MOVI2R(R0, js.compilerPC + 8);
		STR(R1, R0);
		break;
	default:
		_dbg_assert_msg_(CPU,0,"Trying to compile instruction that can't be compiled");
		break;
	}

  WriteExitDestInR(R8);
	js.compiling = false;
}

	
void Jit::Comp_Syscall(u32 op)
{
	FlushAll();

	ARMABI_CallFunctionC((void *)&CallSyscall, op);

	WriteSyscallExit();
	js.compiling = false;
}

}   // namespace Mipscomp
