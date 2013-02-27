// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/globals.h"
#if defined(TARGET_ARCH_ARM)

#include "vm/assembler.h"
#include "vm/assembler_macros.h"
#include "vm/dart_entry.h"
#include "vm/instructions.h"
#include "vm/stack_frame.h"
#include "vm/stub_code.h"

#define __ assembler->

namespace dart {

void StubCode::GenerateCallToRuntimeStub(Assembler* assembler) {
  __ Unimplemented("CallToRuntime stub");
}


void StubCode::GeneratePrintStopMessageStub(Assembler* assembler) {
  __ Unimplemented("PrintStopMessage stub");
}


void StubCode::GenerateCallNativeCFunctionStub(Assembler* assembler) {
  __ Unimplemented("CallNativeCFunction stub");
}


void StubCode::GenerateCallStaticFunctionStub(Assembler* assembler) {
  __ Unimplemented("CallStaticFunction stub");
}


void StubCode::GenerateFixCallersTargetStub(Assembler* assembler) {
  __ Unimplemented("FixCallersTarget stub");
}


void StubCode::GenerateInstanceFunctionLookupStub(Assembler* assembler) {
  __ Unimplemented("InstanceFunctionLookup stub");
}


void StubCode::GenerateDeoptimizeLazyStub(Assembler* assembler) {
  __ Unimplemented("DeoptimizeLazy stub");
}


void StubCode::GenerateDeoptimizeStub(Assembler* assembler) {
  __ Unimplemented("Deoptimize stub");
}


void StubCode::GenerateMegamorphicMissStub(Assembler* assembler) {
  __ Unimplemented("MegamorphicMiss stub");
}


void StubCode::GenerateAllocateArrayStub(Assembler* assembler) {
  __ Unimplemented("AllocateArray stub");
}


void StubCode::GenerateCallClosureFunctionStub(Assembler* assembler) {
  __ Unimplemented("CallClosureFunction stub");
}


// Called when invoking Dart code from C++ (VM code).
// Input parameters:
//   LR : points to return address.
//   R0 : entrypoint of the Dart function to call.
//   R1 : arguments descriptor array.
//   R2 : arguments array.
//   R3 : new context containing the current isolate pointer.
void StubCode::GenerateInvokeDartCodeStub(Assembler* assembler) {
  // Save frame pointer coming in.
  AssemblerMacros::EnterStubFrame(assembler);

  // Save new context and C++ ABI callee-saved registers.
  const intptr_t kNewContextOffset = -(1 + kAbiPreservedRegCount) * kWordSize;
  __ PushList((1 << R3) | kAbiPreservedRegs);

  // The new Context structure contains a pointer to the current Isolate
  // structure. Cache the Context pointer in the CTX register so that it is
  // available in generated code and calls to Isolate::Current() need not be
  // done. The assumption is that this register will never be clobbered by
  // compiled or runtime stub code.

  // Cache the new Context pointer into CTX while executing Dart code.
  __ ldr(CTX, Address(R3, VMHandles::kOffsetOfRawPtrInHandle));

  // Load Isolate pointer from Context structure into temporary register R8.
  __ ldr(R8, FieldAddress(CTX, Context::isolate_offset()));

  // Save the top exit frame info. Use R5 as a temporary register.
  // StackFrameIterator reads the top exit frame info saved in this frame.
  {
    // TODO(regis): Add assembler macro for {Load,Store}BasedOffset with no
    // restriction on the offset.
    const intptr_t offset = Isolate::top_exit_frame_info_offset();
    const int32_t offset12_hi = offset & ~kOffset12Mask;  // signed
    const uint32_t offset12_lo = offset & kOffset12Mask;  // unsigned
    __ AddImmediate(R7, R8, offset12_hi);
    __ ldr(R5, Address(R7, offset12_lo));
    __ LoadImmediate(R6, 0);
    __ str(R6, Address(R7, offset12_lo));
  }

  // Save the old Context pointer. Use R4 as a temporary register.
  // Note that VisitObjectPointers will find this saved Context pointer during
  // GC marking, since it traverses any information between SP and
  // FP - kExitLinkOffsetInEntryFrame.
  // EntryFrame::SavedContext reads the context saved in this frame.
  {
    const intptr_t offset = Isolate::top_context_offset();
    const int32_t offset12_hi = offset & ~kOffset12Mask;  // signed
    const uint32_t offset12_lo = offset & kOffset12Mask;  // unsigned
    __ AddImmediate(R7, R8, offset12_hi);
    __ ldr(R4, Address(R7, offset12_lo));
  }

  // The constants kSavedContextOffsetInEntryFrame and
  // kExitLinkOffsetInEntryFrame must be kept in sync with the code below.
  __ PushList((1 << R4) | (1 << R5));

  // The stack pointer is restore after the call to this location.
  const intptr_t kSavedContextOffsetInEntryFrame = -10 * kWordSize;

  // Load arguments descriptor array into R4, which is passed to Dart code.
  __ ldr(R4, Address(R1, VMHandles::kOffsetOfRawPtrInHandle));

  // Load number of arguments into R5.
  __ ldr(R5, FieldAddress(R4, ArgumentsDescriptor::count_offset()));
  __ SmiUntag(R5);

  // Compute address of 'arguments array' data area into R2.
  __ ldr(R2, Address(R2, VMHandles::kOffsetOfRawPtrInHandle));
  __ AddImmediate(R2, R2, Array::data_offset() - kHeapObjectTag);

  // Set up arguments for the Dart call.
  Label push_arguments;
  Label done_push_arguments;
  __ CompareImmediate(R5, 0);  // check if there are arguments.
  __ b(&done_push_arguments, EQ);
  __ LoadImmediate(R1, 0);
  __ Bind(&push_arguments);
  __ ldr(R3, Address(R2));
  __ Push(R3);
  __ AddImmediate(R2, kWordSize);
  __ AddImmediate(R1, 1);
  __ cmp(R1, ShifterOperand(R5));
  __ b(&push_arguments, LT);
  __ Bind(&done_push_arguments);

  // Call the Dart code entrypoint.
  __ blx(R0);  // R4 is the arguments descriptor array.

  // Read the saved new Context pointer.
  __ ldr(CTX, Address(FP, kNewContextOffset));
  __ ldr(CTX, Address(CTX, VMHandles::kOffsetOfRawPtrInHandle));

  // Get rid of arguments pushed on the stack.
  __ AddImmediate(SP, FP, kSavedContextOffsetInEntryFrame);

  // Load Isolate pointer from Context structure into CTX. Drop Context.
  __ ldr(CTX, FieldAddress(CTX, Context::isolate_offset()));

  // Restore the saved Context pointer into the Isolate structure.
  // Uses R4 as a temporary register for this.
  // Restore the saved top exit frame info back into the Isolate structure.
  // Uses R5 as a temporary register for this.
  __ PopList((1 << R4) | (1 << R5));
  {
    const intptr_t offset = Isolate::top_context_offset();
    const int32_t offset12_hi = offset & ~kOffset12Mask;  // signed
    const uint32_t offset12_lo = offset & kOffset12Mask;  // unsigned
    __ AddImmediate(R7, CTX, offset12_hi);
    __ str(R4, Address(R7, offset12_lo));
  }
  {
    const intptr_t offset = Isolate::top_exit_frame_info_offset();
    const int32_t offset12_hi = offset & ~kOffset12Mask;  // signed
    const uint32_t offset12_lo = offset & kOffset12Mask;  // unsigned
    __ AddImmediate(R7, CTX, offset12_hi);
    __ str(R5, Address(R7, offset12_lo));
  }

  // Restore C++ ABI callee-saved registers.
  __ PopList((1 << R3) | kAbiPreservedRegs);  // Ignore restored R3.

  // Restore the frame pointer.
  AssemblerMacros::LeaveStubFrame(assembler);

  __ Ret();
}


void StubCode::GenerateAllocateContextStub(Assembler* assembler) {
  __ Unimplemented("AllocateContext stub");
}


void StubCode::GenerateUpdateStoreBufferStub(Assembler* assembler) {
  __ Unimplemented("UpdateStoreBuffer stub");
}


void StubCode::GenerateAllocationStubForClass(Assembler* assembler,
                                              const Class& cls) {
  __ Unimplemented("AllocateObject stub");
}


void StubCode::GenerateAllocationStubForClosure(Assembler* assembler,
                                                const Function& func) {
  __ Unimplemented("AllocateClosure stub");
}


void StubCode::GenerateCallNoSuchMethodFunctionStub(Assembler* assembler) {
  __ Unimplemented("CallNoSuchMethodFunction stub");
}


void StubCode::GenerateOptimizedUsageCounterIncrement(Assembler* assembler) {
  __ Unimplemented("OptimizedUsageCounterIncrement stub");
}


void StubCode::GenerateUsageCounterIncrement(Assembler* assembler,
                                             Register temp_reg) {
  __ Unimplemented("UsageCounterIncrement stub");
}


void StubCode::GenerateNArgsCheckInlineCacheStub(Assembler* assembler,
                                                 intptr_t num_args) {
  __ Unimplemented("NArgsCheckInlineCache stub");
}


void StubCode::GenerateOneArgCheckInlineCacheStub(Assembler* assembler) {
  __ Unimplemented("GenerateOneArgCheckInlineCacheStub stub");
}


void StubCode::GenerateTwoArgsCheckInlineCacheStub(Assembler* assembler) {
  __ Unimplemented("GenerateTwoArgsCheckInlineCacheStub stub");
}


void StubCode::GenerateThreeArgsCheckInlineCacheStub(Assembler* assembler) {
  __ Unimplemented("GenerateThreeArgsCheckInlineCacheStub stub");
}


void StubCode::GenerateOneArgOptimizedCheckInlineCacheStub(
    Assembler* assembler) {
  GenerateOptimizedUsageCounterIncrement(assembler);
  GenerateNArgsCheckInlineCacheStub(assembler, 1);
}


void StubCode::GenerateTwoArgsOptimizedCheckInlineCacheStub(
    Assembler* assembler) {
  GenerateOptimizedUsageCounterIncrement(assembler);
  GenerateNArgsCheckInlineCacheStub(assembler, 2);
}


void StubCode::GenerateThreeArgsOptimizedCheckInlineCacheStub(
    Assembler* assembler) {
  GenerateOptimizedUsageCounterIncrement(assembler);
  GenerateNArgsCheckInlineCacheStub(assembler, 3);
}


void StubCode::GenerateClosureCallInlineCacheStub(Assembler* assembler) {
  GenerateNArgsCheckInlineCacheStub(assembler, 1);
}


void StubCode::GenerateMegamorphicCallStub(Assembler* assembler) {
  GenerateNArgsCheckInlineCacheStub(assembler, 1);
}


void StubCode::GenerateBreakpointStaticStub(Assembler* assembler) {
  __ Unimplemented("BreakpointStatic stub");
}


void StubCode::GenerateBreakpointReturnStub(Assembler* assembler) {
  __ Unimplemented("BreakpointReturn stub");
}


void StubCode::GenerateBreakpointDynamicStub(Assembler* assembler) {
  __ Unimplemented("BreakpointDynamic stub");
}


void StubCode::GenerateSubtype1TestCacheStub(Assembler* assembler) {
  __ Unimplemented("Subtype1TestCache Stub");
}


void StubCode::GenerateSubtype2TestCacheStub(Assembler* assembler) {
  __ Unimplemented("Subtype2TestCache Stub");
}


void StubCode::GenerateSubtype3TestCacheStub(Assembler* assembler) {
  __ Unimplemented("Subtype3TestCache Stub");
}


// Return the current stack pointer address, used to stack alignment
// checks.
void StubCode::GenerateGetStackPointerStub(Assembler* assembler) {
  __ Unimplemented("GetStackPointer Stub");
}


// Jump to the exception handler.
// No Result.
void StubCode::GenerateJumpToExceptionHandlerStub(Assembler* assembler) {
  __ Unimplemented("JumpToExceptionHandler Stub");
}


// Jump to the error handler.
// No Result.
void StubCode::GenerateJumpToErrorHandlerStub(Assembler* assembler) {
  __ Unimplemented("JumpToErrorHandler Stub");
}


void StubCode::GenerateEqualityWithNullArgStub(Assembler* assembler) {
  __ Unimplemented("EqualityWithNullArg stub");
}


void StubCode::GenerateOptimizeFunctionStub(Assembler* assembler) {
  __ Unimplemented("OptimizeFunction stub");
}


void StubCode::GenerateIdenticalWithNumberCheckStub(Assembler* assembler) {
  __ Unimplemented("IdenticalWithNumberCheck stub");
}

}  // namespace dart

#endif  // defined TARGET_ARCH_ARM
