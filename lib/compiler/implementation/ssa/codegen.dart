// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

class SsaCodeGeneratorTask extends CompilerTask {
  final JavaScriptBackend backend;
  SsaCodeGeneratorTask(JavaScriptBackend backend)
      : this.backend = backend,
        super(backend.compiler);
  String get name() => 'SSA code generator';
  NativeEmitter get nativeEmitter() => backend.emitter.nativeEmitter;


  String buildJavaScriptFunction(FunctionElement element,
                                 String parameters,
                                 String body) {
    String extraSpace = "";
    // Members are emitted inside a JavaScript object literal. To line up the
    // indentation we want the closing curly brace to be indented by one space.
    // Example:
    // defineClass("A", "B", ... , {
    //  foo$1: function(..) {
    //  },  /* <========== indent by 1. */
    //  bar$2: function(..) {
    //  },  /* <========== indent by 1. */
    //
    // For static functions this is not necessary:
    // $.staticFun = function() {
    //   ...
    // };
    if (element.isInstanceMember() ||
        element.kind == ElementKind.GENERATIVE_CONSTRUCTOR_BODY) {
      extraSpace = " ";
    }
    return 'function($parameters) {\n$body$extraSpace}';
  }

  String generateMethod(WorkItem work, HGraph graph) {
    return measure(() {
      compiler.tracer.traceGraph("codegen", graph);
      Map<Element, String> parameterNames = getParameterNames(work);
      String parameters = Strings.join(parameterNames.getValues(), ', ');
      SsaOptimizedCodeGenerator codegen = new SsaOptimizedCodeGenerator(
          backend, work, parameters, parameterNames);
      codegen.visitGraph(graph);

      FunctionElement element = work.element;
      String code;
      if (element.isInstanceMember()
          && element.enclosingElement.isClass()
          && element.enclosingElement.isNative()
          && native.isOverriddenMethod(
              element, element.enclosingElement, nativeEmitter)) {
        // Record that this method is overridden. In case of optional
        // arguments, the emitter will generate stubs to handle them,
        // and needs to know if the method is overridden.
        nativeEmitter.overriddenMethods.add(element);
        StringBuffer buffer = new StringBuffer();
        native.generateMethodWithPrototypeCheckForElement(
            compiler, buffer, element, '${codegen.buffer}', parameters);
        code = buffer.toString();
      } else {
        code = codegen.buffer.toString();
      }
      return buildJavaScriptFunction(element, parameters, code);
    });
  }

  String generateBailoutMethod(WorkItem work, HGraph graph) {
    return measure(() {
      compiler.tracer.traceGraph("codegen-bailout", graph);
      new SsaBailoutPropagator(compiler).visitGraph(graph);

      Map<Element, String> parameterNames = getParameterNames(work);
      String parameters = Strings.join(parameterNames.getValues(), ', ');
      SsaUnoptimizedCodeGenerator codegen = new SsaUnoptimizedCodeGenerator(
          backend, work, parameters, parameterNames);
      codegen.visitGraph(graph);

      StringBuffer newParameters = new StringBuffer();
      if (!parameterNames.isEmpty()) newParameters.add('$parameters, ');
      newParameters.add('state');

      for (int i = 0; i < codegen.maxBailoutParameters; i++) {
        newParameters.add(', env$i');
      }

      Element element = work.element;
      String body = '${codegen.setup}${codegen.buffer}';
      return buildJavaScriptFunction(element, newParameters.toString(), body);
    });
  }

  Map<Element, String> getParameterNames(WorkItem work) {
    Map<Element, String> parameterNames = new LinkedHashMap<Element, String>();
    FunctionElement function = work.element;

    // The dom/html libraries have inline JS code that reference
    // parameter names directly. Long-term such code will be rejected.
    // Now, just don't mangle the parameter name.
    function.computeSignature(compiler).forEachParameter((Element element) {
      parameterNames[element] = function.isNative()
          ? element.name.slowToString()
          : JsNames.getValid('${element.name.slowToString()}');
    });
    return parameterNames;
  }
}

typedef void ElementAction(Element element);

class SsaCodeGenerator implements HVisitor, HBlockInformationVisitor {
  /**
   * Current state for generating simple (non-local-control) code.
   * It is generated as either statements (indented and ';'-terminated),
   * expressions (comma separated) or declarations (also comma separated,
   * but expected to be preceeded by a 'var' so it declares its variables);
   */
  static final int STATE_STATEMENT = 0;
  static final int STATE_FIRST_EXPRESSION = 1;
  static final int STATE_FIRST_DECLARATION = 2;
  static final int STATE_EXPRESSION = 3;
  static final int STATE_DECLARATION = 4;

  /**
   * Returned by [expressionType] to tell how code can be generated for
   * a subgraph.
   * - [TYPE_STATEMENT] means that the graph must be generated as a statement,
   * which is always possible.
   * - [TYPE_EXPRESSION] means that the graph can be generated as an expression,
   * or possibly several comma-separated expressions.
   * - [TYPE_DECLARATION] means that the graph can be generated as an
   * expression, and that it only generates expressions of the form
   *   variable = expression
   * which are also valid as parts of a "var" declaration.
   */
  static final int TYPE_STATEMENT = 0;
  static final int TYPE_EXPRESSION = 1;
  static final int TYPE_DECLARATION = 2;

  static final String TEMPORARY_PREFIX = 't';

  final JavaScriptBackend backend;
  final WorkItem work;
  final StringBuffer buffer;
  final String parameters;

  final Map<Element, String> parameterNames;
  final Map<int, String> names;
  final Set<String> usedNames;
  final Set<HInstruction> declaredInstructions;
  final Map<String, int> prefixes;
  final Set<HInstruction> generateAtUseSite;
  final Map<HPhi, String> logicalOperations;
  final Map<Element, ElementAction> breakAction;
  final Map<Element, ElementAction> continueAction;
  final Equivalence<HPhi> phiEquivalence;

  Element equalsNullElement;
  Element boolifiedEqualsNullElement;
  int indent = 0;
  int expectedPrecedence = JSPrecedence.STATEMENT_PRECEDENCE;
  JSBinaryOperatorPrecedence unsignedShiftPrecedences;
  HGraph currentGraph;
  /**
   * Whether the code-generation should try to generate an expression
   * instead of a sequence of statements.
   */
  int generationState = STATE_STATEMENT;
  /**
   * While generating expressions, we can't insert variable declarations.
   * Instead we declare them at the end of the function
   */
  Link<String> delayedVarDecl = const EmptyLink<String>();
  HBasicBlock currentBlock;

  // Records a block-information that is being handled specially.
  // Used to break bad recursion.
  HBlockInformation currentBlockInformation;
  // The subgraph is used to delimit traversal for some constructions, e.g.,
  // if branches.
  SubGraph subGraph;

  LibraryElement get currentLibrary() => work.element.getLibrary();
  Compiler get compiler() => backend.compiler;
  NativeEmitter get nativeEmitter() => backend.emitter.nativeEmitter;
  Enqueuer get world() => backend.compiler.enqueuer.codegen;

  bool isGenerateAtUseSite(HInstruction instruction) {
    return generateAtUseSite.contains(instruction);
  }

  SsaCodeGenerator(this.backend,
                   this.work,
                   this.parameters,
                   this.parameterNames)
    : names = new Map<int, String>(),
      prefixes = new Map<String, int>(),
      usedNames = new Set<String>(),
      declaredInstructions = new Set<HInstruction>(),
      buffer = new StringBuffer(),
      generateAtUseSite = new Set<HInstruction>(),
      logicalOperations = new Map<HPhi, String>(),
      breakAction = new Map<Element, ElementAction>(),
      continueAction = new Map<Element, ElementAction>(),
      phiEquivalence = new Equivalence<HPhi>(),
      unsignedShiftPrecedences = JSPrecedence.binary['>>>'] {

    for (final name in parameterNames.getValues()) {
      prefixes[name] = 0;
    }

    // Create a namespace for temporaries.
    prefixes[TEMPORARY_PREFIX] = 0;

    Interceptors interceptors = backend.builder.interceptors;
    equalsNullElement = interceptors.getEqualsNullInterceptor();
    boolifiedEqualsNullElement =
        interceptors.getBoolifiedVersionOf(equalsNullElement);
  }

  abstract visitTypeGuard(HTypeGuard node);

  abstract beginGraph(HGraph graph);
  abstract endGraph(HGraph graph);

  abstract beginLoop(HBasicBlock block);
  abstract endLoop(HBasicBlock block);
  abstract handleLoopCondition(HLoopBranch node);

  abstract startIf(HIf node);
  abstract endIf(HIf node);
  abstract startThen(HIf node);
  abstract endThen(HIf node);
  abstract startElse(HIf node);
  abstract endElse(HIf node);

  abstract preLabeledBlock(HLabeledBlockInformation labeledBlockInfo);
  abstract startLabeledBlock(HLabeledBlockInformation labeledBlockInfo);
  abstract endLabeledBlock(HLabeledBlockInformation labeledBlockInfo);

  void beginExpression(int precedence) {
    if (precedence < expectedPrecedence) {
      buffer.add('(');
    }
  }

  void endExpression(int precedence) {
    if (precedence < expectedPrecedence) {
      buffer.add(')');
    }
  }

  void preGenerateMethod(HGraph graph) {
    new SsaInstructionMerger(generateAtUseSite).visitGraph(graph);
    new SsaConditionMerger(generateAtUseSite,
                           logicalOperations).visitGraph(graph);
    new PhiEquivalator(phiEquivalence, logicalOperations).analyzeGraph(graph);
  }

  visitGraph(HGraph graph) {
    preGenerateMethod(graph);
    currentGraph = graph;
    indent++;  // We are already inside a function.
    subGraph = new SubGraph(graph.entry, graph.exit);
    beginGraph(graph);
    visitBasicBlock(graph.entry);
    if (!delayedVarDecl.isEmpty()) {
      addIndented("var ");
      while (true) {
        buffer.add(delayedVarDecl.head);
        delayedVarDecl = delayedVarDecl.tail;
        if (delayedVarDecl.isEmpty()) break;
        buffer.add(", ");
      }
      buffer.add(";\n");
    }
    endGraph(graph);
  }

  void visitSubGraph(SubGraph newSubGraph) {
    SubGraph oldSubGraph = subGraph;
    subGraph = newSubGraph;
    visitBasicBlock(subGraph.start);
    subGraph = oldSubGraph;
  }

