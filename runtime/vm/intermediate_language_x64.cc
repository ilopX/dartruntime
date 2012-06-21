// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/globals.h"  // Needed here to get TARGET_ARCH_X64.
#if defined(TARGET_ARCH_X64)

#include "vm/intermediate_language.h"

#include "lib/error.h"
#include "vm/flow_graph_compiler.h"
#include "vm/locations.h"
#include "vm/object_store.h"
#include "vm/parser.h"
#include "vm/stub_code.h"

#define __ compiler->assembler()->

namespace dart {

DECLARE_FLAG(int, optimization_counter_threshold);
DECLARE_FLAG(bool, trace_functions);

void BindInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  computation()->EmitNativeCode(compiler);
  if (locs()->out().kind() == Location::kRegister) {
    // TODO(vegorov): this should really happen only for comparisons fused
    // with branches.  Currrently IR does not provide an easy way to remove
    // instructions from the graph so we just leave fused comparison in it
    // but change its result location to be NoLocation.
    compiler->frame_register_allocator()->Push(locs()->out().reg(), this);
  }
}


LocationSummary* ReturnInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 1;
  LocationSummary* locs = new LocationSummary(kNumInputs, kNumTemps);
  locs->set_in(0, Location::RegisterLocation(RAX));
  locs->set_temp(0, Location::RequiresRegister());
  return locs;
}


void ReturnInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register result = locs()->in(0).reg();
  Register temp = locs()->temp(0).reg();
  ASSERT(result == RAX);
  if (!compiler->is_optimizing()) {
    // Count only in unoptimized code.
    // TODO(srdjan): Replace the counting code with a type feedback
    // collection and counting stub.
    const Function& function =
          Function::ZoneHandle(compiler->parsed_function().function().raw());
    __ LoadObject(temp, function);
    __ incq(FieldAddress(temp, Function::usage_counter_offset()));
    if (CodeGenerator::CanOptimize()) {
      // Do not optimize if usage count must be reported.
      __ cmpl(FieldAddress(temp, Function::usage_counter_offset()),
          Immediate(FLAG_optimization_counter_threshold));
      Label not_yet_hot;
      __ j(LESS_EQUAL, &not_yet_hot, Assembler::kNearJump);
      __ pushq(result);  // Preserve result.
      __ pushq(temp);  // Argument for runtime: function to optimize.
      __ CallRuntime(kOptimizeInvokedFunctionRuntimeEntry);
      __ popq(temp);  // Remove argument.
      __ popq(result);  // Restore result.
      __ Bind(&not_yet_hot);
    }
  }
  if (FLAG_trace_functions) {
    __ pushq(result);  // Preserve result.
    const Function& function =
        Function::ZoneHandle(compiler->parsed_function().function().raw());
    __ LoadObject(temp, function);
    __ pushq(temp);
    compiler->GenerateCallRuntime(AstNode::kNoId,
                                  0,
                                  CatchClauseNode::kInvalidTryIndex,
                                  kTraceFunctionExitRuntimeEntry);
    __ popq(temp);  // Remove argument.
    __ popq(result);  // Restore result.
  }
  __ LeaveFrame();
  __ ret();

  // Generate 8 bytes of NOPs so that the debugger can patch the
  // return pattern with a call to the debug stub.
  __ nop(1);
  __ nop(1);
  __ nop(1);
  __ nop(1);
  __ nop(1);
  __ nop(1);
  __ nop(1);
  __ nop(1);
  compiler->AddCurrentDescriptor(PcDescriptors::kReturn,
                                 cid(),
                                 token_index(),
                                 CatchClauseNode::kInvalidTryIndex);
}


// Generic summary for call instructions that have all arguments pushed
// on the stack and return the result in a fixed register RAX.
LocationSummary* Computation::MakeCallSummary() {
  LocationSummary* result = new LocationSummary(0, 0, LocationSummary::kCall);
  result->set_out(Location::RegisterLocation(RAX));
  return result;
}


LocationSummary* ClosureCallComp::MakeLocationSummary() const {
  const intptr_t kNumInputs = 0;
  const intptr_t kNumTemps = 1;
  LocationSummary* result = new LocationSummary(kNumInputs,
                                                kNumTemps,
                                                LocationSummary::kCall);
  result->set_out(Location::RegisterLocation(RAX));
  result->set_temp(0, Location::RegisterLocation(R10));  // Arg. descriptor.
  return result;
}


LocationSummary* LoadLocalComp::MakeLocationSummary() const {
  return LocationSummary::Make(0, Location::RequiresRegister());
}


void LoadLocalComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register result = locs()->out().reg();
  __ movq(result, Address(RBP, local().index() * kWordSize));
}


LocationSummary* StoreLocalComp::MakeLocationSummary() const {
  return LocationSummary::Make(1, Location::SameAsFirstInput());
}


void StoreLocalComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register value = locs()->in(0).reg();
  Register result = locs()->out().reg();
  ASSERT(result == value);  // Assert that register assignment is correct.
  __ movq(Address(RBP, local().index() * kWordSize), value);
}


LocationSummary* ConstantVal::MakeLocationSummary() const {
  return LocationSummary::Make(0, Location::RequiresRegister());
}


void ConstantVal::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register result = locs()->out().reg();
  if (value().IsSmi()) {
    int64_t imm = reinterpret_cast<int64_t>(value().raw());
    __ movq(result, Immediate(imm));
  } else {
    __ LoadObject(result, value());
  }
}


LocationSummary* AssertAssignableComp::MakeLocationSummary() const {
  LocationSummary* summary = new LocationSummary(3, 0);
  summary->set_in(0, Location::RegisterLocation(RAX));  // Value.
  summary->set_in(1, Location::RegisterLocation(RCX));  // Instantiator.
  summary->set_in(2, Location::RegisterLocation(RDX));  // Type arguments.
  summary->set_out(Location::RegisterLocation(RAX));
  return summary;
}


void AssertBooleanComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register obj = locs()->in(0).reg();
  Register result = locs()->out().reg();

  // Check that the type of the value is allowed in conditional context.
  // Call the runtime if the object is not bool::true or bool::false.
  Label done;
  __ CompareObject(obj, compiler->bool_true());
  __ j(EQUAL, &done, Assembler::kNearJump);
  __ CompareObject(obj, compiler->bool_false());
  __ j(EQUAL, &done, Assembler::kNearJump);

  __ pushq(Immediate(Smi::RawValue(token_index())));  // Source location.
  __ pushq(obj);  // Push the source object.
  compiler->GenerateCallRuntime(cid(),
                                token_index(),
                                try_index(),
                                kConditionTypeErrorRuntimeEntry);
  // We should never return here.
  __ int3();

  __ Bind(&done);
  ASSERT(obj == result);
}


LocationSummary* EqualityCompareComp::MakeLocationSummary() const {
  const LocationSummary::ContainsBranch contains_branch =
      is_fused_with_branch() ? LocationSummary::kBranch
                             : LocationSummary::kNoBranch;

  const intptr_t kNumInputs = 2;
  if (HasICData() &&
      (ic_data()->NumberOfChecks() == 1) &&
      (ic_data()->GetReceiverClassIdAt(0) == kSmi)) {
    const intptr_t kNumTemps = 1;
    LocationSummary* locs = new LocationSummary(kNumInputs,
                                                kNumTemps,
                                                LocationSummary::kNoCall,
                                                contains_branch);
    locs->set_in(0, Location::RequiresRegister());
    locs->set_in(1, Location::RequiresRegister());
    locs->set_temp(0, Location::RequiresRegister());
    if (!is_fused_with_branch()) {
      locs->set_out(Location::RequiresRegister());
    }
    return locs;
  }
  if (HasICData() && (ic_data()->NumberOfChecks() > 0)) {
    const intptr_t kNumTemps = 1;
    LocationSummary* locs = new LocationSummary(kNumInputs,
                                                kNumTemps,
                                                LocationSummary::kCall,
                                                contains_branch);
    locs->set_in(0, Location::RequiresRegister());
    locs->set_in(1, Location::RequiresRegister());
    locs->set_temp(0, Location::RequiresRegister());
    if (!is_fused_with_branch()) {
      locs->set_out(Location::RegisterLocation(RAX));
    }
    return locs;
  }
  const intptr_t kNumTemps = 0;
  LocationSummary* locs = new LocationSummary(kNumInputs,
                                              kNumTemps,
                                              LocationSummary::kCall,
                                              contains_branch);
  locs->set_in(0, Location::RequiresRegister());
  locs->set_in(1, Location::RequiresRegister());
  if (!is_fused_with_branch()) {
    locs->set_out(Location::RegisterLocation(RAX));
  }
  return locs;
}


