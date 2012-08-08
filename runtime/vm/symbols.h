// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef VM_SYMBOLS_H_
#define VM_SYMBOLS_H_

#include "vm/object.h"
#include "vm/snapshot_ids.h"

namespace dart {

// Forward declarations.
class Isolate;
class ObjectPointerVisitor;

#define PREDEFINED_SYMBOLS_LIST(V)                                             \
  V(Dot, ".")                                                                  \
  V(Equals, "=")                                                               \
  V(IndexToken, "[]")                                                          \
  V(AssignIndexToken, "[]=")                                                   \
  V(TopLevel, "::")                                                            \
  V(Empty, "")                                                                 \
  V(This, "this")                                                              \
  V(HasNext, "hasNext")                                                        \
  V(Next, "next")                                                              \
  V(Value, "value")                                                            \
  V(ExprTemp, ":expr_temp")                                                    \
  V(AnonymousClosure, "<anonymous closure>")                                   \
  V(PhaseParameter, ":phase")                                                  \
  V(TypeArgumentsParameter, ":type_arguments")                                 \
  V(AssertionError, "AssertionError")                                          \
  V(TypeError, "TypeError")                                                    \
  V(FallThroughError, "FallThroughError")                                      \
  V(StaticResolutionException, "StaticResolutionException")                    \
  V(ThrowNew, "_throwNew")                                                     \
  V(ListLiteralFactoryClass, "_ListLiteralFactory")                            \
  V(ListLiteralFactory, "List.fromLiteral")                                    \
  V(MapLiteralFactoryClass, "_MapLiteralFactory")                              \
  V(MapLiteralFactory, "Map.fromLiteral")                                      \
  V(ImmutableMap, "ImmutableMap")                                              \
  V(ImmutableMapConstructor, "ImmutableMap._create")                           \
  V(StringBase, "StringBase")                                                  \
  V(Interpolate, "_interpolate")                                               \
  V(GetIterator, "iterator")                                                   \
  V(NoSuchMethod, "noSuchMethod")                                              \
  V(SavedContextVar, ":saved_context_var")                                     \
  V(ExceptionVar, ":exception_var")                                            \
  V(StacktraceVar, ":stacktrace_var")                                          \
  V(ListLiteralElement, "list literal element")                                \
  V(ForInIter, ":for-in-iter")                                                 \
  V(Library, "library")                                                        \
  V(Import, "import")                                                          \
  V(Source, "source")                                                          \
  V(Resource, "resource")                                                      \

// Contains a list of frequently used strings in a canonicalized form. This
// list is kept in the vm_isolate in order to share the copy across isolates
// without having to maintain copies in each isolate.
class Symbols : public AllStatic {
 public:
  // List of strings that are pre created in the vm isolate.
  enum {
    kIllegal = 0,

#define DEFINE_SYMBOL_INDEX(symbol, literal)                                   \
    k##symbol,
PREDEFINED_SYMBOLS_LIST(DEFINE_SYMBOL_INDEX)
#undef DEFINE_SYMBOL_INDEX

    kMaxId,
  };

  // Access methods for symbols stored in the vm isolate.
#define DEFINE_SYMBOL_ACCESSOR(symbol, literal)                                \
  static RawString* symbol() { return predefined_[k##symbol]; }
PREDEFINED_SYMBOLS_LIST(DEFINE_SYMBOL_ACCESSOR)
#undef DEFINE_SYMBOL_ACCESSOR

  // Initialize frequently used symbols in the vm isolate.
  static void InitOnce(Isolate* isolate);

  // Initialize and setup a symbol table for the isolate.
  static void SetupSymbolTable(Isolate* isolate);

  // Get number of symbols in an isolate's symbol table.
  static intptr_t Size(Isolate* isolate);

  // Helper functions to create a symbol given a string or set of characters.
  static RawString* New(const char* str);
  template<typename T>
  static RawString* New(const T* characters, intptr_t len);
  static RawString* New(const String& str);
  static RawString* New(const String& str,
                        intptr_t begin_index,
                        intptr_t length);


 private:
  enum {
    kInitialVMIsolateSymtabSize = ((Symbols::kMaxId + 15) & -16),
    kInitialSymtabSize = 256
  };

  // Add the string into the VM isolate symbol table.
  static void Add(const Array& symbol_table, const String& str);

  // Insert symbol into symbol table, growing it if necessary.
  static void InsertIntoSymbolTable(const Array& symbol_table,
                                    const String& symbol,
                                    intptr_t index);

  // Grow the symbol table.
  static void GrowSymbolTable(const Array& symbol_table);

  // Return index in symbol table if the symbol already exists or
  // return the index into which the new symbol can be added.
  template<typename T>
  static intptr_t FindIndex(const Array& symbol_table,
                            const T* characters,
                            intptr_t len,
                            intptr_t hash);
  static intptr_t FindIndex(const Array& symbol_table,
                            const String& str,
                            intptr_t begin_index,
                            intptr_t len,
                            intptr_t hash);
  static intptr_t LookupVMSymbol(RawObject* obj);
  static RawObject* GetVMSymbol(intptr_t object_id);
  static bool IsVMSymbolId(intptr_t object_id) {
    return (object_id >= kMaxPredefinedObjectIds &&
            object_id < (kMaxPredefinedObjectIds + Symbols::kMaxId));
  }

  // List of symbols that are stored in the vm isolate for easy access.
  static RawString* predefined_[Symbols::kMaxId];

  friend class SnapshotReader;
  friend class SnapshotWriter;
  friend class ApiMessageReader;

  DISALLOW_COPY_AND_ASSIGN(Symbols);
};

}  // namespace dart

#endif  // VM_SYMBOLS_H_
