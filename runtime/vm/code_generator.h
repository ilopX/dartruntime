// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef VM_CODE_GENERATOR_H_
#define VM_CODE_GENERATOR_H_

#include "vm/globals.h"
#include "vm/runtime_entry.h"

namespace dart {

// Declaration of runtime entries called from stub or generated code.
DECLARE_RUNTIME_ENTRY(AllocateArray);
DECLARE_RUNTIME_ENTRY(AllocateClosure);
DECLARE_RUNTIME_ENTRY(AllocateImplicitInstanceClosure);
DECLARE_RUNTIME_ENTRY(AllocateImplicitStaticClosure);
DECLARE_RUNTIME_ENTRY(AllocateContext);
DECLARE_RUNTIME_ENTRY(AllocateObject);
DECLARE_RUNTIME_ENTRY(AllocateObjectWithBoundsCheck);
DECLARE_RUNTIME_ENTRY(BreakpointStaticHandler);
DECLARE_RUNTIME_ENTRY(BreakpointReturnHandler);
DECLARE_RUNTIME_ENTRY(BreakpointDynamicHandler);
DECLARE_RUNTIME_ENTRY(CloneContext);
DECLARE_RUNTIME_ENTRY(ClosureArgumentMismatch);
DECLARE_RUNTIME_ENTRY(Deoptimize);
DECLARE_RUNTIME_ENTRY(FixCallersTarget);
DECLARE_RUNTIME_ENTRY(InlineCacheMissHandlerOneArg);
DECLARE_RUNTIME_ENTRY(InlineCacheMissHandlerTwoArgs);
DECLARE_RUNTIME_ENTRY(Instanceof);
DECLARE_RUNTIME_ENTRY(InstantiateTypeArguments);
DECLARE_RUNTIME_ENTRY(InvokeImplicitClosureFunction);
DECLARE_RUNTIME_ENTRY(InvokeNoSuchMethodFunction);
DECLARE_RUNTIME_ENTRY(OptimizeInvokedFunction);
DECLARE_RUNTIME_ENTRY(PatchStaticCall);
DECLARE_RUNTIME_ENTRY(ReportObjectNotClosure);
DECLARE_RUNTIME_ENTRY(ResolveCompileInstanceFunction);
DECLARE_RUNTIME_ENTRY(ResolveImplicitClosureFunction);
DECLARE_RUNTIME_ENTRY(ResolveImplicitClosureThroughGetter);
DECLARE_RUNTIME_ENTRY(ReThrow);
DECLARE_RUNTIME_ENTRY(StackOverflow);
DECLARE_RUNTIME_ENTRY(Throw);
DECLARE_RUNTIME_ENTRY(TraceFunctionEntry);
DECLARE_RUNTIME_ENTRY(TraceFunctionExit);

#define DEOPT_REASONS(V) \
  V(DeoptUnknown) \
  V(DeoptIncrLocal) \
  V(DeoptIncrInstance) \
  V(DeoptIncrInstanceOneClass) \
  V(DeoptInstanceGetterSameTarget) \
  V(DeoptInstanceGetter) \
  V(DeoptStoreIndexed) \
  V(DeoptStoreIndexedPolymorphic) \
  V(DeoptPolymorphicInstanceCallSmiOnly) \
  V(DeoptPolymorphicInstanceCallSmiFail) \
  V(DeoptPolymorphicInstanceCallTestFail) \
  V(DeoptIntegerToDouble) \
  V(DeoptDoubleToDouble) \
  V(DeoptSmiBinaryOp) \
  V(DeoptMintBinaryOp) \
  V(DeoptDoubleBinaryOp) \
  V(DeoptInstanceSetterSameTarget) \
  V(DeoptInstanceSetter) \
  V(DeoptSmiEquality) \
  V(DeoptEquality) \
  V(DeoptSmiCompareSmi) \
  V(DeoptSmiCompareAny) \
  V(DeoptDoubleCompareDouble) \
  V(DeoptEqualityNoFeedback) \
  V(DeoptEqualityClassCheck) \
  V(DeoptDoubleComparison) \
  V(DeoptLoadIndexedFixedArray) \
  V(DeoptLoadIndexedGrowableArray) \
  V(DeoptLoadIndexedPolymorphic) \
  V(DeoptNoTypeFeedback) \
  V(DeoptSAR) \
  V(DeoptUnaryOp) \

enum DeoptReasonId {
#define DEFINE_ENUM_LIST(name) k##name,
DEOPT_REASONS(DEFINE_ENUM_LIST)
#undef DEFINE_ENUM_LIST
};

// This class wraps around the array RawClass::functions_cache_.
// The structure of that array is specified by FunctionsCache::Entries.
// The last entry in the array is always NULL, the lookup code can rely on
// last entry being NULL to terminate its search.
// The compiled functions are added sequentially (except for the last entry
// being empty). Names of functions with variable number of arguments
// can appear several times, once for each valid argument count.
class FunctionsCache : public ValueObject {
 public:
  // Entries in the RawClass::functions_cache_. The size of initially allocated
  // functions_cache_ array is (kInitialSize * kNumEntries).
  enum Entries {
    kFunctionName = 0,
    kArgCount = 1,
    kNamedArgCount = 2,
    kFunction = 3,
    kNumEntries = 4
  };

  explicit FunctionsCache(const Class& cls) : class_(cls) {}

  void AddCompiledFunction(const Function& function,
                           int num_arguments,
                           int num_named_arguments);

  // This is a testing function, the lookup (will) occur inlined in stub code.
  RawCode* LookupCode(const String& function_name,
                      int num_arguments,
                      int num_named_arguments);

 private:
  static void EnterFunctionAt(int i,
                              const Array& cache,
                              const Function& function,
                              int num_arguments,
                              int num_named_arguments);

  const Class& class_;
};


RawCode* ResolveCompileInstanceCallTarget(Isolate* isolate,
                                          const Instance& receiver);

}  // namespace dart

#endif  // VM_CODE_GENERATOR_H_