static void EmitSmiEqualityCompare(FlowGraphCompiler* compiler,
                                   EqualityCompareComp* comp) {
  Register left = comp->locs()->in(0).reg();
  Register right = comp->locs()->in(1).reg();
  Register temp = comp->locs()->temp(0).reg();
  Label* deopt = compiler->AddDeoptStub(comp->cid(),
                                        comp->token_index(),
                                        comp->try_index(),
                                        kDeoptSmiCompareSmis,
                                        left,
                                        right);
  // TODO(srdjan): Should we always include NULL test (common case)?
  __ movq(temp, left);
  __ orq(temp, right);
  __ testq(temp, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, deopt);
  __ cmpq(left, right);
  if (comp->is_fused_with_branch()) {
    comp->fused_with_branch()->EmitBranchOnCondition(compiler, EQUAL);
  } else {
    Register result = comp->locs()->out().reg();
    Label load_true, done;
    __ j(EQUAL, &load_true, Assembler::kNearJump);
    __ LoadObject(result, compiler->bool_false());
    __ jmp(&done, Assembler::kNearJump);
    __ Bind(&load_true);
    __ LoadObject(result, compiler->bool_true());
    __ Bind(&done);
  }
}


static void EmitEqualityAsInstanceCall(FlowGraphCompiler* compiler,
                                       EqualityCompareComp* comp) {
  compiler->AddCurrentDescriptor(PcDescriptors::kDeopt,
                                 comp->cid(),
                                 comp->token_index(),
                                 comp->try_index());
  const String& operator_name = String::ZoneHandle(String::NewSymbol("=="));
  const int kNumberOfArguments = 2;
  const Array& kNoArgumentNames = Array::Handle();
  const int kNumArgumentsChecked = 2;

  compiler->GenerateInstanceCall(comp->cid(),
                                 comp->token_index(),
                                 comp->try_index(),
                                 operator_name,
                                 kNumberOfArguments,
                                 kNoArgumentNames,
                                 kNumArgumentsChecked);
  ASSERT(comp->is_fused_with_branch() || (comp->locs()->out().reg() == RAX));

  if (comp->is_fused_with_branch()) {
    __ CompareObject(RAX, compiler->bool_true());
    comp->fused_with_branch()->EmitBranchOnCondition(compiler, EQUAL);
  }
}


static void EmitEqualityAsPolymorphicCall(FlowGraphCompiler* compiler,
                                          EqualityCompareComp* comp,
                                          Register left,
                                          Register right) {
  ASSERT(comp->HasICData());
  const ICData& ic_data = *comp->ic_data();
  ASSERT(ic_data.NumberOfChecks() > 0);
  ASSERT(ic_data.num_args_tested() == 1);
  Label* deopt = compiler->AddDeoptStub(comp->cid(),
                                        comp->token_index(),
                                        comp->try_index(),
                                        kDeoptEquality);
  __ testq(left, Immediate(kSmiTagMask));
  Register temp = comp->locs()->temp(0).reg();
  if (ic_data.GetReceiverClassIdAt(0) == kSmi) {
    Label done, load_class_id;
    __ j(NOT_ZERO, &load_class_id, Assembler::kNearJump);
    __ movq(temp, Immediate(kSmi));
    __ jmp(&done, Assembler::kNearJump);
    __ Bind(&load_class_id);
    __ LoadClassId(temp, left);
    __ Bind(&done);
  } else {
    __ j(ZERO, deopt);  // Smi deopts.
    __ LoadClassId(temp, left);
  }
  Label done;
  for (intptr_t i = 0; i < ic_data.NumberOfChecks(); i++) {
    ASSERT((ic_data.GetReceiverClassIdAt(i) != kSmi) || (i == 0));
    Label next_test;
    __ cmpq(temp, Immediate(ic_data.GetReceiverClassIdAt(i)));
    __ j(NOT_EQUAL, &next_test, Assembler::kNearJump);
    const Function& target = Function::ZoneHandle(ic_data.GetTargetAt(i));
    ObjectStore* object_store = Isolate::Current()->object_store();
    if (target.owner() == object_store->object_class()) {
      // Object.== is same as ===.
      __ Drop(2);
      __ cmpq(left, right);
      if (comp->is_fused_with_branch()) {
        comp->fused_with_branch()->EmitBranchOnCondition(compiler, EQUAL);
      } else {
        // This case should be rare.
        Register result = comp->locs()->out().reg();
        Label load_true;
        __ j(EQUAL, &load_true, Assembler::kNearJump);
        __ LoadObject(result, compiler->bool_false());
        __ jmp(&done);
        __ Bind(&load_true);
        __ LoadObject(result, compiler->bool_true());
      }
    } else {
      const int kNumberOfArguments = 2;
      const Array& kNoArgumentNames = Array::Handle();
      compiler->GenerateStaticCall(comp->cid(),
                                   comp->token_index(),
                                   comp->try_index(),
                                   target,
                                   kNumberOfArguments,
                                   kNoArgumentNames);
      ASSERT(comp->is_fused_with_branch() ||
            (comp->locs()->out().reg() == RAX));
      if (comp->is_fused_with_branch()) {
        __ CompareObject(RAX, compiler->bool_true());
        comp->fused_with_branch()->EmitBranchOnCondition(compiler, EQUAL);
      }
    }
    __ jmp(&done);
    __ Bind(&next_test);
  }
  // Fall through leads to deoptimization
  __ jmp(deopt);
  __ Bind(&done);
}


// First test if receiver is NULL, in which case === is applied.
// If type feedback was provided (lists of <class-id, target>), do a
// type by type check (either === or static call to the operator.
static void EmitGenericEqualityCompare(FlowGraphCompiler* compiler,
                                       EqualityCompareComp* comp) {
  const Immediate raw_null =
        Immediate(reinterpret_cast<intptr_t>(Object::null()));
  Register left = comp->locs()->in(0).reg();
  Register right = comp->locs()->in(1).reg();
  Label done, non_null_compare;
  __ cmpq(left, raw_null);
  __ j(NOT_EQUAL, &non_null_compare, Assembler::kNearJump);
  // Comparison with NULL is "===".
  __ cmpq(left, right);
  if (comp->is_fused_with_branch()) {
    comp->fused_with_branch()->EmitBranchOnCondition(compiler, EQUAL);
  } else {
    Register result = comp->locs()->out().reg();
    Label load_true;
    __ j(EQUAL, &load_true, Assembler::kNearJump);
    __ LoadObject(result, compiler->bool_false());
    __ jmp(&done);
    __ Bind(&load_true);
    __ LoadObject(result, compiler->bool_true());
  }
  __ jmp(&done);

  __ Bind(&non_null_compare);  // Receiver is not null.
  __ pushq(left);
  __ pushq(right);
  if (comp->HasICData() && (comp->ic_data()->NumberOfChecks() > 0)) {
    EmitEqualityAsPolymorphicCall(compiler, comp, left, right);
  } else {
    EmitEqualityAsInstanceCall(compiler, comp);
  }
  __ Bind(&done);
}


void EqualityCompareComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  if (HasICData() &&
      (ic_data()->NumberOfChecks() == 1) &&
      (ic_data()->GetReceiverClassIdAt(0) == kSmi)) {
    EmitSmiEqualityCompare(compiler, this);
    return;
  }
  EmitGenericEqualityCompare(compiler, this);
}


LocationSummary* RelationalOpComp::MakeLocationSummary() const {
  const LocationSummary::ContainsBranch contains_branch =
      is_fused_with_branch() ? LocationSummary::kBranch
                             : LocationSummary::kNoBranch;

  if (operands_class_id() == kSmi || operands_class_id() == kDouble) {
    const intptr_t kNumInputs = 2;
    const intptr_t kNumTemps = 1;
    LocationSummary* summary = new LocationSummary(kNumInputs,
                                                   kNumTemps,
                                                   LocationSummary::kCall,
                                                   contains_branch);
    summary->set_in(0, Location::RequiresRegister());
    summary->set_in(1, Location::RequiresRegister());
    if (!is_fused_with_branch()) {
      summary->set_out(Location::RequiresRegister());
    }
    summary->set_temp(0, Location::RequiresRegister());
    return summary;
  }
  ASSERT(!is_fused_with_branch());
  ASSERT(operands_class_id() == kObject);
  return MakeCallSummary();
}


static Condition TokenKindToSmiCondition(Token::Kind kind) {
  switch (kind) {
    case Token::kEQ: return EQUAL;
    case Token::kNE: return NOT_EQUAL;
    case Token::kLT: return LESS;
    case Token::kGT: return GREATER;
    case Token::kLTE: return LESS_EQUAL;
    case Token::kGTE: return  GREATER_EQUAL;
    default:
      UNREACHABLE();
      return OVERFLOW;
  }
}


static void EmitSmiRelationalOp(FlowGraphCompiler* compiler,
                                RelationalOpComp* comp) {
  Register left = comp->locs()->in(0).reg();
  Register right = comp->locs()->in(1).reg();
  Register temp =  comp->locs()->temp(0).reg();
  Label* deopt = compiler->AddDeoptStub(comp->cid(),
                                        comp->token_index(),
                                        comp->try_index(),
                                        kDeoptSmiCompareSmis,
                                        left,
                                        right);
  __ movq(temp, left);
  __ orq(temp, right);
  __ testq(temp, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, deopt);

  Condition true_condition = TokenKindToSmiCondition(comp->kind());
  __ cmpq(left, right);

  if (comp->is_fused_with_branch()) {
    comp->fused_with_branch()->EmitBranchOnCondition(compiler, true_condition);
  } else {
    Register result = comp->locs()->out().reg();
    Label done, is_true;
    __ j(true_condition, &is_true);
    __ LoadObject(result, compiler->bool_false());
    __ jmp(&done);
    __ Bind(&is_true);
    __ LoadObject(result, compiler->bool_true());
    __ Bind(&done);
  }
}


