/*
    _____ _____ _____ __
   |   __|     |  |  |  |      The SOUL language
   |__   |  |  |  |  |  |__    Copyright (c) 2019 - ROLI Ltd.
   |_____|_____|_____|_____|

   The code in this file is provided under the terms of the ISC license:

   Permission to use, copy, modify, and/or distribute this software for any purpose
   with or without fee is hereby granted, provided that the above copyright notice and
   this permission notice appear in all copies.

   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD
   TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN
   NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
   DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
   IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
   CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

namespace soul
{

//==============================================================================
/**
    Converts SOUL AST -> HEART AST
*/
struct HEARTGenerator  : public ASTVisitor
{
    struct UnresolvedFunctionCall
    {
        heart::FunctionCallPtr call;
        AST::FunctionPtr function;

        void resolve()   { call->function = function->getGeneratedFunction(); }
    };

    using UnresolvedFunctionCallList = std::vector<UnresolvedFunctionCall>;

    //==============================================================================
    static void run (AST::ModuleBase& source, Module& targetModule,
                     UnresolvedFunctionCallList& unresolvedCalls,
                     uint32_t maxNestedExpressionDepth = 255)
    {
        SanityCheckPass::runPostResolution (source);
        HEARTGenerator (source, targetModule, unresolvedCalls, maxNestedExpressionDepth).visitObject (source);
    }

private:
    using super = ASTVisitor;

    HEARTGenerator (AST::ModuleBase& source, Module& targetModule,
                    std::vector<UnresolvedFunctionCall>& unresolvedCalls, uint32_t maxDepth) noexcept
        : module (targetModule), builder (targetModule),
          maxExpressionDepth (maxDepth), unresolvedFunctionCalls (unresolvedCalls)
    {
        module.moduleName = source.getFullyQualifiedPath().toString();
    }

    pool_ptr<const AST::Graph> sourceGraph;
    pool_ptr<const AST::Processor> sourceProcessor;
    Module& module;

    uint32_t loopIndex = 0, ifIndex = 0;
    bool parsingStateVariables = false;

    FunctionBuilder builder;
    heart::VariablePtr currentTargetVariable;
    uint32_t expressionDepth = 0;
    const uint32_t maxExpressionDepth;
    heart::BlockPtr breakTarget, continueTarget;
    UnresolvedFunctionCallList& unresolvedFunctionCalls;

    //==============================================================================
    Identifier convertIdentifier (Identifier i)
    {
        return module.allocator.get (i);
    }

    heart::Variable& createVariableDeclaration (AST::VariableDeclaration& v,
                                                heart::Variable::Role role)
    {
        auto& av = module.allocate<heart::Variable> (v.context.location, v.getType(),
                                                     convertIdentifier (v.name), role);
        v.generatedVariable = av;
        av.annotation = v.annotation.toPlainAnnotation();
        return av;
    }

    void addBranchIf (AST::Expression& condition, heart::Block& trueBranch,
                      heart::Block& falseBranch, heart::BlockPtr subsequentBranch)
    {
        builder.addBranchIf (evaluateAsExpression (condition, PrimitiveType::bool_),
                             trueBranch, falseBranch, subsequentBranch);
    }

    void visitWithDestination (heart::VariablePtr destVar, AST::StatementPtr s)
    {
        auto oldTarget = currentTargetVariable;
        auto oldDepth = expressionDepth;
        currentTargetVariable = destVar;
        expressionDepth = 0;
        visitObject (*s);
        currentTargetVariable = oldTarget;
        expressionDepth = oldDepth;
    }

    void visitAsStatement (AST::StatementPtr s)
    {
        if (s != nullptr)
            visitWithDestination (nullptr, s);
    }

    //==============================================================================
    void visit (AST::Processor& p) override
    {
        sourceProcessor = p;
        generateStructs (p.structures);
        module.annotation = p.annotation.toPlainAnnotation();

        parsingStateVariables = true;
        super::visit (p);
        parsingStateVariables = false;

        createInitFunction();
        generateFunctions (p.functions);
    }

    void visit (AST::Graph& g) override
    {
        module.annotation = g.annotation.toPlainAnnotation();
        sourceGraph = g;

        parsingStateVariables = true;
        super::visit (g);
        parsingStateVariables = false;
    }

    void visit (AST::Namespace& n) override
    {
        generateStructs (n.structures);
        for (auto& f : n.functions)   visitObject (*f);
        for (auto& s : n.structures)  visitObject (*s);
        for (auto& u : n.usings)      visitObject (*u);

        parsingStateVariables = true;

        for (auto& c : n.constants)
            if (c->isExternal)
                visitObject (*c);

        parsingStateVariables = false;
        generateFunctions (n.functions);
    }

