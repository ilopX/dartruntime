// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef VM_INTERMEDIATE_LANGUAGE_H_
#define VM_INTERMEDIATE_LANGUAGE_H_

#include "vm/allocation.h"
#include "vm/ast.h"
#include "vm/growable_array.h"
#include "vm/handles_impl.h"
#include "vm/locations.h"
#include "vm/object.h"

namespace dart {

// TODO(srdjan): Add _ByteArrayBase, get:length.

#define RECOGNIZED_LIST(V)                                                     \
  V(ObjectArray, get:length, ObjectArrayLength)                                \
  V(ImmutableArray, get:length, ImmutableArrayLength)                          \
  V(GrowableObjectArray, get:length, GrowableArrayLength)                      \
  V(StringBase, get:length, StringBaseLength)                                  \
  V(IntegerImplementation, toDouble, IntegerToDouble)                          \
  V(Double, toDouble, DoubleToDouble)                                          \
  V(Math, sqrt, MathSqrt)                                                      \

// Class that recognizes the name and owner of a function and returns the
// corresponding enum. See RECOGNIZED_LIST above for list of recognizable
// functions.
class MethodRecognizer : public AllStatic {
 public:
  enum Kind {
    kUnknown,
#define DEFINE_ENUM_LIST(class_name, function_name, enum_name) k##enum_name,
RECOGNIZED_LIST(DEFINE_ENUM_LIST)
#undef DEFINE_ENUM_LIST
  };

  static Kind RecognizeKind(const Function& function);
  static const char* KindToCString(Kind kind);
};


class BitVector;
class FlowGraphCompiler;
class FlowGraphVisitor;
class Function;
class LocalVariable;

// M is a two argument macro.  It is applied to each concrete value's
// typename and classname.
#define FOR_EACH_VALUE(M)                                                      \
  M(Use, UseVal)                                                               \
  M(Constant, ConstantVal)                                                     \


// M is a two argument macro.  It is applied to each concrete instruction's
// (including the values) typename and classname.
#define FOR_EACH_COMPUTATION(M)                                                \
  FOR_EACH_VALUE(M)                                                            \
  M(AssertAssignable, AssertAssignableComp)                                    \
  M(AssertBoolean, AssertBooleanComp)                                          \
  M(CurrentContext, CurrentContextComp)                                        \
  M(StoreContext, StoreContextComp)                                            \
  M(ClosureCall, ClosureCallComp)                                              \
  M(InstanceCall, InstanceCallComp)                                            \
  M(PolymorphicInstanceCall, PolymorphicInstanceCallComp)                      \
  M(StaticCall, StaticCallComp)                                                \
  M(LoadLocal, LoadLocalComp)                                                  \
  M(StoreLocal, StoreLocalComp)                                                \
  M(StrictCompare, StrictCompareComp)                                          \
  M(EqualityCompare, EqualityCompareComp)                                      \
  M(RelationalOp, RelationalOpComp)                                            \
  M(NativeCall, NativeCallComp)                                                \
  M(LoadIndexed, LoadIndexedComp)                                              \
  M(StoreIndexed, StoreIndexedComp)                                            \
  M(InstanceSetter, InstanceSetterComp)                                        \
  M(StaticSetter, StaticSetterComp)                                            \
  M(LoadInstanceField, LoadInstanceFieldComp)                                  \
  M(StoreInstanceField, StoreInstanceFieldComp)                                \
  M(LoadStaticField, LoadStaticFieldComp)                                      \
  M(StoreStaticField, StoreStaticFieldComp)                                    \
  M(BooleanNegate, BooleanNegateComp)                                          \
  M(InstanceOf, InstanceOfComp)                                                \
  M(CreateArray, CreateArrayComp)                                              \
  M(CreateClosure, CreateClosureComp)                                          \
  M(AllocateObject, AllocateObjectComp)                                        \
  M(AllocateObjectWithBoundsCheck, AllocateObjectWithBoundsCheckComp)          \
  M(LoadVMField, LoadVMFieldComp)                                              \
  M(StoreVMField, StoreVMFieldComp)                                            \
  M(InstantiateTypeArguments, InstantiateTypeArgumentsComp)                    \
  M(ExtractConstructorTypeArguments, ExtractConstructorTypeArgumentsComp)      \
  M(ExtractConstructorInstantiator, ExtractConstructorInstantiatorComp)        \
  M(AllocateContext, AllocateContextComp)                                      \
  M(ChainContext, ChainContextComp)                                            \
  M(CloneContext, CloneContextComp)                                            \
  M(CatchEntry, CatchEntryComp)                                                \
  M(BinaryOp, BinaryOpComp)                                                    \
  M(UnarySmiOp, UnarySmiOpComp)                                                \
  M(NumberNegate, NumberNegateComp)                                            \
  M(CheckStackOverflow, CheckStackOverflowComp)                                \
  M(ToDouble, ToDoubleComp)                                                    \


#define FORWARD_DECLARATION(ShortName, ClassName) class ClassName;
FOR_EACH_COMPUTATION(FORWARD_DECLARATION)
#undef FORWARD_DECLARATION

// Forward declarations.
class BufferFormatter;
class BranchInstr;
class Instruction;
class Value;


class Computation : public ZoneAllocated {
 public:
  static const int kNoCid = -1;

  Computation() : cid_(-1), ic_data_(NULL), instr_(NULL), locs_(NULL) {
    Isolate* isolate = Isolate::Current();
    cid_ = GetNextCid(isolate);
    ic_data_ = GetICDataForCid(cid_, isolate);
  }

  // Unique computation/instruction id, used for deoptimization.
  intptr_t cid() const { return cid_; }

  ICData* ic_data() const { return ic_data_; }
  void set_ic_data(ICData* value) { ic_data_ = value; }
  bool HasICData() const {
    return (ic_data() != NULL) && !ic_data()->IsNull();
  }

  // Visiting support.
  virtual void Accept(FlowGraphVisitor* visitor) = 0;

  virtual intptr_t InputCount() const = 0;
  virtual Value* InputAt(intptr_t i) const = 0;
  virtual void SetInputAt(intptr_t i, Value* value) = 0;

  // Static type of the computation.
  virtual RawAbstractType* StaticType() const = 0;

  // Mutate assigned_vars to add the local variable index for all
  // frame-allocated locals assigned to by the computation.
  virtual void RecordAssignedVars(BitVector* assigned_vars);

  virtual const char* DebugName() const = 0;

  // Printing support. These functions are sometimes overridden for custom
  // formatting. Otherwise, it prints in the format "opcode(op1, op2, op3)".
  virtual void PrintTo(BufferFormatter* f) const;
  virtual void PrintOperandsTo(BufferFormatter* f) const;

  // Returns structure describing location constraints required
  // to emit native code for this computation.
  LocationSummary* locs() {
    if (locs_ == NULL) {
      locs_ = MakeLocationSummary();
    }
    return locs_;
  }

  // TODO(srdjan): Eliminate Instructions hierarchy. If 'use' is NULL
  // it acts as a DoInstr, otherwise a BindInstr.
  void set_instr(Instruction* value) { instr_ = value; }
  Instruction* instr() const { return instr_; }

  // Create a location summary for this computation.
  // TODO(fschneider): Temporarily returns NULL for instructions
  // that are not yet converted to the location based code generation.
  virtual LocationSummary* MakeLocationSummary() const = 0;

  // TODO(fschneider): Make EmitNativeCode and locs const.
  virtual void EmitNativeCode(FlowGraphCompiler* compiler) = 0;

  static LocationSummary* MakeCallSummary();

  void ReplaceWith(Computation* other);

  // Declare an enum value used to define type-test predicates.
  enum ComputationType {
#define DECLARE_COMPUTATION_TYPE(ShortName, ClassName) k##ShortName,

  FOR_EACH_COMPUTATION(DECLARE_COMPUTATION_TYPE)

#undef DECLARE_COMPUTATION_TYPE
  };

  virtual ComputationType computation_type() const = 0;

  // Declare predicate for each computation.
#define DECLARE_PREDICATE(ShortName, ClassName)                             \
  inline bool Is##ShortName() const;                                        \
  inline ClassName* As##ShortName();
  FOR_EACH_COMPUTATION(DECLARE_PREDICATE)
#undef DECLARE_PREDICATE

 private:
  friend class Instruction;
  static intptr_t GetNextCid(Isolate* isolate) {
    intptr_t tmp = isolate->computation_id();
    isolate->set_computation_id(tmp + 1);
    return tmp;
  }
  static ICData* GetICDataForCid(intptr_t cid, Isolate* isolate) {
    if (isolate->ic_data_array() == Array::null()) {
      return NULL;
    } else {
      const Array& array_handle = Array::Handle(isolate->ic_data_array());
      if (cid >= array_handle.Length()) {
        // For computations being added in the optimizing compiler.
        return NULL;
      }
      ICData& ic_data_handle = ICData::ZoneHandle();
      ic_data_handle ^= array_handle.At(cid);
      return &ic_data_handle;
    }
  }

  intptr_t cid_;
  ICData* ic_data_;
  Instruction* instr_;
  LocationSummary* locs_;

  DISALLOW_COPY_AND_ASSIGN(Computation);
};


// An embedded container with N elements of type T.  Used (with partial
// specialization for N=0) because embedded arrays cannot have size 0.
template<typename T, intptr_t N>
class EmbeddedArray {
 public:
  EmbeddedArray() {
    for (intptr_t i = 0; i < N; i++) elements_[i] = NULL;
  }