  /**
   * Check whether a sub-graph can be generated as an expression, or even
   * as a declaration, or if it has to fall back to being generated as
   * a statement.
   * Expressions are anything that doesn't generate control flow constructs.
   * Declarations must only generate assignments on the form "id = expression",
   * and not, e.g., expressions where the value isn't assigned, or where it's
   * assigned to something that's not a simple variable.
   */
  int expressionType(HExpressionInformation info) {
    // The only HExpressionInformation used as part of a HBlockInformation is
    // current HSubExpressionBlockInformation, so it's the only one reaching
    // here. If we start using the other HExpressionInformation types too,
    // this code should be generalized.
    assert(info is HSubExpressionBlockInformation);
    HSubExpressionBlockInformation expressionInfo = info;
    SubGraph limits = expressionInfo.subExpression;

    // Start assuming that we can generate declarations. If we find a
    // counter-example, we degrade our assumption to either expression or
    // statement, and in the latter case, we can return immediately since
    // it can't get any worse. E.g., a function call where the return value
    // isn't used can't be in a declaration. A bailout can't be in an
    // expression.
    int result = TYPE_DECLARATION;
    HBasicBlock basicBlock = limits.start;
    do {
      HInstruction current = basicBlock.first;
      while (current != basicBlock.last) {
        // E.g, type guards.
        if (current.isControlFlow()) {
          return TYPE_STATEMENT;
        }
        // HFieldSet generates code on the form x.y = ..., which isn't
        // valid in a declaration, but it also always have no uses, so
        // it's caught by that test too.
        assert(current is! HFieldSet || current.usedBy.isEmpty());
        if (current.usedBy.isEmpty()) {
          result = TYPE_EXPRESSION;
        }
        current = current.next;
      }
      if (current is HGoto) {
        basicBlock = basicBlock.successors[0];
      } else if (current is HConditionalBranch) {
        if (generateAtUseSite.contains(current)) {
          // Short-circuit logical operator trickery.
          // Check the second half, which will continue into the join.
          // (The first half is [inputs[0]], the second half is [successors[0]],
          // and [successors[1]] is the join-block).
          basicBlock = basicBlock.successors[0];
        } else {
          // We allow an expression to end on an HIf (a condition expression).
          return basicBlock === limits.end ? result : TYPE_STATEMENT;
        }
      } else {
        // Expression-incompatible control flow.
        return TYPE_STATEMENT;
      }
    } while (limits.contains(basicBlock));
    return result;
  }

  bool isJSExpression(HExpressionInformation info) {
    return expressionType(info) !== TYPE_STATEMENT;
  }

  bool isJSDeclaration(HExpressionInformation info) {
    return expressionType(info) === TYPE_DECLARATION;
  }

  bool isJSCondition(HExpressionInformation info) {
    HSubExpressionBlockInformation graph = info;
    SubExpression limits = graph.subExpression;
    return expressionType(info) !== TYPE_STATEMENT &&
       (limits.end.last is HConditionalBranch);
  }

  /**
   * Generate statements from block information.
   * If the block information contains expressions, generate only
   * assignments, and if it ends in a conditional branch, don't generate
   * the condition.
   */
  void generateStatements(HBlockInformation block) {
    int oldState = generationState;
    generationState = STATE_STATEMENT;
    if (block is HStatementInformation) {
      block.accept(this);
    } else {
      HSubExpressionBlockInformation expression = block;
      visitSubGraph(expression.subExpression);
    }
    generationState = oldState;
  }

  /**
   * Generate expressions from block information.
   */
  void generateExpression(HExpressionInformation expression) {
    // Currently we only handle sub-expression graphs.
    assert(expression is HSubExpressionBlockInformation);
    HSubExpressionBlockInformation expressionSubGraph = expression;

    int oldState = generationState;
    generationState = STATE_FIRST_EXPRESSION;
    visitSubGraph(expressionSubGraph.subExpression);
    generationState = oldState;
  }

  void generateDeclaration(HExpressionInformation expression) {
    // Currently we only handle sub-expression graphs.
    assert(expression is HSubExpressionBlockInformation);
    HSubExpressionBlockInformation expressionSubGraph = expression;

    int oldState = generationState;
    generationState = STATE_FIRST_DECLARATION;
    visitSubGraph(expressionSubGraph.subExpression);
    generationState = oldState;
  }

  void generateCondition(HBlockInformation condition) {
    generateExpression(condition);
  }

  String temporary(HInstruction instruction) {
    int id = instruction.id;
    String name = names[id];
    if (name !== null) return name;

    String prefix = TEMPORARY_PREFIX;
    if (instruction.sourceElement !== null) {
      Element element = instruction.sourceElement;
      if (element !== null && !element.name.isEmpty()) {
        prefix = element.name.slowToString();
        // Special case the variable named [TEMPORARY_PREFIX] to allow
        // keeping its name.
        if (prefix == TEMPORARY_PREFIX && !usedNames.contains(prefix)) {
          return newName(id, prefix);
        }
        // If we've never seen that prefix before, try to use it
        // directly.
        if (!prefixes.containsKey(prefix)) {
          // Make sure the variable name does not conflict with our mangling.
          while (usedNames.contains(prefix)) {
            prefix = '${prefix}_';
          }
          prefixes[prefix] = 0;
          return newName(id, prefix);
        }
      }
    }

    name = '${prefix}${prefixes[prefix]++}';
    while (usedNames.contains(name)) {
      name = '${prefix}${prefixes[prefix]++}';
    }
    return newName(id, name);
  }

  // TODO(floitsch): share more code with [temporary].
  String freshTemporary() {
    String prefix = TEMPORARY_PREFIX;
    String name = '${prefix}${prefixes[prefix]++}';
    while (usedNames.contains(name)) {
      name = '${prefix}${prefixes[prefix]++}';
    }
    String result = JsNames.getValid(name);
    usedNames.add(result);
    return result;
  }

  String newName(int id, String name) {
    String result = JsNames.getValid(name);
    names[id] = result;
    usedNames.add(result);
    return result;
  }

  /**
    * Only visits the arguments starting at inputs[HInvoke.ARGUMENTS_OFFSET].
    */
  void visitArguments(List<HInstruction> inputs) {
    assert(inputs.length >= HInvoke.ARGUMENTS_OFFSET);
    buffer.add('(');
    for (int i = HInvoke.ARGUMENTS_OFFSET; i < inputs.length; i++) {
      if (i != HInvoke.ARGUMENTS_OFFSET) buffer.add(', ');
      use(inputs[i], JSPrecedence.ASSIGNMENT_PRECEDENCE);
    }
    buffer.add(')');
  }

  /**
   * Whether we are currently generating expressions instead of statements.
   * This includes declarations, which are generated as expressions.
   */
  bool isGeneratingExpression() {
    return generationState != STATE_STATEMENT;
  }

  /**
   * Whether we are generating a declaration.
   */
  bool isGeneratingDeclaration() {
    return (generationState == STATE_DECLARATION ||
            generationState == STATE_FIRST_DECLARATION);
  }

  /**
   * Called before writing an expression.
   * Ensures that expressions are comma spearated.
   */
  void addExpressionSeparator() {
    if (generationState == STATE_FIRST_DECLARATION) {
      buffer.add("var ");
      generationState = STATE_DECLARATION;
    } else if (generationState == STATE_FIRST_EXPRESSION) {
      generationState = STATE_EXPRESSION;
    } else {
      buffer.add(", ");
    }
  }

  void declareVariable(String variableName) {
    if (isGeneratingExpression()) {
      buffer.add(variableName);
      if (!isGeneratingDeclaration()) {
        delayedVarDecl = delayedVarDecl.prepend(variableName);
      }
    } else {
      buffer.add("var ");
      buffer.add(variableName);
    }
  }

  void declareInstruction(HInstruction instruction) {
    declaredInstructions.add(instruction);
    String name = temporary(instruction);
    declareVariable(name);
  }

  bool needsNewVariable(HInstruction instruction) {
    bool needsVar = !instruction.usedBy.isEmpty();
    if (needsVar && instruction is HCheck) {
      HCheck check = instruction;
      HInstruction input = check.checkedInput;
      // We only need a new var if [input] is generated at use site
      // but is not a trivial code motion invariant instruction like
      // for parameters or this.
      //
      // For example:
      // Foo a = this;
      // print(a);
      // print(a);
      //
      // In checked mode no new variable is needed.
      // FooTypeCheck(this);
      // print(this);
      // print(this);
      //
      // But for this example:
      // Foo a = foo();
      // print(a);
      // print(a);
      //
      // We need a new variable:
      // var a = FooTypeCheck(foo());
      // print(a);
      // print(a);
      needsVar = isGenerateAtUseSite(input) && !input.isCodeMotionInvariant();
    }
    return needsVar;
  }

  void define(HInstruction instruction) {
    if (needsNewVariable(instruction)) {
      declareInstruction(instruction);
      buffer.add(" = ");
      visit(instruction, JSPrecedence.ASSIGNMENT_PRECEDENCE);
    } else {
      visit(instruction, JSPrecedence.STATEMENT_PRECEDENCE);
    }
  }

  void use(HInstruction argument, int expectedPrecedenceForArgument) {
    if (argument is HCheck) {
      HCheck instruction  = argument;
      HInstruction input = instruction.checkedInput;
      if (isGenerateAtUseSite(argument) && isGenerateAtUseSite(input)) {
        // If both instructions can be generated at use site, we can
        // just visit [argument].
        //
        // For example:
        // Foo a = foo();
        // print(a);
        //
        // In checked mode will turn into:
        // print(FooTypeCheck(foo()));
        visit(argument, expectedPrecedenceForArgument);
      } else if (isGenerateAtUseSite(input)) {
        // Get the real input of that checked instruction.
        while (input is HCheck) {
          HCheck check = input;
          input = check.checkedInput;
        }

        // If [argument] cannot be generated at use site, but [input]
        // can, use the temporary of [argument]. A code motion
        // invariant instruction does not have a temporary, so we just
        //
        // For example:
        // Foo a = foo();
        // print(a);
        // print(a);
        //
        // In checked mode will turn into:
        // var a = FooTypeCheck(foo());
        // print(a);
        // print(a);
        //
        // Note that in case the input is code motion invariant, like
        // for parameters or this, we just need to visit it, since
        // there is no temporary for such instruction.
        if (input.isCodeMotionInvariant()) {
          visit(input, expectedPrecedenceForArgument);
        } else {
          buffer.add(temporary(argument));
        }
      } else {
        // Otherwise we just use [input]. [argument] has already been
        // emitted, and we just need the temporary of [input].
        //
        // For example:
        // var a = foo();
        // print(a);
        // Foo b = a;
        // print(b);
        //
        // In checked mode will turn into:
        // var a = foo();
        // print(a);
        // FooTypeCheck(a);
        // print(a);
        use(input, expectedPrecedenceForArgument);
      }
    } else if (isGenerateAtUseSite(argument)) {
      visit(argument, expectedPrecedenceForArgument);
    } else {
      buffer.add(temporary(argument));
    }
  }

  visit(HInstruction node, int expectedPrecedenceForNode) {
    int oldPrecedence = this.expectedPrecedence;
    this.expectedPrecedence = expectedPrecedenceForNode;
    node.accept(this);
    this.expectedPrecedence = oldPrecedence;
  }

  void continueAsBreak(LabelElement target) {
    addIndented("break ");
    writeContinueLabel(target);
    buffer.add(";\n");
  }

  void implicitContinueAsBreak(TargetElement target) {
    addIndented("break ");
    writeImplicitContinueLabel(target);
    buffer.add(";\n");
  }

  void implicitBreakWithLabel(TargetElement target) {
    addIndented("break ");
    writeImplicitLabel(target);
    buffer.add(";\n");
  }

  bool visitIfInfo(HIfBlockInformation info) {
    HInstruction condition = info.condition.conditionExpression;
    if (condition.isConstant()) {
      // If the condition is constant, only generate one branch (if any).
      HConstant constantCondition = condition;
      Constant constant = constantCondition.constant;
      generateStatements(info.condition);
      if (constant.isTrue()) {
        generateStatements(info.thenGraph);
      } else if (info.elseGraph !== null) {
        generateStatements(info.elseGraph);
      }
    } else {
      generateStatements(info.condition);
      addIndented("if (");
      use(condition, JSPrecedence.EXPRESSION_PRECEDENCE);
      buffer.add(") {\n");
      indent++;
      generateStatements(info.thenGraph);
      indent--;
      addIndented("}");
      if (info.elseGraph !== null) {
        buffer.add(" else {\n");
        indent++;
        generateStatements(info.elseGraph);
        indent--;
        addIndented("}");
      }
      buffer.add("\n");
    }
    return true;
  }