    //==============================================================================
    void visit (AST::InputDeclaration& io) override
    {
        auto& i = module.allocate<heart::InputDeclaration> (io.context.location);
        i.name = convertIdentifier (io.name);
        i.index = (uint32_t) module.inputs.size();
        i.kind = io.kind;
        i.sampleTypes = io.getResolvedSampleTypes();
        i.annotation = io.annotation.toPlainAnnotation();
        i.arraySize = getProcessorArraySize (io.arraySize);
        io.generatedInput = i;

        SOUL_ASSERT (module.findOutput (io.name) == nullptr);
        SOUL_ASSERT (module.findInput (io.name) == nullptr);

        module.inputs.push_back (i);
    }

    void visit (AST::OutputDeclaration& io) override
    {
        auto& o = module.allocate<heart::OutputDeclaration> (io.context.location);
        o.name = convertIdentifier (io.name);
        o.index = (uint32_t) module.outputs.size();
        o.kind = io.kind;
        o.sampleTypes = io.getResolvedSampleTypes();
        o.annotation = io.annotation.toPlainAnnotation();
        o.arraySize = getProcessorArraySize (io.arraySize);
        io.generatedOutput = o;

        SOUL_ASSERT (module.findOutput (io.name) == nullptr);
        SOUL_ASSERT (module.findInput (io.name) == nullptr);

        module.outputs.push_back (o);
    }

    void visit (AST::Connection& conn) override
    {
        auto& c = module.allocate<heart::Connection> (conn.context.location);
        module.connections.push_back (c);

        c.sourceProcessor   = getOrAddProcessorInstance (*conn.source.processorName);
        c.destProcessor     = getOrAddProcessorInstance (*conn.dest.processorName);
        c.sourceChannel     = conn.source.channel;
        c.destChannel       = conn.dest.channel;
        c.interpolationType = conn.interpolationType;
        c.delayLength       = getDelayLength (conn.delayLength);
    }

    static int64_t getDelayLength (AST::ExpPtr delay)
    {
        if (delay != nullptr)
        {
            if (auto c = delay->getAsConstant())
                return SanityCheckPass::checkDelayLineLength (c->context, c->value);

            delay->context.throwError (Errors::delayLineMustBeConstant());
        }

        return 0;
    }

    static uint32_t getProcessorArraySize (AST::ExpPtr size)
    {
        if (size != nullptr)
        {
            if (auto c = size->getAsConstant())
            {
                if (c->value.getType().isPrimitiveInteger())
                {
                    auto value = c->value.getAsInt64();

                    if (value < 1 || value > (int64_t) AST::maxProcessorArraySize)
                        size->context.throwError (Errors::illegalArraySize());

                    return (uint32_t) value;
                }

                size->context.throwError (Errors::expectedInteger());
            }

            size->context.throwError (Errors::expectedConstant());
        }

        return 1;
    }

    heart::ProcessorInstancePtr getOrAddProcessorInstance (const AST::QualifiedIdentifier& processorName)
    {
        if (processorName.path.empty())
            return {};

        for (auto i : module.processorInstances)
            if (processorName.path.toString() == i->instanceName)
                return i;

        SOUL_ASSERT (sourceGraph != nullptr);

        for (auto& i : sourceGraph->processorInstances)
        {
            if (processorName == *i->instanceName)
            {
                auto targetProcessor = sourceGraph->findSingleMatchingProcessor (*i);
                SOUL_ASSERT (targetProcessor != nullptr);

                auto& p = module.allocate<heart::ProcessorInstance>();
                p.instanceName = processorName.path.toString();
                p.sourceName = targetProcessor->getFullyQualifiedPath().toString();
                p.arraySize = getProcessorArraySize (i->arrayArgument);

                if (i->clockMultiplierRatio != nullptr)
                {
                    if (auto c = i->clockMultiplierRatio->getAsConstant())
                        p.clockMultiplier = heart::getClockRatioFromValue (i->clockMultiplierRatio->context, c->value);
                    else
                        i->clockMultiplierRatio->context.throwError (Errors::ratioMustBeInteger());
                }

                if (i->clockDividerRatio != nullptr)
                {
                    if (auto c = i->clockDividerRatio->getAsConstant())
                        p.clockDivider = heart::getClockRatioFromValue (i->clockDividerRatio->context, c->value);
                    else
                        i->clockDividerRatio->context.throwError (Errors::ratioMustBeInteger());
                }

                for (auto& arg : i->specialisationArgs)
                {
                    heart::SpecialisationArgument newArg;

                    if (AST::isResolvedAsType (arg))
                        newArg.type = arg->resolveAsType();
                    else if (auto pr = cast<AST::ProcessorRef> (arg))
                        newArg.processorName = pr->processor->getFullyQualifiedPath().toString();
                    else if (auto c = arg->getAsConstant())
                        newArg.value = c->value;
                    else
                        arg->context.throwError (Errors::cannotResolveSpecialisationValue());

                    p.specialisationArgs.push_back (newArg);
                }

                module.processorInstances.push_back (p);
                return p;
            }
        }

        return {};
    }