  intptr_t length() const { return N; }

  const T& operator[](intptr_t i) const {
    ASSERT(i < length());
    return elements_[i];
  }

  T& operator[](intptr_t i) {
    ASSERT(i < length());
    return elements_[i];
  }

  const T& At(intptr_t i) const {
    return (*this)[i];
  }

  void SetAt(intptr_t i, const T& val) {
    (*this)[i] = val;
  }

 private:
  T elements_[N];
};


template<typename T>
class EmbeddedArray<T, 0> {
 public:
  intptr_t length() const { return 0; }
  const T& operator[](intptr_t i) const {
    UNREACHABLE();
    static T sentinel = 0;
    return sentinel;
  }
  T& operator[](intptr_t i) {
    UNREACHABLE();
    static T sentinel = 0;
    return sentinel;
  }
};


template<intptr_t N>
class TemplateComputation : public Computation {
 public:
  virtual intptr_t InputCount() const { return N; }
  virtual Value* InputAt(intptr_t i) const { return inputs_[i]; }
  virtual void SetInputAt(intptr_t i, Value* value) { inputs_[i] = value; }

 protected:
  EmbeddedArray<Value*, N> inputs_;
};


class Value : public TemplateComputation<0> {
 public:
  Value() { }

 private:
  DISALLOW_COPY_AND_ASSIGN(Value);
};


// Functions defined in all concrete computation classes.
#define DECLARE_COMPUTATION(ShortName)                                         \
  virtual void Accept(FlowGraphVisitor* visitor);                              \
  virtual ComputationType computation_type() const {                           \
    return Computation::k##ShortName;                                          \
  }                                                                            \
  virtual const char* DebugName() const { return #ShortName; }                 \
  virtual RawAbstractType* StaticType() const;                                 \
  virtual LocationSummary* MakeLocationSummary() const;                        \
  virtual void EmitNativeCode(FlowGraphCompiler* compiler);

// Functions defined in all concrete value classes.
#define DECLARE_VALUE(ShortName)                                               \
  DECLARE_COMPUTATION(ShortName)                                               \
  virtual void PrintTo(BufferFormatter* f) const;


class Definition;
class BindInstr;
class PhiInstr;

class UseVal : public Value {
 public:
  explicit UseVal(Definition* definition) : definition_(definition) {}

  DECLARE_VALUE(Use)

  inline Definition* definition() const;
  void set_definition(Definition* definition) {
    definition_ = definition;
  }

 private:
  Definition* definition_;

  DISALLOW_COPY_AND_ASSIGN(UseVal);
};


class ConstantVal: public Value {
 public:
  explicit ConstantVal(const Object& value)
      : value_(value) {
    ASSERT(value.IsZoneHandle());
  }

  DECLARE_VALUE(Constant)

  const Object& value() const { return value_; }

 private:
  const Object& value_;

  DISALLOW_COPY_AND_ASSIGN(ConstantVal);
};

#undef DECLARE_VALUE


class AssertAssignableComp : public TemplateComputation<3> {
 public:
  AssertAssignableComp(intptr_t token_pos,
                       intptr_t try_index,
                       Value* value,
                       Value* instantiator,
                       Value* instantiator_type_arguments,
                       const AbstractType& dst_type,
                       const String& dst_name)
      : token_pos_(token_pos),
        try_index_(try_index),
        dst_type_(dst_type),
        dst_name_(dst_name) {
    ASSERT(value != NULL);
    ASSERT(instantiator != NULL);
    ASSERT(instantiator_type_arguments != NULL);
    ASSERT(!dst_type.IsNull());
    ASSERT(!dst_name.IsNull());
    inputs_[0] = value;
    inputs_[1] = instantiator;
    inputs_[2] = instantiator_type_arguments;
  }

  DECLARE_COMPUTATION(AssertAssignable)

  Value* value() const { return inputs_[0]; }
  Value* instantiator() const { return inputs_[1]; }
  Value* instantiator_type_arguments() const { return inputs_[2]; }

  intptr_t token_pos() const { return token_pos_; }
  intptr_t try_index() const { return try_index_; }
  const AbstractType& dst_type() const { return dst_type_; }
  const String& dst_name() const { return dst_name_; }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const intptr_t token_pos_;
  const intptr_t try_index_;
  const AbstractType& dst_type_;
  const String& dst_name_;

  DISALLOW_COPY_AND_ASSIGN(AssertAssignableComp);
};


class AssertBooleanComp : public TemplateComputation<1> {
 public:
  AssertBooleanComp(intptr_t token_pos,
                    intptr_t try_index,
                    Value* value)
      : token_pos_(token_pos),
        try_index_(try_index) {
    ASSERT(value != NULL);
    inputs_[0] = value;
  }

  DECLARE_COMPUTATION(AssertBoolean)

  intptr_t token_pos() const { return token_pos_; }
  intptr_t try_index() const { return try_index_; }
  Value* value() const { return inputs_[0]; }

 private:
  const intptr_t token_pos_;
  const intptr_t try_index_;

  DISALLOW_COPY_AND_ASSIGN(AssertBooleanComp);
};


// Denotes the current context, normally held in a register.  This is
// a computation, not a value, because it's mutable.
class CurrentContextComp : public TemplateComputation<0> {
 public:
  CurrentContextComp() { }

  DECLARE_COMPUTATION(CurrentContext)

 private:
  DISALLOW_COPY_AND_ASSIGN(CurrentContextComp);
};


class StoreContextComp : public TemplateComputation<1> {
 public:
  explicit StoreContextComp(Value* value) {
    ASSERT(value != NULL);
    inputs_[0] = value;
  }

  DECLARE_COMPUTATION(StoreContext);

  Value* value() const { return inputs_[0]; }

 private:
  DISALLOW_COPY_AND_ASSIGN(StoreContextComp);
};


class ClosureCallComp : public Computation {
 public:
  ClosureCallComp(ClosureCallNode* node,
                  intptr_t try_index,
                  ZoneGrowableArray<Value*>* arguments)
      : ast_node_(*node),
        try_index_(try_index),
        arguments_(arguments) { }

  DECLARE_COMPUTATION(ClosureCall)

  const Array& argument_names() const { return ast_node_.arguments()->names(); }
  intptr_t token_pos() const { return ast_node_.token_pos(); }
  intptr_t try_index() const { return try_index_; }

  intptr_t ArgumentCount() const { return arguments_->length(); }
  Value* ArgumentAt(intptr_t index) const { return (*arguments_)[index]; }

  virtual intptr_t InputCount() const;
  virtual Value* InputAt(intptr_t i) const { return ArgumentAt(i); }
  virtual void SetInputAt(intptr_t i, Value* value) {
    (*arguments_)[i] = value;
  }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const ClosureCallNode& ast_node_;
  const intptr_t try_index_;
  ZoneGrowableArray<Value*>* arguments_;

  DISALLOW_COPY_AND_ASSIGN(ClosureCallComp);
};


class InstanceCallComp : public Computation {
 public:
  InstanceCallComp(intptr_t token_pos,
                   intptr_t try_index,
                   const String& function_name,
                   Token::Kind token_kind,
                   ZoneGrowableArray<Value*>* arguments,
                   const Array& argument_names,
                   intptr_t checked_argument_count)
      : token_pos_(token_pos),
        try_index_(try_index),
        function_name_(function_name),
        token_kind_(token_kind),
        arguments_(arguments),
        argument_names_(argument_names),
        checked_argument_count_(checked_argument_count) {
    ASSERT(function_name.IsZoneHandle());
    ASSERT(!arguments->is_empty());
    ASSERT(argument_names.IsZoneHandle());
    ASSERT(Token::IsBinaryToken(token_kind) ||
           Token::IsUnaryToken(token_kind) ||
           token_kind == Token::kGET ||
           token_kind == Token::kILLEGAL);
  }

  DECLARE_COMPUTATION(InstanceCall)

  intptr_t token_pos() const { return token_pos_; }
  intptr_t try_index() const { return try_index_; }
  const String& function_name() const { return function_name_; }
  Token::Kind token_kind() const { return token_kind_; }
  intptr_t ArgumentCount() const { return arguments_->length(); }
  Value* ArgumentAt(intptr_t index) const { return (*arguments_)[index]; }
  const Array& argument_names() const { return argument_names_; }
  intptr_t checked_argument_count() const { return checked_argument_count_; }

  virtual intptr_t InputCount() const;
  virtual Value* InputAt(intptr_t i) const { return ArgumentAt(i); }
  virtual void SetInputAt(intptr_t i, Value* value) {
    (*arguments_)[i] = value;
  }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

  bool VerifyComputation();

 private:
  const intptr_t token_pos_;
  const intptr_t try_index_;
  const String& function_name_;
  const Token::Kind token_kind_;  // Binary op, unary op, kGET or kILLEGAL.
  ZoneGrowableArray<Value*>* const arguments_;
  const Array& argument_names_;
  const intptr_t checked_argument_count_;

  DISALLOW_COPY_AND_ASSIGN(InstanceCallComp);
};


class PolymorphicInstanceCallComp : public Computation {
 public:
  explicit PolymorphicInstanceCallComp(InstanceCallComp* comp)
      : instance_call_(comp) {
    ASSERT(instance_call_ != NULL);
  }

  InstanceCallComp* instance_call() const { return instance_call_; }

  virtual intptr_t InputCount() const { return instance_call()->InputCount(); }
  virtual Value* InputAt(intptr_t i) const {
    return instance_call()->ArgumentAt(i);
  }
  virtual void SetInputAt(intptr_t index, Value* value) {
    instance_call()->SetInputAt(index, value);
  }

  virtual void PrintOperandsTo(BufferFormatter* f) const {
    instance_call()->PrintOperandsTo(f);
  }

  DECLARE_COMPUTATION(PolymorphicInstanceCall)

 private:
  InstanceCallComp* instance_call_;

  DISALLOW_COPY_AND_ASSIGN(PolymorphicInstanceCallComp);
};


class ComparisonComp : public TemplateComputation<2> {
 public:
  ComparisonComp(Value* left, Value* right)
      : fused_with_branch_(NULL) {
    ASSERT(left != NULL);
    ASSERT(right != NULL);
    inputs_[0] = left;
    inputs_[1] = right;
  }

  void MarkFusedWithBranch(BranchInstr* branch) {
    fused_with_branch_ = branch;
  }

  BranchInstr* fused_with_branch() const {
    ASSERT(is_fused_with_branch());
    return fused_with_branch_;
  }

  bool is_fused_with_branch() const {
    return fused_with_branch_ != NULL;
  }

  Value* left() const { return inputs_[0]; }
  Value* right() const { return inputs_[1]; }

 private:
  BranchInstr* fused_with_branch_;
};


class StrictCompareComp : public ComparisonComp {
 public:
  StrictCompareComp(Token::Kind kind, Value* left, Value* right)
      : ComparisonComp(left, right), kind_(kind) {
    ASSERT((kind_ == Token::kEQ_STRICT) || (kind_ == Token::kNE_STRICT));
  }

  DECLARE_COMPUTATION(StrictCompare)

  Token::Kind kind() const { return kind_; }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const Token::Kind kind_;

  DISALLOW_COPY_AND_ASSIGN(StrictCompareComp);
};


class EqualityCompareComp : public ComparisonComp {
 public:
  EqualityCompareComp(intptr_t token_pos,
                      intptr_t try_index,
                      Value* left,
                      Value* right)
      : ComparisonComp(left, right),
        token_pos_(token_pos),
        try_index_(try_index) {
  }

  DECLARE_COMPUTATION(EqualityCompare)

  intptr_t token_pos() const { return token_pos_; }
  intptr_t try_index() const { return try_index_; }
  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const intptr_t token_pos_;
  const intptr_t try_index_;

  DISALLOW_COPY_AND_ASSIGN(EqualityCompareComp);
};


class RelationalOpComp : public ComparisonComp {
 public:
  RelationalOpComp(intptr_t token_pos,
                   intptr_t try_index,
                   Token::Kind kind,
                   Value* left,
                   Value* right)
      : ComparisonComp(left, right),
        token_pos_(token_pos),
        try_index_(try_index),
        kind_(kind),
        operands_class_id_(kObject) {
    ASSERT(Token::IsRelationalOperator(kind));
  }

  DECLARE_COMPUTATION(RelationalOp)

  intptr_t token_pos() const { return token_pos_; }
  intptr_t try_index() const { return try_index_; }
  Token::Kind kind() const { return kind_; }

  // TODO(srdjan): instead of class-id pass an enum that can differentiate
  // between boxed and unboxed doubles and integers.
  void set_operands_class_id(intptr_t value) {
    operands_class_id_ = value;
  }

  intptr_t operands_class_id() const { return operands_class_id_; }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const intptr_t token_pos_;
  const intptr_t try_index_;
  const Token::Kind kind_;
  intptr_t operands_class_id_;  // class id of both operands.

  DISALLOW_COPY_AND_ASSIGN(RelationalOpComp);
};


class StaticCallComp : public Computation {
 public:
  StaticCallComp(intptr_t token_pos,
                 intptr_t try_index,
                 const Function& function,
                 const Array& argument_names,
                 ZoneGrowableArray<Value*>* arguments)
      : token_pos_(token_pos),
        try_index_(try_index),
        function_(function),
        argument_names_(argument_names),
        arguments_(arguments),
        recognized_(MethodRecognizer::kUnknown) {
    ASSERT(function.IsZoneHandle());
    ASSERT(argument_names.IsZoneHandle());
  }

  DECLARE_COMPUTATION(StaticCall)

  // Accessors forwarded to the AST node.
  const Function& function() const { return function_; }
  const Array& argument_names() const { return argument_names_; }
  intptr_t token_pos() const { return token_pos_; }
  intptr_t try_index() const { return try_index_; }

  intptr_t ArgumentCount() const { return arguments_->length(); }
  Value* ArgumentAt(intptr_t index) const { return (*arguments_)[index]; }

  MethodRecognizer::Kind recognized() const { return recognized_; }
  void set_recognized(MethodRecognizer::Kind kind) { recognized_ = kind; }

  virtual intptr_t InputCount() const;
  virtual Value* InputAt(intptr_t i) const { return ArgumentAt(i); }
  virtual void SetInputAt(intptr_t i, Value* value) {
    (*arguments_)[i] = value;
  }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const intptr_t token_pos_;
  const intptr_t try_index_;
  const Function& function_;
  const Array& argument_names_;
  ZoneGrowableArray<Value*>* arguments_;
  MethodRecognizer::Kind recognized_;

  DISALLOW_COPY_AND_ASSIGN(StaticCallComp);
};


class LoadLocalComp : public TemplateComputation<0> {
 public:
  LoadLocalComp(const LocalVariable& local, intptr_t context_level)
      : local_(local),
        context_level_(context_level) { }

  DECLARE_COMPUTATION(LoadLocal)

  const LocalVariable& local() const { return local_; }
  intptr_t context_level() const { return context_level_; }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const LocalVariable& local_;
  const intptr_t context_level_;

  DISALLOW_COPY_AND_ASSIGN(LoadLocalComp);
};


class StoreLocalComp : public TemplateComputation<1> {
 public:
  StoreLocalComp(const LocalVariable& local,
                 Value* value,
                 intptr_t context_level)
      : local_(local),
        context_level_(context_level) {
    inputs_[0] = value;
  }

  DECLARE_COMPUTATION(StoreLocal)

  const LocalVariable& local() const { return local_; }
  Value* value() const { return inputs_[0]; }
  intptr_t context_level() const { return context_level_; }

  virtual void RecordAssignedVars(BitVector* assigned_vars);

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const LocalVariable& local_;
  const intptr_t context_level_;

  DISALLOW_COPY_AND_ASSIGN(StoreLocalComp);
};


class NativeCallComp : public TemplateComputation<0> {
 public:
  NativeCallComp(NativeBodyNode* node, intptr_t try_index)
      : ast_node_(*node), try_index_(try_index) {}

  DECLARE_COMPUTATION(NativeCall)

  intptr_t token_pos() const { return ast_node_.token_pos(); }
  intptr_t try_index() const { return try_index_; }

  const String& native_name() const {
    return ast_node_.native_c_function_name();
  }

  NativeFunction native_c_function() const {
    return ast_node_.native_c_function();
  }

  intptr_t argument_count() const { return ast_node_.argument_count(); }

  bool has_optional_parameters() const {
    return ast_node_.has_optional_parameters();
  }

  bool is_native_instance_closure() const {
    return ast_node_.is_native_instance_closure();
  }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const NativeBodyNode& ast_node_;
  const intptr_t try_index_;

  DISALLOW_COPY_AND_ASSIGN(NativeCallComp);
};


class LoadInstanceFieldComp : public TemplateComputation<1> {
 public:
  LoadInstanceFieldComp(const Field& field,
                        Value* instance,
                        InstanceCallComp* original)  // Maybe NULL.
      : field_(field), original_(original) {
    ASSERT(instance != NULL);
    inputs_[0] = instance;
  }

  DECLARE_COMPUTATION(LoadInstanceField)

  const Field& field() const { return field_; }
  Value* instance() const { return inputs_[0]; }
  const InstanceCallComp* original() const { return original_; }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const Field& field_;
  const InstanceCallComp* original_;  // For optimizations.

  DISALLOW_COPY_AND_ASSIGN(LoadInstanceFieldComp);
};


class StoreInstanceFieldComp : public TemplateComputation<2> {
 public:
  StoreInstanceFieldComp(const Field& field,
                         Value* instance,
                         Value* value,
                         InstanceSetterComp* original)  // Maybe NULL.
      : field_(field), original_(original) {
    ASSERT(instance != NULL);
    ASSERT(value != NULL);
    inputs_[0] = instance;
    inputs_[1] = value;
  }

  DECLARE_COMPUTATION(StoreInstanceField)

  const Field& field() const { return field_; }

  Value* instance() const { return inputs_[0]; }
  Value* value() const { return inputs_[1]; }

  const InstanceSetterComp* original() const { return original_; }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const Field& field_;
  const InstanceSetterComp* original_;  // For optimizations.

  DISALLOW_COPY_AND_ASSIGN(StoreInstanceFieldComp);
};


class LoadStaticFieldComp : public TemplateComputation<0> {
 public:
  explicit LoadStaticFieldComp(const Field& field) : field_(field) {}

  DECLARE_COMPUTATION(LoadStaticField);

  const Field& field() const { return field_; }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const Field& field_;

  DISALLOW_COPY_AND_ASSIGN(LoadStaticFieldComp);
};


class StoreStaticFieldComp : public TemplateComputation<1> {
 public:
  StoreStaticFieldComp(const Field& field, Value* value)
      : field_(field) {
    ASSERT(field.IsZoneHandle());
    ASSERT(value != NULL);
    inputs_[0] = value;
  }

  DECLARE_COMPUTATION(StoreStaticField);

  const Field& field() const { return field_; }
  Value* value() const { return inputs_[0]; }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const Field& field_;

  DISALLOW_COPY_AND_ASSIGN(StoreStaticFieldComp);
};


class LoadIndexedComp : public TemplateComputation<2> {
 public:
  LoadIndexedComp(intptr_t token_pos,
                  intptr_t try_index,
                  Value* array,
                  Value* index)
      : token_pos_(token_pos),
        try_index_(try_index),
        receiver_type_(kIllegalObjectKind) {
    ASSERT(array != NULL);
    ASSERT(index != NULL);
    inputs_[0] = array;
    inputs_[1] = index;
  }

  DECLARE_COMPUTATION(LoadIndexed)

  intptr_t token_pos() const { return token_pos_; }
  intptr_t try_index() const { return try_index_; }
  Value* array() const { return inputs_[0]; }
  Value* index() const { return inputs_[1]; }

  void set_receiver_type(ObjectKind receiver_type) {
    receiver_type_ = receiver_type;
  }

  ObjectKind receiver_type() const {
    return receiver_type_;
  }

 private:
  const intptr_t token_pos_;
  const intptr_t try_index_;
  ObjectKind receiver_type_;

  DISALLOW_COPY_AND_ASSIGN(LoadIndexedComp);
};


// Not simply an InstanceCall because it has somewhat more complicated
// semantics: the value operand is preserved before the call.
class StoreIndexedComp : public TemplateComputation<3> {
 public:
  StoreIndexedComp(intptr_t token_pos,
                   intptr_t try_index,
                   Value* array,
                   Value* index,
                   Value* value)
      : token_pos_(token_pos),
        try_index_(try_index),
        receiver_type_(kIllegalObjectKind) {
    inputs_[0] = array;
    inputs_[1] = index;
    inputs_[2] = value;
  }

  DECLARE_COMPUTATION(StoreIndexed)

  intptr_t token_pos() const { return token_pos_; }
  intptr_t try_index() const { return try_index_; }
  Value* array() const { return inputs_[0]; }
  Value* index() const { return inputs_[1]; }
  Value* value() const { return inputs_[2]; }

  void set_receiver_type(ObjectKind receiver_type) {
    receiver_type_ = receiver_type;
  }

  ObjectKind receiver_type() const {
    return receiver_type_;
  }

 private:
  const intptr_t token_pos_;
  const intptr_t try_index_;
  ObjectKind receiver_type_;

  DISALLOW_COPY_AND_ASSIGN(StoreIndexedComp);
};


// Not simply an InstanceCall because it has somewhat more complicated
// semantics: the value operand is preserved before the call.
class InstanceSetterComp : public TemplateComputation<2> {
 public:
  InstanceSetterComp(intptr_t token_pos,
                     intptr_t try_index,
                     const String& field_name,
                     Value* receiver,
                     Value* value)
      : token_pos_(token_pos),
        try_index_(try_index),
        field_name_(field_name) {
    inputs_[0] = receiver;
    inputs_[1] = value;
  }

  DECLARE_COMPUTATION(InstanceSetter)

  intptr_t token_pos() const { return token_pos_; }
  intptr_t try_index() const { return try_index_; }
  const String& field_name() const { return field_name_; }
  Value* receiver() const { return inputs_[0]; }
  Value* value() const { return inputs_[1]; }

 private:
  const intptr_t token_pos_;
  const intptr_t try_index_;
  const String& field_name_;

  DISALLOW_COPY_AND_ASSIGN(InstanceSetterComp);
};


// Not simply a StaticCall because it has somewhat more complicated
// semantics: the value operand is preserved before the call.
class StaticSetterComp : public TemplateComputation<1> {
 public:
  StaticSetterComp(intptr_t token_pos,
                   intptr_t try_index,
                   const Function& setter_function,
                   Value* value)
      : token_pos_(token_pos),
        try_index_(try_index),
        setter_function_(setter_function) {
    inputs_[0] = value;
  }

  DECLARE_COMPUTATION(StaticSetter)

  intptr_t token_pos() const { return token_pos_; }
  intptr_t try_index() const { return try_index_; }
  const Function& setter_function() const { return setter_function_; }
  Value* value() const { return inputs_[0]; }

 private:
  const intptr_t token_pos_;
  const intptr_t try_index_;
  const Function& setter_function_;

  DISALLOW_COPY_AND_ASSIGN(StaticSetterComp);
};


// Note overrideable, built-in: value? false : true.
class BooleanNegateComp : public TemplateComputation<1> {
 public:
  explicit BooleanNegateComp(Value* value) {
    inputs_[0] = value;
  }

  DECLARE_COMPUTATION(BooleanNegate)

  Value* value() const { return inputs_[0]; }

 private:
  DISALLOW_COPY_AND_ASSIGN(BooleanNegateComp);
};


class InstanceOfComp : public TemplateComputation<3> {
 public:
  InstanceOfComp(intptr_t token_pos,
                 intptr_t try_index,
                 Value* value,
                 Value* instantiator,
                 Value* instantiator_type_arguments,
                 const AbstractType& type,
                 bool negate_result)
      : token_pos_(token_pos),
        try_index_(try_index),
        type_(type),
        negate_result_(negate_result) {
    ASSERT(value != NULL);
    ASSERT(instantiator != NULL);
    ASSERT(instantiator_type_arguments != NULL);
    ASSERT(!type.IsNull());
    inputs_[0] = value;
    inputs_[1] = instantiator;
    inputs_[2] = instantiator_type_arguments;
  }

  DECLARE_COMPUTATION(InstanceOf)

  Value* value() const { return inputs_[0]; }
  Value* instantiator() const { return inputs_[1]; }
  Value* instantiator_type_arguments() const { return inputs_[2]; }

  bool negate_result() const { return negate_result_; }
  const AbstractType& type() const { return type_; }
  intptr_t token_pos() const { return token_pos_; }
  intptr_t try_index() const { return try_index_; }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const intptr_t token_pos_;
  const intptr_t try_index_;
  Value* value_;
  Value* instantiator_;
  Value* type_arguments_;
  const AbstractType& type_;
  const bool negate_result_;

  DISALLOW_COPY_AND_ASSIGN(InstanceOfComp);
};


class AllocateObjectComp : public Computation {
 public:
  AllocateObjectComp(ConstructorCallNode* node,
                     intptr_t try_index,
                     ZoneGrowableArray<Value*>* arguments)
      : ast_node_(*node), try_index_(try_index), arguments_(arguments) {
    // Either no arguments or one type-argument and one instantiator.
    ASSERT(arguments->is_empty() || (arguments->length() == 2));
  }

  DECLARE_COMPUTATION(AllocateObject)

  const Function& constructor() const { return ast_node_.constructor(); }
  intptr_t token_pos() const { return ast_node_.token_pos(); }
  intptr_t try_index() const { return try_index_; }
  const ZoneGrowableArray<Value*>& arguments() const { return *arguments_; }

  virtual intptr_t InputCount() const;
  virtual Value* InputAt(intptr_t i) const { return arguments()[i]; }
  virtual void SetInputAt(intptr_t i, Value* value) {
    (*arguments_)[i] = value;
  }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const ConstructorCallNode& ast_node_;
  const intptr_t try_index_;
  ZoneGrowableArray<Value*>* const arguments_;
  DISALLOW_COPY_AND_ASSIGN(AllocateObjectComp);
};


class AllocateObjectWithBoundsCheckComp : public Computation {
 public:
  AllocateObjectWithBoundsCheckComp(ConstructorCallNode* node,
                                    intptr_t try_index,
                                    ZoneGrowableArray<Value*>* arguments)
      : ast_node_(*node), try_index_(try_index), arguments_(arguments) {
    // One type-argument and one instantiator.
    ASSERT(arguments->length() == 2);
  }

  DECLARE_COMPUTATION(AllocateObjectWithBoundsCheck)

  const Function& constructor() const { return ast_node_.constructor(); }
  intptr_t token_pos() const { return ast_node_.token_pos(); }
  intptr_t try_index() const { return try_index_; }
  const ZoneGrowableArray<Value*>& arguments() const { return *arguments_; }

  virtual intptr_t InputCount() const;
  virtual Value* InputAt(intptr_t i) const { return arguments()[i]; }
  virtual void SetInputAt(intptr_t i, Value* value) {
    (*arguments_)[i] = value;
  }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const ConstructorCallNode& ast_node_;
  const intptr_t try_index_;
  ZoneGrowableArray<Value*>* const arguments_;
  DISALLOW_COPY_AND_ASSIGN(AllocateObjectWithBoundsCheckComp);
};


class CreateArrayComp : public TemplateComputation<1> {
 public:
  CreateArrayComp(intptr_t token_pos,
                  intptr_t try_index,
                  ZoneGrowableArray<Value*>* elements,
                  Value* element_type)
      : token_pos_(token_pos),
        try_index_(try_index),
        elements_(elements) {
#if defined(DEBUG)
    for (int i = 0; i < ElementCount(); ++i) {
      ASSERT(ElementAt(i) != NULL);
    }
    ASSERT(element_type != NULL);
#endif
    inputs_[0] = element_type;
  }

  DECLARE_COMPUTATION(CreateArray)

  intptr_t token_pos() const { return token_pos_; }
  intptr_t try_index() const { return try_index_; }
  intptr_t ElementCount() const { return elements_->length(); }
  Value* ElementAt(intptr_t i) const { return (*elements_)[i]; }
  Value* element_type() const { return inputs_[0]; }

  virtual intptr_t InputCount() const;
  virtual Value* InputAt(intptr_t i) const;
  virtual void SetInputAt(intptr_t i, Value* value);

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const intptr_t token_pos_;
  const intptr_t try_index_;
  ZoneGrowableArray<Value*>* const elements_;

  DISALLOW_COPY_AND_ASSIGN(CreateArrayComp);
};


class CreateClosureComp : public TemplateComputation<2> {
 public:
  CreateClosureComp(ClosureNode* node,
                    intptr_t try_index,
                    Value* type_arguments,
                    Value* receiver)
      : ast_node_(*node),
        try_index_(try_index) {
    ASSERT(type_arguments != NULL);
    ASSERT(receiver != NULL);
    inputs_[0] = type_arguments;
    inputs_[1] = receiver;
  }

  DECLARE_COMPUTATION(CreateClosure)

  intptr_t token_pos() const { return ast_node_.token_pos(); }
  intptr_t try_index() const { return try_index_; }
  const Function& function() const { return ast_node_.function(); }
  Value* type_arguments() const { return inputs_[0]; }
  Value* receiver() const { return inputs_[1]; }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const ClosureNode& ast_node_;
  const intptr_t try_index_;

  DISALLOW_COPY_AND_ASSIGN(CreateClosureComp);
};


class LoadVMFieldComp : public TemplateComputation<1> {
 public:
  LoadVMFieldComp(Value* value,
                  intptr_t offset_in_bytes,
                  const AbstractType& type)
      : offset_in_bytes_(offset_in_bytes),
        type_(type),
        original_(NULL) {
    ASSERT(value != NULL);
    ASSERT(type.IsZoneHandle());  // May be null if field is not an instance.
    inputs_[0] = value;
  }

  DECLARE_COMPUTATION(LoadVMField)

  Value* value() const { return inputs_[0]; }
  intptr_t offset_in_bytes() const { return offset_in_bytes_; }
  const AbstractType& type() const { return type_; }
  const InstanceCallComp* original() const { return original_; }
  void set_original(InstanceCallComp* value) { original_ = value; }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const intptr_t offset_in_bytes_;
  const AbstractType& type_;
  const InstanceCallComp* original_;  // For optimizations.
  // If non-NULL, the instruction is valid only for the class ids listed.

  DISALLOW_COPY_AND_ASSIGN(LoadVMFieldComp);
};


class StoreVMFieldComp : public TemplateComputation<2> {
 public:
  StoreVMFieldComp(Value* dest,
                   intptr_t offset_in_bytes,
                   Value* value,
                   const AbstractType& type)
      : offset_in_bytes_(offset_in_bytes), type_(type) {
    ASSERT(value != NULL);
    ASSERT(type.IsZoneHandle());  // May be null if field is not an instance.
    inputs_[0] = value;
    inputs_[1] = dest;
  }

  DECLARE_COMPUTATION(StoreVMField)

  Value* value() const { return inputs_[0]; }
  Value* dest() const { return inputs_[1]; }
  intptr_t offset_in_bytes() const { return offset_in_bytes_; }
  const AbstractType& type() const { return type_; }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const intptr_t offset_in_bytes_;
  const AbstractType& type_;

  DISALLOW_COPY_AND_ASSIGN(StoreVMFieldComp);
};


class InstantiateTypeArgumentsComp : public TemplateComputation<1> {
 public:
  InstantiateTypeArgumentsComp(intptr_t token_pos,
                               intptr_t try_index,
                               const AbstractTypeArguments& type_arguments,
                               Value* instantiator)
      : token_pos_(token_pos),
        try_index_(try_index),
        type_arguments_(type_arguments) {
    ASSERT(instantiator != NULL);
    inputs_[0] = instantiator;
  }

  DECLARE_COMPUTATION(InstantiateTypeArguments)

  Value* instantiator() const { return inputs_[0]; }
  const AbstractTypeArguments& type_arguments() const {
    return type_arguments_;
  }
  intptr_t token_pos() const { return token_pos_; }
  intptr_t try_index() const { return try_index_; }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const intptr_t token_pos_;
  const intptr_t try_index_;
  const AbstractTypeArguments& type_arguments_;

  DISALLOW_COPY_AND_ASSIGN(InstantiateTypeArgumentsComp);
};


class ExtractConstructorTypeArgumentsComp : public TemplateComputation<1> {
 public:
  ExtractConstructorTypeArgumentsComp(
      intptr_t token_pos,
      intptr_t try_index,
      const AbstractTypeArguments& type_arguments,
      Value* instantiator)
      : token_pos_(token_pos),
        try_index_(try_index),
        type_arguments_(type_arguments) {
    ASSERT(instantiator != NULL);
    inputs_[0] = instantiator;
  }

  DECLARE_COMPUTATION(ExtractConstructorTypeArguments)

  Value* instantiator() const { return inputs_[0]; }
  const AbstractTypeArguments& type_arguments() const {
    return type_arguments_;
  }
  intptr_t token_pos() const { return token_pos_; }
  intptr_t try_index() const { return try_index_; }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const intptr_t token_pos_;
  const intptr_t try_index_;
  const AbstractTypeArguments& type_arguments_;

  DISALLOW_COPY_AND_ASSIGN(ExtractConstructorTypeArgumentsComp);
};


class ExtractConstructorInstantiatorComp : public TemplateComputation<1> {
 public:
  ExtractConstructorInstantiatorComp(ConstructorCallNode* ast_node,
                                     Value* instantiator)
      : ast_node_(*ast_node) {
    ASSERT(instantiator != NULL);
    inputs_[0] = instantiator;
  }

  DECLARE_COMPUTATION(ExtractConstructorInstantiator)

  Value* instantiator() const { return inputs_[0]; }
  const AbstractTypeArguments& type_arguments() const {
    return ast_node_.type_arguments();
  }
  const Function& constructor() const { return ast_node_.constructor(); }
  intptr_t token_pos() const { return ast_node_.token_pos(); }

 private:
  const ConstructorCallNode& ast_node_;

  DISALLOW_COPY_AND_ASSIGN(ExtractConstructorInstantiatorComp);
};


class AllocateContextComp : public TemplateComputation<0> {
 public:
  AllocateContextComp(intptr_t token_pos,
                      intptr_t try_index,
                      intptr_t num_context_variables)
      : token_pos_(token_pos),
        try_index_(try_index),
        num_context_variables_(num_context_variables) {}

  DECLARE_COMPUTATION(AllocateContext);

  intptr_t token_pos() const { return token_pos_; }
  intptr_t try_index() const { return try_index_; }
  intptr_t num_context_variables() const { return num_context_variables_; }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const intptr_t token_pos_;
  const intptr_t try_index_;
  const intptr_t num_context_variables_;

  DISALLOW_COPY_AND_ASSIGN(AllocateContextComp);
};


class ChainContextComp : public TemplateComputation<1> {
 public:
  explicit ChainContextComp(Value* context_value) {
    ASSERT(context_value != NULL);
    inputs_[0] = context_value;
  }

  DECLARE_COMPUTATION(ChainContext)

  Value* context_value() const { return inputs_[0]; }

 private:
  DISALLOW_COPY_AND_ASSIGN(ChainContextComp);
};


class CloneContextComp : public TemplateComputation<1> {
 public:
  CloneContextComp(intptr_t token_pos,
                   intptr_t try_index,
                   Value* context_value)
      : token_pos_(token_pos),
        try_index_(try_index) {
    ASSERT(context_value != NULL);
    inputs_[0] = context_value;
  }

  intptr_t token_pos() const { return token_pos_; }
  intptr_t try_index() const { return try_index_; }
  Value* context_value() const { return inputs_[0]; }

  DECLARE_COMPUTATION(CloneContext)

 private:
  const intptr_t token_pos_;
  const intptr_t try_index_;

  DISALLOW_COPY_AND_ASSIGN(CloneContextComp);
};


class CatchEntryComp : public TemplateComputation<0> {
 public:
  CatchEntryComp(const LocalVariable& exception_var,
                 const LocalVariable& stacktrace_var)
      : exception_var_(exception_var), stacktrace_var_(stacktrace_var) {}

  const LocalVariable& exception_var() const { return exception_var_; }
  const LocalVariable& stacktrace_var() const { return stacktrace_var_; }

  DECLARE_COMPUTATION(CatchEntry)

  virtual void PrintOperandsTo(BufferFormatter* f) const;

 private:
  const LocalVariable& exception_var_;
  const LocalVariable& stacktrace_var_;

  DISALLOW_COPY_AND_ASSIGN(CatchEntryComp);
};


class BinaryOpComp : public TemplateComputation<2> {
 public:
  enum OperandsType {
    kDynamicOperands,
    kSmiOperands,
    kMintOperands,
    kDoubleOperands
  };

  BinaryOpComp(Token::Kind op_kind,
               OperandsType operands_type,
               InstanceCallComp* instance_call,
               Value* left,
               Value* right)
      : op_kind_(op_kind),
        operands_type_(operands_type),
        instance_call_(instance_call) {
    ASSERT(left != NULL);
    ASSERT(right != NULL);
    inputs_[0] = left;
    inputs_[1] = right;
  }

  Value* left() const { return inputs_[0]; }
  Value* right() const { return inputs_[1]; }

  Token::Kind op_kind() const { return op_kind_; }

  OperandsType operands_type() const { return operands_type_; }

  InstanceCallComp* instance_call() const { return instance_call_; }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

  DECLARE_COMPUTATION(BinaryOp)

 private:
  const Token::Kind op_kind_;
  const OperandsType operands_type_;
  InstanceCallComp* instance_call_;

  DISALLOW_COPY_AND_ASSIGN(BinaryOpComp);
};


// Handles both Smi operations: BIT_OR and NEGATE.
class UnarySmiOpComp : public TemplateComputation<1> {
 public:
  UnarySmiOpComp(Token::Kind op_kind,
                 InstanceCallComp* instance_call,
                Value* value)
      : op_kind_(op_kind), instance_call_(instance_call) {
    ASSERT(value != NULL);
    inputs_[0] = value;
  }

  Value* value() const { return inputs_[0]; }
  Token::Kind op_kind() const { return op_kind_; }

  InstanceCallComp* instance_call() const { return instance_call_; }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

  DECLARE_COMPUTATION(UnarySmiOp)

 private:
  const Token::Kind op_kind_;
  InstanceCallComp* instance_call_;

  DISALLOW_COPY_AND_ASSIGN(UnarySmiOpComp);
};


// Handles non-Smi NEGATE operations
class NumberNegateComp : public TemplateComputation<1> {
 public:
  NumberNegateComp(InstanceCallComp* instance_call,
                   Value* value) : instance_call_(instance_call) {
    ASSERT(value != NULL);
    inputs_[0] = value;
  }

  Value* value() const { return inputs_[0]; }

  InstanceCallComp* instance_call() const { return instance_call_; }

  DECLARE_COMPUTATION(NumberNegate)

 private:
  InstanceCallComp* instance_call_;

  DISALLOW_COPY_AND_ASSIGN(NumberNegateComp);
};


class CheckStackOverflowComp : public TemplateComputation<0> {
 public:
  CheckStackOverflowComp(intptr_t token_pos, intptr_t try_index)
      : token_pos_(token_pos),
        try_index_(try_index) {}

  intptr_t token_pos() const { return token_pos_; }
  intptr_t try_index() const { return try_index_; }

  DECLARE_COMPUTATION(CheckStackOverflow)

 private:
  const intptr_t token_pos_;
  const intptr_t try_index_;

  DISALLOW_COPY_AND_ASSIGN(CheckStackOverflowComp);
};


class ToDoubleComp : public TemplateComputation<1> {
 public:
  ToDoubleComp(Value* value,
               ObjectKind from,
               InstanceCallComp* instance_call)
      : from_(from), instance_call_(instance_call) {
    ASSERT(value != NULL);
    inputs_[0] = value;
  }

  Value* value() const { return inputs_[0]; }
  ObjectKind from() const { return from_; }

  InstanceCallComp* instance_call() const { return instance_call_; }

  virtual void PrintOperandsTo(BufferFormatter* f) const;

  DECLARE_COMPUTATION(ToDouble)

 private:
  const ObjectKind from_;
  InstanceCallComp* instance_call_;

  DISALLOW_COPY_AND_ASSIGN(ToDoubleComp);
};


#undef DECLARE_COMPUTATION


// Implementation of type testers and cast functins.
#define DEFINE_PREDICATE(ShortName, ClassName)                                 \
bool Computation::Is##ShortName() const {                                      \
  return computation_type() == k##ShortName;                                   \
}                                                                              \
ClassName* Computation::As##ShortName() {                                      \
  if (!Is##ShortName()) return NULL;                                           \
  return static_cast<ClassName*>(this);                                        \
}
FOR_EACH_COMPUTATION(DEFINE_PREDICATE)
#undef DEFINE_PREDICATE


// Instructions.
//
// <Instruction> ::= JoinEntry <Instruction>
//                 | TargetEntry <Instruction>
//                 | Do <Computation> <Instruction>
//                 | Return <Value>
//                 | Branch <Value> <Instruction> <Instruction>
// <Definition>  ::= Bind <int> <Computation> <Instruction>

// M is a single argument macro.  It is applied to each concrete instruction
// type name.  The concrete instruction classes are the name with Instr
// concatenated.
#define FOR_EACH_INSTRUCTION(M)                                                \
  M(GraphEntry)                                                                \
  M(JoinEntry)                                                                 \
  M(TargetEntry)                                                               \
  M(Do)                                                                        \
  M(Bind)                                                                      \
  M(Phi)                                                                       \
  M(Return)                                                                    \
  M(Throw)                                                                     \
  M(ReThrow)                                                                   \
  M(Branch)                                                                    \
  M(ParallelMove)                                                              \
  M(Parameter)


// Forward declarations for Instruction classes.
class BlockEntryInstr;
class FlowGraphBuilder;
class Environment;

#define FORWARD_DECLARATION(type) class type##Instr;
FOR_EACH_INSTRUCTION(FORWARD_DECLARATION)
#undef FORWARD_DECLARATION


// Functions required in all concrete instruction classes.
#define DECLARE_INSTRUCTION(type)                                              \
  virtual Instruction* Accept(FlowGraphVisitor* visitor);                      \
  virtual bool Is##type() const { return true; }                               \
  virtual type##Instr* As##type() { return this; }                             \
  virtual intptr_t InputCount() const;                                         \
  virtual Value* InputAt(intptr_t i) const;                                    \
  virtual void SetInputAt(intptr_t i, Value* value);                           \
  virtual const char* DebugName() const { return #type; }                      \
  virtual void PrintTo(BufferFormatter* f) const;                              \
  virtual void PrintToVisualizer(BufferFormatter* f) const;


class Instruction : public ZoneAllocated {
 public:
  Instruction()
      : cid_(-1),
        ic_data_(NULL),
        successor_(NULL),
        previous_(NULL),
        env_(NULL) {
    Isolate* isolate = Isolate::Current();
    cid_ = Computation::GetNextCid(isolate);
    ic_data_ = Computation::GetICDataForCid(cid_, isolate);
  }

  // Unique computation/instruction id, used for deoptimization, e.g. for
  // ReturnInstr, ThrowInstr and ReThrowInstr.
  intptr_t cid() const { return cid_; }

  const ICData* ic_data() const { return ic_data_; }

  virtual bool IsBlockEntry() const { return false; }
  BlockEntryInstr* AsBlockEntry() {
    return IsBlockEntry() ? reinterpret_cast<BlockEntryInstr*>(this) : NULL;
  }
  virtual bool IsDefinition() const { return false; }
  virtual Definition* AsDefinition() { return NULL; }

  virtual intptr_t InputCount() const = 0;
  virtual Value* InputAt(intptr_t i) const = 0;
  virtual void SetInputAt(intptr_t i, Value* value) = 0;

  // Visiting support.
  virtual Instruction* Accept(FlowGraphVisitor* visitor) = 0;

  Instruction* successor() const { return successor_; }
  void set_successor(Instruction* instr) {
    ASSERT(!IsGraphEntry());
    ASSERT(!IsReturn());
    ASSERT(!IsBranch());
    ASSERT(!IsPhi());
    // TODO(fschneider): Also add Throw and ReThrow to the list of instructions
    // that do not have a successor. Currently, the graph builder will continue
    // to append instruction in case of a Throw inside an expression. This
    // condition should be handled in the graph builder
    successor_ = instr;
  }

  Instruction* previous() const { return previous_; }
  void set_previous(Instruction* instr) {
    ASSERT(!IsBlockEntry());
    previous_ = instr;
  }

  // Remove instruction from the graph and return the instruction following the
  // removed instruction.
  Instruction* RemoveFromGraph();

  // Normal instructions can have 0 (inside a block) or 1 (last instruction in
  // a block) successors. Branch instruction with >1 successors override this
  // function.
  virtual intptr_t SuccessorCount() const;
  virtual BlockEntryInstr* SuccessorAt(intptr_t index) const;

  virtual void replace_computation(Computation* value) {
    UNREACHABLE();
  }
  // Discover basic-block structure by performing a recursive depth first
  // traversal of the instruction graph reachable from this instruction.  As
  // a side effect, the block entry instructions in the graph are assigned
  // numbers in both preorder and postorder.  The array 'preorder' maps
  // preorder block numbers to the block entry instruction with that number
  // and analogously for the array 'postorder'.  The depth first spanning
  // tree is recorded in the array 'parent', which maps preorder block
  // numbers to the preorder number of the block's spanning-tree parent.
  // The array 'assigned_vars' maps preorder block numbers to the set of
  // assigned frame-allocated local variables in the block.  As a side
  // effect of this function, the set of basic block predecessors (e.g.,
  // block entry instructions of predecessor blocks) and also the last
  // instruction in the block is recorded in each entry instruction.
  virtual void DiscoverBlocks(
      BlockEntryInstr* current_block,
      GrowableArray<BlockEntryInstr*>* preorder,
      GrowableArray<BlockEntryInstr*>* postorder,
      GrowableArray<intptr_t>* parent,
      GrowableArray<BitVector*>* assigned_vars,
      intptr_t variable_count) {
    // Never called for instructions except block entries and branches.
    UNREACHABLE();
  }

  // Mutate assigned_vars to add the local variable index for all
  // frame-allocated locals assigned to by the instruction.
  virtual void RecordAssignedVars(BitVector* assigned_vars);

  // Printing support.
  virtual void PrintTo(BufferFormatter* f) const = 0;
  virtual void PrintToVisualizer(BufferFormatter* f) const = 0;

#define INSTRUCTION_TYPE_CHECK(type)                                           \
  virtual bool Is##type() const { return false; }                              \
  virtual type##Instr* As##type() { return NULL; }
FOR_EACH_INSTRUCTION(INSTRUCTION_TYPE_CHECK)
#undef INSTRUCTION_TYPE_CHECK

  // Static type of the instruction.
  virtual RawAbstractType* StaticType() const {
    UNREACHABLE();
    return AbstractType::null();
  }

  // Returns structure describing location constraints required
  // to emit native code for this instruction.
  virtual LocationSummary* locs() {
    // TODO(vegorov): This should be pure virtual method.
    // However we are temporary using NULL for instructions that
    // were not converted to the location based code generation yet.
    return NULL;
  }

  virtual void EmitNativeCode(FlowGraphCompiler* compiler) {
    UNIMPLEMENTED();
  }

  Environment* env() const { return env_; }
  void set_env(Environment* env) { env_ = env; }

 private:
  intptr_t cid_;
  ICData* ic_data_;
  Instruction* successor_;
  Instruction* previous_;
  Environment* env_;
  DISALLOW_COPY_AND_ASSIGN(Instruction);
};


class InstructionWithInputs : public Instruction {
 public:
  InstructionWithInputs() : locs_(NULL) {
  }

  virtual LocationSummary* locs() {
    if (locs_ == NULL) {
      locs_ = MakeLocationSummary();
    }
    return locs_;
  }

  virtual LocationSummary* MakeLocationSummary() const = 0;

 private:
  LocationSummary* locs_;
  DISALLOW_COPY_AND_ASSIGN(InstructionWithInputs);
};


// Basic block entries are administrative nodes.  There is a distinguished
// graph entry with no predecessor.  Joins are the only nodes with multiple
// predecessors.  Targets are all other basic block entries.  The types
// enforce edge-split form---joins are forbidden as the successors of
// branches.
class BlockEntryInstr : public Instruction {
 public:
  virtual bool IsBlockEntry() const { return true; }

  virtual intptr_t PredecessorCount() const = 0;
  virtual BlockEntryInstr* PredecessorAt(intptr_t index) const = 0;
  virtual void AddPredecessor(BlockEntryInstr* predecessor) = 0;
  virtual void PrepareEntry(FlowGraphCompiler* compiler) = 0;

  intptr_t preorder_number() const { return preorder_number_; }
  void set_preorder_number(intptr_t number) { preorder_number_ = number; }

  intptr_t postorder_number() const { return postorder_number_; }
  void set_postorder_number(intptr_t number) { postorder_number_ = number; }

  intptr_t block_id() const { return block_id_; }
  void set_block_id(intptr_t value) { block_id_ = value; }

  BlockEntryInstr* dominator() const { return dominator_; }
  void set_dominator(BlockEntryInstr* instr) { dominator_ = instr; }

  const GrowableArray<BlockEntryInstr*>& dominated_blocks() {
    return dominated_blocks_;
  }

  void AddDominatedBlock(BlockEntryInstr* block) {
    dominated_blocks_.Add(block);
  }

  Instruction* last_instruction() const { return last_instruction_; }
  void set_last_instruction(Instruction* instr) { last_instruction_ = instr; }

  virtual void DiscoverBlocks(
      BlockEntryInstr* current_block,
      GrowableArray<BlockEntryInstr*>* preorder,
      GrowableArray<BlockEntryInstr*>* postorder,
      GrowableArray<intptr_t>* parent,
      GrowableArray<BitVector*>* assigned_vars,
      intptr_t variable_count);

 protected:
  BlockEntryInstr()
      : preorder_number_(-1),
        postorder_number_(-1),
        block_id_(-1),
        dominator_(NULL),
        dominated_blocks_(1),
        last_instruction_(NULL) { }

 private:
  intptr_t preorder_number_;
  intptr_t postorder_number_;
  intptr_t block_id_;
  BlockEntryInstr* dominator_;  // Immediate dominator, NULL for graph entry.
  // TODO(fschneider): Optimize the case of one child to save space.
  GrowableArray<BlockEntryInstr*> dominated_blocks_;
  Instruction* last_instruction_;

  DISALLOW_COPY_AND_ASSIGN(BlockEntryInstr);
};


class GraphEntryInstr : public BlockEntryInstr {
 public:
  explicit GraphEntryInstr(TargetEntryInstr* normal_entry)
      : BlockEntryInstr(),
        normal_entry_(normal_entry),
        catch_entries_(),
        start_env_(NULL) { }

  DECLARE_INSTRUCTION(GraphEntry)

  virtual intptr_t PredecessorCount() const { return 0; }
  virtual BlockEntryInstr* PredecessorAt(intptr_t index) const {
    UNREACHABLE();
    return NULL;
  }
  virtual void AddPredecessor(BlockEntryInstr* predecessor) { UNREACHABLE(); }

  virtual intptr_t SuccessorCount() const;
  virtual BlockEntryInstr* SuccessorAt(intptr_t index) const;

  virtual void DiscoverBlocks(
      BlockEntryInstr* current_block,
      GrowableArray<BlockEntryInstr*>* preorder,
      GrowableArray<BlockEntryInstr*>* postorder,
      GrowableArray<intptr_t>* parent,
      GrowableArray<BitVector*>* assigned_vars,
      intptr_t variable_count);

  void AddCatchEntry(TargetEntryInstr* entry) { catch_entries_.Add(entry); }

  virtual void PrepareEntry(FlowGraphCompiler* compiler);

  Environment* start_env() const { return start_env_; }
  void set_start_env(Environment* env) { start_env_ = env; }

 private:
  TargetEntryInstr* normal_entry_;
  GrowableArray<TargetEntryInstr*> catch_entries_;
  Environment* start_env_;

  DISALLOW_COPY_AND_ASSIGN(GraphEntryInstr);
};


class JoinEntryInstr : public BlockEntryInstr {
 public:
  JoinEntryInstr()
      : BlockEntryInstr(),
        predecessors_(2),  // Two is the assumed to be the common case.
        phis_(NULL),
        phi_count_(0) { }

  DECLARE_INSTRUCTION(JoinEntry)

  virtual intptr_t PredecessorCount() const { return predecessors_.length(); }
  virtual BlockEntryInstr* PredecessorAt(intptr_t index) const {
    return predecessors_[index];
  }
  virtual void AddPredecessor(BlockEntryInstr* predecessor) {
    predecessors_.Add(predecessor);
  }

  ZoneGrowableArray<PhiInstr*>* phis() const { return phis_; }

  virtual void PrepareEntry(FlowGraphCompiler* compiler);

  void InsertPhi(intptr_t var_index, intptr_t var_count);

  intptr_t phi_count() const { return phi_count_; }

 private:
  ZoneGrowableArray<BlockEntryInstr*> predecessors_;
  ZoneGrowableArray<PhiInstr*>* phis_;
  intptr_t phi_count_;

  DISALLOW_COPY_AND_ASSIGN(JoinEntryInstr);
};


class TargetEntryInstr : public BlockEntryInstr {
 public:
  TargetEntryInstr()
      : BlockEntryInstr(),
        predecessor_(NULL),
        try_index_(CatchClauseNode::kInvalidTryIndex) { }

  // Used for exception catch entries.
  explicit TargetEntryInstr(intptr_t try_index)
      : BlockEntryInstr(),
        predecessor_(NULL),
        try_index_(try_index) { }

  DECLARE_INSTRUCTION(TargetEntry)

  virtual intptr_t PredecessorCount() const {
    return (predecessor_ == NULL) ? 0 : 1;
  }
  virtual BlockEntryInstr* PredecessorAt(intptr_t index) const {
    ASSERT((index == 0) && (predecessor_ != NULL));
    return predecessor_;
  }
  virtual void AddPredecessor(BlockEntryInstr* predecessor) {
    ASSERT(predecessor_ == NULL);
    predecessor_ = predecessor;
  }

  bool HasTryIndex() const {
    return try_index_ != CatchClauseNode::kInvalidTryIndex;
  }

  intptr_t try_index() const {
    ASSERT(HasTryIndex());
    return try_index_;
  }

  virtual void PrepareEntry(FlowGraphCompiler* compiler);

 private:
  BlockEntryInstr* predecessor_;
  const intptr_t try_index_;

  DISALLOW_COPY_AND_ASSIGN(TargetEntryInstr);
};


class DoInstr : public Instruction {
 public:
  explicit DoInstr(Computation* computation)
      : computation_(computation) {
    ASSERT(computation != NULL);
    computation->set_instr(this);
  }

  DECLARE_INSTRUCTION(Do)

  Computation* computation() const { return computation_; }
  virtual void replace_computation(Computation* value) { computation_ = value; }

  virtual void RecordAssignedVars(BitVector* assigned_vars);

  virtual LocationSummary* locs() {
    return computation()->locs();
  }

  virtual void EmitNativeCode(FlowGraphCompiler* compiler) {
    computation()->EmitNativeCode(compiler);
  }

 private:
  Computation* computation_;

  DISALLOW_COPY_AND_ASSIGN(DoInstr);
};


// Abstract super-class of all instructions that define a value (Bind, Phi).
class Definition : public Instruction {
 public:
  Definition() : temp_index_(-1), ssa_temp_index_(-1) { }

  virtual bool IsDefinition() const { return true; }
  virtual Definition* AsDefinition() { return this; }

  intptr_t temp_index() const { return temp_index_; }
  void set_temp_index(intptr_t index) { temp_index_ = index; }

  intptr_t ssa_temp_index() const { return ssa_temp_index_; }
  void set_ssa_temp_index(intptr_t index) { ssa_temp_index_ = index; }

 private:
  intptr_t temp_index_;
  intptr_t ssa_temp_index_;

  DISALLOW_COPY_AND_ASSIGN(Definition);
};


Definition* UseVal::definition() const {
  // Check that the definition is either a Phi or a linked in the the IR.
  ASSERT(definition_ != NULL);
  return definition_;
}


class BindInstr : public Definition {
 public:
  explicit BindInstr(Computation* computation)
      : computation_(computation) {
    ASSERT(computation != NULL);
    computation->set_instr(this);
  }

  DECLARE_INSTRUCTION(Bind)

  Computation* computation() const { return computation_; }
  virtual void replace_computation(Computation* value) { computation_ = value; }

  // Static type of the underlying computation.
  virtual RawAbstractType* StaticType() const {
    return computation()->StaticType();
  }

  virtual void RecordAssignedVars(BitVector* assigned_vars);

  virtual LocationSummary* locs() {
    return computation()->locs();
  }

  virtual void EmitNativeCode(FlowGraphCompiler* compiler);

 private:
  Computation* computation_;

  DISALLOW_COPY_AND_ASSIGN(BindInstr);
};


class PhiInstr: public Definition {
 public:
  explicit PhiInstr(intptr_t num_inputs) : inputs_(num_inputs) {
    for (intptr_t i = 0; i < num_inputs; ++i) {
      inputs_.Add(NULL);
    }
  }

  DECLARE_INSTRUCTION(Phi)

 private:
  GrowableArray<Value*> inputs_;

  DISALLOW_COPY_AND_ASSIGN(PhiInstr);
};


class ParameterInstr: public Definition {
 public:
  explicit ParameterInstr(intptr_t index) : index_(index) { }

  intptr_t index() const { return index_; }

  DECLARE_INSTRUCTION(Parameter)

 private:
  intptr_t index_;

  DISALLOW_COPY_AND_ASSIGN(ParameterInstr);
};


class ReturnInstr : public InstructionWithInputs {
 public:
  ReturnInstr(intptr_t token_pos, Value* value)
      : InstructionWithInputs(), token_pos_(token_pos), value_(value) {
    ASSERT(value_ != NULL);
  }

  DECLARE_INSTRUCTION(Return)

  Value* value() const { return value_; }
  intptr_t token_pos() const { return token_pos_; }

  virtual LocationSummary* MakeLocationSummary() const;

  virtual void EmitNativeCode(FlowGraphCompiler* compiler);

 private:
  const intptr_t token_pos_;
  Value* value_;

  DISALLOW_COPY_AND_ASSIGN(ReturnInstr);
};


class ThrowInstr : public InstructionWithInputs {
 public:
  ThrowInstr(intptr_t token_pos,
             intptr_t try_index,
             Value* exception)
      : InstructionWithInputs(),
        token_pos_(token_pos),
        try_index_(try_index),
        exception_(exception) {
    ASSERT(exception_ != NULL);
  }

  DECLARE_INSTRUCTION(Throw)

  intptr_t token_pos() const { return token_pos_; }
  intptr_t try_index() const { return try_index_; }
  Value* exception() const { return exception_; }

  virtual LocationSummary* MakeLocationSummary() const;

  virtual void EmitNativeCode(FlowGraphCompiler* compiler);

 private:
  const intptr_t token_pos_;
  const intptr_t try_index_;
  Value* exception_;

  DISALLOW_COPY_AND_ASSIGN(ThrowInstr);
};


class ReThrowInstr : public InstructionWithInputs {
 public:
  ReThrowInstr(intptr_t token_pos,
               intptr_t try_index,
               Value* exception,
               Value* stack_trace)
      : InstructionWithInputs(),
        token_pos_(token_pos),
        try_index_(try_index),
        exception_(exception),
        stack_trace_(stack_trace) {
    ASSERT(exception_ != NULL);
    ASSERT(stack_trace_ != NULL);
  }

  DECLARE_INSTRUCTION(ReThrow)

  intptr_t token_pos() const { return token_pos_; }
  intptr_t try_index() const { return try_index_; }
  Value* exception() const { return exception_; }
  Value* stack_trace() const { return stack_trace_; }

  virtual LocationSummary* MakeLocationSummary() const;

  virtual void EmitNativeCode(FlowGraphCompiler* compiler);

 private:
  const intptr_t token_pos_;
  const intptr_t try_index_;
  Value* exception_;
  Value* stack_trace_;

  DISALLOW_COPY_AND_ASSIGN(ReThrowInstr);
};


class BranchInstr : public InstructionWithInputs {
 public:
  explicit BranchInstr(Value* value)
      : InstructionWithInputs(),
        value_(value),
        true_successor_(NULL),
        false_successor_(NULL),
        is_fused_with_comparison_(false),
        is_negated_(false) { }

  DECLARE_INSTRUCTION(Branch)

  Value* value() const { return value_; }
  TargetEntryInstr* true_successor() const { return true_successor_; }
  TargetEntryInstr* false_successor() const { return false_successor_; }

  TargetEntryInstr** true_successor_address() { return &true_successor_; }
  TargetEntryInstr** false_successor_address() { return &false_successor_; }

  virtual intptr_t SuccessorCount() const;
  virtual BlockEntryInstr* SuccessorAt(intptr_t index) const;

  virtual void DiscoverBlocks(
      BlockEntryInstr* current_block,
      GrowableArray<BlockEntryInstr*>* preorder,
      GrowableArray<BlockEntryInstr*>* postorder,
      GrowableArray<intptr_t>* parent,
      GrowableArray<BitVector*>* assigned_vars,
      intptr_t variable_count);

  virtual LocationSummary* MakeLocationSummary() const;

  virtual void EmitNativeCode(FlowGraphCompiler* compiler);

  void EmitBranchOnCondition(FlowGraphCompiler* compiler,
                             Condition true_condition);

  void MarkFusedWithComparison() {
    is_fused_with_comparison_ = true;
  }

  bool is_fused_with_comparison() const { return is_fused_with_comparison_; }
  bool is_negated() const { return is_negated_; }
  void set_is_negated(bool value) { is_negated_ = value; }

 private:
  Value* value_;
  TargetEntryInstr* true_successor_;
  TargetEntryInstr* false_successor_;
  bool is_fused_with_comparison_;
  bool is_negated_;

  DISALLOW_COPY_AND_ASSIGN(BranchInstr);
};


class MoveOperands : public ValueObject {
 public:
  MoveOperands(Location dest, Location src) : dest_(dest), src_(src) { }

  Location src() const { return src_; }
  Location dest() const { return dest_; }

 private:
  Location dest_;
  Location src_;
};


class ParallelMoveInstr : public Instruction {
 public:
  ParallelMoveInstr() : moves_(1) { }

  DECLARE_INSTRUCTION(ParallelMove)

  void AddMove(Location dest, Location src) {
    moves_.Add(MoveOperands(dest, src));
  }

  const GrowableArray<MoveOperands>& moves() { return moves_; }

 private:
  GrowableArray<MoveOperands> moves_;

  DISALLOW_COPY_AND_ASSIGN(ParallelMoveInstr);
};

#undef DECLARE_INSTRUCTION


class Environment : public ZoneAllocated {
 public:
  // Construct an environment by copying from an array of values.
  explicit Environment(ZoneGrowableArray<Value*>* values)
      : values_(values->length()) {
          values_.AddArray(*values);
        }

  const ZoneGrowableArray<Value*>& values() const {
    return values_;
  }

  void PrintTo(BufferFormatter* f) const;

 private:
  ZoneGrowableArray<Value*> values_;
  DISALLOW_COPY_AND_ASSIGN(Environment);
};


// Visitor base class to visit each instruction and computation in a flow
// graph as defined by a reversed list of basic blocks.
class FlowGraphVisitor : public ValueObject {
 public:
  explicit FlowGraphVisitor(const GrowableArray<BlockEntryInstr*>& block_order)
      : block_order_(block_order) { }
  virtual ~FlowGraphVisitor() { }

  // Visit each block in the block order, and for each block its
  // instructions in order from the block entry to exit.
  virtual void VisitBlocks();

  // Visit functions for instruction and computation classes, with empty
  // default implementations.
#define DECLARE_VISIT_COMPUTATION(ShortName, ClassName)                        \
  virtual void Visit##ShortName(ClassName* comp) { }

#define DECLARE_VISIT_INSTRUCTION(ShortName)                                   \
  virtual void Visit##ShortName(ShortName##Instr* instr) { }

  FOR_EACH_COMPUTATION(DECLARE_VISIT_COMPUTATION)
  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_COMPUTATION
#undef DECLARE_VISIT_INSTRUCTION

 protected:
  const GrowableArray<BlockEntryInstr*>& block_order_;

 private:
  DISALLOW_COPY_AND_ASSIGN(FlowGraphVisitor);
};


}  // namespace dart

#endif  // VM_INTERMEDIATE_LANGUAGE_H_