static Condition TokenKindToDoubleCondition(Token::Kind kind) {
  switch (kind) {
    case Token::kEQ: return EQUAL;
    case Token::kLT: return BELOW;
    case Token::kGT: return ABOVE;
    case Token::kLTE: return BELOW_EQUAL;
    case Token::kGTE: return ABOVE_EQUAL;
    default:
      UNREACHABLE();
      return OVERFLOW;
  }
}


static void EmitDoubleRelationalOp(FlowGraphCompiler* compiler,
                                   RelationalOpComp* comp) {
  Register left = comp->locs()->in(0).reg();
  Register right = comp->locs()->in(1).reg();
  // TODO(srdjan): temp is only needed if a conversion Smi->Double occurs.
  Register temp = comp->locs()->temp(0).reg();
  Label* deopt = compiler->AddDeoptStub(comp->cid(),
                                        comp->token_index(),
                                        comp->try_index(),
                                        kDeoptDoubleComparison,
                                        left,
                                        right);
  compiler->LoadDoubleOrSmiToXmm(XMM0, left, temp, deopt);
  compiler->LoadDoubleOrSmiToXmm(XMM1, right, temp, deopt);

  Condition true_condition = TokenKindToDoubleCondition(comp->kind());
  __ comisd(XMM0, XMM1);

  if (comp->is_fused_with_branch()) {
    BranchInstr* branch = comp->fused_with_branch();
    BlockEntryInstr* nan_result = branch->is_negated() ?
        branch->true_successor() : branch->false_successor();
    __ j(PARITY_EVEN, compiler->GetBlockLabel(nan_result));
    branch->EmitBranchOnCondition(compiler, true_condition);
  } else {
    Register result = comp->locs()->out().reg();
    Label is_false, is_true, done;
    __ j(PARITY_EVEN, &is_false, Assembler::kNearJump);
    __ j(true_condition, &is_true, Assembler::kNearJump);
    __ Bind(&is_false);
    __ LoadObject(result, compiler->bool_false());
    __ jmp(&done);
    __ Bind(&is_true);
    __ LoadObject(result, compiler->bool_true());
    __ Bind(&done);
  }
}


void RelationalOpComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  if (operands_class_id() == kSmi) {
    EmitSmiRelationalOp(compiler, this);
    return;
  }
  if (operands_class_id() == kDouble) {
    EmitDoubleRelationalOp(compiler, this);
    return;
  }
  const String& function_name =
      String::ZoneHandle(String::NewSymbol(Token::Str(kind())));
  compiler->AddCurrentDescriptor(PcDescriptors::kDeopt,
                                 cid(),
                                 token_index(),
                                 try_index());
  const intptr_t kNumArguments = 2;
  const intptr_t kNumArgsChecked = 2;  // Type-feedback.
  compiler->GenerateInstanceCall(cid(),
                                 token_index(),
                                 try_index(),
                                 function_name,
                                 kNumArguments,
                                 Array::ZoneHandle(),  // No optional arguments.
                                 kNumArgsChecked);
  ASSERT(locs()->out().reg() == RAX);
}


LocationSummary* NativeCallComp::MakeLocationSummary() const {
  const intptr_t kNumInputs = 0;
  const intptr_t kNumTemps = 3;
  LocationSummary* locs = new LocationSummary(kNumInputs,
                                              kNumTemps,
                                              LocationSummary::kCall);
  locs->set_temp(0, Location::RegisterLocation(RAX));
  locs->set_temp(1, Location::RegisterLocation(RBX));
  locs->set_temp(2, Location::RegisterLocation(R10));
  locs->set_out(Location::RequiresRegister());
  return locs;
}


void NativeCallComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  ASSERT(locs()->temp(0).reg() == RAX);
  ASSERT(locs()->temp(1).reg() == RBX);
  ASSERT(locs()->temp(2).reg() == R10);
  Register result = locs()->out().reg();

  // Push the result place holder initialized to NULL.
  __ PushObject(Object::ZoneHandle());
  // Pass a pointer to the first argument in RAX.
  intptr_t arg_count = argument_count();
  if (is_native_instance_closure()) {
    arg_count += 1;
  }
  if (!has_optional_parameters() && !is_native_instance_closure()) {
    __ leaq(RAX, Address(RBP, (1 + arg_count) * kWordSize));
  } else {
    __ leaq(RAX,
            Address(RBP, ParsedFunction::kFirstLocalSlotIndex * kWordSize));
  }
  __ movq(RBX, Immediate(reinterpret_cast<uword>(native_c_function())));
  __ movq(R10, Immediate(arg_count));
  compiler->GenerateCall(token_index(),
                         try_index(),
                         &StubCode::CallNativeCFunctionLabel(),
                         PcDescriptors::kOther);
  __ popq(result);
}


LocationSummary* LoadIndexedComp::MakeLocationSummary() const {
  const intptr_t kNumInputs = 2;
  if (receiver_type() == kGrowableObjectArray) {
    const intptr_t kNumTemps = 1;
    LocationSummary* locs = new LocationSummary(kNumInputs, kNumTemps);
    locs->set_in(0, Location::RequiresRegister());
    locs->set_in(1, Location::RequiresRegister());
    locs->set_temp(0, Location::RequiresRegister());
    locs->set_out(Location::RequiresRegister());
    return locs;
  } else if (receiver_type() == kArray || receiver_type() == kImmutableArray) {
    return LocationSummary::Make(kNumInputs, Location::RequiresRegister());
  } else {
    ASSERT(receiver_type() == kIllegalObjectKind);
    return MakeCallSummary();
  }
}


static void EmitLoadIndexedPolymorphic(FlowGraphCompiler* compiler,
                                        LoadIndexedComp* comp) {
  Label* deopt = compiler->AddDeoptStub(comp->cid(),
                                        comp->token_index(),
                                        comp->try_index(),
                                        kDeoptLoadIndexedPolymorphic);
  if (comp->ic_data()->NumberOfChecks() == 0) {
    __ jmp(deopt);
    return;
  }
  ASSERT(comp->HasICData());
  const ICData& ic_data = *comp->ic_data();
  ASSERT(ic_data.num_args_tested() == 1);
  // No indexed access on Smi.
  ASSERT(ic_data.GetReceiverClassIdAt(0) != kSmi);
  // Load receiver into RAX.
  const intptr_t kNumArguments = 2;
  __ movq(RAX, Address(RSP, (kNumArguments - 1) * kWordSize));
  __ testq(RAX, Immediate(kSmiTagMask));
  __ j(ZERO, deopt);
  Label done;
  __ LoadClassId(RDI, RAX);
  compiler->EmitTestAndCall(ic_data,
                            RDI,  // Class id register.
                            kNumArguments,
                            Array::Handle(),  // No named arguments.
                            deopt, &done,  // Labels.
                            comp->cid(),
                            comp->token_index(),
                            comp->try_index());
  __ Bind(&done);
}



void LoadIndexedComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  if (receiver_type() == kIllegalObjectKind) {
    if (HasICData()) {
      EmitLoadIndexedPolymorphic(compiler, this);
    } else {
      compiler->EmitLoadIndexedGeneric(this);
    }
    ASSERT(locs()->out().reg() == RAX);
    return;
  }

  Register receiver = locs()->in(0).reg();
  Register index = locs()->in(1).reg();
  Register result = locs()->out().reg();

  const DeoptReasonId deopt_reason = (receiver_type() == kGrowableObjectArray) ?
      kDeoptLoadIndexedGrowableArray : kDeoptLoadIndexedFixedArray;

  Label* deopt = compiler->AddDeoptStub(cid(),
                                        token_index(),
                                        try_index(),
                                        deopt_reason,
                                        receiver,
                                        index);

  __ testq(receiver, Immediate(kSmiTagMask));  // Deoptimize if Smi.
  __ j(ZERO, deopt);
  __ CompareClassId(receiver, receiver_type());
  __ j(NOT_EQUAL, deopt);

  __ testq(index, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, deopt);

  switch (receiver_type()) {
    case kArray:
    case kImmutableArray:
      __ cmpq(index, FieldAddress(receiver, Array::length_offset()));
      __ j(ABOVE_EQUAL, deopt);
      // Note that index is Smi, i.e, times 4.
      ASSERT(kSmiTagShift == 1);
      __ movq(result, FieldAddress(receiver, index, TIMES_4, sizeof(RawArray)));
      break;

    case kGrowableObjectArray: {
      Register temp = locs()->temp(0).reg();

      __ cmpq(index,
              FieldAddress(receiver, GrowableObjectArray::length_offset()));
      __ j(ABOVE_EQUAL, deopt);
      __ movq(temp, FieldAddress(receiver, GrowableObjectArray::data_offset()));
      // Note that index is Smi, i.e, times 4.
      ASSERT(kSmiTagShift == 1);
      __ movq(result, FieldAddress(temp, index, TIMES_4, sizeof(RawArray)));
      break;
    }

    default:
      UNREACHABLE();
      break;
  }
}


