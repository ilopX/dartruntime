// Copyright (c) 2011, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/globals.h"
#if defined(TARGET_ARCH_X64)

#include "vm/isolate.h"
#include "vm/dart_entry.h"
#include "vm/native_entry.h"
#include "vm/native_entry_test.h"
#include "vm/object.h"
#include "vm/runtime_entry.h"
#include "vm/stub_code.h"
#include "vm/symbols.h"
#include "vm/unit_test.h"

#define __ assembler->

namespace dart {

DECLARE_RUNTIME_ENTRY(TestSmiSub);
DECLARE_LEAF_RUNTIME_ENTRY(RawObject*, TestLeafSmiAdd, RawObject*, RawObject*);


static Function* CreateFunction(const char* name) {
  const String& function_name = String::ZoneHandle(Symbols::New(name));
  Function& function = Function::ZoneHandle(
      Function::New(function_name, RawFunction::kRegularFunction,
                    true, false, false, 0));
  return &function;
}


// Test calls to stub code which calls into the runtime.
static void GenerateCallToCallRuntimeStub(Assembler* assembler,
                                          int value1, int value2) {
  const int argc = 2;
  const Smi& smi1 = Smi::ZoneHandle(Smi::New(value1));
  const Smi& smi2 = Smi::ZoneHandle(Smi::New(value2));
  const Object& result = Object::ZoneHandle();
  const Context& context = Context::ZoneHandle(Context::New(0, Heap::kOld));
  ASSERT(context.isolate() == Isolate::Current());
  __ enter(Immediate(0));
  __ LoadObject(CTX, context);
  __ PushObject(result);  // Push Null object for return value.
  __ PushObject(smi1);  // Push argument 1 smi1.
  __ PushObject(smi2);  // Push argument 2 smi2.
  ASSERT(kTestSmiSubRuntimeEntry.argument_count() == argc);
  __ CallRuntime(kTestSmiSubRuntimeEntry);  // Call SmiSub runtime func.
  __ AddImmediate(RSP, Immediate(argc * kWordSize));
  __ popq(RAX);  // Pop return value from return slot.
  __ leave();
  __ ret();
}


TEST_CASE(CallRuntimeStubCode) {
  extern const Function& RegisterFakeFunction(const char* name,
                                              const Code& code);
  const int value1 = 10;
  const int value2 = 20;
  const char* kName = "Test_CallRuntimeStubCode";
  Assembler _assembler_;
  GenerateCallToCallRuntimeStub(&_assembler_, value1, value2);
  const Code& code = Code::Handle(Code::FinalizeCode(
      *CreateFunction("Test_CallRuntimeStubCode"), &_assembler_));
  const Function& function = RegisterFakeFunction(kName, code);
  GrowableArray<const Object*>  arguments;
  const Array& kNoArgumentNames = Array::Handle();
  Smi& result = Smi::Handle();
  result ^= DartEntry::InvokeStatic(function, arguments, kNoArgumentNames);
  EXPECT_EQ((value1 - value2), result.Value());
}


// Test calls to stub code which calls into the runtime.
static void GenerateCallToCallLeafRuntimeStub(Assembler* assembler,
                                              int value1,
                                              int value2) {
  const Smi& smi1 = Smi::ZoneHandle(Smi::New(value1));
  const Smi& smi2 = Smi::ZoneHandle(Smi::New(value2));
  __ enter(Immediate(0));
  __ ReserveAlignedFrameSpace(0);
  __ LoadObject(RDI, smi1);  // Set up argument 1 smi1.
  __ LoadObject(RSI, smi2);  // Set up argument 2 smi2.
  __ CallRuntime(kTestLeafSmiAddRuntimeEntry);  // Call SmiAdd runtime func.
  __ leave();
  __ ret();  // Return value is in RAX.
}


TEST_CASE(CallLeafRuntimeStubCode) {
  extern const Function& RegisterFakeFunction(const char* name,
                                              const Code& code);
  const int value1 = 10;
  const int value2 = 20;
  const char* kName = "Test_CallLeafRuntimeStubCode";
  Assembler _assembler_;
  GenerateCallToCallLeafRuntimeStub(&_assembler_, value1, value2);
  const Code& code = Code::Handle(Code::FinalizeCode(
      *CreateFunction("Test_CallLeafRuntimeStubCode"), &_assembler_));
  const Function& function = RegisterFakeFunction(kName, code);
  GrowableArray<const Object*>  arguments;
  const Array& kNoArgumentNames = Array::Handle();
  Smi& result = Smi::Handle();
  result ^= DartEntry::InvokeStatic(function, arguments, kNoArgumentNames);
  EXPECT_EQ((value1 + value2), result.Value());
}


// Test calls to stub code which calls a native C function.
static void GenerateCallToCallNativeCFunctionStub(Assembler* assembler,
                                                  int value1, int value2) {
  const int argc = 2;
  const Smi& smi1 = Smi::ZoneHandle(Smi::New(value1));
  const Smi& smi2 = Smi::ZoneHandle(Smi::New(value2));
  const Object& result = Object::ZoneHandle();
  const String& native_name = String::ZoneHandle(String::New("TestSmiSub"));
  Dart_NativeFunction native_function =
      NativeTestEntry_Lookup(native_name, argc);
  ASSERT(native_function != NULL);
  const Context& context = Context::ZoneHandle(Context::New(0, Heap::kOld));
  ASSERT(context.isolate() == Isolate::Current());
  __ enter(Immediate(0));
  __ LoadObject(CTX, context);
  __ PushObject(smi1);  // Push argument 1 smi1.
  __ PushObject(smi2);  // Push argument 2 smi2.
  __ PushObject(result);  // Push Null object for return value.
  // Pass a pointer to the first argument in RAX.
  __ leaq(RAX, Address(RSP, 2 * kWordSize));
  __ movq(RBX, Immediate(reinterpret_cast<uword>(native_function)));
  __ movq(R10, Immediate(argc));
  __ call(&StubCode::CallNativeCFunctionLabel());
  __ popq(RAX);  // Pop return value from return slot.
  __ AddImmediate(RSP, Immediate(argc * kWordSize));
  __ leave();
  __ ret();
}


TEST_CASE(CallNativeCFunctionStubCode) {
  extern const Function& RegisterFakeFunction(const char* name,
                                              const Code& code);
  const int value1 = 15;
  const int value2 = 20;
  const char* kName = "Test_CallNativeCFunctionStubCode";
  Assembler _assembler_;
  GenerateCallToCallNativeCFunctionStub(&_assembler_, value1, value2);
  const Code& code = Code::Handle(Code::FinalizeCode(
      *CreateFunction(kName), &_assembler_));
  const Function& function = RegisterFakeFunction(kName, code);
  GrowableArray<const Object*>  arguments;
  const Array& kNoArgumentNames = Array::Handle();
  Smi& result = Smi::Handle();
  result ^= DartEntry::InvokeStatic(function, arguments, kNoArgumentNames);
  EXPECT_EQ((value1 - value2), result.Value());
}

}  // namespace dart

#endif  // defined TARGET_ARCH_X64
