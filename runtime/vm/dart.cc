// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/dart.h"

#include "vm/dart_api_state.h"
#include "vm/flags.h"
#include "vm/freelist.h"
#include "vm/handles.h"
#include "vm/heap.h"
#include "vm/isolate.h"
#include "vm/object.h"
#include "vm/object_store.h"
#include "vm/port.h"
#include "vm/snapshot.h"
#include "vm/stub_code.h"
#include "vm/thread_pool.h"
#include "vm/virtual_memory.h"
#include "vm/zone.h"

namespace dart {

DECLARE_FLAG(bool, print_class_table);
DECLARE_FLAG(bool, trace_isolates);

Isolate* Dart::vm_isolate_ = NULL;
ThreadPool* Dart::thread_pool_ = NULL;
DebugInfo* Dart::pprof_symbol_generator_ = NULL;
FileWriterFunction Dart::flow_graph_writer_ = NULL;

// An object visitor which will mark all visited objects. This is used to
// premark all objects in the vm_isolate_ heap.
class PremarkingVisitor : public ObjectVisitor {
 public:
  void VisitObject(RawObject* obj) {
    // RawInstruction objects are premarked on allocation.
    if (!obj->IsMarked()) {
      obj->SetMarkBit();
    }
  }
};


// TODO(turnidge): We should add a corresponding Dart::Cleanup.
bool Dart::InitOnce(Dart_IsolateCreateCallback create,
                    Dart_IsolateInterruptCallback interrupt,
                    Dart_IsolateShutdownCallback shutdown) {
  // TODO(iposva): Fix race condition here.
  if (vm_isolate_ != NULL || !Flags::Initialized()) {
    return false;
  }
  OS::InitOnce();
  VirtualMemory::InitOnce();
  Isolate::InitOnce();
  PortMap::InitOnce();
  FreeListElement::InitOnce();
  Api::InitOnce();
  // Create the VM isolate and finish the VM initialization.
  ASSERT(thread_pool_ == NULL);
  thread_pool_ = new ThreadPool();
  {
    ASSERT(vm_isolate_ == NULL);
    ASSERT(Flags::Initialized());
    vm_isolate_ = Isolate::Init("vm-isolate");
    Zone zone(vm_isolate_);
    HandleScope handle_scope(vm_isolate_);
    Heap::Init(vm_isolate_);
    ObjectStore::Init(vm_isolate_);
    Object::InitOnce();
    StubCode::InitOnce();
    Scanner::InitOnce();
    PremarkingVisitor premarker;
    vm_isolate_->heap()->IterateOldObjects(&premarker);
  }
  Isolate::SetCurrent(NULL);  // Unregister the VM isolate from this thread.
  Isolate::SetCreateCallback(create);
  Isolate::SetInterruptCallback(interrupt);
  Isolate::SetShutdownCallback(shutdown);
  return true;
}


Isolate* Dart::CreateIsolate(const char* name_prefix) {
  // Create a new isolate.
  Isolate* isolate = Isolate::Init(name_prefix);
  ASSERT(isolate != NULL);
  return isolate;
}


RawError* Dart::InitializeIsolate(const uint8_t* snapshot_buffer, void* data) {
  // Initialize the new isolate.
  TIMERSCOPE(time_isolate_initialization);
  Isolate* isolate = Isolate::Current();
  ASSERT(isolate != NULL);
  Zone zone(isolate);
  HandleScope handle_scope(isolate);
  Heap::Init(isolate);
  ObjectStore::Init(isolate);

  if (snapshot_buffer == NULL) {
    const Error& error = Error::Handle(Object::Init(isolate));
    if (!error.IsNull()) {
      return error.raw();
    }
  } else {
    // Initialize from snapshot (this should replicate the functionality
    // of Object::Init(..) in a regular isolate creation path.
    Object::InitFromSnapshot(isolate);
    const Snapshot* snapshot = Snapshot::SetupFromBuffer(snapshot_buffer);
    if (FLAG_trace_isolates) {
      OS::Print("Size of isolate snapshot = %ld\n", snapshot->length());
    }
    SnapshotReader reader(snapshot, isolate);
    reader.ReadFullSnapshot();
    if (FLAG_trace_isolates) {
      isolate->heap()->PrintSizes();
    }
  }

  StubCode::Init(isolate);
  isolate->heap()->EnableGrowthControl();
  isolate->set_init_callback_data(data);
  if (FLAG_print_class_table) {
    isolate->class_table()->Print();
  }
  return Error::null();
}


void Dart::ShutdownIsolate() {
  Isolate* isolate = Isolate::Current();
  void* callback_data = isolate->init_callback_data();
  isolate->Shutdown();
  delete isolate;

  Dart_IsolateShutdownCallback callback = Isolate::ShutdownCallback();
  if (callback != NULL) {
    (callback)(callback_data);
  }
}

}  // namespace dart