  bool visitSequenceInfo(HStatementSequenceInformation info) {
    return false;
  }

  bool visitSubGraphInfo(HSubGraphBlockInformation info) {
    visitSubGraph(info.subGraph);
    return true;
  }

  bool visitSubExpressionInfo(HSubExpressionBlockInformation info) {
    return false;
  }

  bool visitAndOrInfo(HAndOrBlockInformation info) {
    return false;
  }

  bool visitTryInfo(HTryBlockInformation info) {
    addIndented("try {\n");
    indent++;
    generateStatements(info.body);
    indent--;
    addIndented("}");
    if (info.catchBlock !== null) {
      // Printing the catch part.
      HParameterValue exception = info.catchVariable;
      String name = temporary(exception);
      parameterNames[exception.element] = name;
      buffer.add('catch ($name) {\n');
      indent++;
      generateStatements(info.catchBlock);
      parameterNames.remove(exception.element);
      indent--;
      addIndented('}');
    }
    if (info.finallyBlock != null) {
      buffer.add(" finally {\n");
      indent++;
      generateStatements(info.finallyBlock);
      indent--;
      addIndented("}");
    }
    buffer.add("\n");
    return true;
  }

  bool visitLoopInfo(HLoopBlockInformation info) {
    HExpressionInformation condition = info.condition;
    bool isConditionExpression = isJSCondition(condition);

    void visitBodyIgnoreLabels() {
      if (info.body.start.isLabeledBlock()) {
        HBlockInformation oldInfo = currentBlockInformation;
        currentBlockInformation = info.body.start.blockFlow.body;
        generateStatements(info.body);
        currentBlockInformation = oldInfo;
      } else {
        generateStatements(info.body);
      }
    }

    switch (info.kind) {
      // Treate all three "test-first" loops the same way.
      case HLoopBlockInformation.FOR_LOOP:
      case HLoopBlockInformation.WHILE_LOOP:
      case HLoopBlockInformation.FOR_IN_LOOP: {
        HBlockInformation initialization = info.initializer;
        int initializationType = TYPE_STATEMENT;
        if (initialization !== null) {
          initializationType = expressionType(initialization);
          if (initializationType == TYPE_STATEMENT) {
            generateStatements(initialization);
            initialization = null;
          }
        }
        for (LabelElement label in info.labels) {
          if (label.isTarget) {
            writeLabel(label);
            buffer.add(":");
          }
        }
        if (isConditionExpression &&
            info.updates !== null && isJSExpression(info.updates)) {
          // If we have an updates graph, and it's expressible as an
          // expression, generate a for-loop.
          addIndented("for (");
          if (initialization !== null) {
            if (initializationType != TYPE_DECLARATION) {
              generateExpression(initialization);
            } else {
              generateDeclaration(initialization);
            }
          }
          buffer.add("; ");
          generateCondition(condition);
          buffer.add("; ");
          generateExpression(info.updates);
          buffer.add(") {\n");
          indent++;
          // The body might be labeled. Ignore this when recursing on the
          // subgraph.
          // TODO(lrn): Remove this extra labeling when handling all loops
          // using subgraphs.
          visitBodyIgnoreLabels();

          indent--;
        } else {
          // We have either no update graph, or it's too complex to
          // put in an expression.
          if (initialization !== null) {
            generateStatements(initialization);
          }
          addIndented("while (");
          if (isConditionExpression) {
            generateCondition(condition);
            buffer.add(") {\n");
            indent++;
          } else {
            buffer.add("true) {\n");
            indent++;
            generateStatements(condition);
            addIndented("if (!");
            use(condition.conditionExpression, JSPrecedence.PREFIX_PRECEDENCE);
            buffer.add(") break;\n");
          }
          if (info.updates !== null) {
            wrapLoopBodyForContinue(info);
            generateStatements(info.updates);
          } else {
            visitBodyIgnoreLabels();
          }
          indent--;
        }
        addIndented("}\n");
        break;
      }
      case HLoopBlockInformation.DO_WHILE_LOOP: {
        // Generate do-while loop in all cases.
        if (info.initializer !== null) {
          generateStatements(info.initializer);
        }
        addIndentation();
        for (LabelElement label in info.labels) {
          if (label.isTarget) {
            writeLabel(label);
            buffer.add(":");
          }
        }
        buffer.add("do {\n");
        indent++;
        if (!isConditionExpression || info.updates !== null) {
          wrapLoopBodyForContinue(info);
        } else {
          visitBodyIgnoreLabels();
        }
        if (info.updates !== null) {
          generateStatements(info.updates);
        }
        if (isConditionExpression) {
          indent--;
          addIndented("} while (");
          generateExpression(condition);
          buffer.add(");\n");
        } else {
          generateStatements(condition);
          indent--;
          addIndented("} while (");
          use(condition.conditionExpression, JSPrecedence.PREFIX_PRECEDENCE);
          buffer.add(");\n");
        }
        break;
      }
      default:
        compiler.internalError(
          'Unexpected loop kind: ${info.kind}',
          instruction: condition.conditionExpression);
    }
    return true;
  }

  bool visitLabeledBlockInfo(HLabeledBlockInformation labeledBlockInfo) {
    preLabeledBlock(labeledBlockInfo);
    addIndentation();
    Link<Element> continueOverrides = const EmptyLink<Element>();
    // If [labeledBlockInfo.isContinue], the block is an artificial
    // block around the body of a loop with an update block, so that
    // continues of the loop can be written as breaks of the body
    // block.
    if (labeledBlockInfo.isContinue) {
      for (LabelElement label in labeledBlockInfo.labels) {
        if (label.isContinueTarget) {
          writeContinueLabel(label);
          buffer.add(':');
          continueAction[label] = continueAsBreak;
          continueOverrides = continueOverrides.prepend(label);
        }
      }
      // For handling unlabeled continues from the body of a loop.
      // TODO(lrn): Consider recording whether the target is in fact
      // a target of an unlabeled continue, and not generate this if it isn't.
      TargetElement target = labeledBlockInfo.target;
      writeImplicitContinueLabel(target);
      buffer.add(':');
      continueAction[target] = implicitContinueAsBreak;
      continueOverrides = continueOverrides.prepend(target);
    } else {
      for (LabelElement label in labeledBlockInfo.labels) {
        if (label.isBreakTarget) {
          writeLabel(label);
          buffer.add(':');
        }
      }
      TargetElement target = labeledBlockInfo.target;
      if (target.isSwitch) {
        // This is an extra block around a switch that is generated
        // as a nested if/else chain. We add an extra break target
        // so that case code can break.
        writeImplicitLabel(target);
        buffer.add(':');
        breakAction[target] = implicitBreakWithLabel;
      }
    }
    buffer.add('{\n');
    indent++;

    startLabeledBlock(labeledBlockInfo);
    generateStatements(labeledBlockInfo.body);
    endLabeledBlock(labeledBlockInfo);

    indent--;
    addIndented('}\n');

    if (labeledBlockInfo.isContinue) {
      while (!continueOverrides.isEmpty()) {
        continueAction.remove(continueOverrides.head);
        continueOverrides = continueOverrides.tail;
      }
    } else {
      breakAction.remove(labeledBlockInfo.target);
    }
    return true;
  }

  void emitLogicalOperation(HPhi node, String operation) {
    JSBinaryOperatorPrecedence operatorPrecedence =
        JSPrecedence.binary[operation];
    beginExpression(operatorPrecedence.precedence);
    use(node.inputs[0], operatorPrecedence.left);
    buffer.add(" $operation ");
    use(node.inputs[1], operatorPrecedence.right);
    endExpression(operatorPrecedence.precedence);
  }

  // Wraps a loop body in a block to make continues have a target to break
  // to (if necessary).
  void wrapLoopBodyForContinue(HLoopBlockInformation info) {
    TargetElement target = info.target;
    if (target !== null && target.isContinueTarget) {
      addIndentation();
      for (LabelElement label in info.labels) {
        if (label.isContinueTarget) {
          writeContinueLabel(label);
          buffer.add(":");
          continueAction[label] = continueAsBreak;
        }
      }
      writeImplicitContinueLabel(target);
      buffer.add(":{\n");
      continueAction[info.target] = implicitContinueAsBreak;
      indent++;
      generateStatements(info.body);
      indent--;
      addIndented("}\n");
      continueAction.remove(info.target);
      for (LabelElement label in info.labels) {
        if (label.isContinueTarget) {
          continueAction.remove(label);
        }
      }
    } else {
      // Loop body contains no continues, so we don't need a break target.
      generateStatements(info.body);
    }
  }

  bool handleBlockFlow(HBlockFlow block) {
    HBlockInformation info = block.body;
    // If we reach here again while handling the attached information,
    // e.g., because we call visitSubGraph on a subgraph starting on
    // the same block, don't handle it again.
    // When the structure graph is complete, we will be able to have
    // different structures starting on the same basic block (e.g., an
    // "if" and its condition).
    if (info === currentBlockInformation) return false;

    HBlockInformation oldBlockInformation = currentBlockInformation;
    currentBlockInformation = info;
    bool success = info.accept(this);
    currentBlockInformation = oldBlockInformation;
    if (success) {
      HBasicBlock continuation = block.continuation;
      if (continuation !== null) {
        visitBasicBlock(continuation);
      }
    }
    return success;
  }

  void visitBasicBlock(HBasicBlock node) {
    // Abort traversal if we are leaving the currently active sub-graph.
    if (!subGraph.contains(node)) return;

    currentBlock = node;
    // If this node has block-structure based information attached,
    // try using that to traverse from here.
    if (node.blockFlow !== null &&
        handleBlockFlow(node.blockFlow)) {
      return;
    }
    // Flow based traversal.
    if (node.isLoopHeader() &&
        node.loopInformation.loopBlockInformation !== currentBlockInformation) {
      beginLoop(node);
    }
    iterateBasicBlock(node);
  }

