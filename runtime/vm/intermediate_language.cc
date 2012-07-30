// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/intermediate_language.h"

#include "vm/bit_vector.h"
#include "vm/dart_entry.h"
#include "vm/flow_graph_builder.h"
#include "vm/flow_graph_compiler.h"
#include "vm/locations.h"
#include "vm/object.h"
#include "vm/os.h"
#include "vm/scopes.h"
#include "vm/stub_code.h"
#include "vm/symbols.h"

namespace dart {

DECLARE_FLAG(bool, enable_type_checks);


MethodRecognizer::Kind MethodRecognizer::RecognizeKind(
    const Function& function) {
  // Only core library methods can be recognized.
  const Library& core_lib = Library::Handle(Library::CoreLibrary());
  const Library& core_impl_lib = Library::Handle(Library::CoreImplLibrary());
  const Class& function_class = Class::Handle(function.owner());
  if ((function_class.library() != core_lib.raw()) &&
      (function_class.library() != core_impl_lib.raw())) {
    return kUnknown;
  }
  const String& recognize_name = String::Handle(function.name());
  const String& recognize_class = String::Handle(function_class.Name());
  String& test_function_name = String::Handle();
  String& test_class_name = String::Handle();
#define RECOGNIZE_FUNCTION(class_name, function_name, enum_name)               \
  test_function_name = Symbols::New(#function_name);                           \
  test_class_name = Symbols::New(#class_name);                                 \
  if (recognize_name.Equals(test_function_name) &&                             \
      recognize_class.Equals(test_class_name)) {                               \
    return k##enum_name;                                                       \
  }
RECOGNIZED_LIST(RECOGNIZE_FUNCTION)
#undef RECOGNIZE_FUNCTION
  return kUnknown;
}


const char* MethodRecognizer::KindToCString(Kind kind) {
#define KIND_TO_STRING(class_name, function_name, enum_name)                   \
  if (kind == k##enum_name) return #enum_name;
RECOGNIZED_LIST(KIND_TO_STRING)
#undef KIND_TO_STRING
  return "?";
}


// ==== Support for visiting flow graphs.
#define DEFINE_ACCEPT(ShortName, ClassName)                                    \
void ClassName::Accept(FlowGraphVisitor* visitor, BindInstr* instr) {          \
  visitor->Visit##ShortName(this, instr);                                      \
}

FOR_EACH_COMPUTATION(DEFINE_ACCEPT)

#undef DEFINE_ACCEPT


#define DEFINE_ACCEPT(ShortName)                                               \
void ShortName##Instr::Accept(FlowGraphVisitor* visitor) {                     \
  visitor->Visit##ShortName(this);                                             \
}

FOR_EACH_INSTRUCTION(DEFINE_ACCEPT)

#undef DEFINE_ACCEPT


Instruction* Instruction::RemoveFromGraph(bool return_previous) {
  ASSERT(!IsBlockEntry());
  ASSERT(!IsBranch());
  ASSERT(!IsThrow());
  ASSERT(!IsReturn());
  ASSERT(!IsReThrow());
  ASSERT(!IsGoto());
  ASSERT(previous() != NULL);
  Instruction* prev_instr = previous();
  Instruction* next_instr = next();
  ASSERT(next_instr != NULL);
  ASSERT(!next_instr->IsBlockEntry());
  prev_instr->set_next(next_instr);
  next_instr->set_previous(prev_instr);
  // Reset successor and previous instruction to indicate
  // that the instruction is removed from the graph.
  set_previous(NULL);
  set_next(NULL);
  return return_previous ? prev_instr : next_instr;
}


void ForwardInstructionIterator::RemoveCurrentFromGraph() {
  current_ = current_->RemoveFromGraph(true);  // Set current_ to previous.
}


// Default implementation of visiting basic blocks.  Can be overridden.
void FlowGraphVisitor::VisitBlocks() {
  for (intptr_t i = 0; i < block_order_.length(); ++i) {
    BlockEntryInstr* entry = block_order_[i];
    entry->Accept(this);
    for (ForwardInstructionIterator it(entry); !it.Done(); it.Advance()) {
      it.Current()->Accept(this);
    }
  }
}


intptr_t AllocateObjectComp::InputCount() const {
  return arguments().length();
}


intptr_t AllocateObjectWithBoundsCheckComp::InputCount() const {
  return arguments().length();
}


intptr_t CreateArrayComp::InputCount() const {
  return ElementCount() + 1;
}


Value* CreateArrayComp::InputAt(intptr_t i) const {
  if (i == 0) {
    return element_type();
  } else {
    return ElementAt(i - 1);
  }
}


void CreateArrayComp::SetInputAt(intptr_t i, Value* value) {
  if (i == 0) {
    inputs_[0] = value;
  } else {
    (*elements_)[i - 1] = value;
  }
}


intptr_t BranchInstr::InputCount() const {
  return 2;
}


Value* BranchInstr::InputAt(intptr_t i) const {
  if (i == 0) return left();
  if (i == 1) return right();
  UNREACHABLE();
  return NULL;
}


void BranchInstr::SetInputAt(intptr_t i, Value* value) {
  if (i == 0) {
    left_ = value;
  } else if (i == 1) {
    right_ = value;
  } else {
    UNREACHABLE();
  }
}


intptr_t ParallelMoveInstr::InputCount() const {
  UNREACHABLE();
  return 0;
}


Value* ParallelMoveInstr::InputAt(intptr_t i) const {
  UNREACHABLE();
  return NULL;
}


void ParallelMoveInstr::SetInputAt(intptr_t i, Value* value) {
  UNREACHABLE();
}


intptr_t ReThrowInstr::InputCount() const {
  return 2;
}


Value* ReThrowInstr::InputAt(intptr_t i) const {
  if (i == 0) return exception();
  if (i == 1) return stack_trace();
  UNREACHABLE();
  return NULL;
}


void ReThrowInstr::SetInputAt(intptr_t i, Value* value) {
  if (i == 0) {
    exception_ = value;
    return;
  }
  if (i == 1) {
    stack_trace_ = value;
    return;
  }
  UNREACHABLE();
}


intptr_t ThrowInstr::InputCount() const {
  return 1;
}


Value* ThrowInstr::InputAt(intptr_t i) const {
  if (i == 0) return exception();
  UNREACHABLE();
  return NULL;
}


void ThrowInstr::SetInputAt(intptr_t i, Value* value) {
  if (i == 0) {
    exception_ = value;
    return;
  }
  UNREACHABLE();
}


intptr_t GotoInstr::InputCount() const {
  return 0;
}


Value* GotoInstr::InputAt(intptr_t i) const {
  UNREACHABLE();
  return NULL;
}


void GotoInstr::SetInputAt(intptr_t i, Value* value) {
  UNREACHABLE();
}


intptr_t PushArgumentInstr::InputCount() const {
  return 1;
}


Value* PushArgumentInstr::InputAt(intptr_t i) const {
  if (i == 0) return value();
  UNREACHABLE();
  return NULL;
}


void PushArgumentInstr::SetInputAt(intptr_t i, Value* value) {
  if (i == 0) {
    value_ = value;
    return;
  }
  UNREACHABLE();
}


intptr_t ReturnInstr::InputCount() const {
  return 1;
}


Value* ReturnInstr::InputAt(intptr_t i) const {
  if (i == 0) return value();
  UNREACHABLE();
  return NULL;
}


void ReturnInstr::SetInputAt(intptr_t i, Value* value) {
  if (i == 0) {
    value_ = value;
    return;
  }
  UNREACHABLE();
}


intptr_t BindInstr::InputCount() const {
  return computation()->InputCount();
}


Value* BindInstr::InputAt(intptr_t i) const {
  return computation()->InputAt(i);
}


void BindInstr::SetInputAt(intptr_t i, Value* value) {
  computation()->SetInputAt(i, value);
}


intptr_t PhiInstr::InputCount() const {
  return inputs_.length();
}


Value* PhiInstr::InputAt(intptr_t i) const {
  return inputs_[i];
}


void PhiInstr::SetInputAt(intptr_t i, Value* value) {
  inputs_[i] = value;
}


RawAbstractType* PhiInstr::StaticType() const {
  // TODO(regis): Return the least upper bound of the input static types.
  return Type::DynamicType();
}


intptr_t ParameterInstr::InputCount() const {
  return 0;
}


Value* ParameterInstr::InputAt(intptr_t i) const {
  UNREACHABLE();
  return NULL;
}


void ParameterInstr::SetInputAt(intptr_t i, Value* value) {
  UNREACHABLE();
}


RawAbstractType* ParameterInstr::StaticType() const {
  // TODO(regis): Can type feedback provide information about the static type
  // of a passed-in parameter?
  // Note that in checked mode, we could return the static type of the formal
  // parameter. However, this would be wrong if ParameterInstr is used to type
  // check the passed-in parameter, since the type check would then always be
  // wrongly eliminated.
  return Type::DynamicType();
}


intptr_t GraphEntryInstr::InputCount() const {
  return 0;
}


Value* GraphEntryInstr::InputAt(intptr_t i) const {
  UNREACHABLE();
  return NULL;
}


void GraphEntryInstr::SetInputAt(intptr_t i, Value* value) {
  UNREACHABLE();
}


intptr_t TargetEntryInstr::InputCount() const {
  return 0;
}


Value* TargetEntryInstr::InputAt(intptr_t i) const {
  UNREACHABLE();
  return NULL;
}


void TargetEntryInstr::SetInputAt(intptr_t i, Value* value) {
  UNREACHABLE();
}


intptr_t JoinEntryInstr::InputCount() const {
  return 0;
}


Value* JoinEntryInstr::InputAt(intptr_t i) const {
  UNREACHABLE();
  return NULL;
}


void JoinEntryInstr::SetInputAt(intptr_t i, Value* value) {
  UNREACHABLE();
}


intptr_t JoinEntryInstr::IndexOfPredecessor(BlockEntryInstr* pred) const {
  for (intptr_t i = 0; i < predecessors_.length(); ++i) {
    if (predecessors_[i] == pred) return i;
  }
  return -1;
}


// ==== Recording assigned variables.
void Computation::RecordAssignedVars(BitVector* assigned_vars,
                                     intptr_t fixed_parameter_count) {
  // Nothing to do for the base class.
}


void StoreLocalComp::RecordAssignedVars(BitVector* assigned_vars,
                                        intptr_t fixed_parameter_count) {
  if (!local().is_captured()) {
    assigned_vars->Add(local().BitIndexIn(fixed_parameter_count));
  }
}


void Instruction::RecordAssignedVars(BitVector* assigned_vars,
                                     intptr_t fixed_parameter_count) {
  // Nothing to do for the base class.
}


void BindInstr::RecordAssignedVars(BitVector* assigned_vars,
                                   intptr_t fixed_parameter_count) {
  computation()->RecordAssignedVars(assigned_vars, fixed_parameter_count);
}


// ==== Postorder graph traversal.
void GraphEntryInstr::DiscoverBlocks(
    BlockEntryInstr* current_block,
    GrowableArray<BlockEntryInstr*>* preorder,
    GrowableArray<BlockEntryInstr*>* postorder,
    GrowableArray<intptr_t>* parent,
    GrowableArray<BitVector*>* assigned_vars,
    intptr_t variable_count,
    intptr_t fixed_parameter_count) {
  // We only visit this block once, first of all blocks.
  ASSERT(preorder_number() == -1);
  ASSERT(current_block == NULL);
  ASSERT(preorder->is_empty());
  ASSERT(postorder->is_empty());
  ASSERT(parent->is_empty());

  // This node has no parent, indicated by -1.  The preorder number is 0.
  parent->Add(-1);
  set_preorder_number(0);
  preorder->Add(this);
  BitVector* vars =
      (variable_count == 0) ? NULL : new BitVector(variable_count);
  assigned_vars->Add(vars);

  // The graph entry consists of only one instruction.
  set_last_instruction(this);

  // Iteratively traverse all successors.  In the unoptimized code, we will
  // enter the function at the first successor in reverse postorder, so we
  // must visit the normal entry last.
  for (intptr_t i = catch_entries_.length() - 1; i >= 0; --i) {
    catch_entries_[i]->DiscoverBlocks(this, preorder, postorder,
                                      parent, assigned_vars,
                                      variable_count, fixed_parameter_count);
  }
  normal_entry_->DiscoverBlocks(this, preorder, postorder,
                                parent, assigned_vars,
                                variable_count, fixed_parameter_count);

  // Assign postorder number.
  set_postorder_number(postorder->length());
  postorder->Add(this);
}


// Base class implementation used for JoinEntry and TargetEntry.
void BlockEntryInstr::DiscoverBlocks(
    BlockEntryInstr* current_block,
    GrowableArray<BlockEntryInstr*>* preorder,
    GrowableArray<BlockEntryInstr*>* postorder,
    GrowableArray<intptr_t>* parent,
    GrowableArray<BitVector*>* assigned_vars,
    intptr_t variable_count,
    intptr_t fixed_parameter_count) {
  // We have already visited the graph entry, so we can assume current_block
  // is non-null and preorder array is non-empty.
  ASSERT(current_block != NULL);
  ASSERT(!preorder->is_empty());

  // 1. Record control-flow-graph basic-block predecessors.
  AddPredecessor(current_block);

  // 2. If the block has already been reached by the traversal, we are
  // done.  Blocks with a single predecessor cannot have been reached
  // before.
  ASSERT(!IsTargetEntry() || (preorder_number() == -1));
  if (preorder_number() >= 0) return;

  // 3. The current block is the spanning-tree parent.
  parent->Add(current_block->preorder_number());

  // 4. Assign preorder number and add the block entry to the list.
  // Allocate an empty set of assigned variables for the block.
  set_preorder_number(preorder->length());
  preorder->Add(this);
  BitVector* vars =
      (variable_count == 0) ? NULL : new BitVector(variable_count);
  assigned_vars->Add(vars);
  // The preorder, parent, and assigned_vars arrays are all indexed by
  // preorder block number, so they should stay in lockstep.
  ASSERT(preorder->length() == parent->length());
  ASSERT(preorder->length() == assigned_vars->length());

  // 5. Iterate straight-line successors until a branch instruction or
  // another basic block entry instruction, and visit that instruction.
  ASSERT(next() != NULL);
  Instruction* next_instr = next();
  if (next_instr->IsBlockEntry()) {
    set_last_instruction(this);
  } else {
    while ((next_instr != NULL) &&
           !next_instr->IsBlockEntry() &&
           !next_instr->IsBranch()) {
      if (vars != NULL) {
        next_instr->RecordAssignedVars(vars, fixed_parameter_count);
      }
      set_last_instruction(next_instr);
      GotoInstr* goto_instr = next_instr->AsGoto();
      next_instr =
          (goto_instr != NULL) ? goto_instr->successor() : next_instr->next();
    }
  }
  if (next_instr != NULL) {
    next_instr->DiscoverBlocks(this, preorder, postorder,
                               parent, assigned_vars,
                               variable_count, fixed_parameter_count);
  }

  // 6. Assign postorder number and add the block entry to the list.
  set_postorder_number(postorder->length());
  postorder->Add(this);
}


void BranchInstr::DiscoverBlocks(
    BlockEntryInstr* current_block,
    GrowableArray<BlockEntryInstr*>* preorder,
    GrowableArray<BlockEntryInstr*>* postorder,
    GrowableArray<intptr_t>* parent,
    GrowableArray<BitVector*>* assigned_vars,
    intptr_t variable_count,
    intptr_t fixed_parameter_count) {
  current_block->set_last_instruction(this);
  // Visit the false successor before the true successor so they appear in
  // true/false order in reverse postorder used as the block ordering in the
  // nonoptimizing compiler.
  ASSERT(true_successor_ != NULL);
  ASSERT(false_successor_ != NULL);
  false_successor_->DiscoverBlocks(current_block, preorder, postorder,
                                   parent, assigned_vars,
                                   variable_count, fixed_parameter_count);
  true_successor_->DiscoverBlocks(current_block, preorder, postorder,
                                  parent, assigned_vars,
                                  variable_count, fixed_parameter_count);
}


void JoinEntryInstr::InsertPhi(intptr_t var_index, intptr_t var_count) {
  // Lazily initialize the array of phis.
  // Currently, phis are stored in a sparse array that holds the phi
  // for variable with index i at position i.
  // TODO(fschneider): Store phis in a more compact way.
  if (phis_ == NULL) {
    phis_ = new ZoneGrowableArray<PhiInstr*>(var_count);
    for (intptr_t i = 0; i < var_count; i++) {
      phis_->Add(NULL);
    }
  }
  ASSERT((*phis_)[var_index] == NULL);
  (*phis_)[var_index] = new PhiInstr(PredecessorCount());
  phi_count_++;
}


intptr_t Instruction::SuccessorCount() const {
  return 0;
}


BlockEntryInstr* Instruction::SuccessorAt(intptr_t index) const {
  // Called only if index is in range.  Only control-transfer instructions
  // can have non-zero successor counts and they override this function.
  UNREACHABLE();
  return NULL;
}


intptr_t GraphEntryInstr::SuccessorCount() const {
  return 1 + catch_entries_.length();
}


BlockEntryInstr* GraphEntryInstr::SuccessorAt(intptr_t index) const {
  if (index == 0) return normal_entry_;
  return catch_entries_[index - 1];
}


intptr_t BranchInstr::SuccessorCount() const {
  return 2;
}


BlockEntryInstr* BranchInstr::SuccessorAt(intptr_t index) const {
  if (index == 0) return true_successor_;
  if (index == 1) return false_successor_;
  UNREACHABLE();
  return NULL;
}


intptr_t GotoInstr::SuccessorCount() const {
  return 1;
}


BlockEntryInstr* GotoInstr::SuccessorAt(intptr_t index) const {
  ASSERT(index == 0);
  return successor();
}


void Instruction::Goto(JoinEntryInstr* entry) {
  set_next(new GotoInstr(entry));
}


// ==== Support for propagating static type.
RawAbstractType* ConstantVal::StaticType() const {
  if (value().IsInstance()) {
    return Instance::Cast(value()).GetType();
  } else {
    UNREACHABLE();
    return AbstractType::null();
  }
}


RawAbstractType* UseVal::StaticType() const {
  return definition()->StaticType();
}


RawAbstractType* AssertAssignableComp::StaticType() const {
  const AbstractType& value_static_type =
      AbstractType::Handle(value()->StaticType());
  if (value_static_type.IsMoreSpecificThan(dst_type(), NULL)) {
    return value_static_type.raw();
  }
  return dst_type().raw();
}


RawAbstractType* AssertBooleanComp::StaticType() const {
  return Type::BoolInterface();
}


RawAbstractType* CurrentContextComp::StaticType() const {
  UNREACHABLE();
  return AbstractType::null();
}


RawAbstractType* StoreContextComp::StaticType() const {
  UNREACHABLE();
  return AbstractType::null();
}


RawAbstractType* ClosureCallComp::StaticType() const {
  // Because of function subtyping rules, the static return type of a closure
  // call cannot be relied upon for static type analysis. For example, a
  // function returning Dynamic can be assigned to a closure variable declared
  // to return int and may actually return a double at run-time.
  return Type::DynamicType();
}


RawAbstractType* InstanceCallComp::StaticType() const {
  // TODO(regis): Return a more specific type than Dynamic for recognized
  // combinations of receiver static type and method name.
  return Type::DynamicType();
}


RawAbstractType* PolymorphicInstanceCallComp::StaticType() const {
  return Type::DynamicType();
}


RawAbstractType* StaticCallComp::StaticType() const {
  return function().result_type();
}


RawAbstractType* LoadLocalComp::StaticType() const {
  // TODO(regis): Verify that the type of the receiver is properly set.
  if (FLAG_enable_type_checks) {
    return local().type().raw();
  }
  return Type::DynamicType();
}


RawAbstractType* StoreLocalComp::StaticType() const {
  return value()->StaticType();
}


RawAbstractType* StrictCompareComp::StaticType() const {
  return Type::BoolInterface();
}


RawAbstractType* EqualityCompareComp::StaticType() const {
  return Type::BoolInterface();
}


RawAbstractType* RelationalOpComp::StaticType() const {
  return Type::BoolInterface();
}


RawAbstractType* NativeCallComp::StaticType() const {
  // The result type of the native function is identical to the result type of
  // the enclosing native Dart function. However, we prefer to check the type
  // of the value returned from the native call.
  return Type::DynamicType();
}


RawAbstractType* LoadIndexedComp::StaticType() const {
  return Type::DynamicType();
}


RawAbstractType* StoreIndexedComp::StaticType() const {
  UNREACHABLE();
  return AbstractType::null();
}


RawAbstractType* InstanceSetterComp::StaticType() const {
  return value()->StaticType();
}


RawAbstractType* StaticSetterComp::StaticType() const {
  return value()->StaticType();
}


RawAbstractType* LoadInstanceFieldComp::StaticType() const {
  if (FLAG_enable_type_checks) {
    return field().type();
  }
  return Type::DynamicType();
}


RawAbstractType* StoreInstanceFieldComp::StaticType() const {
  return value()->StaticType();
}


RawAbstractType* LoadStaticFieldComp::StaticType() const {
  if (FLAG_enable_type_checks) {
    return field().type();
  }
  return Type::DynamicType();
}


RawAbstractType* StoreStaticFieldComp::StaticType() const {
  return value()->StaticType();
}


RawAbstractType* BooleanNegateComp::StaticType() const {
  return Type::BoolInterface();
}


RawAbstractType* InstanceOfComp::StaticType() const {
  return Type::BoolInterface();
}


RawAbstractType* CreateArrayComp::StaticType() const {
  UNREACHABLE();
  return AbstractType::null();
}


RawAbstractType* CreateClosureComp::StaticType() const {
  const Function& fun = function();
  const Class& signature_class = Class::Handle(fun.signature_class());
  return signature_class.SignatureType();
}


RawAbstractType* AllocateObjectComp::StaticType() const {
  UNREACHABLE();
  return AbstractType::null();
}


RawAbstractType* AllocateObjectWithBoundsCheckComp::StaticType() const {
  UNREACHABLE();
  return AbstractType::null();
}


RawAbstractType* LoadVMFieldComp::StaticType() const {
  ASSERT(!type().IsNull());
  return type().raw();
}


RawAbstractType* StoreVMFieldComp::StaticType() const {
  return value()->StaticType();
}


RawAbstractType* InstantiateTypeArgumentsComp::StaticType() const {
  UNREACHABLE();
  return AbstractType::null();
}


RawAbstractType* ExtractConstructorTypeArgumentsComp::StaticType() const {
  UNREACHABLE();
  return AbstractType::null();
}


RawAbstractType* ExtractConstructorInstantiatorComp::StaticType() const {
  UNREACHABLE();
  return AbstractType::null();
}


RawAbstractType* AllocateContextComp::StaticType() const {
  UNREACHABLE();
  return AbstractType::null();
}


RawAbstractType* ChainContextComp::StaticType() const {
  UNREACHABLE();
  return AbstractType::null();
}


RawAbstractType* CloneContextComp::StaticType() const {
  UNREACHABLE();
  return AbstractType::null();
}


RawAbstractType* CatchEntryComp::StaticType() const {
  UNREACHABLE();
  return AbstractType::null();
}


RawAbstractType* CheckStackOverflowComp::StaticType() const {
  UNREACHABLE();
  return AbstractType::null();
}


RawAbstractType* BinaryOpComp::StaticType() const {
  // TODO(srdjan): Compute based on input types (ICData).
  return Type::DynamicType();
}


RawAbstractType* UnarySmiOpComp::StaticType() const {
  return Type::IntInterface();
}


RawAbstractType* NumberNegateComp::StaticType() const {
  return Type::NumberInterface();
}


RawAbstractType* ToDoubleComp::StaticType() const {
  return Type::DoubleInterface();
}


// Shared code generation methods (EmitNativeCode, MakeLocationSummary, and
// PrepareEntry). Only assembly code that can be shared across all architectures
// can be used. Machine specific register allocation and code generation
// is located in intermediate_language_<arch>.cc


// True iff. the arguments to a call will be properly pushed and can
// be popped after the call.
template <typename T> static bool VerifyCallComputation(T* comp) {
  // Argument values should be consecutive temps.
  //
  // TODO(kmillikin): implement stack height tracking so we can also assert
  // they are on top of the stack.
  intptr_t previous = -1;
  for (int i = 0; i < comp->ArgumentCount(); ++i) {
    Value* val = comp->ArgumentAt(i);
    if (!val->IsUse()) return false;
    intptr_t current = val->AsUse()->definition()->temp_index();
    if (i != 0) {
      if (current != (previous + 1)) return false;
    }
    previous = current;
  }
  return true;
}


#define __ compiler->assembler()->

void GraphEntryInstr::PrepareEntry(FlowGraphCompiler* compiler) {
  // Nothing to do.
}


void JoinEntryInstr::PrepareEntry(FlowGraphCompiler* compiler) {
  __ Bind(compiler->GetBlockLabel(this));
}


void TargetEntryInstr::PrepareEntry(FlowGraphCompiler* compiler) {
  __ Bind(compiler->GetBlockLabel(this));
  if (HasTryIndex()) {
    compiler->AddExceptionHandler(try_index(),
                                  compiler->assembler()->CodeSize());
  }
}


LocationSummary* StoreInstanceFieldComp::MakeLocationSummary() const {
  const intptr_t kNumInputs = 2;
  const intptr_t num_temps = HasICData() ? 1 : 0;
  LocationSummary* summary = new LocationSummary(kNumInputs, num_temps);
  summary->set_in(0, Location::RequiresRegister());
  summary->set_in(1, Location::RequiresRegister());
  if (HasICData()) {
    summary->set_temp(0, Location::RequiresRegister());
  }
  return summary;
}


void StoreInstanceFieldComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register instance_reg = locs()->in(0).reg();
  Register value_reg = locs()->in(1).reg();

  if (HasICData()) {
    ASSERT(original() != NULL);
    Label* deopt = compiler->AddDeoptStub(original()->cid(),
                                          original()->token_pos(),
                                          original()->try_index(),
                                          kDeoptInstanceGetterSameTarget,
                                          instance_reg,
                                          value_reg);
    // Smis do not have instance fields (Smi class is always first).
    Register temp_reg = locs()->temp(0).reg();
    ASSERT(temp_reg != instance_reg);
    ASSERT(temp_reg != value_reg);
    ASSERT(ic_data() != NULL);
    compiler->EmitClassChecksNoSmi(*ic_data(), instance_reg, temp_reg, deopt);
  }
  __ StoreIntoObject(instance_reg, FieldAddress(instance_reg, field().Offset()),
                     value_reg);
}


LocationSummary* ThrowInstr::MakeLocationSummary() const {
  const int kNumInputs = 0;
  const int kNumTemps = 0;
  return new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
}



void ThrowInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  ASSERT(exception()->IsUse());
  compiler->GenerateCallRuntime(cid(),
                                token_pos(),
                                try_index(),
                                kThrowRuntimeEntry);
  __ int3();
}


LocationSummary* ReThrowInstr::MakeLocationSummary() const {
  const int kNumInputs = 0;
  const int kNumTemps = 0;
  return new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
}


void ReThrowInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  ASSERT(exception()->IsUse());
  ASSERT(stack_trace()->IsUse());
  compiler->GenerateCallRuntime(cid(),
                                token_pos(),
                                try_index(),
                                kReThrowRuntimeEntry);
  __ int3();
}