LocationSummary* StoreIndexedComp::MakeLocationSummary() const {
  const intptr_t kNumInputs = 3;
  if (receiver_type() == kGrowableObjectArray) {
    const intptr_t kNumTemps = 1;
    LocationSummary* locs = new LocationSummary(kNumInputs, kNumTemps);
    locs->set_in(0, Location::RequiresRegister());
    locs->set_in(1, Location::RequiresRegister());
    locs->set_in(2, Location::RequiresRegister());
    locs->set_temp(0, Location::RequiresRegister());
    locs->set_out(Location::NoLocation());
    return locs;
  } else if (receiver_type() == kArray) {
    return LocationSummary::Make(kNumInputs, Location::NoLocation());
  } else {
    ASSERT(receiver_type() == kIllegalObjectKind);
    return MakeCallSummary();
  }
}



static void EmitStoreIndexedGeneric(FlowGraphCompiler* compiler,
                                    StoreIndexedComp* comp) {
  const String& function_name =
      String::ZoneHandle(String::NewSymbol(Token::Str(Token::kASSIGN_INDEX)));

  compiler->AddCurrentDescriptor(PcDescriptors::kDeopt,
                                 comp->cid(),
                                 comp->token_index(),
                                 comp->try_index());

  const intptr_t kNumArguments = 3;
  const intptr_t kNumArgsChecked = 1;  // Type-feedback.
  compiler->GenerateInstanceCall(comp->cid(),
                                 comp->token_index(),
                                 comp->try_index(),
                                 function_name,
                                 kNumArguments,
                                 Array::ZoneHandle(),  // No names arguments.
                                 kNumArgsChecked);
}


static void EmitStoreIndexedPolymorphic(FlowGraphCompiler* compiler,
                                        StoreIndexedComp* comp) {
  Label* deopt = compiler->AddDeoptStub(comp->cid(),
                                        comp->token_index(),
                                        comp->try_index(),
                                        kDeoptStoreIndexedPolymorphic);
  if (comp->ic_data()->NumberOfChecks() == 0) {
    __ jmp(deopt);
    return;
  }
  ASSERT(comp->HasICData());
  const ICData& ic_data = *comp->ic_data();
  ASSERT(ic_data.num_args_tested() == 1);
  // No indexed access on Smi.
  ASSERT(ic_data.GetReceiverClassIdAt(0) != kSmi);
  // Load receiver into RAX.
  const intptr_t kNumArguments = 3;
  __ movq(RAX, Address(RSP, (kNumArguments - 1) * kWordSize));
  __ testq(RAX, Immediate(kSmiTagMask));
  __ j(ZERO, deopt);
  Label done;
  __ LoadClassId(RDI, RAX);
  compiler->EmitTestAndCall(ic_data,
                            RDI,  // Class id register.
                            kNumArguments,
                            Array::Handle(),  // No named arguments.
                            deopt, &done,  // Labels.
                            comp->cid(),
                            comp->token_index(),
                            comp->try_index());
  __ Bind(&done);
}


void StoreIndexedComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  if (receiver_type() == kIllegalObjectKind) {
    if (HasICData()) {
      EmitStoreIndexedPolymorphic(compiler, this);
    } else {
      EmitStoreIndexedGeneric(compiler, this);
    }
    return;
  }

  Register receiver = locs()->in(0).reg();
  Register index = locs()->in(1).reg();
  Register value = locs()->in(2).reg();

  const Class& receiver_class =
      Class::ZoneHandle(Isolate::Current()->class_table()->At(
          receiver_type()));

  Label* deopt = compiler->AddDeoptStub(cid(),
                                        token_index(),
                                        try_index(),
                                        kDeoptStoreIndexed,
                                        receiver,
                                        index,
                                        value);

  __ testq(receiver, Immediate(kSmiTagMask));  // Deoptimize if Smi.
  __ j(ZERO, deopt);
  __ CompareClassId(receiver, receiver_class.id());
  __ j(NOT_EQUAL, deopt);

  __ testq(index, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, deopt);

  switch (receiver_type()) {
    case kArray:
    case kImmutableArray:
      __ cmpq(index, FieldAddress(receiver, Array::length_offset()));
      __ j(ABOVE_EQUAL, deopt);
      // Note that index is Smi, i.e, times 4.
      ASSERT(kSmiTagShift == 1);
      __ StoreIntoObject(receiver,
          FieldAddress(receiver, index, TIMES_4, sizeof(RawArray)),
          value);
      break;

    case kGrowableObjectArray: {
      Register temp = locs()->temp(0).reg();
      __ cmpq(index,
              FieldAddress(receiver, GrowableObjectArray::length_offset()));
      __ j(ABOVE_EQUAL, deopt);
      __ movq(temp, FieldAddress(receiver, GrowableObjectArray::data_offset()));
      // Note that index is Smi, i.e, times 4.
      ASSERT(kSmiTagShift == 1);
      __ StoreIntoObject(temp,
          FieldAddress(temp, index, TIMES_4, sizeof(RawArray)),
          value);
      break;
    }

    default:
      UNREACHABLE();
      break;
  }
}


LocationSummary* InstanceSetterComp::MakeLocationSummary() const {
  return MakeCallSummary();
}


void InstanceSetterComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  const String& function_name =
      String::ZoneHandle(Field::SetterSymbol(field_name()));

  compiler->AddCurrentDescriptor(PcDescriptors::kDeopt,
                                 cid(),
                                 token_index(),
                                 try_index());
  const intptr_t kArgumentCount = 2;
  const intptr_t kCheckedArgumentCount = 1;
  compiler->GenerateInstanceCall(cid(),
                                 token_index(),
                                 try_index(),
                                 function_name,
                                 kArgumentCount,
                                 Array::ZoneHandle(),
                                 kCheckedArgumentCount);
}


LocationSummary* StaticSetterComp::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  return LocationSummary::Make(kNumInputs, Location::RequiresRegister());
}


void StaticSetterComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register value = locs()->in(0).reg();
  Register result = locs()->out().reg();

  // Preserve the argument as the result of the computation,
  // then call the setter.

  // Duplicate the argument.
  // TODO(fschneider): Avoid preserving the value if the result is not used.
  __ pushq(value);
  __ pushq(value);
  compiler->GenerateStaticCall(cid(),
                               token_index(),
                               try_index(),
                               setter_function(),
                               1,
                               Array::ZoneHandle());
  __ popq(result);
}


LocationSummary* LoadInstanceFieldComp::MakeLocationSummary() const {
  // TODO(fschneider): For this instruction the input register may be
  // reused for the result (but is not required to) because the input
  // is not used after the result is defined.  We should consider adding
  // this information to the input policy.
  return LocationSummary::Make(1, Location::RequiresRegister());
}


void LoadInstanceFieldComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register instance_reg = locs()->in(0).reg();
  Register result_reg = locs()->out().reg();

  if (HasICData()) {
    ASSERT(original() != NULL);
    Label* deopt = compiler->AddDeoptStub(original()->cid(),
                                          original()->token_index(),
                                          original()->try_index(),
                                          kDeoptInstanceGetterSameTarget,
                                          instance_reg);
    // Smis do not have instance fields (Smi class is always first).
    // Use 'result' as temporary register.
    ASSERT(result_reg != instance_reg);
    ASSERT(ic_data() != NULL);
    compiler->EmitClassChecksNoSmi(*ic_data(), instance_reg, result_reg, deopt);
  }
  __ movq(result_reg, FieldAddress(instance_reg, field().Offset()));
}


LocationSummary* LoadStaticFieldComp::MakeLocationSummary() const {
  return LocationSummary::Make(0, Location::RequiresRegister());
}


void LoadStaticFieldComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register result = locs()->out().reg();
  __ LoadObject(result, field());
  __ movq(result, FieldAddress(result, Field::value_offset()));
}


LocationSummary* InstanceOfComp::MakeLocationSummary() const {
  LocationSummary* summary = new LocationSummary(3, 0);
  summary->set_in(0, Location::RegisterLocation(RAX));
  summary->set_in(1, Location::RegisterLocation(RCX));
  summary->set_in(2, Location::RegisterLocation(RDX));
  summary->set_out(Location::RegisterLocation(RAX));
  return summary;
}


void InstanceOfComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  ASSERT(locs()->in(0).reg() == RAX);  // Value.
  ASSERT(locs()->in(1).reg() == RCX);  // Instantiator.
  ASSERT(locs()->in(2).reg() == RDX);  // Instantiator type arguments.

  compiler->GenerateInstanceOf(cid(),
                               token_index(),
                               try_index(),
                               type(),
                               negate_result());
  ASSERT(locs()->out().reg() == RAX);
}


