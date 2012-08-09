// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

class BailoutException {
  final String reason;

  const BailoutException(this.reason);
}

class DartBackend extends Backend {
  final List<CompilerTask> tasks;
  final UnparseValidator unparseValidator;

  Map<Element, TreeElements> get resolvedElements() =>
      compiler.enqueuer.resolution.resolvedElements;
  Map<ClassElement, Set<Element>> resolvedClassMembers;

  DartBackend(Compiler compiler, [bool validateUnparse = false])
      : tasks = <CompilerTask>[],
      unparseValidator = new UnparseValidator(compiler, validateUnparse),
      resolvedClassMembers = new Map<ClassElement, Set<Element>>(),
      super(compiler) {
    tasks.add(unparseValidator);
  }

  void enqueueHelpers(Enqueuer world) {
    // TODO(antonm): Implement this method, if needed.
  }

  CodeBuffer codegen(WorkItem work) { return new CodeBuffer(); }

  void processNativeClasses(Enqueuer world,
                            Collection<LibraryElement> libraries) {
  }

  /**
   * Adds given class element with its member element to resolved classes
   * collections.
   */
  void addMemberToClass(Element element, ClassElement classElement) {
    // ${element} should have ${classElement} as enclosing.
    assert(element.enclosingElement == classElement);
    Set<Element> resolvedElementsInClass = resolvedClassMembers.putIfAbsent(
        classElement, () => new Set<Element>());
    resolvedElementsInClass.add(element);
  }

  void assembleProgram() {
    resolvedElements.forEach((element, treeElements) {
      unparseValidator.check(element);
    });

    // TODO(antonm): Eventually bailouts will be proper errors.
    void bailout(String reason) {
      throw new BailoutException(reason);
    }

    /**
     * Tells whether we should output given element. Corelib classes like
     * Object should not be in the resulting code.
     */
    final LIBS_TO_IGNORE = [
      compiler.jsHelperLibrary,
      compiler.interceptorsLibrary,
    ];
    bool shouldOutput(Element element) =>
      element.kind !== ElementKind.VOID &&
      LIBS_TO_IGNORE.indexOf(element.getLibrary()) == -1 &&
      !isDartCoreLib(compiler, element.getLibrary());

    try {
      PlaceholderCollector collector = new PlaceholderCollector(compiler);
      resolvedElements.forEach((element, treeElements) {
        if (!shouldOutput(element)) return;
        collector.collect(element, treeElements);
      });

      ConflictingRenamer renamer =
          new ConflictingRenamer(compiler, collector.placeholders);
      Emitter emitter = new Emitter(compiler, renamer);
      resolvedElements.forEach((element, treeElements) {
        if (!shouldOutput(element)) return;
        if (element.isMember()) {
          var enclosingClass = element.enclosingElement;
          assert(enclosingClass.isClass());
          assert(enclosingClass.isTopLevel());
          addMemberToClass(element, enclosingClass);
          return;
        }
        if (!element.isTopLevel()) {
          bailout('Cannot process non top-level $element');
        }

        emitter.outputElement(element);
      });

      // Now output resolved classes with inner elements we met before.
      resolvedClassMembers.forEach(emitter.outputClass);
      compiler.assembledCode = emitter.toString();
    } catch (BailoutException e) {
      compiler.assembledCode = '''
main() {
  final bailout_reason = "${e.reason}";
}
''';
    }
  }

  log(String message) => compiler.log('[DartBackend] $message');
}

/**
 * Checks if [:libraryElement:] is a core lib, that is a library
 * provided by the implementation like dart:core, dart:coreimpl, etc.
 */
bool isDartCoreLib(Compiler compiler, LibraryElement libraryElement) {
  final libraries = compiler.libraries;
  for (final uri in libraries.getKeys()) {
    if (libraryElement === libraries[uri]) {
      if (uri.startsWith('dart:')) return true;
    }
  }
  return false;
}