LocationSummary* GotoInstr::MakeLocationSummary() const {
  return new LocationSummary(0, 0);
}


void GotoInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  // We can fall through if the successor is the next block in the list.
  // Otherwise, we need a jump.
  if (!compiler->IsNextBlock(successor())) {
    __ jmp(compiler->GetBlockLabel(successor()));
  }
}


static Condition NegateCondition(Condition condition) {
  switch (condition) {
    case EQUAL:         return NOT_EQUAL;
    case NOT_EQUAL:     return EQUAL;
    case LESS:          return GREATER_EQUAL;
    case LESS_EQUAL:    return GREATER;
    case GREATER:       return LESS_EQUAL;
    case GREATER_EQUAL: return LESS;
    case BELOW:         return ABOVE_EQUAL;
    case BELOW_EQUAL:   return ABOVE;
    case ABOVE:         return BELOW_EQUAL;
    case ABOVE_EQUAL:   return BELOW;
    default:
      OS::Print("Error %d\n", condition);
      UNIMPLEMENTED();
      return EQUAL;
  }
}


void BranchInstr::EmitBranchOnCondition(FlowGraphCompiler* compiler,
                                        Condition true_condition) {
  if (compiler->IsNextBlock(false_successor())) {
    // If the next block is the false successor we will fall through to it.
    __ j(true_condition, compiler->GetBlockLabel(true_successor()));
  } else {
    // If the next block is the true successor we negate comparison and fall
    // through to it.
    ASSERT(compiler->IsNextBlock(true_successor()));
    Condition false_condition = NegateCondition(true_condition);
    __ j(false_condition, compiler->GetBlockLabel(false_successor()));
  }
}