  /** Generates the assignments for all phis of successors blocks. */
  void assignPhisOfAllSuccessors(HBasicBlock node) {
    Map<HInstruction, String> temporaryNamesOfPhis = null;

    /**
     * Generates the assignment [canonicalPhi] = [value].
     *
     * If the [canonicalPhi] has a temporary name (in [temporaryNamesOfPhis])
     * then the temporary is assigned instead of the [canonicalPhi]. However,
     * when the left and right-hand side are equal ([:canonicalPhi === value:])
     * then the [canonicalPhi] is assigned with the temporary.
     */
    void generateAssignment(HPhi canonicalPhi, HInstruction value) {
      if (isGeneratingExpression()) {
        addExpressionSeparator();
      } else {
        addIndentation();
      }
      if (temporaryNamesOfPhis !== null &&
          canonicalPhi !== value &&
          temporaryNamesOfPhis.containsKey(canonicalPhi)) {
        // This is the assignment to the temporary.
        declareVariable(temporaryNamesOfPhis[canonicalPhi]);
      } else if (!declaredInstructions.contains(canonicalPhi)) {
        declareInstruction(canonicalPhi);
      } else {
        buffer.add(temporary(canonicalPhi));
      }
      buffer.add(" = ");
      bool isLogicalOperation = logicalOperations.containsKey(canonicalPhi);
      if (isLogicalOperation) {
        emitLogicalOperation(canonicalPhi, logicalOperations[canonicalPhi]);
      } else if (canonicalPhi === value) {
        buffer.add(temporaryNamesOfPhis[value]);
      } else {
        use(value, JSPrecedence.ASSIGNMENT_PRECEDENCE);
      }
      if (!isGeneratingExpression()) {
        buffer.add(';\n');
      }
    }

    // Assignments are delayed so that we don't overwrite phis that might
    // be used as inputs.
    // TODO(floitsch): improve phi assignments. Currently we introduce
    // way too many temporary variables.
    Map<HPhi, HInstruction> phiAssignments = new Map<HPhi, HInstruction>();

    for (HBasicBlock successor in node.successors) {
      int index = successor.predecessors.indexOf(node);
      successor.forEachPhi((HPhi phi) {
        bool isLogicalOperation = logicalOperations.containsKey(phi);
        // In case the phi is being generated by another
        // instruction.
        if (isLogicalOperation && isGenerateAtUseSite(phi)) return;
        HPhi canonicalPhi = phiEquivalence.getRepresentative(phi);
        assert(!isLogicalOperation || canonicalPhi === phi);
        HInstruction input = phi.inputs[index];
        if (input is HPhi) {
          input = phiEquivalence.getRepresentative(input);
          // If we use the same variable, we don't need to create an
          // assignment.
          if (input === canonicalPhi) {
            assert(!isLogicalOperation);
            return;
          }
        }
        phiAssignments[canonicalPhi] = input;
      });
    }

    Set<HPhi> inputPhis = new Set<HPhi>();
    List<HPhi> phis = <HPhi>[];

    /**
     * Transitively collects the phis that are used when emitting the [input]
     * and adds them to [inputPhis]. Does not add phis that are equal to the
     * [targetPhi] or are not in the [phiAssignments] map.
     */
    void collectInputPhis(HInstruction input, HPhi targetPhi) {
      if (input is HPhi) {
        HPhi canonicalPhi = phiEquivalence.getRepresentative(input);
        // Self-updates are ok.
        if (canonicalPhi !== targetPhi &&
            phiAssignments.containsKey(canonicalPhi)) {
          inputPhis.add(canonicalPhi);
        }
      } else if (isGenerateAtUseSite(input)) {
        for (HInstruction inputOfInput in input.inputs) {
          collectInputPhis(inputOfInput, targetPhi);
        }
      }
    }

    phiAssignments.forEach((HPhi targetPhi, HInstruction input) {
      phis.add(targetPhi);
      collectInputPhis(input, targetPhi);
    });

    if (inputPhis.isEmpty()) {
      phiAssignments.forEach(generateAssignment);
    } else {
      // Emit the phiX=phiY assignments, taking care to break cycles.
      // Example program:
      //   var x = 499;
      //   var y = 99;
      //   while (x != 99) {
      //     var tmp = x;  // <=== The tmp variable is removed in Ssa-form.
      //     x = y;
      //     y = tmp;
      //   }
      //
      temporaryNamesOfPhis = new HashMap<HInstruction, String>();
      // For phis that are used as inputs simply *always* allocate a
      // temporary.
      for (HPhi phi in phis) {
        if (inputPhis.contains(phi)) {
          String temporaryPhi = freshTemporary();
          temporaryNamesOfPhis[phi] = temporaryPhi;
        }
        // The assignPhi uses temporary variables for the left-hand side if
        // they exist.
        generateAssignment(phi, phiAssignments[phi]);
      }
      // Finally assign the input phis.
      for (HPhi phi in inputPhis) {
        // The assignPhi special-cases phi assignments to itself and recognizes
        // it as assignment from the temporary variable to the actual phi.
        generateAssignment(phi, phi);
      }
    }
  }

  void iterateBasicBlock(HBasicBlock node) {
    HInstruction instruction = node.first;
    while (instruction != null) {
      if (instruction === node.last) {
        assignPhisOfAllSuccessors(node);
      }

      if (isGenerateAtUseSite(instruction)) {
        if (instruction is HIf) {
          HIf hif = instruction;
          // The "if" is implementing part of a logical expression.
          // Skip directly forward to to its latest successor, since everything
          // in-between must also be generateAtUseSite.
          assert(hif.trueBranch.id < hif.falseBranch.id);
          visitBasicBlock(hif.falseBranch);
        }
      } else if (instruction is HControlFlow) {
        if (instruction is HLoopBranch && isGeneratingExpression()) {
          addExpressionSeparator();
        }
        visit(instruction, JSPrecedence.STATEMENT_PRECEDENCE);
      } else if (instruction is HTypeGuard) {
        visit(instruction, JSPrecedence.STATEMENT_PRECEDENCE);
      } else {
        if (isGeneratingExpression()) {
          addExpressionSeparator();
        } else {
          addIndentation();
        }
        define(instruction);
        if (!isGeneratingExpression()) buffer.add(';\n');
      }
      instruction = instruction.next;
    }
  }

  visitInvokeBinary(HInvokeBinary node, String op) {
    if (node.builtin) {
      JSBinaryOperatorPrecedence operatorPrecedences = JSPrecedence.binary[op];
      beginExpression(operatorPrecedences.precedence);
      use(node.left, operatorPrecedences.left);
      buffer.add(' $op ');
      use(node.right, operatorPrecedences.right);
      endExpression(operatorPrecedences.precedence);
    } else {
      visitInvokeStatic(node);
    }
  }

  // We want the outcome of bit-operations to be positive. We use the unsigned
  // shift operator to achieve this.
  visitBitInvokeBinary(HBinaryBitOp node, String op) {
    if (node.builtin){
      beginExpression(unsignedShiftPrecedences.precedence);
      int oldPrecedence = this.expectedPrecedence;
      this.expectedPrecedence = JSPrecedence.SHIFT_PRECEDENCE;
      visitInvokeBinary(node, op);
      buffer.add(' >>> 0');
      this.expectedPrecedence = oldPrecedence;
      endExpression(unsignedShiftPrecedences.precedence);
    } else {
      visitInvokeBinary(node, op);
    }
  }

  visitInvokeUnary(HInvokeUnary node, String op) {
    if (node.builtin) {
      beginExpression(JSPrecedence.PREFIX_PRECEDENCE);
      buffer.add('$op');
      use(node.operand, JSPrecedence.PREFIX_PRECEDENCE);
      endExpression(JSPrecedence.PREFIX_PRECEDENCE);
    } else {
      visitInvokeStatic(node);
    }
  }

  // We want the outcome of bit-operations to be positive. We use the unsigned
  // shift operator to achieve this.
  visitBitInvokeUnary(HInvokeUnary node, String op) {
    if (node.builtin){
      beginExpression(unsignedShiftPrecedences.precedence);
      int oldPrecedence = this.expectedPrecedence;
      this.expectedPrecedence = JSPrecedence.SHIFT_PRECEDENCE;
      visitInvokeUnary(node, op);
      buffer.add(' >>> 0');
      this.expectedPrecedence = oldPrecedence;
      endExpression(unsignedShiftPrecedences.precedence);
    } else {
      visitInvokeUnary(node, op);
    }
  }

  visitEquals(HEquals node) {
    if (node.builtin) {
      beginExpression(JSPrecedence.EQUALITY_PRECEDENCE);
      use(node.left, JSPrecedence.EQUALITY_PRECEDENCE);
      buffer.add(' === ');
      use(node.right, JSPrecedence.RELATIONAL_PRECEDENCE);
      endExpression(JSPrecedence.EQUALITY_PRECEDENCE);
    } else if (node.element === equalsNullElement ||
               node.element === boolifiedEqualsNullElement) {
      beginExpression(JSPrecedence.CALL_PRECEDENCE);
      use(node.target, JSPrecedence.CALL_PRECEDENCE);
      buffer.add('(');
      use(node.left, JSPrecedence.ASSIGNMENT_PRECEDENCE);
      buffer.add(')');
      endExpression(JSPrecedence.CALL_PRECEDENCE);
    } else {
      visitInvokeStatic(node);
    }
  }

  visitAdd(HAdd node)               => visitInvokeBinary(node, '+');
  visitDivide(HDivide node)         => visitInvokeBinary(node, '/');
  visitMultiply(HMultiply node)     => visitInvokeBinary(node, '*');
  visitSubtract(HSubtract node)     => visitInvokeBinary(node, '-');
  // Truncating divide does not have a JS equivalent.
  visitTruncatingDivide(HTruncatingDivide node) => visitInvokeStatic(node);
  // Modulo cannot be mapped to the native operator (different semantics).
  visitModulo(HModulo node)                     => visitInvokeStatic(node);

  visitBitAnd(HBitAnd node)         => visitBitInvokeBinary(node, '&');
  visitBitNot(HBitNot node)         => visitBitInvokeUnary(node, '~');
  visitBitOr(HBitOr node)           => visitBitInvokeBinary(node, '|');
  visitBitXor(HBitXor node)         => visitBitInvokeBinary(node, '^');

  // We need to check if the left operand is negative in order to use
  // the native operator.
  visitShiftRight(HShiftRight node) => visitInvokeStatic(node);

  // Shift left cannot be mapped to the native operator (different semantics).
  visitShiftLeft(HShiftLeft node)   => visitInvokeStatic(node);

  visitNegate(HNegate node)         => visitInvokeUnary(node, '-');

  visitIdentity(HIdentity node)         => visitInvokeBinary(node, '===');
  visitLess(HLess node)                 => visitInvokeBinary(node, '<');
  visitLessEqual(HLessEqual node)       => visitInvokeBinary(node, '<=');
  visitGreater(HGreater node)           => visitInvokeBinary(node, '>');
  visitGreaterEqual(HGreaterEqual node) => visitInvokeBinary(node, '>=');

  visitBoolify(HBoolify node) {
    beginExpression(JSPrecedence.EQUALITY_PRECEDENCE);
    assert(node.inputs.length == 1);
    use(node.inputs[0], JSPrecedence.EQUALITY_PRECEDENCE);
    buffer.add(' === true');
    endExpression(JSPrecedence.EQUALITY_PRECEDENCE);
  }

  visitExit(HExit node) {
    // Don't do anything.
  }

  visitGoto(HGoto node) {
    assert(currentBlock.successors.length == 1);
    List<HBasicBlock> dominated = currentBlock.dominatedBlocks;
    // With the exception of the entry-node which dominates its successor
    // and the exit node, no block finishing with a 'goto' can have more than
    // one dominated block (since it has only one successor).
    // If the successor is dominated by another block, then the other block
    // is responsible for visiting the successor.
    if (dominated.isEmpty()) return;
    if (dominated.length > 2) {
      compiler.internalError('dominated.length = ${dominated.length}',
                             instruction: node);
    }
    if (dominated.length == 2 && currentBlock !== currentGraph.entry) {
      compiler.internalError('currentBlock !== currentGraph.entry',
                             instruction: node);
    }
    assert(dominated[0] == currentBlock.successors[0]);
    visitBasicBlock(dominated[0]);
  }

  // Used to write the name of labels.
  void writeLabel(LabelElement label) {
    buffer.add('\$${label.labelName}\$${label.target.nestingLevel}');
  }

  void writeImplicitLabel(TargetElement target) {
    buffer.add('\$${target.nestingLevel}');
  }

  // We sometimes handle continue targets differently from break targets,
  // so we have special continue-only labels.
  void writeContinueLabel(LabelElement label) {
    buffer.add('c\$${label.labelName}\$${label.target.nestingLevel}');
  }

  void writeImplicitContinueLabel(TargetElement target) {
    buffer.add('c\$${target.nestingLevel}');
  }

  /**
   * Checks if [map] contains an [ElementAction] for [element], and
   * if so calls that action and returns true.
   * Otherwise returns false.
   */
  bool tryCallAction(Map<Element, ElementAction> map, Element element) {
    ElementAction action = map[element];
    if (action === null) return false;
    action(element);
    return true;
  }