    void visit (AST::Function& f) override
    {
        if (! f.isGeneric())
        {
            auto& af = module.allocate<heart::Function>();
            af.name = getFunctionName (f);
            module.functions.push_back (af);
            f.generatedFunction = af;
            af.intrinsic = f.intrinsic;
            af.isRunFunction = f.isRunFunction();
            af.isEventFunction = f.isEventFunction();
            af.annotation = f.annotation.toPlainAnnotation();
            af.location = f.context.location;
        }
    }

    Identifier getFunctionName (const AST::Function& f)
    {
        auto nameRoot = f.name.toString();

        if (f.isEventFunction())
        {
            auto name = heart::getEventFunctionName (nameRoot, f.parameters[0]->getType());
            SOUL_ASSERT (module.findFunction (name) == nullptr);
            return module.allocator.get (name);
        }

        return module.allocator.get (addSuffixToMakeUnique (nameRoot,
                                                            [this] (const std::string& name)
                                                            {
                                                                return module.findFunction (name) != nullptr;
                                                            }));
    }

    void generateStructs (ArrayView<AST::StructDeclarationPtr> structs)
    {
        for (auto& s : structs)
            module.structs.push_back (s->getStruct());
    }

    void createInitFunction()
    {
        auto& af = module.allocate<heart::Function>();

        af.name = module.allocator.get (heart::getInitFunctionName());
        af.isInitFunction = true;
        af.returnType = soul::Type (soul::PrimitiveType::void_);

        module.functions.push_back (af);

        builder.beginFunction (af);
        addStateVariableInitialisationCode();
        builder.endFunction();
        builder.checkFunctionBlocksForTermination();
    }

    void generateFunctions (ArrayView<AST::FunctionPtr> functions)
    {
        for (auto& f : functions)
            if (! f->isGeneric())
                generateFunction (*f);
    }

    void generateFunction (AST::Function& f)
    {
        auto& af = f.getGeneratedFunction();
        af.returnType = f.returnType->resolveAsType();

        builder.beginFunction (af);

        for (auto p : f.parameters)
        {
            auto& v = createVariableDeclaration (*p, heart::Variable::Role::parameter);

            if (af.isEventFunction && v.getType().isNonConstReference())
                p->context.throwError (Errors::eventParamsCannotBeNonConstReference());

            builder.addParameter (v);
        }

        if (f.block != nullptr)
        {
            visitObject (*f.block);

            builder.endFunction();

            if (! builder.checkFunctionBlocksForTermination())
            {
                // This will fail if the function isn't void but some blocks terminate without returning a value,
                // however, we'll make sure they're not unreachable before flagging this as an error
                Optimisations::optimiseFunctionBlocks (af, module.allocator);

                if (! builder.checkFunctionBlocksForTermination())
                    f.context.throwError (Errors::notAllControlPathsReturnAValue (f.name));
            }
        }
        else
        {
            af.hasNoBody = true;
            builder.endFunction();
        }
    }

    void addStateVariableInitialisationCode()
    {
        SOUL_ASSERT (sourceProcessor != nullptr);

        for (auto& v : sourceProcessor->stateVariables)
        {
            if (v->generatedVariable != nullptr)
            {
                if (v->initialValue != nullptr)
                    visitWithDestination (v->generatedVariable, v->initialValue);
                else if (! v->isExternal)
                    builder.addZeroAssignment (v->generatedVariable);
            }
        }
    }

    void visit (AST::Block& b) override
    {
        if (b.isFunctionMainBlock())
            builder.beginBlock (builder.createNewBlock());

        for (auto& s : b.statements)
        {
            builder.ensureBlockIsReady();
            expressionDepth = 0;
            visitAsStatement (s);
        }
    }

    heart::Expression& getAsReference (AST::Expression& e, bool isConstRef)
    {
        if (auto v = cast<AST::VariableRef> (e))
            return v->variable->getGeneratedVariable();

        if (auto member = cast<AST::StructMemberRef> (e))
            return createStructSubElement (*member, getAsReference (*member->object, isConstRef));

        if (auto subscript = cast<AST::ArrayElementRef> (e))
            return createArraySubElement (*subscript, getAsReference (*subscript->object, isConstRef));

        if (isConstRef)
            return getExpressionAsMutableLocalCopy (e);

        e.context.throwError (Errors::expressionNotAssignable());
    }