LocationSummary* CurrentContextComp::MakeLocationSummary() const {
  return LocationSummary::Make(0, Location::RequiresRegister());
}


void CurrentContextComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  __ MoveRegister(locs()->out().reg(), CTX);
}


LocationSummary* StoreContextComp::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary = new LocationSummary(kNumInputs, kNumTemps);
  summary->set_in(0, Location::RegisterLocation(CTX));
  return summary;
}


void StoreContextComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  // Nothing to do.  Context register were loaded by register allocator.
  ASSERT(locs()->in(0).reg() == CTX);
}


LocationSummary* StrictCompareComp::MakeLocationSummary() const {
  return LocationSummary::Make(2, Location::SameAsFirstInput());
}


void StrictCompareComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register left = locs()->in(0).reg();
  Register right = locs()->in(1).reg();

  ASSERT(kind() == Token::kEQ_STRICT || kind() == Token::kNE_STRICT);
  Condition true_condition = (kind() == Token::kEQ_STRICT) ? EQUAL : NOT_EQUAL;
  __ CompareRegisters(left, right);

  Register result = locs()->out().reg();
  Label load_true, done;
  __ j(true_condition, &load_true, Assembler::kNearJump);
  __ LoadObject(result, compiler->bool_false());
  __ jmp(&done, Assembler::kNearJump);
  __ Bind(&load_true);
  __ LoadObject(result, compiler->bool_true());
  __ Bind(&done);
}


void ClosureCallComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  // The arguments to the stub include the closure.  The arguments
  // descriptor describes the closure's arguments (and so does not include
  // the closure).
  Register temp_reg = locs()->temp(0).reg();
  int argument_count = ArgumentCount();
  const Array& arguments_descriptor =
      DartEntry::ArgumentsDescriptor(argument_count - 1,
                                         argument_names());
  __ LoadObject(temp_reg, arguments_descriptor);

  compiler->GenerateCall(token_pos(),
                         try_index(),
                         &StubCode::CallClosureFunctionLabel(),
                         PcDescriptors::kOther);
  __ Drop(argument_count);
}


LocationSummary* InstanceCallComp::MakeLocationSummary() const {
  return MakeCallSummary();
}


void InstanceCallComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  compiler->AddCurrentDescriptor(PcDescriptors::kDeopt,
                                 cid(),
                                 token_pos(),
                                 try_index());
  compiler->GenerateInstanceCall(cid(),
                                 token_pos(),
                                 try_index(),
                                 function_name(),
                                 ArgumentCount(),
                                 argument_names(),
                                 checked_argument_count());
}


LocationSummary* StaticCallComp::MakeLocationSummary() const {
  return MakeCallSummary();
}


void StaticCallComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  Label done;
  if (recognized() == MethodRecognizer::kMathSqrt) {
    compiler->GenerateInlinedMathSqrt(&done);
    // Falls through to static call when operand type is not double or smi.
  }
  compiler->GenerateStaticCall(cid(),
                               token_pos(),
                               try_index(),
                               function(),
                               ArgumentCount(),
                               argument_names());
  __ Bind(&done);
}