  visitBreak(HBreak node) {
    assert(currentBlock.successors.length == 1);
    if (node.label !== null) {
      LabelElement label = node.label;
      if (!tryCallAction(breakAction, label)) {
        addIndented("break ");
        writeLabel(label);
        buffer.add(";\n");
      }
    } else {
      TargetElement target = node.target;
      if (!tryCallAction(breakAction, target)) {
        addIndented("break;\n");
      }
    }
  }

  visitContinue(HContinue node) {
    assert(currentBlock.successors.length == 1);
    if (node.label !== null) {
      LabelElement label = node.label;
      if (!tryCallAction(continueAction, label)) {
        addIndented("continue ");
        writeLabel(label);
        buffer.add(";\n");
      }
    } else {
      TargetElement target = node.target;
      if (!tryCallAction(continueAction, target)) {
        addIndented("continue;\n");
      }
    }
  }

  visitTry(HTry node) {
    // We should never get here. Try/catch/finally is always handled using block
    // information in [visitTryInfo], or not at all, in the case of the bailout
    // generator.
    compiler.internalError('visitTry should not be called', instruction: node);
  }

  visitIf(HIf node) {
    if (subGraph !== null && node.block === subGraph.end) {
      if (isGeneratingExpression()) {
        use(node.inputs[0], JSPrecedence.EXPRESSION_PRECEDENCE);
      }
      return;
    }
    HInstruction condition = node.inputs[0];
    int preVisitedBlocks = 0;
    List<HBasicBlock> dominated = node.block.dominatedBlocks;
    HIfBlockInformation info = node.blockInformation.body;
    if (condition.isConstant()) {
      HConstant constant = condition;
      if (constant.constant.isTrue()) {
        generateStatements(info.thenGraph);
      } else if (node.hasElse) {
        generateStatements(info.elseGraph);
      }
      // We ignore the other branch, even if it isn't visited.
      preVisitedBlocks = node.hasElse ? 2 : 1;
    } else {
      startIf(node);
      assert(!isGenerateAtUseSite(node));
      startThen(node);
      assert(node.thenBlock === dominated[0]);
      generateStatements(info.thenGraph);
      preVisitedBlocks++;
      endThen(node);
      if (node.hasElse) {
        startElse(node);
        assert(node.elseBlock === dominated[1]);
        generateStatements(info.elseGraph);
        preVisitedBlocks++;
        endElse(node);
      }
      endIf(node);
    }
    HBasicBlock joinBlock = node.joinBlock;
    if (joinBlock !== null && joinBlock.dominator !== node.block) {
      // The join block is dominated by a block in one of the branches.
      // The subgraph traversal never reached it, so we visit it here
      // instead.
      visitBasicBlock(joinBlock);
    }

    // Visit all the dominated blocks that are not part of the then or else
    // branches, and is not the join block.
    // Depending on how the then/else branches terminate
    // (e.g., return/throw/break) there can be any number of these.
    int dominatedCount = dominated.length;
    for (int i = preVisitedBlocks; i < dominatedCount; i++) {
      HBasicBlock dominatedBlock = dominated[i];
      assert(dominatedBlock.dominator === node.block);
      visitBasicBlock(dominatedBlock);
    }
  }

  visitInvokeDynamicMethod(HInvokeDynamicMethod node) {
    beginExpression(JSPrecedence.CALL_PRECEDENCE);
    use(node.receiver, JSPrecedence.MEMBER_PRECEDENCE);
    buffer.add('.');
    // Avoid adding the generative constructor name to the list of
    // seen selectors.
    if (node.inputs[0] is HForeignNew) {
      HForeignNew foreignNew = node.inputs[0];
      // Remove 'this' from the number of arguments.
      int argumentCount = node.inputs.length - 1;

      // TODO(ahe): The constructor name was statically resolved in
      // SsaBuilder.buildFactory. Is there a cleaner way to do this?
      node.name.printOn(buffer);
      visitArguments(node.inputs);
    } else {
      buffer.add(compiler.namer.instanceMethodInvocationName(
          currentLibrary, node.name, node.selector));
      visitArguments(node.inputs);
      if (node.element !== null) {
        // If we know we're calling a specific method, register that
        // method only.
        world.registerDynamicInvocationOf(node.element);
      } else {
        world.registerDynamicInvocation(
            node.name, getOptimizedSelectorFor(node, node.selector));
      }
    }
    endExpression(JSPrecedence.CALL_PRECEDENCE);
  }

  Selector getOptimizedSelectorFor(HInvoke node, Selector defaultSelector) {
    Type receiverType = node.inputs[0].propagatedType.computeType(compiler);
    if (receiverType !== null) {
      return new TypedSelector(receiverType, defaultSelector);
    } else {
      return defaultSelector;
    }
  }

  visitInvokeDynamicSetter(HInvokeDynamicSetter node) {
    beginExpression(JSPrecedence.CALL_PRECEDENCE);
    use(node.receiver, JSPrecedence.MEMBER_PRECEDENCE);
    buffer.add('.');
    buffer.add(compiler.namer.setterName(currentLibrary, node.name));
    visitArguments(node.inputs);
    world.registerDynamicSetter(
        node.name, getOptimizedSelectorFor(node, Selector.SETTER));
    endExpression(JSPrecedence.CALL_PRECEDENCE);
  }

  visitInvokeDynamicGetter(HInvokeDynamicGetter node) {
    beginExpression(JSPrecedence.CALL_PRECEDENCE);
    use(node.receiver, JSPrecedence.MEMBER_PRECEDENCE);
    buffer.add('.');
    buffer.add(compiler.namer.getterName(currentLibrary, node.name));
    visitArguments(node.inputs);
    world.registerDynamicGetter(
        node.name, getOptimizedSelectorFor(node, Selector.GETTER));
    endExpression(JSPrecedence.CALL_PRECEDENCE);
  }

  visitInvokeClosure(HInvokeClosure node) {
    beginExpression(JSPrecedence.CALL_PRECEDENCE);
    use(node.receiver, JSPrecedence.MEMBER_PRECEDENCE);
    buffer.add('.');
    buffer.add(compiler.namer.closureInvocationName(node.selector));
    visitArguments(node.inputs);
    // TODO(floitsch): we should have a separate list for closure invocations.
    world.registerDynamicInvocation(Namer.CLOSURE_INVOCATION_NAME,
                                    node.selector);
    endExpression(JSPrecedence.CALL_PRECEDENCE);
  }

  visitInvokeStatic(HInvokeStatic node) {
    beginExpression(JSPrecedence.CALL_PRECEDENCE);
    use(node.target, JSPrecedence.CALL_PRECEDENCE);
    visitArguments(node.inputs);
    endExpression(JSPrecedence.CALL_PRECEDENCE);
  }

  visitInvokeSuper(HInvokeSuper node) {
    beginExpression(JSPrecedence.CALL_PRECEDENCE);
    Element superMethod = node.element;
    Element superClass = superMethod.enclosingElement;
    // Remove the element and 'this'.
    int argumentCount = node.inputs.length - 2;
    String className = compiler.namer.isolateAccess(superClass);
    if (superMethod.kind == ElementKind.FUNCTION ||
        superMethod.kind == ElementKind.GENERATIVE_CONSTRUCTOR) {
      String methodName = compiler.namer.instanceMethodName(
          currentLibrary, superMethod.name, argumentCount);
      buffer.add('$className.prototype.$methodName.call');
      visitArguments(node.inputs);
    } else if (superMethod.kind == ElementKind.FIELD) {
      buffer.add('this.${compiler.namer.getName(superMethod)}');
    } else {
      assert(superMethod.kind == ElementKind.GETTER);
      String methodName =
          compiler.namer.getterName(currentLibrary, superMethod.name);
      buffer.add('$className.prototype.$methodName.call()');
    }
    endExpression(JSPrecedence.CALL_PRECEDENCE);
    world.registerStaticUse(superMethod);
  }

  visitFieldGet(HFieldGet node) {
    if (node.receiver !== null) {
      String name =
          compiler.namer.instanceFieldName(currentLibrary, node.name);
      beginExpression(JSPrecedence.MEMBER_PRECEDENCE);
      use(node.receiver, JSPrecedence.MEMBER_PRECEDENCE);
      buffer.add('.');
      buffer.add(name);
      beginExpression(JSPrecedence.MEMBER_PRECEDENCE);
    } else {
      buffer.add(JsNames.getValid(node.name.slowToString()));
    }
  }

  visitFieldSet(HFieldSet node) {
    String name;
    if (node.receiver !== null) {
      name =
          compiler.namer.instanceFieldName(currentLibrary, node.name);
      beginExpression(JSPrecedence.ASSIGNMENT_PRECEDENCE);
      use(node.receiver, JSPrecedence.MEMBER_PRECEDENCE);
      buffer.add('.');
      buffer.add(name);
    } else {
      // TODO(ngeoffray): Remove the 'var' once we don't globally box
      // variables used in a try/catch.
      name = JsNames.getValid(node.name.slowToString());
      declareVariable(name);
    }
    buffer.add(' = ');
    use(node.value, JSPrecedence.ASSIGNMENT_PRECEDENCE);
    if (node.receiver !== null) {
      endExpression(JSPrecedence.ASSIGNMENT_PRECEDENCE);
    }
  }

  visitForeign(HForeign node) {
    String code = node.code.slowToString();
    List<HInstruction> inputs = node.inputs;
    List<String> parts = code.split('#');
    if (parts.length != inputs.length + 1) {
      compiler.internalError(
          'Wrong number of arguments for JS', instruction: node);
    }
    beginExpression(JSPrecedence.EXPRESSION_PRECEDENCE);
    buffer.add(parts[0]);
    for (int i = 0; i < inputs.length; i++) {
      use(inputs[i], JSPrecedence.EXPRESSION_PRECEDENCE);
      buffer.add(parts[i + 1]);
    }
    endExpression(JSPrecedence.EXPRESSION_PRECEDENCE);
  }

  visitForeignNew(HForeignNew node) {
    String jsClassReference = compiler.namer.isolateAccess(node.element);
    beginExpression(JSPrecedence.MEMBER_PRECEDENCE);
    buffer.add('new $jsClassReference(');
    // We can't use 'visitArguments', since our arguments start at input[0].
    List<HInstruction> inputs = node.inputs;
    for (int i = 0; i < inputs.length; i++) {
      if (i != 0) buffer.add(', ');
      use(inputs[i], JSPrecedence.ASSIGNMENT_PRECEDENCE);
    }
    buffer.add(')');
    endExpression(JSPrecedence.MEMBER_PRECEDENCE);
  }

  visitConstant(HConstant node) {
    assert(isGenerateAtUseSite(node));
    // TODO(floitsch): the compile-time constant handler and the codegen
    // need to work together to avoid the parenthesis. See r4928 for an
    // implementation that still dealt with precedence.
    ConstantHandler handler = compiler.constantHandler;
    String name = handler.getNameForConstant(node.constant);
    if (name === null) {
      assert(!node.constant.isObject());
      if (node.constant.isNum()
          && expectedPrecedence == JSPrecedence.MEMBER_PRECEDENCE) {
        buffer.add('(');
        handler.writeConstant(buffer, node.constant);
        buffer.add(')');
      } else {
        handler.writeConstant(buffer, node.constant);
      }
    } else {
      buffer.add(compiler.namer.CURRENT_ISOLATE);
      buffer.add(".");
      buffer.add(name);
    }
  }

