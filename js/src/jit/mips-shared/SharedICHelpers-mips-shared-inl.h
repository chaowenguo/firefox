/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips_shared_SharedICHelpers_mips_shared_inl_h
#define jit_mips_shared_SharedICHelpers_mips_shared_inl_h

#include "jit/BaselineFrame.h"
#include "jit/SharedICHelpers.h"

#include "jit/MacroAssembler-inl.h"

namespace js {
namespace jit {

inline void EmitBaselineTailCallVM(TrampolinePtr target, MacroAssembler& masm,
                                   uint32_t argSize) {
#ifdef DEBUG
  Register scratch = R2.scratchReg();

  // Compute frame size.
  masm.movePtr(FramePointer, scratch);
  masm.subPtr(StackPointer, scratch);

  // Store frame size without VMFunction arguments for debug assertions.
  masm.subPtr(Imm32(argSize), scratch);
  Address frameSizeAddr(FramePointer,
                        BaselineFrame::reverseOffsetOfDebugFrameSize());
  masm.store32(scratch, frameSizeAddr);
  masm.addPtr(Imm32(argSize), scratch);
#endif

  // Push frame descriptor and perform the tail call.
  masm.push(FrameDescriptor(FrameType::BaselineJS));

  // The return address will be pushed by the VM wrapper, for compatibility
  // with direct calls. Refer to the top of generateVMWrapper().
  // ICTailCallReg (lr) already contains the return address (as we keep
  // it there through the stub calls).
  MOZ_ASSERT(ICTailCallReg == ra);

  masm.jump(target);
}

inline void EmitBaselineCallVM(TrampolinePtr target, MacroAssembler& masm) {
  masm.push(FrameDescriptor(FrameType::BaselineStub));
  masm.call(target);
}

inline void EmitBaselineEnterStubFrame(MacroAssembler& masm, Register scratch) {
  MOZ_ASSERT(scratch != ICTailCallReg);

#ifdef DEBUG
  // Compute frame size.
  masm.movePtr(FramePointer, scratch);
  masm.subPtr(StackPointer, scratch);

  Address frameSizeAddr(FramePointer,
                        BaselineFrame::reverseOffsetOfDebugFrameSize());
  masm.store32(scratch, frameSizeAddr);
#endif

  // Note: when making changes here, don't forget to update
  // BaselineStubFrame if needed.

  // Push frame descriptor and return address.
  masm.Push(FrameDescriptor(FrameType::BaselineJS));
  masm.Push(ICTailCallReg);

  // Save old frame pointer, stack pointer and stub reg.
  masm.Push(FramePointer);
  masm.movePtr(StackPointer, FramePointer);
  masm.Push(ICStubReg);

  // Stack should remain aligned.
  masm.assertStackAlignment(sizeof(Value), 0);
}

}  // namespace jit
}  // namespace js

#endif /* jit_mips_shared_SharedICHelpers_mips_shared_inl_h */