LocationSummary* UseVal::MakeLocationSummary() const {
  return NULL;
}


void UseVal::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


void AssertAssignableComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  compiler->GenerateAssertAssignable(cid(),
                                     token_pos(),
                                     try_index(),
                                     dst_type(),
                                     dst_name());
  ASSERT(locs()->in(0).reg() == locs()->out().reg());
}


LocationSummary* StoreStaticFieldComp::MakeLocationSummary() const {
  LocationSummary* locs = new LocationSummary(1, 1);
  locs->set_in(0, Location::RequiresRegister());
  locs->set_temp(0, Location::RequiresRegister());
  locs->set_out(Location::SameAsFirstInput());
  return locs;
}


void StoreStaticFieldComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register value = locs()->in(0).reg();
  Register temp = locs()->temp(0).reg();
  ASSERT(locs()->out().reg() == value);

  __ LoadObject(temp, field());
  __ StoreIntoObject(temp, FieldAddress(temp, Field::value_offset()), value);
}


LocationSummary* BooleanNegateComp::MakeLocationSummary() const {
  return LocationSummary::Make(1, Location::RequiresRegister());
}


void BooleanNegateComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register value = locs()->in(0).reg();
  Register result = locs()->out().reg();

  Label done;
  __ LoadObject(result, compiler->bool_true());
  __ CompareRegisters(result, value);
  __ j(NOT_EQUAL, &done, Assembler::kNearJump);
  __ LoadObject(result, compiler->bool_false());
  __ Bind(&done);
}