  visitLoopBranch(HLoopBranch node) {
    if (subGraph !== null && node.block === subGraph.end) {
      // We are generating code for a loop condition.
      // If doing this as part of a SubGraph traversal, the
      // calling code will handle the control flow logic.

      // If we are generating the subgraph as an expression, the
      // condition will be generated as the expression.
      // Otherwise, we don't generate the expression, and leave that
      // to the code that called [visitSubGraph].
      if (isGeneratingExpression()) {
        use(node.inputs[0], JSPrecedence.EXPRESSION_PRECEDENCE);
      }
      return;
    }
    HBasicBlock branchBlock = currentBlock;
    addIndentation();
    handleLoopCondition(node);
    List<HBasicBlock> dominated = currentBlock.dominatedBlocks;
    // For a do while loop, the body has already been visited.
    if (!node.isDoWhile()) {
      visitBasicBlock(dominated[0]);
    }
    endLoop(node.block);

    // If the branch does not dominate the code after the loop, the
    // dominator will visit it.
    if (branchBlock.successors[1].dominator !== branchBlock) return;

    visitBasicBlock(branchBlock.successors[1]);
    // With labeled breaks we can have more dominated blocks.
    if (dominated.length >= 3) {
      for (int i = 2; i < dominated.length; i++) {
        visitBasicBlock(dominated[i]);
      }
    }
  }

  visitNot(HNot node) {
    bool isBuiltinRelational(HInstruction instruction) {
      if (instruction is !HRelational) return false;
      HRelational relational = instruction;
      return relational.builtin;
    }

    assert(node.inputs.length == 1);
    HInstruction input = node.inputs[0];
    if (input is HBoolify && isGenerateAtUseSite(input)) {
      beginExpression(JSPrecedence.EQUALITY_PRECEDENCE);
      assert(node.inputs.length == 1);
      use(input.inputs[0], JSPrecedence.EQUALITY_PRECEDENCE);
      buffer.add(' !== true');
      endExpression(JSPrecedence.EQUALITY_PRECEDENCE);
    } else if (isBuiltinRelational(input) &&
               isGenerateAtUseSite(input) &&
               input.inputs[0].propagatedType.isUseful() &&
               !input.inputs[0].isDouble() &&
               input.inputs[1].propagatedType.isUseful() &&
               !input.inputs[1].isDouble()) {
      // This optimization doesn't work for NaN, so we only do it if the
      // type is known to be non-Double.
      Map<String, String> inverseOperator = const <String>{
        "==" : "!=",
        "!=" : "==",
        "===": "!==",
        "!==": "===",
        "<"  : ">=",
        "<=" : ">",
        ">"  : "<=",
        ">=" : "<"
      };
      HRelational relational = input;
      visitInvokeBinary(input,
                        inverseOperator[relational.operation.name.stringValue]);
    } else {
      beginExpression(JSPrecedence.PREFIX_PRECEDENCE);
      buffer.add('!');
      use(input, JSPrecedence.PREFIX_PRECEDENCE);
      endExpression(JSPrecedence.PREFIX_PRECEDENCE);
    }
  }

  visitParameterValue(HParameterValue node) {
    assert(isGenerateAtUseSite(node));
    buffer.add(parameterNames[node.element]);
  }

  visitPhi(HPhi node) {
    String operation = logicalOperations[node];
    if (operation !== null) {
      emitLogicalOperation(node, operation);
    } else {
      HPhi canonicalPhi = phiEquivalence.getRepresentative(node);
      buffer.add('${temporary(canonicalPhi)}');
    }
  }

  visitReturn(HReturn node) {
    addIndentation();
    assert(node.inputs.length == 1);
    HInstruction input = node.inputs[0];
    if (input.isConstantNull()) {
      buffer.add('return;\n');
    } else {
      buffer.add('return ');
      use(node.inputs[0], JSPrecedence.EXPRESSION_PRECEDENCE);
      buffer.add(';\n');
    }
  }

  visitThis(HThis node) {
    buffer.add('this');
  }

  visitThrow(HThrow node) {
    addIndentation();
    if (node.isRethrow) {
      buffer.add('throw ');
      use(node.inputs[0], JSPrecedence.EXPRESSION_PRECEDENCE);
    } else {
      generateThrowWithHelper('captureStackTrace', node.inputs[0]);
    }
    buffer.add(';\n');
  }

  visitBoundsCheck(HBoundsCheck node) {
    // TODO(ngeoffray): Separate the two checks of the bounds check, so,
    // e.g., the zero checks can be shared if possible.

    // If the checks always succeede, we would have removed the bounds check
    // completely.
    assert(node.staticChecks != HBoundsCheck.ALWAYS_TRUE);
    if (node.staticChecks != HBoundsCheck.ALWAYS_FALSE) {
      buffer.add('if (');
      if (node.staticChecks != HBoundsCheck.ALWAYS_ABOVE_ZERO) {
        assert(node.staticChecks == HBoundsCheck.FULL_CHECK);
        use(node.index, JSPrecedence.RELATIONAL_PRECEDENCE);
        buffer.add(' < 0 || ');
      }
      use(node.index, JSPrecedence.RELATIONAL_PRECEDENCE);
      buffer.add(' >= ');
      use(node.length, JSPrecedence.SHIFT_PRECEDENCE);
      buffer.add(") ");
    }
    generateThrowWithHelper('ioore', node.index);
  }

  visitIntegerCheck(HIntegerCheck node) {
    if (!node.alwaysFalse) {
      buffer.add('if (');
      use(node.value, JSPrecedence.EQUALITY_PRECEDENCE);
      buffer.add(' !== (');
      use(node.value, JSPrecedence.BITWISE_OR_PRECEDENCE);
      buffer.add(" | 0)) ");
    }
    generateThrowWithHelper('iae', node.value);
  }

  void generateThrowWithHelper(String helperName, HInstruction argument) {
    Element helper = compiler.findHelper(new SourceString(helperName));
    world.registerStaticUse(helper);
    buffer.add('throw ');
    beginExpression(JSPrecedence.EXPRESSION_PRECEDENCE);
    beginExpression(JSPrecedence.CALL_PRECEDENCE);
    buffer.add(compiler.namer.isolateAccess(helper));
    visitArguments([null, argument]);
    endExpression(JSPrecedence.CALL_PRECEDENCE);
    endExpression(JSPrecedence.EXPRESSION_PRECEDENCE);
  }

  void addIndentation() {
    for (int i = 0; i < indent; i++) {
      buffer.add('  ');
    }
  }

  void addIndented(String text) {
    addIndentation();
    buffer.add(text);
  }

  void visitStatic(HStatic node) {
    world.registerStaticUse(node.element);
    buffer.add(compiler.namer.isolateAccess(node.element));
  }

  void visitStaticStore(HStaticStore node) {
    world.registerStaticUse(node.element);
    beginExpression(JSPrecedence.ASSIGNMENT_PRECEDENCE);
    buffer.add(compiler.namer.isolateAccess(node.element));
    buffer.add(' = ');
    use(node.inputs[0], JSPrecedence.ASSIGNMENT_PRECEDENCE);
    endExpression(JSPrecedence.ASSIGNMENT_PRECEDENCE);
  }

  void visitLiteralList(HLiteralList node) {
    generateArrayLiteral(node);
  }

  void generateArrayLiteral(HLiteralList node) {
    buffer.add('[');
    int len = node.inputs.length;
    for (int i = 0; i < len; i++) {
      if (i != 0) buffer.add(', ');
      use(node.inputs[i], JSPrecedence.ASSIGNMENT_PRECEDENCE);
    }
    buffer.add(']');
  }

  void visitIndex(HIndex node) {
    if (node.builtin) {
      beginExpression(JSPrecedence.MEMBER_PRECEDENCE);
      use(node.inputs[1], JSPrecedence.MEMBER_PRECEDENCE);
      buffer.add('[');
      use(node.inputs[2], JSPrecedence.EXPRESSION_PRECEDENCE);
      buffer.add(']');
      endExpression(JSPrecedence.MEMBER_PRECEDENCE);
    } else {
      visitInvokeStatic(node);
    }
  }

  void visitIndexAssign(HIndexAssign node) {
    if (node.builtin) {
      beginExpression(JSPrecedence.ASSIGNMENT_PRECEDENCE);
      use(node.inputs[1], JSPrecedence.MEMBER_PRECEDENCE);
      buffer.add('[');
      use(node.inputs[2], JSPrecedence.EXPRESSION_PRECEDENCE);
      buffer.add('] = ');
      use(node.inputs[3], JSPrecedence.ASSIGNMENT_PRECEDENCE);
      endExpression(JSPrecedence.ASSIGNMENT_PRECEDENCE);
    } else {
      visitInvokeStatic(node);
    }
  }

  String builtinJsName(HInvokeInterceptor interceptor) {
    HInstruction receiver = interceptor.inputs[1];
    bool getter = interceptor.getter;
    SourceString name = interceptor.name;

    if (receiver.isIndexablePrimitive()) {
      if (interceptor.isLengthGetter()) {
        return 'length';
      } else if (!getter
                 && name == const SourceString('indexOf')
                 && interceptor.inputs.length == 3) {
        // If there are three inputs, the start index is not given,
        // and we share the same default value with the native
        // implementation.
        return 'indexOf';
      } else if (!getter
                 && name == const SourceString('lastIndexOf')
                 && interceptor.inputs.length == 3) {
        // If there are three inputs, the start index is not given,
        // and we share the same default value with the native
        // implementation.
        return 'lastIndexOf';
      }
    }

    if (receiver.isExtendableArray() && !getter) {
      if (name == const SourceString('add')) {
        return 'push';
      }
      if (name == const SourceString('removeLast')) {
        return 'pop';
      }
    }

    if (receiver.isString() && !getter) {
      if (name == const SourceString('concat')
          && interceptor.inputs[2].isString()) {
        return '+';
      }
    }

    return null;
  }

  void visitInvokeInterceptor(HInvokeInterceptor node) {
    String builtin = builtinJsName(node);
    if (builtin !== null) {
      if (builtin == '+') {
        beginExpression(JSPrecedence.ADDITIVE_PRECEDENCE);
        use(node.inputs[1], JSPrecedence.ADDITIVE_PRECEDENCE);
        buffer.add(' + ');
        use(node.inputs[2], JSPrecedence.MULTIPLICATIVE_PRECEDENCE);
        endExpression(JSPrecedence.ADDITIVE_PRECEDENCE);
      } else {
        beginExpression(JSPrecedence.CALL_PRECEDENCE);
        use(node.inputs[1], JSPrecedence.MEMBER_PRECEDENCE);
        buffer.add('.');
        buffer.add(builtin);
        if (node.getter) return;
        buffer.add('(');
        for (int i = 2; i < node.inputs.length; i++) {
          if (i != 2) buffer.add(', ');
          use(node.inputs[i], JSPrecedence.ASSIGNMENT_PRECEDENCE);
        }
        buffer.add(")");
        endExpression(JSPrecedence.CALL_PRECEDENCE);
      }
    } else {
      return visitInvokeStatic(node);
    }
  }

  void checkInt(HInstruction input, String cmp) {
    beginExpression(JSPrecedence.EQUALITY_PRECEDENCE);
    use(input, JSPrecedence.EQUALITY_PRECEDENCE);
    buffer.add(' $cmp (');
    use(input, JSPrecedence.BITWISE_OR_PRECEDENCE);
    buffer.add(' | 0)');
    endExpression(JSPrecedence.EQUALITY_PRECEDENCE);
  }