LocationSummary* CreateArrayComp::MakeLocationSummary() const {
  // TODO(regis): The elements of the array could be considered as arguments to
  // CreateArrayComp, thereby making CreateArrayComp a call.
  // For VerifyCallComputation to work, CreateArrayComp would need an
  // ArgumentCount getter and an ArgumentAt getter.
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 1;
  LocationSummary* locs = new LocationSummary(kNumInputs,
                                              kNumTemps,
                                              LocationSummary::kCall);
  locs->set_in(0, Location::RegisterLocation(RBX));
  locs->set_temp(0, Location::RegisterLocation(R10));
  locs->set_out(Location::RegisterLocation(RAX));
  return locs;
}


void CreateArrayComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register temp_reg = locs()->temp(0).reg();
  Register result_reg = locs()->out().reg();

  // 1. Allocate the array.  R10 = length, RBX = element type.
  ASSERT(temp_reg == R10);
  ASSERT(locs()->in(0).reg() == RBX);
  __ movq(temp_reg, Immediate(Smi::RawValue(ElementCount())));
  compiler->GenerateCall(token_index(),
                         try_index(),
                         &StubCode::AllocateArrayLabel(),
                         PcDescriptors::kOther);
  ASSERT(result_reg == RAX);
  // 2. Initialize the array in result_reg with the element values.
  __ leaq(temp_reg, FieldAddress(result_reg, Array::data_offset()));
  for (int i = ElementCount() - 1; i >= 0; --i) {
    ASSERT(ElementAt(i)->IsUse());
    __ popq(Address(temp_reg, i * kWordSize));
  }
}


LocationSummary* AllocateObjectWithBoundsCheckComp::
    MakeLocationSummary() const {
  return LocationSummary::Make(2,
                               Location::RequiresRegister(),
                               LocationSummary::kCall);
}


void AllocateObjectWithBoundsCheckComp::EmitNativeCode(
    FlowGraphCompiler* compiler) {
  const Class& cls = Class::ZoneHandle(constructor().owner());
  Register type_arguments = locs()->in(0).reg();
  Register instantiator_type_arguments = locs()->in(1).reg();
  Register result = locs()->out().reg();

  // Push the result place holder initialized to NULL.
  __ PushObject(Object::ZoneHandle());
  __ pushq(Immediate(Smi::RawValue(token_index())));
  __ PushObject(cls);
  __ pushq(type_arguments);
  __ pushq(instantiator_type_arguments);
  compiler->GenerateCallRuntime(cid(),
                                token_index(),
                                try_index(),
                                kAllocateObjectWithBoundsCheckRuntimeEntry);
  // Pop instantiator type arguments, type arguments, class, and
  // source location.
  __ Drop(4);
  __ popq(result);  // Pop new instance.
}


LocationSummary* LoadVMFieldComp::MakeLocationSummary() const {
  return LocationSummary::Make(1, Location::RequiresRegister());
}


void LoadVMFieldComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register instance_reg = locs()->in(0).reg();
  Register result_reg = locs()->out().reg();
  if (HasICData()) {
    ASSERT(original() != NULL);
    Label* deopt = compiler->AddDeoptStub(original()->cid(),
                                          original()->token_index(),
                                          original()->try_index(),
                                          kDeoptInstanceGetterSameTarget,
                                          instance_reg);
    // Smis do not have instance fields (Smi class is always first).
    // Use 'result' as temporary register.
    ASSERT(result_reg != instance_reg);
    ASSERT(ic_data() != NULL);
    compiler->EmitClassChecksNoSmi(*ic_data(), instance_reg, result_reg, deopt);
  }

  __ movq(result_reg, FieldAddress(instance_reg, offset_in_bytes()));
}


LocationSummary* InstantiateTypeArgumentsComp::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs = new LocationSummary(kNumInputs,
                                              kNumTemps,
                                              LocationSummary::kCall);
  locs->set_in(0, Location::RequiresRegister());
  locs->set_out(Location::SameAsFirstInput());
  return locs;
}


void InstantiateTypeArgumentsComp::EmitNativeCode(
    FlowGraphCompiler* compiler) {
  Register instantiator_reg = locs()->in(0).reg();
  Register result_reg = locs()->out().reg();

  // 'instantiator_reg' is the instantiator AbstractTypeArguments object
  // (or null).
  // If the instantiator is null and if the type argument vector
  // instantiated from null becomes a vector of Dynamic, then use null as
  // the type arguments.
  Label type_arguments_instantiated;
  const intptr_t len = type_arguments().Length();
  if (type_arguments().IsRawInstantiatedRaw(len)) {
    const Immediate raw_null =
        Immediate(reinterpret_cast<intptr_t>(Object::null()));
    __ cmpq(instantiator_reg, raw_null);
    __ j(EQUAL, &type_arguments_instantiated, Assembler::kNearJump);
  }
  // Instantiate non-null type arguments.
  if (type_arguments().IsUninstantiatedIdentity()) {
    Label type_arguments_uninstantiated;
    // Check if the instantiator type argument vector is a TypeArguments of a
    // matching length and, if so, use it as the instantiated type_arguments.
    // No need to check the instantiator ('instantiator_reg') for null here,
    // because a null instantiator will have the wrong class (Null instead of
    // TypeArguments).
    __ CompareClassId(instantiator_reg, kTypeArguments);
    __ j(NOT_EQUAL, &type_arguments_uninstantiated, Assembler::kNearJump);
    __ cmpq(FieldAddress(instantiator_reg, TypeArguments::length_offset()),
            Immediate(Smi::RawValue(len)));
    __ j(EQUAL, &type_arguments_instantiated, Assembler::kNearJump);
    __ Bind(&type_arguments_uninstantiated);
  }
  // A runtime call to instantiate the type arguments is required.
  __ PushObject(Object::ZoneHandle());  // Make room for the result.
  __ PushObject(type_arguments());
  __ pushq(instantiator_reg);  // Push instantiator type arguments.
  compiler->GenerateCallRuntime(cid(),
                                token_index(),
                                try_index(),
                                kInstantiateTypeArgumentsRuntimeEntry);
  __ Drop(2);  // Drop instantiator and uninstantiated type arguments.
  __ popq(result_reg);  // Pop instantiated type arguments.
  __ Bind(&type_arguments_instantiated);
  ASSERT(instantiator_reg == result_reg);
  // 'result_reg': Instantiated type arguments.
}


LocationSummary* ExtractConstructorTypeArgumentsComp::
    MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs = new LocationSummary(kNumInputs, kNumTemps);
  locs->set_in(0, Location::RequiresRegister());
  locs->set_out(Location::SameAsFirstInput());
  return locs;
}


void ExtractConstructorTypeArgumentsComp::EmitNativeCode(
    FlowGraphCompiler* compiler) {
  Register instantiator_reg = locs()->in(0).reg();
  Register result_reg = locs()->out().reg();
  ASSERT(instantiator_reg == result_reg);

  // instantiator_reg is the instantiator type argument vector, i.e. an
  // AbstractTypeArguments object (or null).
  // If the instantiator is null and if the type argument vector
  // instantiated from null becomes a vector of Dynamic, then use null as
  // the type arguments.
  Label type_arguments_instantiated;
  const intptr_t len = type_arguments().Length();
  if (type_arguments().IsRawInstantiatedRaw(len)) {
    const Immediate raw_null =
        Immediate(reinterpret_cast<intptr_t>(Object::null()));
    __ cmpq(instantiator_reg, raw_null);
    __ j(EQUAL, &type_arguments_instantiated, Assembler::kNearJump);
  }
  // Instantiate non-null type arguments.
  if (type_arguments().IsUninstantiatedIdentity()) {
    // Check if the instantiator type argument vector is a TypeArguments of a
    // matching length and, if so, use it as the instantiated type_arguments.
    // No need to check instantiator_reg for null here, because a null
    // instantiator will have the wrong class (Null instead of TypeArguments).
    Label type_arguments_uninstantiated;
    __ CompareClassId(instantiator_reg, kTypeArguments);
    __ j(NOT_EQUAL, &type_arguments_uninstantiated, Assembler::kNearJump);
    Immediate arguments_length =
        Immediate(Smi::RawValue(type_arguments().Length()));
    __ cmpq(FieldAddress(instantiator_reg, TypeArguments::length_offset()),
        arguments_length);
    __ j(EQUAL, &type_arguments_instantiated, Assembler::kNearJump);
    __ Bind(&type_arguments_uninstantiated);
  }
  // In the non-factory case, we rely on the allocation stub to
  // instantiate the type arguments.
  __ LoadObject(result_reg, type_arguments());
  // result_reg: uninstantiated type arguments.
  __ Bind(&type_arguments_instantiated);
  // result_reg: uninstantiated or instantiated type arguments.
}