    void createAssignmentToCurrentTarget (AST::Expression& source)
    {
        if (currentTargetVariable != nullptr)
            createAssignment (*currentTargetVariable, source);
        else if (! source.isOutputEndpoint())
            source.context.throwError (Errors::unusedExpression());
    }

    void createAssignment (heart::Expression& destVar, AST::Expression& source)
    {
        builder.addAssignment (destVar, evaluateAsExpression (source, destVar.getType()));
    }

    heart::Expression& getExpressionAsConstLocalCopy (AST::Expression& e)
    {
        auto& local = builder.createRegisterVariable (e.getResultType().removeConstIfPresent());
        visitWithDestination (local, e);
        return local;
    }

    heart::Expression& getExpressionAsMutableLocalCopy (AST::Expression& e)
    {
        auto& local = builder.createMutableLocalVariable (e.getResultType().removeConstIfPresent());
        visitWithDestination (local, e);
        return local;
    }

    heart::Expression& evaluateAsExpression (AST::Expression& e)
    {
        if (++expressionDepth < maxExpressionDepth)
        {
            if (auto c = e.getAsConstant())
                return module.allocator.allocate<heart::Constant> (c->context.location, c->value);

            if (auto v = cast<AST::VariableRef> (e))
            {
                if (v->variable->isAssignable())
                    if (v->variable->getParentScope()->findModule() != e.getParentScope()->findModule())
                        v->context.throwError (Errors::cannotReferenceOtherProcessorVar());

                if (auto a = v->variable->generatedVariable)
                    return *a;

                if (auto initial = v->variable->initialValue)
                    return evaluateAsExpression (*initial);

                return builder.createZeroInitialiser (v->getResultType());
            }

            if (auto member = cast<AST::StructMemberRef> (e))
            {
                auto structType = getStructType (*member);

                auto& source = evaluateAsExpression (*member->object, structType);
                return createStructSubElement (*member, source);
            }

            if (auto subscript = cast<AST::ArrayElementRef> (e))
            {
                auto arrayOrVectorType = getArrayOrVectorType (*subscript);
                auto& source = evaluateAsExpression (*subscript->object, arrayOrVectorType);
                return createArraySubElement (*subscript, source);
            }

            if (auto c = cast<AST::TypeCast> (e))
            {
                auto numArgs = c->getNumArguments();
                SOUL_ASSERT (numArgs != 0);

                if (numArgs > 1)
                    return createAggregateWithInitialisers (*c);

                auto& sourceExp = evaluateAsExpression (*c->source);
                const auto& sourceType = sourceExp.getType();

                if (TypeRules::canCastTo (c->targetType, sourceType))
                    return builder.createCastIfNeeded (sourceExp, c->targetType);

                if (c->targetType.isFixedSizeAggregate() && c->targetType.getNumAggregateElements() == 1)
                    return createAggregateWithInitialisers (*c);

                SanityCheckPass::expectCastPossible (c->source->context, c->targetType, sourceType);
            }

            if (auto op = cast<AST::BinaryOperator> (e))
            {
                auto operandType = op->getOperandType();

                // (putting these into locals to make sure we evaluate everything in left-to-right order)
                auto& lhs = builder.createCastIfNeeded (evaluateAsExpression (*op->lhs), operandType);
                auto& rhs = builder.createCastIfNeeded (evaluateAsExpression (*op->rhs), operandType);

                return builder.createBinaryOp (op->context.location, lhs, rhs, op->operation, op->getResultType());
            }

            if (auto op = cast<AST::UnaryOperator> (e))
            {
                auto sourceType = op->getResultType();
                auto& source = builder.createCastIfNeeded (evaluateAsExpression (*op->source), sourceType);
                return builder.createUnaryOp (op->context.location, source, op->operation);
            }

            if (auto pp = cast<AST::ProcessorProperty> (e))
            {
                if (module.isNamespace())
                    pp->context.throwError (Errors::processorPropertyUsedOutsideDecl());

                return module.allocator.allocate<heart::ProcessorProperty> (pp->context.location, pp->property);
            }
        }

        return getExpressionAsConstLocalCopy (e);
    }