  void checkNum(HInstruction input, String cmp) {
    beginExpression(JSPrecedence.EQUALITY_PRECEDENCE);
    buffer.add('typeof ');
    use(input, JSPrecedence.PREFIX_PRECEDENCE);
    buffer.add(" $cmp 'number'");
    endExpression(JSPrecedence.EQUALITY_PRECEDENCE);
  }

  void checkDouble(HInstruction input, String cmp) {
    checkNum(input, cmp);
  }

  void checkString(HInstruction input, String cmp) {
    beginExpression(JSPrecedence.EQUALITY_PRECEDENCE);
    buffer.add('typeof ');
    use(input, JSPrecedence.PREFIX_PRECEDENCE);
    buffer.add(" $cmp 'string'");
    endExpression(JSPrecedence.EQUALITY_PRECEDENCE);
  }

  void checkBool(HInstruction input, String cmp) {
    beginExpression(JSPrecedence.EQUALITY_PRECEDENCE);
    buffer.add('typeof ');
    use(input, JSPrecedence.PREFIX_PRECEDENCE);
    buffer.add(" $cmp 'boolean'");
    endExpression(JSPrecedence.EQUALITY_PRECEDENCE);
  }

  void checkObject(HInstruction input, String cmp) {
    beginExpression(JSPrecedence.EQUALITY_PRECEDENCE);
    buffer.add('typeof ');
    use(input, JSPrecedence.PREFIX_PRECEDENCE);
    buffer.add(" $cmp 'object'");
    endExpression(JSPrecedence.EQUALITY_PRECEDENCE);
  }

  void checkArray(HInstruction input, String cmp) {
    beginExpression(JSPrecedence.EQUALITY_PRECEDENCE);
    use(input, JSPrecedence.MEMBER_PRECEDENCE);
    buffer.add('.constructor $cmp Array');
    endExpression(JSPrecedence.EQUALITY_PRECEDENCE);
  }

  void checkImmutableArray(HInstruction input) {
    beginExpression(JSPrecedence.PREFIX_PRECEDENCE);
    buffer.add('!!');
    use(input, JSPrecedence.MEMBER_PRECEDENCE);
    buffer.add('.immutable\$list');
    endExpression(JSPrecedence.PREFIX_PRECEDENCE);
  }

  void checkExtendableArray(HInstruction input) {
    beginExpression(JSPrecedence.PREFIX_PRECEDENCE);
    buffer.add('!!');
    use(input, JSPrecedence.MEMBER_PRECEDENCE);
    buffer.add('.fixed\$length');
    endExpression(JSPrecedence.PREFIX_PRECEDENCE);
  }

  void checkNull(HInstruction input) {
    beginExpression(JSPrecedence.EQUALITY_PRECEDENCE);
    use(input, JSPrecedence.EQUALITY_PRECEDENCE);
    buffer.add(" === (void 0)");
    endExpression(JSPrecedence.EQUALITY_PRECEDENCE);
  }

  void checkFunction(HInstruction input, Element element) {
    beginExpression(JSPrecedence.LOGICAL_OR_PRECEDENCE);
    beginExpression(JSPrecedence.EQUALITY_PRECEDENCE);
    buffer.add('typeof ');
    use(input, JSPrecedence.PREFIX_PRECEDENCE);
    buffer.add(" === 'function'");
    endExpression(JSPrecedence.EQUALITY_PRECEDENCE);
    buffer.add(" || ");
    beginExpression(JSPrecedence.LOGICAL_AND_PRECEDENCE);
    checkObject(input, '===');
    buffer.add(" && ");
    checkType(input, element);
    endExpression(JSPrecedence.LOGICAL_AND_PRECEDENCE);
    endExpression(JSPrecedence.LOGICAL_OR_PRECEDENCE);
  }

  void checkType(HInstruction input, Element element) {
    bool requiresNativeIsCheck =
        backend.emitter.nativeEmitter.requiresNativeIsCheck(element);
    if (!requiresNativeIsCheck) buffer.add('!!');
    use(input, JSPrecedence.MEMBER_PRECEDENCE);
    buffer.add('.');
    buffer.add(compiler.namer.operatorIs(element));
    if (requiresNativeIsCheck) buffer.add('()');
  }

  void handleStringSupertypeCheck(HInstruction input, Element element) {
    // Make sure List and String don't share supertypes, otherwise we
    // would need to check for List too.
    assert(element !== compiler.listClass
           && !Elements.isListSupertype(element, compiler));
    beginExpression(JSPrecedence.LOGICAL_OR_PRECEDENCE);
    checkString(input, '===');
    buffer.add(' || ');
    beginExpression(JSPrecedence.LOGICAL_AND_PRECEDENCE);
    checkObject(input, '===');
    buffer.add(' && ');
    checkType(input, element);
    endExpression(JSPrecedence.LOGICAL_AND_PRECEDENCE);
    endExpression(JSPrecedence.LOGICAL_OR_PRECEDENCE);
  }

  void handleListOrSupertypeCheck(HInstruction input, Element element) {
    // Make sure List and String don't share supertypes, otherwise we
    // would need to check for String too.
    assert(element !== compiler.stringClass
           && !Elements.isStringSupertype(element, compiler));
    beginExpression(JSPrecedence.LOGICAL_AND_PRECEDENCE);
    checkObject(input, '===');
    buffer.add(' && (');
    beginExpression(JSPrecedence.LOGICAL_OR_PRECEDENCE);
    checkArray(input, '===');
    buffer.add(' || ');
    checkType(input, element);
    buffer.add(')');
    endExpression(JSPrecedence.LOGICAL_OR_PRECEDENCE);
    endExpression(JSPrecedence.LOGICAL_AND_PRECEDENCE);
  }

  void visitIs(HIs node) {
    Type type = node.typeExpression;
    Element element = type.element;
    if (element.kind === ElementKind.TYPE_VARIABLE) {
      compiler.unimplemented("visitIs for type variables", instruction: node);
    } else if (element.kind === ElementKind.TYPEDEF) {
      compiler.unimplemented("visitIs for typedefs", instruction: node);
    }
    world.registerIsCheck(type.element);
    LibraryElement coreLibrary = compiler.coreLibrary;
    ClassElement objectClass = compiler.objectClass;
    HInstruction input = node.expression;

    if (node.nullOk) {
      beginExpression(JSPrecedence.LOGICAL_OR_PRECEDENCE);
      checkNull(input);
      buffer.add(' || ');
    }
    if (element === objectClass || element === compiler.dynamicClass) {
      // The constant folder also does this optimization, but we make
      // it safe by assuming it may have not run.
      buffer.add('true');
    } else if (element == compiler.stringClass) {
      checkString(input, '===');
    } else if (element == compiler.doubleClass) {
      checkDouble(input, '===');
    } else if (element == compiler.numClass) {
      checkNum(input, '===');
    } else if (element == compiler.boolClass) {
      checkBool(input, '===');
    } else if (element == compiler.functionClass) {
      checkFunction(input, element);
    } else if (element == compiler.intClass) {
      beginExpression(JSPrecedence.LOGICAL_AND_PRECEDENCE);
      checkNum(input, '===');
      buffer.add(' && ');
      checkInt(input, '===');
      endExpression(JSPrecedence.LOGICAL_AND_PRECEDENCE);
    } else if (Elements.isStringSupertype(element, compiler)) {
      handleStringSupertypeCheck(input, element);
    } else if (element === compiler.listClass
               || Elements.isListSupertype(element, compiler)) {
      handleListOrSupertypeCheck(input, element);
    } else if (input.propagatedType.canBePrimitive()
               || input.propagatedType.canBeNull()) {
      beginExpression(JSPrecedence.LOGICAL_AND_PRECEDENCE);
      checkObject(input, '===');
      buffer.add(' && ');
      checkType(input, element);
      endExpression(JSPrecedence.LOGICAL_AND_PRECEDENCE);
    } else {
      checkType(input, element);
    }
    if (compiler.codegenWorld.rti.hasTypeArguments(type)) {
      InterfaceType interfaceType = type;
      ClassElement cls = type.element;
      Link<Type> arguments = interfaceType.arguments;
      buffer.add(' && ');
      checkObject(node.typeInfoCall, '===');
      cls.typeParameters.forEach((name, _) {
        buffer.add(' && ');
        beginExpression(JSPrecedence.LOGICAL_AND_PRECEDENCE);
        use(node.typeInfoCall, JSPrecedence.EQUALITY_PRECEDENCE);
        buffer.add(".${name.slowToString()} === '${arguments.head}'");
        endExpression(JSPrecedence.LOGICAL_AND_PRECEDENCE);
      });
    }
    if (node.nullOk) {
      endExpression(JSPrecedence.LOGICAL_OR_PRECEDENCE);
    }
  }

  void visitTypeConversion(HTypeConversion node) {
    if (node.checked) {
      Element element = node.type.computeType(compiler).element;
      world.registerIsCheck(element);
      SourceString helper;
      String additionalArgument;
      bool nativeCheck =
          backend.emitter.nativeEmitter.requiresNativeIsCheck(element);
      beginExpression(JSPrecedence.CALL_PRECEDENCE);

      if (element == compiler.stringClass) {
        helper = const SourceString('stringTypeCheck');
      } else if (element == compiler.doubleClass) {
        helper = const SourceString('doubleTypeCheck');
      } else if (element == compiler.numClass) {
        helper = const SourceString('numTypeCheck');
      } else if (element == compiler.boolClass) {
        helper = const SourceString('boolTypeCheck');
      } else if (element == compiler.functionClass || element.isTypedef()) {
        helper = const SourceString('functionTypeCheck');
      } else if (element == compiler.intClass) {
        helper = const SourceString('intTypeCheck');
      } else if (Elements.isStringSupertype(element, compiler)) {
        if (nativeCheck) {
          helper = const SourceString('stringSuperNativeTypeCheck');
        } else {
          helper = const SourceString('stringSuperTypeCheck');
        }
      } else if (element === compiler.listClass) {
        helper = const SourceString('listTypeCheck');
      } else {
        additionalArgument = compiler.namer.operatorIs(element);
        if (Elements.isListSupertype(element, compiler)) {
          if (nativeCheck) {
            helper = const SourceString('listSuperNativeTypeCheck');
          } else {
            helper = const SourceString('listSuperTypeCheck');
          }
        } else if (nativeCheck) {
          helper = const SourceString('callTypeCheck');
        } else {
          helper = const SourceString('propertyTypeCheck');
        }
      }
      Element helperElement = compiler.findHelper(helper);
      world.registerStaticUse(helperElement);
      buffer.add(compiler.namer.isolateAccess(helperElement));
      buffer.add('(');
      use(node.checkedInput, JSPrecedence.EXPRESSION_PRECEDENCE);
      if (additionalArgument !== null) buffer.add(", '$additionalArgument'");
      buffer.add(')');
      endExpression(JSPrecedence.CALL_PRECEDENCE);
    } else {
      use(node.checkedInput, expectedPrecedence);
    }
  }
}

class SsaOptimizedCodeGenerator extends SsaCodeGenerator {
  SsaOptimizedCodeGenerator(backend, work, parameters, parameterNames)
    : super(backend, work, parameters, parameterNames);

  void beginGraph(HGraph graph) {}
  void endGraph(HGraph graph) {}