LocationSummary* ChainContextComp::MakeLocationSummary() const {
  return LocationSummary::Make(1, Location::NoLocation());
}


void ChainContextComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register context_value = locs()->in(0).reg();

  // Chain the new context in context_value to its parent in CTX.
  __ StoreIntoObject(context_value,
                     FieldAddress(context_value, Context::parent_offset()),
                     CTX);
  // Set new context as current context.
  __ MoveRegister(CTX, context_value);
}


LocationSummary* StoreVMFieldComp::MakeLocationSummary() const {
  return LocationSummary::Make(2, Location::SameAsFirstInput());
}


void StoreVMFieldComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register value_reg = locs()->in(0).reg();
  Register dest_reg = locs()->in(1).reg();
  ASSERT(value_reg == locs()->out().reg());

  __ StoreIntoObject(dest_reg, FieldAddress(dest_reg, offset_in_bytes()),
                     value_reg);
}


LocationSummary* AllocateObjectComp::MakeLocationSummary() const {
  return MakeCallSummary();
}


void AllocateObjectComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  const Class& cls = Class::ZoneHandle(constructor().owner());
  const Code& stub = Code::Handle(StubCode::GetAllocationStubForClass(cls));
  const ExternalLabel label(cls.ToCString(), stub.EntryPoint());
  compiler->GenerateCall(token_pos(),
                         try_index(),
                         &label,
                         PcDescriptors::kOther);
  __ Drop(arguments().length());  // Discard arguments.
}