    heart::Expression& evaluateAsExpression (AST::Expression& e, const Type& targetType)
    {
        if (targetType.isReference() && ! targetType.isIdentical (e.getResultType()))
            return evaluateAsExpression (e, targetType.removeReference());

        if (auto list = cast<AST::CommaSeparatedList> (e))
        {
            if (targetType.isArrayOrVector() || targetType.isStruct())
            {
                auto& temp = builder.createMutableLocalVariable (targetType);
                initialiseArrayOrStructElements (temp, list->items, list->context);
                return temp;
            }

            SOUL_ASSERT_FALSE;
        }

        auto& resolved = evaluateAsExpression (e);
        const auto& resolvedType = resolved.getType();

        if (resolvedType.isIdentical (targetType))
            return resolved;

        if (targetType.isReference() && ! resolved.isMutable())
            e.context.throwError (Errors::cannotPassConstAsNonConstRef());

        auto constValue = resolved.getAsConstant();

        if (constValue.isValid() && TypeRules::canSilentlyCastTo (targetType, constValue))
            return module.allocate<heart::Constant> (e.context.location, constValue.castToTypeExpectingSuccess (targetType));

        if (! TypeRules::canSilentlyCastTo (targetType, resolvedType))
            e.context.throwError (Errors::expectedExpressionOfType (targetType.getDescription()));

        return builder.createCastIfNeeded (resolved, targetType);
    }

    heart::SubElement& createStructSubElement (AST::StructMemberRef& member, heart::Expression& source)
    {
        SOUL_ASSERT (member.index < getStructType (member).getStructRef().members.size());
        return builder.createFixedSubElement (source, member.index);
    }

    heart::SubElement& createArraySubElement (AST::ArrayElementRef& subscript, heart::Expression& source)
    {
        auto arrayOrVectorType = getArrayOrVectorType (subscript);

        if (arrayOrVectorType.isUnsizedArray() && subscript.isSlice)
            subscript.context.throwError (Errors::notYetImplemented ("Slices of dynamic arrays"));

        auto& result = builder.module.allocate<heart::SubElement> (subscript.context.location, source);
        result.suppressWrapWarning = subscript.suppressWrapWarning;

        if (subscript.isSlice)
        {
            auto range = subscript.getResolvedSliceRange();

            SOUL_ASSERT (arrayOrVectorType.isValidArrayOrVectorRange (range.start, range.end));
            result.fixedStartIndex = range.start;
            result.fixedEndIndex = range.end;
            result.isRangeTrusted = true;
            return result;
        }

        result.dynamicIndex = evaluateAsExpression (*subscript.startIndex);
        result.suppressWrapWarning = subscript.suppressWrapWarning;
        result.optimiseDynamicIndexIfPossible();
        return result;
    }

    void visit (AST::IfStatement& i) override
    {
        auto labelIndex = ifIndex++;

        auto& trueBlock   = builder.createBlock ("@if_", labelIndex);
        auto& falseBlock  = builder.createBlock ("@ifnot_", labelIndex);

        addBranchIf (*i.condition, trueBlock, falseBlock, trueBlock);

        visitAsStatement (i.trueBranch);

        if (i.falseBranch != nullptr)
        {
            auto& endBlock = builder.createBlock ("@ifend_", labelIndex);
            builder.addBranch (endBlock, falseBlock);
            visitAsStatement (i.falseBranch);
            builder.beginBlock (endBlock);
        }
        else
        {
            builder.beginBlock (falseBlock);
        }
    }

    void visit (AST::LoopStatement& l) override
    {
        auto labelIndex = loopIndex++;
        auto oldbreakTarget = breakTarget;
        auto oldcontinueTarget = continueTarget;
        auto& breakBlock    = builder.createBlock ("@break_", labelIndex);
        auto& continueBlock = builder.createBlock ("@cont_",  labelIndex);

        breakTarget = breakBlock;
        continueTarget = continueBlock;

        if (l.isDoLoop)
        {
            SOUL_ASSERT (l.iterator == nullptr);
            SOUL_ASSERT (l.condition != nullptr);
            builder.beginBlock (continueBlock);
            visitAsStatement (l.body);
            addBranchIf (*l.condition, continueBlock, breakBlock, breakBlock);
        }
        else
        {
            auto& startBlock = builder.createBlock ("@loop_", labelIndex);
            auto& bodyBlock  = builder.createBlock ("@body_", labelIndex);

            if (l.numIterations != nullptr)
            {
                SOUL_ASSERT (l.iterator == nullptr);
                SOUL_ASSERT (l.condition == nullptr);
                auto indexType = l.numIterations->getResultType();

                if (! indexType.isPrimitiveInteger())
                    l.numIterations->context.throwError (Errors::expectedInteger());

                if (indexType.isInteger64())
                {
                    if (auto constNumIterations = l.numIterations->getAsConstant())
                    {
                        auto num = constNumIterations->value.getAsInt64();

                        if (num <= 0x7fffffff)
                            indexType = PrimitiveType::int32;
                    }
                }

                auto& counterVar = builder.createMutableLocalVariable (indexType, "$counter_" + std::to_string (labelIndex));
                builder.addAssignment (counterVar, builder.createCastIfNeeded (evaluateAsExpression (*l.numIterations), indexType));

                builder.beginBlock (startBlock);
                auto& isCounterInRange = builder.createBinaryOp (l.context.location, counterVar,
                                                                 builder.createZeroInitialiser (indexType),
                                                                 BinaryOp::Op::greaterThan,
                                                                 PrimitiveType::bool_);
                builder.addBranchIf (isCounterInRange, bodyBlock, breakBlock, bodyBlock);
                visitAsStatement (l.body);
                builder.beginBlock (continueBlock);
                builder.decrementValue (counterVar);
            }
            else
            {
                builder.beginBlock (startBlock);

                if (l.condition == nullptr)
                    builder.addBranch (bodyBlock, bodyBlock);
                else if (auto c = l.condition->getAsConstant())
                    builder.addBranch (c->value.getAsBool() ? bodyBlock : breakBlock, bodyBlock);
                else
                    addBranchIf (*l.condition, bodyBlock, breakBlock, bodyBlock);

                visitAsStatement (l.body);
                builder.beginBlock (continueBlock);
                visitAsStatement (l.iterator);
            }

            builder.addBranch (startBlock, breakBlock);
        }

        breakTarget = oldbreakTarget;
        continueTarget = oldcontinueTarget;
    }