  void bailout(HTypeGuard guard, String reason) {
    HInstruction input = guard.guarded;
    Namer namer = compiler.namer;
    Element element = work.element;
    buffer.add('return ');
    if (element.isInstanceMember()) {
      // TODO(ngeoffray): This does not work in case we come from a
      // super call. We must make bailout names unique.
      buffer.add('this.${namer.getBailoutName(element)}');
    } else {
      buffer.add(namer.isolateBailoutAccess(element));
    }
    int parametersCount = parameterNames.length;
    buffer.add('($parameters');
    if (parametersCount != 0) buffer.add(', ');
    if (guard.guarded is !HParameterValue) {
      buffer.add('${guard.state}');
      bool first = true;
      // TODO(ngeoffray): if the bailout method takes more arguments,
      // fill the remaining arguments with undefined.
      // TODO(ngeoffray): try to put a variable at a deterministic
      // location, so that multiple bailout calls put the variable at
      // the same parameter index.
      for (int i = 0; i < guard.inputs.length; i++) {
        buffer.add(', ');
        use(guard.inputs[i], JSPrecedence.ASSIGNMENT_PRECEDENCE);
      }
    } else {
      assert(guard.guarded is HParameterValue);
      buffer.add(' 0');
    }
    buffer.add(')');
  }

  void visitTypeGuard(HTypeGuard node) {
    addIndentation();
    HInstruction input = node.guarded;
    if (node.isInteger()) {
      buffer.add('if (');
      checkInt(input, '!==');
      buffer.add(') ');
      bailout(node, 'Not an integer');
    } else if (node.isNumber()) {
      buffer.add('if (');
      checkNum(input, '!==');
      buffer.add(') ');
      bailout(node, 'Not a number');
    } else if (node.isBoolean()) {
      buffer.add('if (');
      checkBool(input, '!==');
      buffer.add(') ');
      bailout(node, 'Not a boolean');
    } else if (node.isString()) {
      buffer.add('if (');
      checkString(input, '!==');
      buffer.add(') ');
      bailout(node, 'Not a string');
    } else if (node.isExtendableArray()) {
      buffer.add('if (');
      checkObject(input, '!==');
      buffer.add('||');
      checkArray(input, '!==');
      buffer.add('||');
      checkExtendableArray(input);
      buffer.add(') ');
      bailout(node, 'Not an extendable array');
    } else if (node.isMutableArray()) {
      buffer.add('if (');
      checkObject(input, '!==');
      buffer.add('||');
      checkArray(input, '!==');
      buffer.add('||');
      checkImmutableArray(input);
      buffer.add(') ');
      bailout(node, 'Not a mutable array');
    } else if (node.isReadableArray()) {
      buffer.add('if (');
      checkObject(input, '!==');
      buffer.add('||');
      checkArray(input, '!==');
      buffer.add(') ');
      bailout(node, 'Not an array');
    } else if (node.isIndexablePrimitive()) {
      buffer.add('if (');
      checkString(input, '!==');
      buffer.add(' && (');
      checkObject(input, '!==');
      buffer.add('||');
      checkArray(input, '!==');
      buffer.add(')) ');
      bailout(node, 'Not a string or array');
    } else {
      compiler.internalError('Unexpected type guard', instruction: input);
    }
    buffer.add(';\n');
  }

  void beginLoop(HBasicBlock block) {
    addIndentation();
    HLoopInformation info = block.loopInformation;
    for (LabelElement label in info.labels) {
      writeLabel(label);
      buffer.add(":");
    }
    buffer.add('while (true) {\n');
    indent++;
  }

  void endLoop(HBasicBlock block) {
    indent--;
    addIndented('}\n');  // Close 'while' loop.
  }

  void handleLoopCondition(HLoopBranch node) {
    buffer.add('if (!');
    use(node.inputs[0], JSPrecedence.PREFIX_PRECEDENCE);
    buffer.add(') break;\n');
  }

  void startIf(HIf node) {
  }

  void endIf(HIf node) {
    indent--;
    addIndented('}\n');
  }

  void startThen(HIf node) {
    addIndented('if (');
    use(node.inputs[0], JSPrecedence.EXPRESSION_PRECEDENCE);
    buffer.add(') {\n');
    indent++;
  }

  void endThen(HIf node) {
  }

  void startElse(HIf node) {
    indent--;
    addIndented('} else {\n');
    indent++;
  }

  void endElse(HIf node) {
  }

  void preLabeledBlock(HLabeledBlockInformation labeledBlockInfo) {
  }

  void startLabeledBlock(HLabeledBlockInformation labeledBlockInfo) {
  }

  void endLabeledBlock(HLabeledBlockInformation labeledBlockInfo) {
  }
}

class SsaUnoptimizedCodeGenerator extends SsaCodeGenerator {

  final StringBuffer setup;
  final List<String> labels;
  int labelId = 0;
  int maxBailoutParameters = 0;

  SsaUnoptimizedCodeGenerator(backend, work, parameters, parameterNames)
    : super(backend, work, parameters, parameterNames),
      setup = new StringBuffer(),
      labels = <String>[];

  String pushLabel() {
    String label = 'L${labelId++}';
    labels.addLast(label);
    return label;
  }

  String popLabel() {
    return labels.removeLast();
  }

  String currentLabel() {
    return labels.last();
  }

  void beginGraph(HGraph graph) {
    if (!graph.entry.hasGuards()) return;
    addIndented('switch (state) {\n');
    indent++;
    addIndented('case 0:\n');
    indent++;

    // The setup phase of a bailout function sets up the environment for
    // each bailout target. Each bailout target will populate this
    // setup phase. It is put at the beginning of the function.
    setup.add('  switch (state) {\n');
  }

  void endGraph(HGraph graph) {
    if (!graph.entry.hasGuards()) return;
    indent--; // Close original case.
    indent--;
    addIndented('}\n');  // Close 'switch'.
    setup.add('  }\n');
  }

  // For instructions that reference a guard or a check, we change that
  // reference to the instruction they guard against. Therefore, we must
  // use that instruction when restoring the environment.
  HInstruction unwrap(HInstruction argument) {
    if (argument is HIntegerCheck) {
      HIntegerCheck instruction = argument;
      return unwrap(instruction.value);
    } else if (argument is HBoundsCheck) {
      HBoundsCheck instruction = argument;
      return unwrap(instruction.index);
    } else if (argument is HTypeGuard) {
      HTypeGuard instruction = argument;
      return unwrap(instruction.guarded);
    } else {
      return argument;
    }
  }

  bool visitAndOrInfo(HAndOrBlockInformation info) => false;
  bool visitIfInfo(HIfBlockInformation info) => false;
  bool visitLoopInfo(HLoopBlockInformation info) => false;
  bool visitTryInfo(HTryBlockInformation info) => false;
  bool visitSequenceInfo(HStatementSequenceInformation info) => false;

  void visitTypeGuard(HTypeGuard node) {
    indent--;
    addIndented('case ${node.state}:\n');
    indent++;
    addIndented('state = 0;\n');

    setup.add('    case ${node.state}:\n');
    int i = 0;
    for (HInstruction input in node.inputs) {
      HInstruction instruction = unwrap(input);
      setup.add('      ${temporary(instruction)} = env$i;\n');
      i++;
    }
    if (i > maxBailoutParameters) maxBailoutParameters = i;
    setup.add('      break;\n');
  }

  void startBailoutCase(List<HTypeGuard> bailouts1,
                        List<HTypeGuard> bailouts2) {
    indent--;
    handleBailoutCase(bailouts1);
    handleBailoutCase(bailouts2);
    indent++;
  }

  void handleBailoutCase(List<HTypeGuard> guards) {
    for (int i = 0, len = guards.length; i < len; i++) {
      addIndented('case ${guards[i].state}:\n');
    }
  }

  void startBailoutSwitch() {
    addIndented('switch (state) {\n');
    indent++;
    addIndented('case 0:\n');
    indent++;
  }

  void endBailoutSwitch() {
    indent--; // Close 'case'.
    indent--;
    addIndented('}\n');  // Close 'switch'.
  }

  void beginLoop(HBasicBlock block) {
    // TODO(ngeoffray): Don't put labels on loops that don't bailout.
    String newLabel = pushLabel();
    if (block.hasGuards()) {
      startBailoutCase(block.guards, const <HTypeGuard>[]);
    }

    addIndentation();
    HLoopInformation loopInformation = block.loopInformation;
    for (LabelElement label in loopInformation.labels) {
      writeLabel(label);
      buffer.add(":");
    }
    buffer.add('$newLabel: while (true) {\n');
    indent++;

    if (block.hasGuards()) {
      startBailoutSwitch();
      if (loopInformation.target !== null) {
        breakAction[loopInformation.target] = (TargetElement target) {
          addIndented("break $newLabel;\n");
        };
      }
    }
  }

  void endLoop(HBasicBlock block) {
    popLabel();
    HBasicBlock header = block.isLoopHeader() ? block : block.parentLoopHeader;
    if (header.hasGuards()) {
      endBailoutSwitch();
      HLoopInformation info = header.loopInformation;
      if (info.target != null) breakAction.remove(info.target);
    }
    indent--;
    addIndented('}\n');  // Close 'while'.
  }

  void handleLoopCondition(HLoopBranch node) {
    buffer.add('if (!');
    use(node.inputs[0], JSPrecedence.PREFIX_PRECEDENCE);
    buffer.add(') break ${currentLabel()};\n');
  }

  void startIf(HIf node) {
    bool hasGuards = node.thenBlock.hasGuards()
        || (node.hasElse && node.elseBlock.hasGuards());
    if (hasGuards) {
      startBailoutCase(node.thenBlock.guards,
          node.hasElse ? node.elseBlock.guards : const <HTypeGuard>[]);
    }
  }

  void endIf(HIf node) {
    indent--;
    addIndented('}\n');
  }

  void startThen(HIf node) {
    bool hasGuards = node.thenBlock.hasGuards()
        || (node.hasElse && node.elseBlock.hasGuards());
    addIndented('if (');
    int precedence = JSPrecedence.EXPRESSION_PRECEDENCE;
    if (hasGuards) {
      // TODO(ngeoffray): Put the condition initialization in the
      // [setup] buffer.
      List<HTypeGuard> guards = node.thenBlock.guards;
      for (int i = 0, len = guards.length; i < len; i++) {
        buffer.add('state == ${guards[i].state} || ');
      }
      buffer.add('(state == 0 && ');
      precedence = JSPrecedence.BITWISE_OR_PRECEDENCE;
    }
    use(node.inputs[0], precedence);
    if (hasGuards) {
      buffer.add(')');
    }
    buffer.add(') {\n');
    indent++;
    if (node.thenBlock.hasGuards()) {
      startBailoutSwitch();
    }
  }

  void endThen(HIf node) {
    if (node.thenBlock.hasGuards()) {
      endBailoutSwitch();
    }
  }

  void startElse(HIf node) {
    indent--;
    addIndented('} else {\n');
    indent++;
    if (node.elseBlock.hasGuards()) {
      startBailoutSwitch();
    }
  }

  void endElse(HIf node) {
    if (node.elseBlock.hasGuards()) {
      endBailoutSwitch();
    }
  }

  void preLabeledBlock(HLabeledBlockInformation labeledBlockInfo) {
    if (labeledBlockInfo.body.start.hasGuards()) {
      indent--;
      handleBailoutCase(labeledBlockInfo.body.start.guards);
      indent++;
    }
  }

  void startLabeledBlock(HLabeledBlockInformation labeledBlockInfo) {
    if (labeledBlockInfo.body.start.hasGuards()) {
      startBailoutSwitch();
    }
  }

  void endLabeledBlock(HLabeledBlockInformation labeledBlockInfo) {
    if (labeledBlockInfo.body.start.hasGuards()) {
      endBailoutSwitch();
    }
  }
}