LocationSummary* ExtractConstructorInstantiatorComp::
    MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs = new LocationSummary(kNumInputs, kNumTemps);
  locs->set_in(0, Location::RequiresRegister());
  locs->set_out(Location::SameAsFirstInput());
  return locs;
}


void ExtractConstructorInstantiatorComp::EmitNativeCode(
    FlowGraphCompiler* compiler) {
  ASSERT(instantiator()->IsUse());
  Register instantiator_reg = locs()->in(0).reg();
  ASSERT(locs()->out().reg() == instantiator_reg);

  // instantiator_reg is the instantiator AbstractTypeArguments object
  // (or null).  If the instantiator is null and if the type argument vector
  // instantiated from null becomes a vector of Dynamic, then use null as
  // the type arguments and do not pass the instantiator.
  Label done;
  const intptr_t len = type_arguments().Length();
  if (type_arguments().IsRawInstantiatedRaw(len)) {
    const Immediate raw_null =
        Immediate(reinterpret_cast<intptr_t>(Object::null()));
    Label instantiator_not_null;
    __ cmpq(instantiator_reg, raw_null);
    __ j(NOT_EQUAL, &instantiator_not_null, Assembler::kNearJump);
    // Null was used in VisitExtractConstructorTypeArguments as the
    // instantiated type arguments, no proper instantiator needed.
    __ movq(instantiator_reg,
            Immediate(Smi::RawValue(StubCode::kNoInstantiator)));
    __ jmp(&done);
    __ Bind(&instantiator_not_null);
  }
  // Instantiate non-null type arguments.
  if (type_arguments().IsUninstantiatedIdentity()) {
    // TODO(regis): The following emitted code is duplicated in
    // VisitExtractConstructorTypeArguments above. The reason is that the code
    // is split between two computations, so that each one produces a
    // single value, rather than producing a pair of values.
    // If this becomes an issue, we should expose these tests at the IL level.

    // Check if the instantiator type argument vector is a TypeArguments of a
    // matching length and, if so, use it as the instantiated type_arguments.
    // No need to check the instantiator (RAX) for null here, because a null
    // instantiator will have the wrong class (Null instead of TypeArguments).
    __ CompareClassId(instantiator_reg, kTypeArguments);
    __ j(NOT_EQUAL, &done, Assembler::kNearJump);
    Immediate arguments_length =
        Immediate(Smi::RawValue(type_arguments().Length()));
    __ cmpq(FieldAddress(instantiator_reg, TypeArguments::length_offset()),
        arguments_length);
    __ j(NOT_EQUAL, &done, Assembler::kNearJump);
    // The instantiator was used in VisitExtractConstructorTypeArguments as the
    // instantiated type arguments, no proper instantiator needed.
    __ movq(instantiator_reg,
            Immediate(Smi::RawValue(StubCode::kNoInstantiator)));
  }
  __ Bind(&done);
  // instantiator_reg: instantiator or kNoInstantiator.
}


LocationSummary* AllocateContextComp::MakeLocationSummary() const {
  const intptr_t kNumInputs = 0;
  const intptr_t kNumTemps = 1;
  LocationSummary* locs = new LocationSummary(kNumInputs,
                                              kNumTemps,
                                              LocationSummary::kCall);
  locs->set_temp(0, Location::RegisterLocation(R10));
  locs->set_out(Location::RegisterLocation(RAX));
  return locs;
}


void AllocateContextComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  ASSERT(locs()->temp(0).reg() == R10);

  __ movq(R10, Immediate(num_context_variables()));
  const ExternalLabel label("alloc_context",
                            StubCode::AllocateContextEntryPoint());
  compiler->GenerateCall(token_index(),
                         try_index(),
                         &label,
                         PcDescriptors::kOther);
}


LocationSummary* CloneContextComp::MakeLocationSummary() const {
  return LocationSummary::Make(1,
                               Location::RequiresRegister(),
                               LocationSummary::kCall);
}


void CloneContextComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register context_value = locs()->in(0).reg();
  Register result = locs()->out().reg();

  __ PushObject(Object::ZoneHandle());  // Make room for the result.
  __ pushq(context_value);
  compiler->GenerateCallRuntime(cid(),
                                token_index(),
                                try_index(),
                                kCloneContextRuntimeEntry);
  __ popq(result);  // Remove argument.
  __ popq(result);  // Get result (cloned context).
}


LocationSummary* CatchEntryComp::MakeLocationSummary() const {
  return LocationSummary::Make(0, Location::NoLocation());
}


// Restore stack and initialize the two exception variables:
// exception and stack trace variables.
void CatchEntryComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  // Restore RSP from RBP as we are coming from a throw and the code for
  // popping arguments has not been run.
  const intptr_t locals_space_size = compiler->StackSize() * kWordSize;
  ASSERT(locals_space_size >= 0);
  const intptr_t offset_size =
      -locals_space_size + FlowGraphCompiler::kLocalsOffsetFromFP;
  __ leaq(RSP, Address(RBP, offset_size));

  ASSERT(!exception_var().is_captured());
  ASSERT(!stacktrace_var().is_captured());
  __ movq(Address(RBP, exception_var().index() * kWordSize),
          kExceptionObjectReg);
  __ movq(Address(RBP, stacktrace_var().index() * kWordSize),
          kStackTraceObjectReg);
}


LocationSummary* CheckStackOverflowComp::MakeLocationSummary() const {
  const intptr_t kNumInputs = 0;
  const intptr_t kNumTemps = 1;
  // TODO(vegorov): spilling is required only on an infrequently executed path.
  LocationSummary* summary = new LocationSummary(kNumInputs,
                                                 kNumTemps,
                                                 LocationSummary::kCall);
  summary->set_temp(0, Location::RequiresRegister());
  return summary;
}


void CheckStackOverflowComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register temp = locs()->temp(0).reg();
  // Generate stack overflow check.
  __ movq(temp, Immediate(Isolate::Current()->stack_limit_address()));
  __ cmpq(RSP, Address(temp, 0));
  Label no_stack_overflow;
  __ j(ABOVE, &no_stack_overflow, Assembler::kNearJump);
  compiler->GenerateCallRuntime(cid(),
                                token_index(),
                                try_index(),
                                kStackOverflowRuntimeEntry);
  __ Bind(&no_stack_overflow);
}


LocationSummary* BinaryOpComp::MakeLocationSummary() const {
  const intptr_t kNumInputs = 2;

  if (operands_type() == kDoubleOperands) {
    return MakeCallSummary();  // Calls into a stub for allocation.
  }

  if (operands_type() == kMintOperands) {
    ASSERT(op_kind() == Token::kBIT_AND);
    const intptr_t kNumTemps = 0;
    LocationSummary* summary =
        new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
    summary->set_in(0, Location::RegisterLocation(RAX));
    summary->set_in(1, Location::RequiresRegister());
    summary->set_out(Location::SameAsFirstInput());
    return summary;
  }

  ASSERT(operands_type() == kSmiOperands);

  if (op_kind() == Token::kTRUNCDIV) {
    const intptr_t kNumTemps = 3;
    LocationSummary* summary = new LocationSummary(kNumInputs, kNumTemps);
    summary->set_in(0, Location::RegisterLocation(RAX));
    summary->set_in(1, Location::RegisterLocation(RCX));
    summary->set_out(Location::SameAsFirstInput());
    summary->set_temp(0, Location::RegisterLocation(RBX));
    // Will be used for for sign extension.
    summary->set_temp(1, Location::RegisterLocation(RDX));
    summary->set_temp(2, Location::RequiresRegister());
    return summary;
  } else if (op_kind() == Token::kSHR) {
    const intptr_t kNumTemps = 1;
    LocationSummary* summary = new LocationSummary(kNumInputs, kNumTemps);
    summary->set_in(0, Location::RequiresRegister());
    summary->set_in(1, Location::RegisterLocation(RCX));
    summary->set_out(Location::SameAsFirstInput());
    summary->set_temp(0, Location::RequiresRegister());
    return summary;
  } else if (op_kind() == Token::kSHL) {
    const intptr_t kNumTemps = 2;
    LocationSummary* summary = new LocationSummary(kNumInputs, kNumTemps);
    summary->set_in(0, Location::RequiresRegister());
    summary->set_in(1, Location::RequiresRegister());
    summary->set_out(Location::SameAsFirstInput());
    summary->set_temp(0, Location::RequiresRegister());
    summary->set_temp(1, Location::RegisterLocation(RCX));
    return summary;
  } else {
    const intptr_t kNumTemps = 1;
    LocationSummary* summary = new LocationSummary(kNumInputs, kNumTemps);
    summary->set_in(0, Location::RequiresRegister());
    summary->set_in(1, Location::RequiresRegister());
    summary->set_out(Location::SameAsFirstInput());
    summary->set_temp(0, Location::RequiresRegister());
    return summary;
  }
}