    void visit (AST::ReturnStatement& r) override
    {
        if (r.returnValue != nullptr)
            builder.addReturn (evaluateAsExpression (*r.returnValue, builder.currentFunction->returnType));
        else
            builder.addReturn();
    }

    void visit (AST::BreakStatement&) override
    {
        SOUL_ASSERT (breakTarget != nullptr);
        builder.addBranch (breakTarget, builder.createNewBlock());
    }

    void visit (AST::ContinueStatement&) override
    {
        SOUL_ASSERT (continueTarget != nullptr);
        builder.addBranch (continueTarget, builder.createNewBlock());
    }

    void visit (AST::TernaryOp& t) override
    {
        if (currentTargetVariable == nullptr)
            t.context.throwError (Errors::ternaryCannotBeStatement());

        auto& targetVar = *currentTargetVariable;
        auto labelIndex = ifIndex++;

        auto& trueBlock   = builder.createBlock ("@if_true_", labelIndex);
        auto& falseBlock  = builder.createBlock ("@if_false_", labelIndex);
        auto& endBlock    = builder.createBlock ("@if_end_", labelIndex);
        auto resultType  = t.getResultType();

        auto& tempVar = module.allocate<heart::Variable> (t.context.location, targetVar.getType(),
                                                          heart::Variable::Role::mutableLocal);
        builder.addZeroAssignment (tempVar);

        addBranchIf (*t.condition, trueBlock, falseBlock, trueBlock);
        visitWithDestination (tempVar, t.trueBranch);
        builder.addBranch (endBlock, falseBlock);
        visitWithDestination (tempVar, t.falseBranch);
        builder.beginBlock (endBlock);
        builder.addAssignment (targetVar, tempVar);
    }

    void visit (AST::Constant& o) override
    {
        if (currentTargetVariable != nullptr)
            builder.addAssignment (currentTargetVariable, o.value.castToTypeWithError (currentTargetVariable->getType(), o.context));
    }

    void visit (AST::VariableDeclaration& v) override
    {
        if (sourceGraph != nullptr)
            return;

        if (parsingStateVariables)
        {
            if (v.isExternal)
            {
                module.stateVariables.push_back (createVariableDeclaration (v, heart::Variable::Role::external));
            }
            else
            {
                const auto& type = v.getType();

                // Skip writing constant or unwritten-to variables to the state
                if (! (type.isConst() || (v.numWrites == 0 && (type.isPrimitive() || type.isBoundedInt()))))
                    module.stateVariables.push_back (createVariableDeclaration (v, heart::Variable::Role::state));
            }
        }
        else
        {
            auto& target = createVariableDeclaration (v, heart::Variable::Role::mutableLocal);

            if (v.initialValue != nullptr)
                visitWithDestination (target, v.initialValue);
            else
                builder.addZeroAssignment (target);
        }
    }

    void visit (AST::VariableRef& v) override
    {
        builder.addCastOrAssignment (currentTargetVariable, v.variable->getGeneratedVariable());
    }

    void createFunctionCall (const AST::FunctionCall& call, heart::VariablePtr targetVariable)
    {
        auto& fc = module.allocate<heart::FunctionCall> (targetVariable, call.targetFunction.generatedFunction);

        if (call.targetFunction.generatedFunction == nullptr)
            unresolvedFunctionCalls.push_back ({ fc, call.targetFunction });

        for (size_t i = 0; i < call.getNumArguments(); ++i)
        {
            auto paramType = call.targetFunction.parameters[i]->getType();
            auto& arg = *call.arguments->items[i];

            if (paramType.isReference())
                fc.arguments.push_back (getAsReference (arg, paramType.isConst()));
            else
                fc.arguments.push_back (evaluateAsExpression (arg, paramType));
        }

        builder.addStatement (fc);
    }

