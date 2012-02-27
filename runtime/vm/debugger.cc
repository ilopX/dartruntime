// Copyright (c) 2011, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/debugger.h"

#include "vm/code_index_table.h"
#include "vm/code_generator.h"
#include "vm/code_patcher.h"
#include "vm/compiler.h"
#include "vm/dart_entry.h"
#include "vm/flags.h"
#include "vm/globals.h"
#include "vm/longjump.h"
#include "vm/object.h"
#include "vm/object_store.h"
#include "vm/os.h"
#include "vm/stack_frame.h"
#include "vm/stub_code.h"
#include "vm/visitor.h"


namespace dart {

static const bool verbose = false;


Breakpoint::Breakpoint(const Function& func, intptr_t pc_desc_index)
    : function_(func.raw()),
      pc_desc_index_(pc_desc_index),
      pc_(0),
      line_number_(-1),
      is_patched_(false),
      next_(NULL) {
  Code& code = Code::Handle(func.code());
  ASSERT(!code.IsNull());  // Function must be compiled.
  PcDescriptors& desc = PcDescriptors::Handle(code.pc_descriptors());
  ASSERT(pc_desc_index < desc.Length());
  this->token_index_ = desc.TokenIndex(pc_desc_index);
  ASSERT(this->token_index_ > 0);
  this->pc_ = desc.PC(pc_desc_index);
  ASSERT(this->pc_ != 0);
  this->breakpoint_kind_ = desc.DescriptorKind(pc_desc_index);
}


RawScript* Breakpoint::SourceCode() {
  const Function& func = Function::Handle(this->function_);
  const Class& cls = Class::Handle(func.owner());
  return cls.script();
}


RawString* Breakpoint::SourceUrl() {
  const Script& script = Script::Handle(this->SourceCode());
  return script.url();
}


intptr_t Breakpoint::LineNumber() {
  // Compute line number lazily since it causes scanning of the script.
  if (this->line_number_ < 0) {
    const Script& script = Script::Handle(this->SourceCode());
    intptr_t ignore_column;
    script.GetTokenLocation(this->token_index_,
                            &this->line_number_, &ignore_column);
  }
  return this->line_number_;
}


void Breakpoint::VisitObjectPointers(ObjectPointerVisitor* visitor) {
  visitor->VisitPointer(reinterpret_cast<RawObject**>(&function_));
}


ActivationFrame::ActivationFrame(uword pc, uword fp, uword sp)
    : pc_(pc), fp_(fp), sp_(sp),
      function_(Function::ZoneHandle()),
      token_index_(-1),
      line_number_(-1),
      var_descriptors_(NULL),
      desc_indices_(8) {
}


const Function& ActivationFrame::DartFunction() {
  if (function_.IsNull()) {
    ASSERT(Isolate::Current() != NULL);
    CodeIndexTable* code_index_table = Isolate::Current()->code_index_table();
    ASSERT(code_index_table != NULL);
    function_ = code_index_table->LookupFunction(pc_);
  }
  return function_;
}


const char* Debugger::QualifiedFunctionName(const Function& func) {
  const String& func_name = String::Handle(func.name());
  Class& func_class = Class::Handle(func.owner());
  String& class_name = String::Handle(func_class.Name());

  const char* kFormat = "%s%s%s";
  intptr_t len = OS::SNPrint(NULL, 0, kFormat,
      func_class.IsTopLevel() ? "" : class_name.ToCString(),
      func_class.IsTopLevel() ? "" : ".",
      func_name.ToCString());
  len++;  // String terminator.
  char* chars = reinterpret_cast<char*>(
      Isolate::Current()->current_zone()->Allocate(len));
  OS::SNPrint(chars, len, kFormat,
              func_class.IsTopLevel() ? "" : class_name.ToCString(),
              func_class.IsTopLevel() ? "" : ".",
              func_name.ToCString());
  return chars;
}


RawString* ActivationFrame::QualifiedFunctionName() {
  const Function& func = DartFunction();
  return String::New(Debugger::QualifiedFunctionName(func));
}


RawString* ActivationFrame::SourceUrl() {
  const Script& script = Script::Handle(SourceScript());
  return script.url();
}


RawScript* ActivationFrame::SourceScript() {
  const Function& func = DartFunction();
  const Class& cls = Class::Handle(func.owner());
  return cls.script();
}


intptr_t ActivationFrame::TokenIndex() {
  if (token_index_ < 0) {
    const Function& func = DartFunction();
    Code& code = Code::Handle(func.code());
    ASSERT(!code.IsNull());
    PcDescriptors& desc = PcDescriptors::Handle(code.pc_descriptors());
    for (int i = 0; i < desc.Length(); i++) {
      if (desc.PC(i) == pc_) {
        token_index_ = desc.TokenIndex(i);
        break;
      }
    }
    ASSERT(token_index_ >= 0);
  }
  return token_index_;
}


intptr_t ActivationFrame::LineNumber() {
  // Compute line number lazily since it causes scanning of the script.
  if (line_number_ < 0) {
    const Script& script = Script::Handle(SourceScript());
    intptr_t ignore_column;
    script.GetTokenLocation(TokenIndex(), &line_number_, &ignore_column);
  }
  return line_number_;
}


void ActivationFrame::GetDescIndices() {
  if (var_descriptors_ == NULL) {
    const Code& code = Code::Handle(DartFunction().code());
    var_descriptors_ =
        &LocalVarDescriptors::ZoneHandle(code.var_descriptors());
    GrowableArray<String*> var_names(8);
    intptr_t activation_token_pos = TokenIndex();
    intptr_t var_desc_len = var_descriptors_->Length();
    for (int cur_idx = 0; cur_idx < var_desc_len; cur_idx++) {
      ASSERT(var_names.length() == desc_indices_.length());
      intptr_t scope_id, begin_pos, end_pos;
      var_descriptors_->GetScopeInfo(cur_idx, &scope_id, &begin_pos, &end_pos);
      if ((begin_pos <= activation_token_pos) &&
          (activation_token_pos <= end_pos)) {
        // The current variable is textually in scope. Now check whether
        // there is another local variable with the same name that shadows
        // or is shadowed by this variable.
        String& var_name = String::Handle(var_descriptors_->GetName(cur_idx));
        intptr_t indices_len = desc_indices_.length();
        bool name_match_found = false;
        for (int i = 0; i < indices_len; i++) {
          if (var_name.Equals(*var_names[i])) {
            // Found two local variables with the same name. Now determine
            // which one is shadowed.
            name_match_found = true;
            intptr_t i_begin_pos, ignore;
            var_descriptors_->GetScopeInfo(
                desc_indices_[i], &ignore, &i_begin_pos, &ignore);
            if (i_begin_pos < begin_pos) {
              // The variable we found earlier is in an outer scope
              // and is shadowed by the current variable. Replace the
              // descriptor index of the previously found variable
              // with the descriptor index of the current variable.
              desc_indices_[i] = cur_idx;
            } else {
              // The variable we found earlier is in an inner scope
              // and shadows the current variable. Skip the current
              // variable. (Nothing to do.)
            }
            break;  // Stop looking for name matches.
          }
        }
        if (!name_match_found) {
          // No duplicate name found. Add the current descriptor index to the
          // list of visible variables.
          desc_indices_.Add(cur_idx);
          var_names.Add(&var_name);
        }
      }
    }
  }
}


intptr_t ActivationFrame::NumLocalVariables() {
  GetDescIndices();
  return desc_indices_.length();
}


void ActivationFrame::VariableAt(intptr_t i,
                                 String* name,
                                 intptr_t* token_pos,
                                 intptr_t* end_pos,
                                 Instance* value) {
  GetDescIndices();
  ASSERT(i < desc_indices_.length());
  ASSERT(name != NULL);
  intptr_t desc_index = desc_indices_[i];
  *name ^= var_descriptors_->GetName(desc_index);
  intptr_t scope_id;
  var_descriptors_->GetScopeInfo(desc_index, &scope_id, token_pos, end_pos);
  ASSERT(value != NULL);
  *value = GetLocalVarValue(var_descriptors_->GetSlotIndex(desc_index));
}


RawArray* ActivationFrame::GetLocalVariables() {
  GetDescIndices();
  intptr_t num_variables = desc_indices_.length();
  String& var_name = String::Handle();
  Instance& value = Instance::Handle();
  const Array& list = Array::Handle(Array::New(2 * num_variables));
  for (int i = 0; i < num_variables; i++) {
    var_name = var_descriptors_->GetName(i);
    list.SetAt(2 * i, var_name);
    value = GetLocalVarValue(var_descriptors_->GetSlotIndex(i));
    list.SetAt((2 * i) + 1, value);
  }
  return list.raw();
}


const char* ActivationFrame::ToCString() {
  const char* kFormat = "Function: '%s' url: '%s' line: %d";

  const Function& func = DartFunction();
  const String& url = String::Handle(SourceUrl());
  intptr_t line = LineNumber();
  const char* func_name = Debugger::QualifiedFunctionName(func);

  intptr_t len =
      OS::SNPrint(NULL, 0, kFormat, func_name, url.ToCString(), line);
  len++;  // String terminator.
  char* chars = reinterpret_cast<char*>(
      Isolate::Current()->current_zone()->Allocate(len));
  OS::SNPrint(chars, len, kFormat, func_name, url.ToCString(), line);
  return chars;
}


void StackTrace::AddActivation(ActivationFrame* frame) {
  this->trace_.Add(frame);
}


void Breakpoint::PatchCode() {
  ASSERT(!is_patched_);
  switch (breakpoint_kind_) {
    case PcDescriptors::kIcCall: {
      int num_args, num_named_args;
      CodePatcher::GetInstanceCallAt(pc_,
          NULL, &num_args, &num_named_args,
          &saved_bytes_.target_address_);
      CodePatcher::PatchInstanceCallAt(
          pc_, StubCode::BreakpointDynamicEntryPoint());
      break;
    }
    case PcDescriptors::kFuncCall: {
      Function& func = Function::Handle();
      CodePatcher::GetStaticCallAt(pc_, &func, &saved_bytes_.target_address_);
      CodePatcher::PatchStaticCallAt(pc_,
          StubCode::BreakpointStaticEntryPoint());
      break;
    }
    case PcDescriptors::kReturn:
      PatchFunctionReturn();
      break;
    default:
      UNREACHABLE();
  }
  is_patched_ = true;
}


void Breakpoint::RestoreCode() {
  ASSERT(is_patched_);
  switch (breakpoint_kind_) {
    case PcDescriptors::kIcCall:
      CodePatcher::PatchInstanceCallAt(pc_, saved_bytes_.target_address_);
      break;
    case PcDescriptors::kFuncCall:
      CodePatcher::PatchStaticCallAt(pc_, saved_bytes_.target_address_);
      break;
    case PcDescriptors::kReturn:
      RestoreFunctionReturn();
      break;
    default:
      UNREACHABLE();
  }
  is_patched_ = false;
}

void Breakpoint::SetActive(bool value) {
  if (value && !is_patched_) {
    PatchCode();
    return;
  }
  if (!value && is_patched_) {
    RestoreCode();
  }
}


bool Breakpoint::IsActive() {
  return is_patched_;
}


Debugger::Debugger()
    : isolate_(NULL),
      initialized_(false),
      bp_handler_(NULL),
      breakpoints_(NULL),
      resume_action_(kContinue) {
}


Debugger::~Debugger() {
  ASSERT(breakpoints_ == NULL);
}


void Debugger::Shutdown() {
  while (breakpoints_ != NULL) {
    Breakpoint* bpt = breakpoints_;
    breakpoints_ = breakpoints_->next();
    UnsetBreakpoint(bpt);
    delete bpt;
  }
}


bool Debugger::IsActive() {
  // TODO(hausner): The code generator uses this function to prevent
  // generation of optimized code when Dart code is being debugged.
  // This is probably not conservative enough (we could set the first
  // breakpoint after optimized code has already been produced).
  // Long-term, we need to be able to de-optimize code.
  return breakpoints_ != NULL;
}


static RawFunction* ResolveLibraryFunction(
                        const Library& library,
                        const String& fname) {
  ASSERT(!library.IsNull());
  Function& function = Function::Handle();
  const Object& object = Object::Handle(library.LookupObject(fname));
  if (!object.IsNull() && object.IsFunction()) {
    function ^= object.raw();
  }
  return function.raw();
}


RawFunction* Debugger::ResolveFunction(const Library& library,
                                       const String& class_name,
                                       const String& function_name) {
  ASSERT(!library.IsNull());
  ASSERT(!class_name.IsNull());
  ASSERT(!function_name.IsNull());
  if (class_name.Length() == 0) {
    return ResolveLibraryFunction(library, function_name);
  }
  const Class& cls = Class::Handle(library.LookupClass(class_name));
  Function& function = Function::Handle();
  if (!cls.IsNull()) {
    function = cls.LookupStaticFunction(function_name);
    if (function.IsNull()) {
      function = cls.LookupDynamicFunction(function_name);
    }
  }
  return function.raw();
}


void Debugger::InstrumentForStepping(const Function &target_function) {
  if (!target_function.HasCode()) {
    Compiler::CompileFunction(target_function);
    // If there were any errors, ignore them silently and return without
    // adding breakpoints to target.
    if (!target_function.HasCode()) {
      return;
    }
  }
  Code& code = Code::Handle(target_function.code());
  ASSERT(!code.IsNull());
  PcDescriptors& desc = PcDescriptors::Handle(code.pc_descriptors());
  for (int i = 0; i < desc.Length(); i++) {
    Breakpoint* bpt = GetBreakpoint(desc.PC(i));
    if (bpt != NULL) {
      // There is already a breakpoint for this address. Leave it alone.
      continue;
    }
    PcDescriptors::Kind kind = desc.DescriptorKind(i);
    if ((kind == PcDescriptors::kIcCall) ||
        (kind == PcDescriptors::kFuncCall) ||
        (kind == PcDescriptors::kReturn)) {
      bpt = new Breakpoint(target_function, i);
      bpt->set_temporary(true);
      bpt->PatchCode();
      RegisterBreakpoint(bpt);
    }
  }
}


// TODO(hausner): Distinguish between newly created breakpoints and
// returning a breakpoint that already exists?
Breakpoint* Debugger::SetBreakpoint(const Function& target_function,
                                    intptr_t token_index,
                                    Error* error) {
  if ((token_index < target_function.token_index()) ||
      (target_function.end_token_index() <= token_index)) {
    // The given token position is not within the target function.
    return NULL;
  }
  if (!target_function.HasCode()) {
    *error = Compiler::CompileFunction(target_function);
    if (!error->IsNull()) {
      return NULL;
    }
  }
  Code& code = Code::Handle(target_function.code());
  ASSERT(!code.IsNull());
  PcDescriptors& desc = PcDescriptors::Handle(code.pc_descriptors());
  for (int i = 0; i < desc.Length(); i++) {
    if (desc.TokenIndex(i) < token_index) {
      continue;
    }
    Breakpoint* bpt = GetBreakpoint(desc.PC(i));
    if (bpt != NULL) {
      // Found existing breakpoint.
      return bpt;
    }
    PcDescriptors::Kind kind = desc.DescriptorKind(i);
    if ((kind == PcDescriptors::kIcCall) ||
        (kind == PcDescriptors::kFuncCall) ||
        (kind == PcDescriptors::kReturn)) {
      bpt = new Breakpoint(target_function, i);
      bpt->PatchCode();
      RegisterBreakpoint(bpt);
      if (verbose) {
        OS::Print("Setting breakpoint at '%s' line %d  (PC %p)\n",
                  String::Handle(bpt->SourceUrl()).ToCString(),
                  bpt->LineNumber(),
                  bpt->pc());
      }
      return bpt;
    }
  }
  return NULL;
}


void Debugger::UnsetBreakpoint(Breakpoint* bpt) {
  bpt->SetActive(false);
}


Breakpoint* Debugger::SetBreakpointAtEntry(const Function& target_function,
                                           Error* error) {
  ASSERT(!target_function.IsNull());
  return SetBreakpoint(target_function, target_function.token_index(), error);
}


Breakpoint* Debugger::SetBreakpointAtLine(const String& script_url,
                                          intptr_t line_number,
                                          Error* error) {
  Library& lib = Library::Handle();
  Script& script = Script::Handle();
  lib = isolate_->object_store()->registered_libraries();
  while (!lib.IsNull()) {
    script = lib.LookupScript(script_url);
    if (!script.IsNull()) {
      break;
    }
    lib = lib.next_registered();
  }
  if (script.IsNull()) {
    return NULL;
  }
  intptr_t token_index_at_line = script.TokenIndexAtLine(line_number);
  if (token_index_at_line < 0) {
    // Script does not contain the given line number.
    return NULL;
  }
  const Function& func =
      Function::Handle(lib.LookupFunctionInScript(script, token_index_at_line));
  if (func.IsNull()) {
    return NULL;
  }
  return SetBreakpoint(func, token_index_at_line, error);
}


static RawArray* MakeNameValueList(const GrowableArray<Object*>& pairs) {
  int pairs_len = pairs.length();
  ASSERT(pairs_len % 2 == 0);
  const Array& list = Array::Handle(Array::New(pairs_len));
  for (int i = 0; i < pairs_len; i++) {
    list.SetAt(i, *pairs[i]);
  }
  return list.raw();
}


// TODO(hausner): Merge some of this functionality with the code in
// dart_api_impl.cc.
RawObject* Debugger::GetInstanceField(const Class& cls,
                                      const String& field_name,
                                      const Instance& object) {
  const Function& getter_func =
      Function::Handle(cls.LookupGetterFunction(field_name));
  ASSERT(!getter_func.IsNull());

  Object& result = Object::Handle();
  LongJump* base = isolate_->long_jump_base();
  LongJump jump;
  isolate_->set_long_jump_base(&jump);
  if (setjmp(*jump.Set()) == 0) {
    GrowableArray<const Object*> noArguments;
    const Array& noArgumentNames = Array::Handle();
    result = DartEntry::InvokeDynamic(object, getter_func,
                                      noArguments, noArgumentNames);
  } else {
    result = isolate_->object_store()->sticky_error();
  }
  isolate_->set_long_jump_base(base);
  return result.raw();
}


RawObject* Debugger::GetStaticField(const Class& cls,
                                    const String& field_name) {
  const Function& getter_func =
      Function::Handle(cls.LookupGetterFunction(field_name));
  ASSERT(!getter_func.IsNull());

  Object& result = Object::Handle();
  LongJump* base = isolate_->long_jump_base();
  LongJump jump;
  isolate_->set_long_jump_base(&jump);
  if (setjmp(*jump.Set()) == 0) {
    GrowableArray<const Object*> noArguments;
    const Array& noArgumentNames = Array::Handle();
    result = DartEntry::InvokeStatic(getter_func, noArguments, noArgumentNames);
  } else {
    result = isolate_->object_store()->sticky_error();
  }
  isolate_->set_long_jump_base(base);
  return result.raw();
}


RawArray* Debugger::GetInstanceFields(const Instance& obj) {
  Class& cls = Class::Handle(obj.clazz());
  Array& fields = Array::Handle();
  Field& field = Field::Handle();
  GrowableArray<Object*> field_list(8);
  // Iterate over fields in class hierarchy to count all instance fields.
  while (!cls.IsNull()) {
    fields = cls.fields();
    for (int i = 0; i < fields.Length(); i++) {
      field ^= fields.At(i);
      if (!field.is_static()) {
        String& field_name = String::Handle(field.name());
        field_list.Add(&field_name);
        Object& field_value = Object::Handle();
        field_value = GetInstanceField(cls, field_name, obj);
        field_list.Add(&field_value);
      }
    }
    cls = cls.SuperClass();
  }
  return MakeNameValueList(field_list);
}


RawArray* Debugger::GetStaticFields(const Class& cls) {
  GrowableArray<Object*> field_list(8);
  Array& fields = Array::Handle(cls.fields());
  Field& field = Field::Handle();
  for (int i = 0; i < fields.Length(); i++) {
    field ^= fields.At(i);
    if (field.is_static()) {
      String& field_name = String::Handle(field.name());
      Object& field_value = Object::Handle(GetStaticField(cls, field_name));
      field_list.Add(&field_name);
      field_list.Add(&field_value);
    }
  }
  return MakeNameValueList(field_list);
}


void Debugger::VisitObjectPointers(ObjectPointerVisitor* visitor) {
  ASSERT(visitor != NULL);
  Breakpoint* bpt = this->breakpoints_;
  while (bpt != NULL) {
    bpt->VisitObjectPointers(visitor);
    bpt = bpt->next();
  }
}


static void DefaultBreakpointHandler(Breakpoint* bpt, StackTrace* stack) {
  String& var_name = String::Handle();
  Instance& value = Instance::Handle();
  for (intptr_t i = 0; i < stack->Length(); i++) {
    ActivationFrame* frame = stack->ActivationFrameAt(i);
    OS::Print("   %d. %s\n",
              i + 1, frame->ToCString());
    intptr_t num_locals = frame->NumLocalVariables();
    for (intptr_t i = 0; i < num_locals; i++) {
      intptr_t token_pos, end_pos;
      frame->VariableAt(i, &var_name, &token_pos, &end_pos, &value);
      OS::Print("      var %s (pos %d) = %s\n",
                var_name.ToCString(), token_pos, value.ToCString());
    }
  }
}


void Debugger::SetBreakpointHandler(BreakpointHandler* handler) {
  bp_handler_ = handler;
  if (bp_handler_ == NULL) {
    bp_handler_ = &DefaultBreakpointHandler;
  }
}


void Debugger::BreakpointCallback() {
  ASSERT(initialized_);
  DartFrameIterator iterator;
  DartFrame* frame = iterator.NextFrame();
  ASSERT(frame != NULL);
  Breakpoint* bpt = GetBreakpoint(frame->pc());
  ASSERT(bpt != NULL);
  if (verbose) {
    OS::Print(">>> %s breakpoint at %s:%d (Address %p)\n",
        bpt->is_temporary() ? "hit temp" : "hit user",
        bpt ? String::Handle(bpt->SourceUrl()).ToCString() : "?",
        bpt ? bpt->LineNumber() : 0,
        frame->pc());
  }
  StackTrace* stack_trace = new StackTrace(8);
  while (frame != NULL) {
    ASSERT(frame->IsValid());
    ASSERT(frame->IsDartFrame());
    ActivationFrame* activation =
        new ActivationFrame(frame->pc(), frame->fp(), frame->sp());
    stack_trace->AddActivation(activation);
    frame = iterator.NextFrame();
  }

  resume_action_ = kContinue;
  if (bp_handler_ != NULL) {
    (*bp_handler_)(bpt, stack_trace);
  }

  if (resume_action_ == kContinue) {
    RemoveTemporaryBreakpoints();
  } else if (resume_action_ == kStepOver) {
    Function& func = Function::Handle(bpt->function());
    if (bpt->breakpoint_kind_ == PcDescriptors::kReturn) {
      // If we are at the function return, do a StepOut action.
      if (stack_trace->Length() > 1) {
        ActivationFrame* caller = stack_trace->ActivationFrameAt(1);
        func = caller->DartFunction().raw();
        RemoveTemporaryBreakpoints();
      }
    }
    InstrumentForStepping(func);
  } else if (resume_action_ == kStepInto) {
    RemoveTemporaryBreakpoints();
    if (bpt->breakpoint_kind_ == PcDescriptors::kIcCall) {
      int num_args, num_named_args;
      uword target;
      CodePatcher::GetInstanceCallAt(bpt->pc_, NULL,
          &num_args, &num_named_args, &target);
      ActivationFrame* top_frame = stack_trace->ActivationFrameAt(0);
      Instance& receiver = Instance::Handle(
          top_frame->GetInstanceCallReceiver(num_args));
      Code& code = Code::Handle(
          ResolveCompileInstanceCallTarget(isolate_, receiver));
      if (!code.IsNull()) {
        Function& callee = Function::Handle(code.function());
        InstrumentForStepping(callee);
      }
    } else if (bpt->breakpoint_kind_ == PcDescriptors::kFuncCall) {
      Function& callee = Function::Handle();
      uword target;
      CodePatcher::GetStaticCallAt(bpt->pc_, &callee, &target);
      InstrumentForStepping(callee);
    } else {
      ASSERT(bpt->breakpoint_kind_ == PcDescriptors::kReturn);
      // Treat like stepping out to caller.
      if (stack_trace->Length() > 1) {
        ActivationFrame* caller = stack_trace->ActivationFrameAt(1);
        InstrumentForStepping(caller->DartFunction());
      }
    }
  } else {
    ASSERT(resume_action_ == kStepOut);
    // Set temporary breakpoints in the caller.
    RemoveTemporaryBreakpoints();
    if (stack_trace->Length() > 1) {
      ActivationFrame* caller = stack_trace->ActivationFrameAt(1);
      InstrumentForStepping(caller->DartFunction());
    }
  }
}


void Debugger::Initialize(Isolate* isolate) {
  if (initialized_) {
    return;
  }
  isolate_ = isolate;
  initialized_ = true;
  SetBreakpointHandler(DefaultBreakpointHandler);
}


Breakpoint* Debugger::GetBreakpoint(uword breakpoint_address) {
  Breakpoint* bpt = this->breakpoints_;
  while (bpt != NULL) {
    if (bpt->pc() == breakpoint_address) {
      return bpt;
    }
    bpt = bpt->next();
  }
  return NULL;
}


void Debugger::RemoveBreakpoint(Breakpoint* bpt) {
  ASSERT(breakpoints_ != NULL);
  Breakpoint* prev_bpt = NULL;
  Breakpoint* curr_bpt = breakpoints_;
  while (curr_bpt != NULL) {
    if (bpt == curr_bpt) {
      if (prev_bpt == NULL) {
        breakpoints_ = breakpoints_->next();
      } else {
        prev_bpt->set_next(curr_bpt->next());
      }
      UnsetBreakpoint(bpt);
      delete bpt;
      return;
    }
    prev_bpt = curr_bpt;
    curr_bpt = curr_bpt->next();
  }
  // bpt is not a registered breakpoint, nothing to do.
}


void Debugger::RemoveTemporaryBreakpoints() {
  Breakpoint* prev_bpt = NULL;
  Breakpoint* curr_bpt = breakpoints_;
  while (curr_bpt != NULL) {
    if (curr_bpt->is_temporary()) {
      if (prev_bpt == NULL) {
        breakpoints_ = breakpoints_->next();
      } else {
        prev_bpt->set_next(curr_bpt->next());
      }
      Breakpoint* temp_bpt = curr_bpt;
      curr_bpt = curr_bpt->next();
      UnsetBreakpoint(temp_bpt);
      delete temp_bpt;
    } else {
      prev_bpt = curr_bpt;
      curr_bpt = curr_bpt->next();
    }
  }
}


Breakpoint* Debugger::GetBreakpointByFunction(const Function& func,
                                              intptr_t token_index) {
  Breakpoint* bpt = this->breakpoints_;
  while (bpt != NULL) {
    if ((bpt->function() == func.raw()) &&
        (bpt->token_index() == token_index)) {
      return bpt;
    }
    bpt = bpt->next();
  }
  return NULL;
}


void Debugger::RegisterBreakpoint(Breakpoint* bpt) {
  ASSERT(bpt->next() == NULL);
  bpt->set_next(this->breakpoints_);
  this->breakpoints_ = bpt;
}


}  // namespace dart