static void EmitSmiBinaryOp(FlowGraphCompiler* compiler, BinaryOpComp* comp) {
  Register left = comp->locs()->in(0).reg();
  Register right = comp->locs()->in(1).reg();
  Register result = comp->locs()->out().reg();
  Register temp = comp->locs()->temp(0).reg();
  ASSERT(left == result);
  Label* deopt = compiler->AddDeoptStub(comp->instance_call()->cid(),
                                        comp->instance_call()->token_index(),
                                        comp->instance_call()->try_index(),
                                        kDeoptSmiBinaryOp,
                                        temp,
                                        right);
  __ movq(temp, left);
  __ orq(left, right);
  __ testq(left, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, deopt);
  __ movq(left, temp);
  switch (comp->op_kind()) {
    case Token::kADD: {
      __ addq(left, right);
      __ j(OVERFLOW, deopt);
      break;
    }
    case Token::kSUB: {
      __ subq(left, right);
      __ j(OVERFLOW, deopt);
      break;
    }
    case Token::kMUL: {
      __ SmiUntag(left);
      __ imulq(left, right);
      __ j(OVERFLOW, deopt);
      break;
    }
    case Token::kBIT_AND: {
      // No overflow check.
      __ andq(left, right);
      break;
    }
    case Token::kBIT_OR: {
      // No overflow check.
      __ orq(left, right);
      break;
    }
    case Token::kBIT_XOR: {
      // No overflow check.
      __ xorq(left, right);
      break;
    }
    case Token::kTRUNCDIV: {
      // Handle divide by zero in runtime.
      // Deoptimization requires that temp and right are preserved.
      __ testq(right, right);
      __ j(ZERO, deopt);
      ASSERT(left == RAX);
      ASSERT((right != RDX) && (right != RAX));
      ASSERT((temp != RDX) && (temp != RAX));
      ASSERT(comp->locs()->temp(1).reg() == RDX);
      ASSERT(result == RAX);
      Register right_temp = comp->locs()->temp(2).reg();
      __ movq(right_temp, right);
      __ SmiUntag(left);
      __ SmiUntag(right_temp);
      __ cqo();  // Sign extend RAX -> RDX:RAX.
      __ idivq(right_temp);  //  RAX: quotient, RDX: remainder.
      // Check the corner case of dividing the 'MIN_SMI' with -1, in which
      // case we cannot tag the result.
      __ cmpq(result, Immediate(0x4000000000000000));
      __ j(EQUAL, deopt);
      __ SmiTag(result);
      break;
    }
    case Token::kSHR: {
      // sarq operation masks the count to 6 bits.
      const Immediate kCountLimit = Immediate(0x3F);
      __ cmpq(right, Immediate(0));
      __ j(LESS, deopt);
      __ SmiUntag(right);
      __ cmpq(right, kCountLimit);
      Label count_ok;
      __ j(LESS, &count_ok, Assembler::kNearJump);
      __ movq(right, kCountLimit);
      __ Bind(&count_ok);
      ASSERT(right == RCX);  // Count must be in RCX
      __ SmiUntag(left);
      __ sarq(left, right);
      __ SmiTag(left);
      break;
    }
    case Token::kSHL: {
      // Check if count too large for handling it inlined.
      __ cmpq(right,
          Immediate(reinterpret_cast<int64_t>(Smi::New(Smi::kBits))));
      __ j(ABOVE_EQUAL, deopt);
      Register right_temp = comp->locs()->temp(1).reg();
      ASSERT(right_temp == RCX);  // Count must be in RCX
      __ movq(right_temp, right);
      __ SmiUntag(right_temp);
      // Overflow test (preserve temp and right);
      __ shlq(left, right_temp);
      __ sarq(left, right_temp);
      __ cmpq(left, temp);
      __ j(NOT_EQUAL, deopt);  // Overflow.
      // Shift for result now we know there is no overflow.
      __ shlq(left, right_temp);
      break;
    }
    case Token::kDIV: {
      // Dispatches to 'Double./'.
      // TODO(srdjan): Implement as conversion to double and double division.
      UNREACHABLE();
      break;
    }
    case Token::kMOD: {
      // TODO(srdjan): Implement.
      UNREACHABLE();
      break;
    }
    case Token::kOR:
    case Token::kAND: {
      // Flow graph builder has dissected this operation to guarantee correct
      // behavior (short-circuit evaluation).
      UNREACHABLE();
      break;
    }
    default:
      UNREACHABLE();
      break;
  }
}


static void EmitMintBinaryOp(FlowGraphCompiler* compiler, BinaryOpComp* comp) {
  // TODO(regis): For now, we only support Token::kBIT_AND for a Mint or Smi
  // receiver and a Mint or Smi argument. We fall back to the run time call if
  // both receiver and argument are Mint or if one of them is Mint and the other
  // is a negative Smi.
  Register left = comp->locs()->in(0).reg();
  Register right = comp->locs()->in(1).reg();
  Register result = comp->locs()->out().reg();
  ASSERT(left == result);
  ASSERT(comp->op_kind() == Token::kBIT_AND);
  Label* deopt = compiler->AddDeoptStub(comp->instance_call()->cid(),
                                        comp->instance_call()->token_index(),
                                        comp->instance_call()->try_index(),
                                        kDeoptMintBinaryOp,
                                        left,
                                        right);
  Label mint_static_call, smi_static_call, non_smi, smi_smi, done;
  __ testq(left, Immediate(kSmiTagMask));  // Is receiver Smi?
  __ j(NOT_ZERO, &non_smi);
  __ testq(right, Immediate(kSmiTagMask));  // Is argument Smi?
  __ j(ZERO, &smi_smi);
  __ CompareClassId(right, kMint);  // Is argument Mint?
  __ j(NOT_EQUAL, deopt);  // Argument neither Smi nor Mint.
  __ cmpq(left, Immediate(0));
  __ j(LESS, &smi_static_call);  // Negative Smi receiver, Mint argument.

  // Positive Smi receiver, Mint argument.
  // Load lower argument Mint word, convert to Smi. It is OK to loose bits.
  __ movq(right, FieldAddress(right, Mint::value_offset()));
  __ SmiTag(right);
  __ andq(result, right);
  __ jmp(&done);

  __ Bind(&non_smi);  // Receiver is non-Smi.
  __ CompareClassId(left, kMint);  // Is receiver Mint?
  __ j(NOT_EQUAL, deopt);  // Receiver neither Smi nor Mint.
  __ testq(right, Immediate(kSmiTagMask));  // Is argument Smi?
  __ j(NOT_ZERO, &mint_static_call);  // Mint receiver, non-Smi argument.
  __ cmpq(right, Immediate(0));
  __ j(LESS, &mint_static_call);  // Mint receiver, negative Smi argument.

  // Mint receiver, positive Smi argument.
  // Load lower receiver Mint word, convert to Smi. It is OK to loose bits.
  __ movq(result, FieldAddress(left, Mint::value_offset()));
  __ SmiTag(result);
  __ Bind(&smi_smi);
  __ andq(result, right);
  __ jmp(&done);

  __ Bind(&smi_static_call);
  {
    Function& target = Function::ZoneHandle(
        comp->ic_data()->GetTargetForReceiverClassId(kSmi));
    if (target.IsNull()) {
      __ jmp(deopt);
    } else {
      __ pushq(left);
      __ pushq(right);
      compiler->GenerateStaticCall(comp->instance_call()->cid(),
                                   comp->instance_call()->token_index(),
                                   comp->instance_call()->try_index(),
                                   target,
                                   comp->instance_call()->ArgumentCount(),
                                   comp->instance_call()->argument_names());
      ASSERT(result == RAX);
      __ jmp(&done);
    }
  }

  __ Bind(&mint_static_call);
  {
    Function& target = Function::ZoneHandle(
        comp->ic_data()->GetTargetForReceiverClassId(kMint));
    if (target.IsNull()) {
      __ jmp(deopt);
    } else {
      __ pushq(left);
      __ pushq(right);
      compiler->GenerateStaticCall(comp->instance_call()->cid(),
                                   comp->instance_call()->token_index(),
                                   comp->instance_call()->try_index(),
                                   target,
                                   comp->instance_call()->ArgumentCount(),
                                   comp->instance_call()->argument_names());
      ASSERT(result == RAX);
    }
  }
  __ Bind(&done);
}


static void EmitDoubleBinaryOp(FlowGraphCompiler* compiler,
                               BinaryOpComp* comp) {
  Register left = RBX;
  Register right = RCX;
  Register temp = RDX;
  Register result = comp->locs()->out().reg();

  const Class& double_class = compiler->double_class();
  const Code& stub =
    Code::Handle(StubCode::GetAllocationStubForClass(double_class));
  const ExternalLabel label(double_class.ToCString(), stub.EntryPoint());
  compiler->GenerateCall(comp->instance_call()->token_index(),
                         comp->instance_call()->try_index(),
                         &label,
                         PcDescriptors::kOther);
  // Newly allocated object is now in the result register (RAX).
  ASSERT(result == RAX);
  __ popq(right);
  __ popq(left);

  Label* deopt = compiler->AddDeoptStub(comp->instance_call()->cid(),
                                        comp->instance_call()->token_index(),
                                        comp->instance_call()->try_index(),
                                        kDeoptDoubleBinaryOp,
                                        left,
                                        right);

  compiler->LoadDoubleOrSmiToXmm(XMM0, left, temp, deopt);
  compiler->LoadDoubleOrSmiToXmm(XMM1, right, temp, deopt);

  switch (comp->op_kind()) {
    case Token::kADD: __ addsd(XMM0, XMM1); break;
    case Token::kSUB: __ subsd(XMM0, XMM1); break;
    case Token::kMUL: __ mulsd(XMM0, XMM1); break;
    case Token::kDIV: __ divsd(XMM0, XMM1); break;
    default: UNREACHABLE();
  }

  __ movsd(FieldAddress(result, Double::value_offset()), XMM0);
}


void BinaryOpComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  switch (operands_type()) {
    case kSmiOperands:
      EmitSmiBinaryOp(compiler, this);
      break;

    case kMintOperands:
      EmitMintBinaryOp(compiler, this);
      break;

    case kDoubleOperands:
      EmitDoubleBinaryOp(compiler, this);
      break;

    default:
      UNREACHABLE();
  }
}


LocationSummary* UnarySmiOpComp::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary = new LocationSummary(kNumInputs, kNumTemps);
  summary->set_in(0, Location::RequiresRegister());
  summary->set_out(Location::SameAsFirstInput());
  return summary;
}


void UnarySmiOpComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  const ICData& ic_data = *instance_call()->ic_data();
  ASSERT(!ic_data.IsNull());
  ASSERT(ic_data.num_args_tested() == 1);
  // TODO(srdjan): Implement for more checks.
  ASSERT(ic_data.NumberOfChecks() == 1);
  intptr_t test_class_id;
  Function& target = Function::Handle();
  ic_data.GetOneClassCheckAt(0, &test_class_id, &target);

  Register value = locs()->in(0).reg();
  Register result = locs()->out().reg();
  ASSERT(value == result);
  Label* deopt = compiler->AddDeoptStub(instance_call()->cid(),
                                        instance_call()->token_index(),
                                        instance_call()->try_index(),
                                        kDeoptSmiBinaryOp,
                                        value);
  if (test_class_id == kSmi) {
    __ testq(value, Immediate(kSmiTagMask));
    __ j(NOT_ZERO, deopt);
    switch (op_kind()) {
      case Token::kNEGATE:
        __ negq(value);
        __ j(OVERFLOW, deopt);
        break;
      case Token::kBIT_NOT:
        __ notq(value);
        __ andq(value, Immediate(~kSmiTagMask));  // Remove inverted smi-tag.
        break;
      default:
        UNREACHABLE();
    }
  } else {
    UNREACHABLE();
  }
}


LocationSummary* NumberNegateComp::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 1;  // Needed for doubles.
  LocationSummary* summary = new LocationSummary(kNumInputs,
                                                 kNumTemps,
                                                 LocationSummary::kCall);
  summary->set_in(0, Location::RequiresRegister());
  summary->set_out(Location::SameAsFirstInput());
  summary->set_temp(0, Location::RequiresRegister());
  return summary;
}


void NumberNegateComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  const ICData& ic_data = *instance_call()->ic_data();
  ASSERT(!ic_data.IsNull());
  ASSERT(ic_data.num_args_tested() == 1);

  // TODO(srdjan): Implement for more checks.
  ASSERT(ic_data.NumberOfChecks() == 1);
  intptr_t test_class_id;
  Function& target = Function::Handle();
  ic_data.GetOneClassCheckAt(0, &test_class_id, &target);

  Register value = locs()->in(0).reg();
  Register result = locs()->out().reg();
  ASSERT(value == result);
  Label* deopt = compiler->AddDeoptStub(instance_call()->cid(),
                                        instance_call()->token_index(),
                                        instance_call()->try_index(),
                                        kDeoptSmiBinaryOp,
                                        value);
  if (test_class_id == kDouble) {
    Register temp = locs()->temp(0).reg();
    __ testq(value, Immediate(kSmiTagMask));
    __ j(ZERO, deopt);  // Smi.
    __ CompareClassId(value, kDouble);
    __ j(NOT_EQUAL, deopt);
    // Allocate result object.
    const Class& double_class = compiler->double_class();
    const Code& stub =
        Code::Handle(StubCode::GetAllocationStubForClass(double_class));
    const ExternalLabel label(double_class.ToCString(), stub.EntryPoint());
    __ pushq(value);
    compiler->GenerateCall(instance_call()->token_index(),
                           instance_call()->try_index(),
                           &label,
                           PcDescriptors::kOther);
    // Result is in RAX.
    ASSERT(result != temp);
    __ movq(result, RAX);
    __ popq(temp);
    __ movsd(XMM0, FieldAddress(temp, Double::value_offset()));
    __ DoubleNegate(XMM0);
    __ movsd(FieldAddress(result, Double::value_offset()), XMM0);
  } else {
    UNREACHABLE();
  }
}


LocationSummary* ToDoubleComp::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  if (from() == kDouble) {
    const intptr_t kNumTemps = 0;
    LocationSummary* locs = new LocationSummary(kNumInputs, kNumTemps);
    locs->set_in(0, Location::RequiresRegister());
    locs->set_out(Location::SameAsFirstInput());
    return locs;
  } else {
    ASSERT(from() == kSmi);
    return MakeCallSummary();
  }
}


void ToDoubleComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register value = (from() == kDouble) ? locs()->in(0).reg() : RBX;
  Register result = locs()->out().reg();

  const DeoptReasonId deopt_reason = (from() == kDouble) ?
      kDeoptDoubleToDouble : kDeoptIntegerToDouble;
  Label* deopt = compiler->AddDeoptStub(instance_call()->cid(),
                                        instance_call()->token_index(),
                                        instance_call()->try_index(),
                                        deopt_reason,
                                        value);

  if (from() == kDouble) {
    __ testq(value, Immediate(kSmiTagMask));
    __ j(ZERO, deopt);  // Deoptimize if Smi.
    __ CompareClassId(value, kDouble);
    __ j(NOT_EQUAL, deopt);  // Deoptimize if not Double.
    ASSERT(value == result);
    return;
  }

  ASSERT(from() == kSmi);

  const Class& double_class = compiler->double_class();
  const Code& stub =
    Code::Handle(StubCode::GetAllocationStubForClass(double_class));
  const ExternalLabel label(double_class.ToCString(), stub.EntryPoint());

  // TODO(vegorov): allocate box in the driver loop to avoid pushing and poping.
  compiler->GenerateCall(instance_call()->token_index(),
                         instance_call()->try_index(),
                         &label,
                         PcDescriptors::kOther);
  ASSERT(result == RAX);
  __ popq(value);

  __ testq(value, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, deopt);  // Deoptimize if not Smi.
  __ SmiUntag(value);
  __ cvtsi2sd(XMM0, value);
  __ movsd(FieldAddress(result, Double::value_offset()), XMM0);
}


LocationSummary* PolymorphicInstanceCallComp::MakeLocationSummary() const {
  return MakeCallSummary();
}


void PolymorphicInstanceCallComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  ASSERT(instance_call()->VerifyComputation());
  Label* deopt = compiler->AddDeoptStub(instance_call()->cid(),
                                        instance_call()->token_index(),
                                        instance_call()->try_index(),
                                        kDeoptPolymorphicInstanceCallTestFail);
  if (!HasICData() || (ic_data()->NumberOfChecks() == 0)) {
    __ jmp(deopt);
    return;
  }
  ASSERT(HasICData());
  ASSERT(ic_data()->num_args_tested() == 1);
  Label handle_smi;
  Label* is_smi_label =
      ic_data()->GetReceiverClassIdAt(0) == kSmi ?  &handle_smi : deopt;
  // Load receiver into RAX.
  __ movq(RAX,
      Address(RSP, (instance_call()->ArgumentCount() - 1) * kWordSize));
  __ testq(RAX, Immediate(kSmiTagMask));
  __ j(ZERO, is_smi_label);
  Label done;
  __ LoadClassId(RDI, RAX);
  compiler->EmitTestAndCall(*ic_data(),
                            RDI,  // Class id register.
                            instance_call()->ArgumentCount(),
                            instance_call()->argument_names(),
                            deopt, &done,  // Labels.
                            instance_call()->cid(),
                            instance_call()->token_index(),
                            instance_call()->try_index());
  if (is_smi_label == &handle_smi) {
    __ Bind(&handle_smi);
    ASSERT(ic_data()->GetReceiverClassIdAt(0) == kSmi);
    const Function& target = Function::ZoneHandle(ic_data()->GetTargetAt(0));
    compiler->GenerateStaticCall(instance_call()->cid(),
                                 instance_call()->token_index(),
                                 instance_call()->try_index(),
                                 target,
                                 instance_call()->ArgumentCount(),
                                 instance_call()->argument_names());
  }
  __ Bind(&done);
}

}  // namespace dart

#undef __

#endif  // defined TARGET_ARCH_X64