    void visit (AST::FunctionCall& call) override
    {
        if (currentTargetVariable != nullptr)
        {
            auto returnType = call.getResultType();
            auto targetType = currentTargetVariable->getType();

            if (! returnType.isIdentical (targetType))
            {
                auto& temp = builder.createRegisterVariable (returnType);
                createFunctionCall (call, temp);
                builder.addAssignment (currentTargetVariable, builder.createCast (call.context.location, temp, targetType));
                return;
            }
        }

        createFunctionCall (call, currentTargetVariable);
    }

    void visit (AST::TypeCast& c) override
    {
        if (c.getNumArguments() > 1)
            if (currentTargetVariable != nullptr && currentTargetVariable->isMutable())
                return initialiseArrayOrStructElements (*currentTargetVariable, c);

        createAssignmentToCurrentTarget (c);
    }

    void initialiseArrayOrStructElements (heart::Expression& target, ArrayView<AST::ExpPtr> list,
                                          const AST::Context& errorLocation)
    {
        const auto& targetType = target.getType();
        SOUL_ASSERT (targetType.isFixedSizeAggregate());
        SanityCheckPass::throwErrorIfWrongNumberOfElements (errorLocation, targetType, list.size());

        builder.addZeroAssignment (target);

        for (size_t i = 0; i < list.size(); ++i)
        {
            auto& v = *list[i];

            if (auto constElement = v.getAsConstant())
                if (constElement->value.isZero()) // no need to assign to elements which are zero
                    continue;

            createAssignment (builder.createFixedSubElement (target, i), v);
        }
    }

    void initialiseArrayOrStructElements (heart::Expression& target, const AST::TypeCast& tc)
    {
        SOUL_ASSERT (target.isMutable());

        if (auto list = cast<AST::CommaSeparatedList> (tc.source))
            initialiseArrayOrStructElements (target, list->items, tc.source->context);
        else
            initialiseArrayOrStructElements (target, { &tc.source, 1u }, tc.source->context);
    }

    heart::Variable& createAggregateWithInitialisers (const AST::TypeCast& tc)
    {
        auto& temp = builder.createMutableLocalVariable (tc.targetType);
        initialiseArrayOrStructElements (temp, tc);
        return temp;
    }

    void visit (AST::UnaryOperator& op) override
    {
        createAssignmentToCurrentTarget (op);
    }

    void visit (AST::BinaryOperator& op) override
    {
        createAssignmentToCurrentTarget (op);
    }

    void visit (AST::Assignment& o) override
    {
        createAssignment (getAsReference (*o.target, false), *o.newValue);
    }

    void visit (AST::ArrayElementRef& a) override
    {
        auto arrayOrVectorType = getArrayOrVectorType (a);
        auto& source = evaluateAsExpression (*a.object, arrayOrVectorType);

        if (a.isSlice)
        {
            auto sliceRange = a.getResolvedSliceRange();
            builder.addCastOrAssignment (currentTargetVariable,
                                         builder.createSubElementSlice (a.context.location,
                                                                        source, sliceRange.start, sliceRange.end));
            return;
        }

        auto& index = evaluateAsExpression (*a.startIndex);
        builder.addCastOrAssignment (currentTargetVariable,
                                     builder.createDynamicSubElement (a.context.location, source, index,
                                                                      false, a.suppressWrapWarning));
    }

    void visit (AST::StructMemberRef& a) override
    {
        auto structType = getStructType (a);
        auto& source = evaluateAsExpression (*a.object, structType);
        builder.addCastOrAssignment (currentTargetVariable, builder.createFixedSubElement (source, a.index));
    }

    void visit (AST::PreOrPostIncOrDec& p) override
    {
        auto resultDestVar = currentTargetVariable;
        auto op = p.isIncrement ? BinaryOp::Op::add
                                : BinaryOp::Op::subtract;

        auto& dest = getAsReference (*p.target, false);
        auto type = dest.getType().removeReferenceIfPresent();

        auto& oldValue = builder.createRegisterVariable (type);
        builder.addAssignment (oldValue, dest);
        auto& one = module.allocator.allocate<heart::Constant> (p.context.location, Value::createInt32 (1).castToTypeExpectingSuccess (type));
        auto& incrementedValue = builder.createBinaryOp (p.context.location, oldValue, one, op, type);

        if (resultDestVar == nullptr)
        {
            builder.addAssignment (dest, incrementedValue);
        }
        else if (p.isPost)
        {
            builder.addAssignment (dest, incrementedValue);
            builder.addAssignment (*resultDestVar, oldValue);
        }
        else
        {
            builder.addAssignment (*resultDestVar, incrementedValue);
            builder.addAssignment (dest, *resultDestVar);
        }
    }