LocationSummary* CreateClosureComp::MakeLocationSummary() const {
  return MakeCallSummary();
}


void CreateClosureComp::EmitNativeCode(FlowGraphCompiler* compiler) {
  const Function& closure_function = function();
  const Code& stub = Code::Handle(
      StubCode::GetAllocationStubForClosure(closure_function));
  const ExternalLabel label(closure_function.ToCString(), stub.EntryPoint());
  compiler->GenerateCall(token_pos(), try_index(), &label,
                         PcDescriptors::kOther);
  __ Drop(2);  // Discard type arguments and receiver.
}


LocationSummary* PushArgumentInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps= 0;
  LocationSummary* locs = new LocationSummary(kNumInputs, kNumTemps);
  // TODO(fschneider): Use Any() once it is supported by all code generators.
  locs->set_in(0, Location::RequiresRegister());
  return locs;
}


void PushArgumentInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  // In SSA mode, we need an explicit push. Nothing to do in non-SSA mode
  // where PushArgument is handled in FrameRegisterAllocator::AllocateRegisters.
  // Instead of popping the value it is left alone on the simulated frame
  // and materialized on the physical stack before the call.
  // TODO(fschneider): Avoid special-casing for SSA mode here.
  if (compiler->is_ssa()) {
    ASSERT(locs()->in(0).IsRegister());
    __ PushRegister(locs()->in(0).reg());
  }
}


#undef __

}  // namespace dart