    void visit (AST::AdvanceClock&) override
    {
        builder.addAdvance();
    }

    void createSeriesOfWrites (AST::Expression& target, ArrayView<AST::ExpPtr> values)
    {
        // Two choices - the target can be an output declaration, or an element of an output declaration
        if (auto output = cast<AST::OutputEndpointRef> (target))
        {
            for (auto v : values)
            {
                if (! output->output->supportsSampleType (*v))
                    target.context.throwError (Errors::cannotWriteTypeToEndpoint (v->getResultType().getDescription(),
                                                                                  output->output->getSampleTypesDescription()));

                auto sampleType = output->output->getSampleType (*v);

                builder.addWriteStream (*output->output->generatedOutput, nullptr,
                                        evaluateAsExpression (*v, sampleType));
            }

            return;
        }

        if (auto arraySubscript = cast<AST::ArrayElementRef> (target))
        {
            if (auto outputRef = cast<AST::OutputEndpointRef> (arraySubscript->object))
            {
                if (outputRef->output->arraySize == nullptr)
                    arraySubscript->context.throwError (Errors::cannotUseBracketsOnNonArrayEndpoint());

                for (auto v : values)
                {
                    // Find the element type that our expression will write to
                    auto sampleType = outputRef->output->getElementSampleType (*v);
                    auto& value = evaluateAsExpression (*v, sampleType);

                    if (arraySubscript->isSlice)
                    {
                        auto slice = arraySubscript->getResolvedSliceRange();

                        for (auto i = slice.start; i < slice.end; ++i)
                            builder.addWriteStream (*outputRef->output->generatedOutput, builder.createConstantInt32 (i), value);
                    }
                    else
                    {
                        auto& index = evaluateAsExpression (*arraySubscript->startIndex);
                        auto& context = arraySubscript->startIndex->context;
                        auto constIndex = index.getAsConstant();
                        auto arraySize = outputRef->output->generatedOutput->arraySize;

                        if (constIndex.isValid())
                        {
                            auto fixedIndex = TypeRules::checkAndGetArrayIndex (context, constIndex);
                            TypeRules::checkConstantArrayIndex (context, fixedIndex, (Type::ArraySize) arraySize);

                            builder.addWriteStream (*outputRef->output->generatedOutput, builder.createConstantInt32 (fixedIndex), value);
                        }
                        else
                        {
                            auto indexType = Type::createWrappedInt ((Type::BoundedIntSize) arraySize);
                            auto& wrappedIndex = builder.createCast (context.location, index, indexType);

                            builder.addWriteStream (*outputRef->output->generatedOutput, wrappedIndex, value);
                        }
                    }
                }

                return;
            }
        }

        target.context.throwError (Errors::targetMustBeOutput());
    }

    static AST::WriteToEndpoint& getTopLevelWriteToEndpoint (AST::WriteToEndpoint& ws, ArrayWithPreallocation<AST::ExpPtr, 4>& values)
    {
        values.insert (values.begin(), ws.value);

        if (auto chainedWrite = cast<AST::WriteToEndpoint> (ws.target))
            return getTopLevelWriteToEndpoint (*chainedWrite, values);

        return ws;
    }

    void visit (AST::WriteToEndpoint& ws) override
    {
        ArrayWithPreallocation<AST::ExpPtr, 4> values;
        auto& topLevelWrite = getTopLevelWriteToEndpoint (ws, values);
        createSeriesOfWrites (*topLevelWrite.target, values);
    }

    void visit (AST::OutputEndpointRef& o) override
    {
        o.context.throwError (Errors::cannotReadFromOutput());
    }

    void visit (AST::InputEndpointRef& i) override
    {
        if (currentTargetVariable != nullptr)
            builder.addReadStream (i.context.location, *currentTargetVariable, *i.input->generatedInput);
        else
            i.context.throwError (Errors::unusedExpression());
    }

    void visit (AST::ProcessorProperty& p) override
    {
        createAssignmentToCurrentTarget (p);
    }

    void visit (AST::QualifiedIdentifier&) override
    {
        SOUL_ASSERT_FALSE;
    }

    Type getStructType (AST::StructMemberRef& a)
    {
        auto structType = a.object->getResultType();

        if (! structType.isStruct())
            a.object->context.throwError (Errors::expectedStructForDotOperator());

        return structType;
    }

    Type getArrayOrVectorType (AST::ArrayElementRef& a)
    {
        auto arrayOrVectorType = a.object->getResultType();

        if (! arrayOrVectorType.isArrayOrVector())
            a.object->context.throwError (Errors::expectedArrayOrVectorForBracketOp());

        return arrayOrVectorType;
    }
};

} // namespace soul
