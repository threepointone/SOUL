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
    Runs multiple passes over the raw AST to attempt to resolve names into references
    to functions, variables, types, etc. and also does some constant and type folding
*/
struct ResolutionPass  final
{
    static void run (AST::Allocator& a, AST::ModuleBase& m, bool ignoreTypeAndConstantErrors)
    {
        ResolutionPass (a, m).run (ignoreTypeAndConstantErrors);
    }

private:
    ResolutionPass (AST::Allocator& a, AST::ModuleBase& m) : allocator (a), module (m)
    {
        intrinsicsNamespacePath = IdentifierPath::fromString (allocator.identifiers, getIntrinsicsNamespaceName());
    }

    AST::Allocator& allocator;
    AST::ModuleBase& module;
    IdentifierPath intrinsicsNamespacePath;

    struct RunStats
    {
        size_t numFailures = 0, numReplaced = 0;

        void clear()
        {
            numFailures = 0;
            numReplaced = 0;
        }

        void add (const RunStats& rhs)
        {
            numFailures += rhs.numFailures;
            numReplaced += rhs.numReplaced;
        }
    };

    RunStats run (bool ignoreTypeAndConstantErrors)
    {
        RunStats runStats;

        if (module.isFullyResolved)
            return runStats;

        for (;;)
        {
            runStats.clear();

            tryPass<QualifiedIdentifierResolver> (runStats, true);
            tryPass<TypeResolver> (runStats, true);
            tryPass<ConvertStreamOperations> (runStats, true);
            rebuildVariableUseCounts (module);
            tryPass<FunctionResolver> (runStats, true);
            tryPass<ConstantFolder> (runStats, true);
            rebuildVariableUseCounts (module);

            if (runStats.numReplaced == 0)
            {
                tryPass<GenericFunctionResolver> (runStats, true);
            }

            // Parse sub-modules too
            for (auto& subModule : module.getSubModules())
            {
                ResolutionPass resolutionPass (allocator, *subModule);
                runStats.add (resolutionPass.run (ignoreTypeAndConstantErrors));
            }

            if (runStats.numFailures == 0)
                break;

            if (runStats.numReplaced == 0)
            {
                // failed to resolve anything new, so can't get any further..
                if (ignoreTypeAndConstantErrors)
                    return runStats;

                tryPass<FunctionResolver> (runStats, false);
                tryPass<QualifiedIdentifierResolver> (runStats, false);
                tryPass<TypeResolver> (runStats, false);
                tryPass<ConvertStreamOperations> (runStats, false);
                tryPass<GenericFunctionResolver> (runStats, false);
                break;
            }
        }

        FullResolver (*this).visitObject (module);
        module.isFullyResolved = true;
        return runStats;
    }

    template <typename PassType>
    void tryPass (RunStats& runStats, bool ignoreErrors)
    {
        PassType pass (*this, ignoreErrors);
        pass.performPass();
        runStats.numFailures += pass.numFails;
        runStats.numReplaced += pass.itemsReplaced;
    }

    //==============================================================================
    struct ErrorIgnoringRewritingASTVisitor  : public RewritingASTVisitor
    {
        ErrorIgnoringRewritingASTVisitor (ResolutionPass& rp, bool shouldIgnoreErrors)
            : owner (rp), allocator (rp.allocator), module (rp.module), ignoreErrors (shouldIgnoreErrors) {}

        void performPass()   { this->visitObject (module); }

        using super = ErrorIgnoringRewritingASTVisitor;

        using RewritingASTVisitor::visit;

        AST::StaticAssertionPtr visit (AST::StaticAssertion& a) override
        {
            RewritingASTVisitor::visit (a);
            a.testAndThrowErrorOnFailure();
            return a;
        }

        ResolutionPass& owner;
        AST::Allocator& allocator;
        AST::ModuleBase& module;
        size_t numFails = 0;
        const bool ignoreErrors;
    };

    //==============================================================================
    static void rebuildVariableUseCounts (AST::ModuleBase& module)
    {
        struct UseCountResetter  : public ASTVisitor
        {
            void visit (AST::VariableDeclaration& v) override
            {
                ASTVisitor::visit (v);
                v.numReads = 0;
                v.numWrites = 0;
            }
        };

        struct UseCounter  : public ASTVisitor
        {
            void visit (AST::Assignment& a) override
            {
                auto oldWriting = isWriting;
                auto oldReading = isReading;
                isReading = false;
                isWriting = true;
                visitObject (*a.target);
                isWriting = oldWriting;
                isReading = oldReading;
                visitObject (*a.newValue);
            }

            void visit (AST::PreOrPostIncOrDec& p) override
            {
                auto oldWriting = isWriting;
                auto oldReading = isReading;
                isReading = true;
                isWriting = true;
                ASTVisitor::visit (p);
                isWriting = oldWriting;
                isReading = oldReading;
            }

            void visit (AST::VariableRef& v) override
            {
                ASTVisitor::visit (v);

                if (isWriting)
                    v.variable->numWrites++;
                else
                    v.variable->numReads++;
            }

            void visit (AST::CallOrCast& c) override
            {
                if (c.arguments != nullptr)
                {
                    // Since we don't know if this might be a function with all pass-by-ref args, we need
                    // to mark all the args as possibly being written..
                    auto oldWriting = isWriting;
                    isWriting = true;
                    ASTVisitor::visit (c);
                    isWriting = oldWriting;
                }
            }

            void visit (AST::FunctionCall& c) override
            {
                if (c.arguments != nullptr)
                {
                    SOUL_ASSERT (c.arguments->items.size() == c.targetFunction.parameters.size());

                    // Visit the function arguments, marking them as writing if the function parameter is pass by reference
                    for (size_t i = 0; i < c.arguments->items.size(); ++i)
                    {
                        auto param = c.targetFunction.parameters[i];
                        auto oldWriting = isWriting;
                        isWriting = param->isResolved() ? param->getType().isReference() : true;
                        visitObject (*(c.arguments->items[i]));
                        isWriting = oldWriting;
                    }
                }
            }

            bool isReading = true, isWriting = false;
        };

        UseCountResetter resetter;
        UseCounter counter;
        resetter.visitObject (module);
        counter.visitObject (module);
    }

    static AST::TypeCastPtr convertToCast (AST::Allocator& a, AST::CallOrCast& call, Type destType)
    {
        SOUL_ASSERT (! call.isMethodCall);

        if (auto list = cast<AST::CommaSeparatedList> (call.arguments))
            if (list->items.size() == 1)
                return a.allocate<AST::TypeCast> (call.context, std::move (destType), list->items.front());

        return a.allocate<AST::TypeCast> (call.context, std::move (destType), call.arguments);
    }

    //==============================================================================
    struct ConvertStreamOperations  : public ErrorIgnoringRewritingASTVisitor
    {
        static inline constexpr const char* getPassName()  { return "ConvertStreamOperations"; }
        using super::visit;

        ConvertStreamOperations (ResolutionPass& rp, bool shouldIgnoreErrors)
            : super (rp, shouldIgnoreErrors) {}

        AST::ExpPtr visit (AST::BinaryOperator& o) override
        {
            super::visit (o);

            if (o.isOutputEndpoint())
            {
                auto w = allocator.allocate<AST::WriteToEndpoint> (o.context, o.lhs, o.rhs);
                visitObject (*w);
                return w;
            }

            return o;
        }
    };

    //==============================================================================
    struct QualifiedIdentifierResolver  : public ErrorIgnoringRewritingASTVisitor
    {
        static inline constexpr const char* getPassName()  { return "QualifiedIdentifierResolver"; }
        using super::visit;

        QualifiedIdentifierResolver (ResolutionPass& rp, bool shouldIgnoreErrors)
            : super (rp, shouldIgnoreErrors) {}

        void performPass()
        {
            super::performPass();

            if (numVariablesResolved > 0)
            {
                struct RecursiveVariableInitialiserCheck  : public ASTVisitor
                {
                    void visit (AST::VariableDeclaration& v) override
                    {
                        if (contains (stack, AST::VariableDeclarationPtr (v)))
                            v.context.throwError (Errors::initialiserRefersToTarget (v.name));

                        if (v.initialValue != nullptr)
                            stack.push_back (v);

                        ASTVisitor::visit (v);

                        if (v.initialValue != nullptr)
                            stack.pop_back();
                    }

                    void visit (AST::VariableRef& vr) override
                    {
                        if (vr.variable != nullptr)
                            visit (*vr.variable);
                    }

                    std::vector<AST::VariableDeclarationPtr> stack;
                };

                RecursiveVariableInitialiserCheck().visitObject (module);
            }
        }

        AST::BlockPtr visit (AST::Block& b) override
        {
            auto oldStatement = currentStatement;

            for (auto& s : b.statements)
            {
                currentStatement = s;
                replaceStatement (s);
            }

            currentStatement = oldStatement;
            return b;
        }

        AST::ExpPtr visit (AST::QualifiedIdentifier& qi) override
        {
            AST::Scope::NameSearch search;
            search.partiallyQualifiedPath = qi.path;
            search.stopAtFirstScopeWithResults = true;
            search.findVariables = true;
            search.findTypes = true;
            search.findFunctions = false;
            search.findProcessorsAndNamespaces = true;
            search.findEndpoints = true;

            if (auto scope = qi.getParentScope())
                scope->performFullNameSearch (search, currentStatement.get());

            if (search.itemsFound.size() == 1)
            {
                auto item = search.itemsFound.front();

                if (auto e = cast<AST::Expression> (item))
                    return e;

                if (auto v = cast<AST::VariableDeclaration> (item))
                {
                    ++numVariablesResolved;
                    return allocator.allocate<AST::VariableRef> (qi.context, v);
                }

                if (auto p = cast<AST::Processor> (item))
                    return allocator.allocate<AST::ProcessorRef> (qi.context, *p);

                if (auto pa = cast<AST::ProcessorAliasDeclaration> (item))
                    if (pa->targetProcessor != nullptr)
                        return allocator.allocate<AST::ProcessorRef> (qi.context, *pa->targetProcessor);

                if (auto e = cast<AST::InputDeclaration> (item))
                    return allocator.allocate<AST::InputEndpointRef> (qi.context, e);

                if (auto e = cast<AST::OutputDeclaration> (item))
                    return allocator.allocate<AST::OutputEndpointRef> (qi.context, e);
            }

            if (auto builtInConstant = getBuiltInConstant (qi))
                return builtInConstant;

            if (! ignoreErrors)
            {
                if (qi.path.isUnqualifiedName ("wrap") || qi.path.isUnqualifiedName ("clamp"))
                    return qi;

                if (search.itemsFound.empty())      qi.context.throwError (Errors::unresolvedSymbol (qi.path));
                if (search.itemsFound.size() > 1)   qi.context.throwError (Errors::ambiguousSymbol (qi.path));
            }

            if (ignoreErrors)
                ++numFails;
            else
                qi.context.throwError (Errors::unresolvedSymbol (qi.path));

            return qi;
        }

        AST::FunctionPtr visit (AST::Function& f) override
        {
            if (! f.isGeneric())
                return super::visit (f);

            return f;
        }

        AST::ExpPtr visit (AST::CallOrCast& call) override
        {
            if (call.arguments != nullptr)
                visitObject (*call.arguments);

            if (call.areAllArgumentsResolved())
            {
                if (AST::isResolvedAsType (call.nameOrType))
                    return convertToCast (allocator, call, call.nameOrType->resolveAsType());

                if (auto name = cast<AST::QualifiedIdentifier> (call.nameOrType))
                {
                    AST::Scope::NameSearch search;
                    search.partiallyQualifiedPath = name->path;
                    search.stopAtFirstScopeWithResults = true;
                    search.findVariables = false;
                    search.findTypes = true;
                    search.findFunctions = false;
                    search.findProcessorsAndNamespaces = false;
                    search.findEndpoints = false;

                    if (auto scope = name->getParentScope())
                        scope->performFullNameSearch (search, currentStatement.get());

                    if (search.itemsFound.size() == 1)
                    {
                        if (auto e = cast<AST::Expression> (search.itemsFound.front()))
                        {
                            if (AST::isResolvedAsType (e))
                            {
                                if (auto list = cast<AST::CommaSeparatedList> (call.arguments))
                                    if (list->items.size() == 1)
                                        return allocator.allocate<AST::TypeCast> (call.context, e->resolveAsType(), list->items.front());

                                return allocator.allocate<AST::TypeCast> (call.context, e->resolveAsType(), call.arguments);
                            }
                        }
                    }
                }
                else
                {
                    replaceExpression (call.nameOrType);
                }
            }

            return call;
        }

        AST::ExpPtr visit (AST::ArrayElementRef& s) override
        {
            auto result = super::visit (s);

            if (s.isResolved())
                SanityCheckPass::checkArraySubscript (s);

            return result;
        }

        AST::ExpPtr createSizeForType (const AST::Context& c, const Type& type)
        {
            uint64_t size = 0;

            if (type.isFixedSizeArray() || type.isVector())
                size = type.getArrayOrVectorSize();
            else if (type.isBoundedInt())
                size = (uint64_t) type.getBoundedIntLimit();

            if (size == 0)
            {
                if (! ignoreErrors)
                    c.throwError (Errors::cannotTakeSizeOfType());

                return {};
            }

            return allocator.allocate<AST::Constant> (c, size > 0x7fffffffu ? Value::createInt64 (size)
                                                                            : Value::createInt32 (size));
        }

        AST::ExpPtr createTypeMetaFunction (AST::QualifiedIdentifier& name, AST::Expression& arg)
        {
            if (name.path.isUnqualified())
            {
                auto op = AST::TypeMetaFunction::getOperationForName (name.path.getFirstPart());

                if (op != AST::TypeMetaFunction::Op::none)
                    return allocator.allocate<AST::TypeMetaFunction> (name.context, arg, op);
            }

            return {};
        }

        AST::ExpPtr visit (AST::DotOperator& d) override
        {
            auto result = super::visit (d);

            if (result != d)
                return result;

            if (AST::isResolvedAsType (d.lhs))
            {
                if (d.rhs->path.isUnqualified())
                {
                    auto lhsType = d.lhs->resolveAsType();

                    if (auto metaFunction = createTypeMetaFunction (*d.rhs, *d.lhs))
                        return metaFunction;
                }
            }
            else if (AST::isResolvedAsValue (d.lhs))
            {
                auto lhsType = d.lhs->getResultType();

                if (lhsType.isStruct())
                {
                    auto& s = lhsType.getStructRef();

                    for (size_t i = 0; i < s.members.size(); ++i)
                        if (d.rhs->path.isUnqualifiedName (s.members[i].name))
                            return allocator.allocate<AST::StructMemberRef> (d.context, d.lhs, s, i);

                    if (! ignoreErrors)
                        d.rhs->context.throwError (Errors::unknownMemberInStruct (d.rhs->toString(), s.name));
                }

                if (d.rhs->path.isUnqualified())
                    if (auto metaFunction = createTypeMetaFunction (*d.rhs, *d.lhs))
                        return metaFunction;
            }
            else if (d.lhs->isOutputEndpoint())
            {
                d.context.throwError (Errors::noSuchOperationOnEndpoint());
            }
            else if (AST::isResolvedAsProcessor (d.lhs))
            {
                d.context.throwError (Errors::noSuchOperationOnProcessor());
            }

            if (ignoreErrors)
                ++numFails;
            else
                d.context.throwError (Errors::invalidDotArguments());

            return d;
        }

        AST::ConstantPtr getBuiltInConstant (AST::QualifiedIdentifier& u)
        {
            if (u.path.isUnqualifiedName ("pi"))     return allocator.allocate<AST::Constant> (u.context, Value (pi));
            if (u.path.isUnqualifiedName ("twoPi"))  return allocator.allocate<AST::Constant> (u.context, Value (twoPi));
            if (u.path.isUnqualifiedName ("nan"))    return allocator.allocate<AST::Constant> (u.context, Value (std::numeric_limits<float>::quiet_NaN()));
            if (u.path.isUnqualifiedName ("inf"))    return allocator.allocate<AST::Constant> (u.context, Value (std::numeric_limits<float>::infinity()));

            return {};
        }

        AST::StatementPtr currentStatement;
        uint32_t numVariablesResolved = 0;
    };

    //==============================================================================
    struct ConstantFolder  : public ErrorIgnoringRewritingASTVisitor
    {
        static inline constexpr const char* getPassName()  { return "ConstantFolder"; }
        using super::visit;

        ConstantFolder (ResolutionPass& rp, bool shouldIgnoreErrors)
            : super (rp, shouldIgnoreErrors) { SOUL_ASSERT (shouldIgnoreErrors); }

        bool isUsedAsReference = false;

        bool failIfNotResolved (AST::ExpPtr e)
        {
            if (e->isResolved())
                return false;

            ++numFails;
            return true;
        }

        AST::ExpPtr createConstant (const AST::Context& c, Value v)
        {
            return allocator.allocate<AST::Constant> (c, std::move (v));
        }

        AST::ExpPtr visitExpression (AST::ExpPtr e) override
        {
            if (e == nullptr)
                return {};

            e = super::visitExpression (e);

            if (e->isResolved())
            {
                if (isUsedAsReference)
                    return e;

                if (auto c = e->getAsConstant())
                {
                    if (c != e)
                        return createConstant (e->context, c->value);

                    return c;
                }

                return e;
            }

            ++numFails;
            return e;
        }

        AST::ExpPtr visit (AST::VariableRef& v) override
        {
            auto e = super::visit (v);

            if (failIfNotResolved (e))
                return e;

            if (v.variable->numWrites == 0 && v.variable->initialValue != nullptr)
            {
                if (failIfNotResolved (v.variable->initialValue))
                    return e;

                if (auto resolvedInitialiser = visitExpression (v.variable->initialValue))
                {
                    if (auto c = resolvedInitialiser->getAsConstant())
                    {
                        auto t = c->getResultType();

                        if (! t.isArray())   // arrays don't work as constants in LLVM
                        {
                            auto variableResolvedType = v.getResultType();

                            if (t.isIdentical (variableResolvedType))
                                return createConstant (v.context, c->value);

                            if (c->canSilentlyCastTo (variableResolvedType))
                                return createConstant (v.context, c->value.castToTypeExpectingSuccess (variableResolvedType));
                        }
                    }
                }
            }

            return e;
        }

        AST::ExpPtr visit (AST::TernaryOp& t) override
        {
            auto e = super::visit (t);

            if (failIfNotResolved (e))
                return e;

            if (auto te = cast<AST::TernaryOp> (e))
                if (auto constant = te->condition->getAsConstant())
                    return constant->value.getAsBool() ? te->trueBranch : te->falseBranch;

            return e;
        }

        AST::ExpPtr visit (AST::FunctionCall& c) override
        {
            if (c.getNumArguments() != 0)
            {
                auto parameters = c.targetFunction.parameters.begin();
                auto savedIsUsedAsReference = isUsedAsReference;

                for (auto& a : c.arguments->items)
                {
                    auto param = *parameters++;

                    if (param->isResolved())
                    {
                        auto paramType = param->getType();
                        isUsedAsReference = paramType.isReference();

                        if (isUsedAsReference && paramType.isNonConstReference()
                             && AST::isResolvedAsValue (a) && ! a->isAssignable())
                            a->context.throwError (Errors::cannotPassConstAsNonConstRef());

                        replaceExpression (a);
                    }
                }

                isUsedAsReference = savedIsUsedAsReference;

                if (c.targetFunction.intrinsic != IntrinsicType::none)
                {
                    ArrayWithPreallocation<Value, 4> constantArgs;

                    if (c.arguments != nullptr)
                    {
                        for (auto& arg : c.arguments->items)
                        {
                            if (auto constant = arg->getAsConstant())
                                constantArgs.emplace_back (constant->value);
                            else
                                break;
                        }
                    }

                    if (constantArgs.size() == c.arguments->items.size())
                    {
                        auto result = performIntrinsic (c.targetFunction.intrinsic, constantArgs);

                        if (result.isValid())
                            return createConstant (c.context, std::move (result));
                    }
                }
            }

            failIfNotResolved (c);
            return c;
        }

        AST::ExpPtr visit (AST::TypeCast& c) override
        {
            super::visit (c);

            if (failIfNotResolved (c))
                return c;

            if (c.getNumArguments() == 0)
                return createConstant (c.context, Value::zeroInitialiser (c.targetType));

            if (auto list = cast<AST::CommaSeparatedList> (c.source))
            {
                auto numArgs = TypeRules::checkArraySizeAndThrowErrorIfIllegal (c.context, list->items.size());

                ArrayWithPreallocation<AST::ConstantPtr, 8> constants;
                constants.reserve (numArgs);

                for (auto& v : list->items)
                {
                    if (auto cv = v->getAsConstant())
                        constants.push_back (cv);
                    else
                        return c;
                }

                if (numArgs == 1 && TypeRules::canCastTo (c.targetType, constants.front()->value.getType()))
                    return allocator.allocate<AST::Constant> (c.context, constants.front()->value.castToTypeExpectingSuccess (c.targetType));

                if (c.targetType.isArrayOrVector())
                {
                    auto elementType = c.targetType.getElementType();

                    ArrayWithPreallocation<Value, 8> elementValues;
                    elementValues.reserve (numArgs);

                    for (auto& cv : constants)
                    {
                        if (TypeRules::canCastTo (elementType, cv->value.getType()))
                            elementValues.push_back (cv->value.castToTypeExpectingSuccess (elementType));
                        else
                            return c;
                    }

                    if (c.targetType.isUnsizedArray())
                        return allocator.allocate<AST::Constant> (c.context, Value::createArrayOrVector (c.targetType.createCopyWithNewArraySize (numArgs),
                                                                                                         elementValues));

                    if (numArgs > 1)
                        SanityCheckPass::throwErrorIfWrongNumberOfElements (c.context, c.targetType, numArgs);

                    return allocator.allocate<AST::Constant> (c.context, Value::createArrayOrVector (c.targetType, elementValues));
                }

                if (c.targetType.isStruct())
                {
                    auto& s = c.targetType.getStructRef();

                    if (numArgs > 1)
                        SanityCheckPass::throwErrorIfWrongNumberOfElements (c.context, c.targetType, numArgs);

                    ArrayWithPreallocation<Value, 8> memberValues;
                    memberValues.reserve (s.members.size());

                    for (size_t i = 0; i < constants.size(); ++i)
                    {
                        auto memberType = s.members[i].type;
                        auto& cv = constants[i]->value;

                        if (TypeRules::canSilentlyCastTo (memberType, cv.getType()))
                            memberValues.push_back (cv.castToTypeExpectingSuccess (memberType));
                        else if (! ignoreErrors)
                            SanityCheckPass::expectSilentCastPossible (constants[i]->context, memberType, *constants[i]);
                        else
                            return c;
                    }

                    return allocator.allocate<AST::Constant> (c.context, Value::createStruct (s, memberValues));
                }

                if (numArgs > 1)
                    c.context.throwError (Errors::wrongTypeForInitialiseList());

                return c;
            }

            if (AST::isResolvedAsValue (c.source) && c.source->getResultType().isIdentical (c.targetType))
                return c.source;

            if (auto cv = c.source->getAsConstant())
                if (TypeRules::canSilentlyCastTo (c.targetType, cv->value))
                    return allocator.allocate<AST::Constant> (c.context, cv->value.castToTypeExpectingSuccess (c.targetType));

            return c;
        }

        AST::ExpPtr visit (AST::UnaryOperator& o) override
        {
            auto e = super::visit (o);

            if (failIfNotResolved (e))
                return e;

            if (auto u = cast<AST::UnaryOperator> (e))
            {
                if (auto constant = u->source->getAsConstant())
                {
                    auto result = constant->value;

                    if (UnaryOp::apply (result, u->operation))
                        return createConstant (u->source->context, std::move (result));
                }
            }

            return e;
        }

        AST::ExpPtr visit (AST::BinaryOperator& b) override
        {
            super::visit (b);

            if (failIfNotResolved (b))
                return b;

            SanityCheckPass::throwErrorIfNotReadableValue (b.rhs);

            if (b.isOutputEndpoint())
            {
                ++numFails;
                return b;
            }

            SanityCheckPass::throwErrorIfNotReadableValue (b.lhs);
            auto resultType = b.getOperandType();

            if (resultType.isValid())
            {
                if (auto lhsConst = b.lhs->getAsConstant())
                {
                    if (auto rhsConst = b.rhs->getAsConstant())
                    {
                        auto result = lhsConst->value;

                        if (BinaryOp::apply (result, rhsConst->value, b.operation,
                                             [&] (CompileMessage message) { b.context.throwError (message); }))
                            return createConstant (b.context, std::move (result));
                    }
                }
            }

            return b;
        }
    };

    //==============================================================================
    struct TypeResolver  : public ErrorIgnoringRewritingASTVisitor
    {
        static inline constexpr const char* getPassName()  { return "TypeResolver"; }
        using super::visit;

        TypeResolver (ResolutionPass& rp, bool shouldIgnoreErrors)
            : super (rp, shouldIgnoreErrors) {}

        void performPass() { visitObject (module); }

        AST::ExpPtr visit (AST::TypeCast& c) override
        {
            super::visit (c);

            if (c.targetType.isUnsizedArray())
            {
                auto numArgs = c.getNumArguments();

                if (c.source->isCompileTimeConstant())
                {
                    auto castValue = c.source->getAsConstant()->value
                                        .tryCastToType (c.targetType.createCopyWithNewArraySize (1));

                    if (castValue.isValid())
                        return allocator.allocate<AST::Constant> (c.source->context, castValue);
                }

                if (numArgs > 1)
                {
                    c.targetType.resolveUnsizedArraySize (numArgs);
                    ++itemsReplaced;
                }
            }

            return c;
        }

        AST::ExpPtr visit (AST::SubscriptWithBrackets& s) override
        {
            super::visit (s);

            if (AST::isResolvedAsValue (s.lhs))
                return allocator.allocate<AST::ArrayElementRef> (s.context, s.lhs, s.rhs, nullptr, false);

            if (AST::isResolvedAsType (s.lhs))
            {
                if (s.rhs == nullptr)
                    return allocator.allocate<AST::ConcreteType> (s.lhs->context,
                                                                  s.lhs->resolveAsType().createUnsizedArray());

                if (AST::isResolvedAsValue (s.rhs))
                {
                    if (s.rhs->isCompileTimeConstant())
                    {
                        if (auto constant = s.rhs->getAsConstant())
                        {
                            auto size = TypeRules::checkAndGetArraySize (s.rhs->context, constant->value);
                            auto elementType = s.lhs->resolveAsType();

                            if (! elementType.canBeArrayElementType())
                                s.lhs->context.throwError (Errors::wrongTypeForArrayElement());

                            return allocator.allocate<AST::ConcreteType> (s.lhs->context, elementType.createArray (size));
                        }
                    }

                    if (! ignoreErrors)
                        s.context.throwError (Errors::arraySizeMustBeConstant());
                }
            }

            if (AST::isResolvedAsEndpoint (s.lhs))
                return allocator.allocate<AST::ArrayElementRef> (s.context, s.lhs, s.rhs, nullptr, false);

            if (ignoreErrors)
                ++numFails;
            else if (AST::isResolvedAsProcessor (s.lhs))
                s.context.throwError (Errors::arraySuffixOnProcessor());
            else
                s.context.throwError (Errors::cannotResolveBracketedExp());

            return s;
        }

        AST::ExpPtr visit (AST::SubscriptWithChevrons& s) override
        {
            super::visit (s);

            if (AST::isResolvedAsType (s.lhs))
            {
                auto type = s.lhs->resolveAsType();

                if (! type.canBeVectorElementType())
                    s.lhs->context.throwError (Errors::wrongTypeForVectorElement());

                if (AST::isResolvedAsValue (s.rhs))
                {
                    if (auto constant = s.rhs->getAsConstant())
                    {
                        auto size = TypeRules::checkAndGetArraySize (s.rhs->context, constant->value);

                        if (! Type::isLegalVectorSize ((int64_t) size))
                            s.rhs->context.throwError (Errors::illegalVectorSize());

                        auto vectorSize = static_cast<Type::ArraySize> (size);
                        return allocator.allocate<AST::ConcreteType> (s.lhs->context, Type::createVector (type.getPrimitiveType(), vectorSize));
                    }
                }
            }

            if (auto name = cast<AST::QualifiedIdentifier> (s.lhs))
            {
                bool isWrap  = name->path.isUnqualifiedName ("wrap");
                bool isClamp = name->path.isUnqualifiedName ("clamp");

                if (isWrap || isClamp)
                {
                    if (AST::isResolvedAsValue (s.rhs))
                    {
                        if (auto constant = s.rhs->getAsConstant())
                        {
                            auto size = TypeRules::checkAndGetArraySize (s.rhs->context, constant->value);

                            if (! Type::isLegalBoundedIntSize (size))
                                s.rhs->context.throwError (Errors::illegalSize());

                            auto boundingSize = static_cast<Type::BoundedIntSize> (size);

                            return allocator.allocate<AST::ConcreteType> (s.lhs->context,
                                                                          isWrap ? Type::createWrappedInt (boundingSize)
                                                                                 : Type::createClampedInt (boundingSize));
                        }
                        else
                        {
                            if (! ignoreErrors)
                                s.context.throwError (Errors::wrapOrClampSizeMustBeConstant());
                        }
                    }
                }
            }

            if (ignoreErrors)
                ++numFails;
            else
                s.context.throwError (Errors::cannotResolveVectorSize());

            return s;
        }

        AST::ExpPtr visit (AST::TypeMetaFunction& c) override
        {
            super::visit (c);

            if (AST::isResolvedAsType (c))
                return allocator.allocate<AST::ConcreteType> (c.context, c.resolveAsType());

            if (AST::isResolvedAsValue (c))
                return allocator.allocate<AST::Constant> (c.context, c.getResultValue());

            if (c.isSizeOfUnsizedType())
            {
                auto argList = allocator.allocate<AST::CommaSeparatedList> (c.context);
                argList->items.push_back (c.source);

                auto name = allocator.identifiers.get ("get_array_size");
                auto qi = allocator.allocate<AST::QualifiedIdentifier> (c.context, IdentifierPath (name));
                return allocator.allocate<AST::CallOrCast> (qi, argList, true);
            }

            if (ignoreErrors)
            {
                ++numFails;
            }
            else
            {
                c.throwErrorIfUnresolved();
                c.context.throwError (Errors::cannotResolveSourceType());
            }

            return c;
        }

        AST::ExpPtr visit (AST::ArrayElementRef& s) override
        {
            super::visit (s);

            if (! ignoreErrors)
                SanityCheckPass::checkArraySubscript (s);

            return s;
        }

        AST::FunctionPtr visit (AST::Function& f) override
        {
            if (f.isGeneric())
                return f;

            return super::visit (f);
        }

        AST::StructDeclarationPtr visit (AST::StructDeclaration& s) override
        {
            recursiveTypeDeclVisitStack.push (s);
            auto e = super::visit (s);
            recursiveTypeDeclVisitStack.pop();
            return e;
        }

        AST::UsingDeclarationPtr visit (AST::UsingDeclaration& u) override
        {
            recursiveTypeDeclVisitStack.push (u);
            auto e = super::visit (u);
            recursiveTypeDeclVisitStack.pop();
            return e;
        }

        AST::StatementPtr visit (AST::VariableDeclaration& v) override
        {
            super::visit (v);

            if (v.initialValue != nullptr && ! v.isResolved())
            {
                if (AST::isResolvedAsType (v.declaredType))
                {
                    auto destType = v.declaredType->resolveAsType();

                    if (destType.isUnsizedArray())
                    {
                        if (auto size = findSizeOfArray (v.initialValue))
                            resolveVariableDeclarationInitialValue (v, destType.createCopyWithNewArraySize (size));
                    }
                    else
                    {
                        resolveVariableDeclarationInitialValue (v, destType);
                    }
                }
                else if (v.declaredType == nullptr)
                {
                    if (AST::isResolvedAsValue (v.initialValue))
                    {
                        auto type = v.initialValue->getResultType();

                        if (type.isUnsizedArray())
                        {
                            if (auto size = findSizeOfArray (v.initialValue))
                                resolveVariableDeclarationInitialValue (v, type.createCopyWithNewArraySize (size));
                            else
                                resolveVariableDeclarationInitialValue (v, type.createCopyWithNewArraySize (1));
                        }
                    }
                    else if (AST::isResolvedAsType (v.initialValue))
                    {
                        v.initialValue->context.throwError (Errors::expectedValue());
                    }
                }
            }

            return v;
        }

        AST::ExpPtr visit (AST::BinaryOperator& b) override
        {
            super::visit (b);

            if (b.isResolved())
            {
                SanityCheckPass::throwErrorIfNotReadableValue (b.rhs);

                if (b.isOutputEndpoint())
                {
                    ++numFails;
                    return b;
                }

                SanityCheckPass::throwErrorIfNotReadableValue (b.lhs);
                auto resultType = b.getOperandType();

                if (! resultType.isValid() && ! ignoreErrors)
                    b.context.throwError (Errors::illegalTypesForBinaryOperator (getSymbol (b.operation),
                                                                                 b.lhs->getResultType().getDescription(),
                                                                                 b.rhs->getResultType().getDescription()));
            }

            return b;
        }

        Type::ArraySize findSizeOfArray (AST::ExpPtr value)
        {
            if (value != nullptr)
            {
                if (AST::isResolvedAsValue (value))
                {
                    auto type = value->getResultType();

                    if (type.isFixedSizeArray())
                        return type.getArraySize();
                }

                if (auto list = cast<AST::CommaSeparatedList> (value))
                    return TypeRules::checkArraySizeAndThrowErrorIfIllegal (value->context, list->items.size());

                if (auto c = cast<AST::TypeCast> (value))
                {
                    if (c->targetType.isFixedSizeArray())
                        return c->targetType.getArraySize();

                    if (c->targetType.isUnsizedArray())
                        return findSizeOfArray (c->source);
                }

                if (auto call = cast<AST::CallOrCast> (value))
                {
                    if (AST::isResolvedAsType (call->nameOrType))
                    {
                        auto type = call->nameOrType->resolveAsType();

                        if (type.isFixedSizeArray())
                            return type.getArraySize();
                    }
                }
            }

            return 0;
        }

        void resolveVariableDeclarationInitialValue (AST::VariableDeclaration& v, const Type& type)
        {
            if (! (AST::isResolvedAsValue (v.initialValue) && v.initialValue->getResultType().isIdentical (type)))
                v.initialValue = allocator.allocate<AST::TypeCast> (v.initialValue->context, type, v.initialValue);

            v.declaredType = {};
            ++itemsReplaced;
        }

        SanityCheckPass::RecursiveTypeDeclVisitStack recursiveTypeDeclVisitStack;
    };

    //==============================================================================
    struct FunctionResolver  : public ErrorIgnoringRewritingASTVisitor
    {
        static inline constexpr const char* getPassName()  { return "FunctionResolver"; }
        using super::visit;

        FunctionResolver (ResolutionPass& rp, bool shouldIgnoreErrors)
            : super (rp, shouldIgnoreErrors) {}

        AST::ExpPtr visit (AST::CallOrCast& call) override
        {
            super::visit (call);

            if (AST::isResolvedAsType (call.nameOrType))
                return convertToCast (allocator, call, call.nameOrType->resolveAsType());

            if (call.areAllArgumentsResolved())
            {
                if (auto name = cast<AST::QualifiedIdentifier> (call.nameOrType))
                {
                    if (name->path.isUnqualifiedName ("advance"))
                        return createAdvanceCall (call);

                    if (name->path.isUnqualifiedName ("static_assert"))
                        return createStaticAssert (call);

                    if (name->path.isUnqualifiedName ("at"))
                        if (auto atCall = createAtCall (call))
                            return atCall;

                    if (call.arguments != nullptr)
                    {
                        for (auto& arg : call.arguments->items)
                        {
                            if (! AST::isResolvedAsValue (arg))
                            {
                                if (ignoreErrors)
                                    return call;

                                SanityCheckPass::throwErrorIfNotReadableValue (arg);
                            }
                        }
                    }

                    auto possibles = findAllPossibleFunctions (call, *name);
                    auto totalMatches = possibles.size();

                    // If there's only one function found, and we can call it (maybe with a cast), then go for it..
                    if (totalMatches == 1 && ! possibles.front().isImpossible)
                    {
                        if (auto resolved = resolveFunction (possibles.front(), call, ignoreErrors))
                            return resolved;

                        return call;
                    }

                    auto exactMatches = countNumberOfExactMatches (possibles);

                    // If there's one exact match, then even if there are others requiring casts, we'll ignore them
                    // and go for the one which is a perfect match..
                    if (exactMatches == 1)
                    {
                        for (auto& f : possibles)
                        {
                            if (f.isExactMatch())
                            {
                                if (auto resolved = resolveFunction (f, call, ignoreErrors))
                                    return resolved;

                                return call;
                            }
                        }

                        SOUL_ASSERT_FALSE;
                    }

                    // If there are any generic functions, see if exactly one of these works
                    ArrayWithPreallocation<AST::ExpPtr, 4> matchingGenerics;

                    for (auto& f : possibles)
                    {
                        if (! f.isImpossible && f.requiresGeneric)
                        {
                            if (auto e = resolveFunction (f, call, true))
                                matchingGenerics.push_back (e);
                            else if (! canResolveGenerics())
                                return call;
                        }
                    }

                    if (matchingGenerics.size() == 1)
                        return matchingGenerics.front();

                    if (! ignoreErrors)
                    {
                        if (totalMatches == 0)
                            throwErrorForUnknownFunction (call, *name);

                        auto possibleWithCast = countNumberOfMatchesWithCast (possibles);

                        if (exactMatches + possibleWithCast == 0)
                        {
                            if (totalMatches == 1 && ! possibles.front().requiresGeneric)
                            {
                                auto paramTypes = possibles.front().function.getParameterTypes();
                                SOUL_ASSERT (paramTypes.size() == call.getNumArguments());

                                for (size_t i = 0; i < paramTypes.size(); ++i)
                                    SanityCheckPass::expectSilentCastPossible (call.arguments->items[i]->context,
                                                                               paramTypes[i], *call.arguments->items[i]);
                            }

                            if (totalMatches == 0 || matchingGenerics.size() <= 1)
                                call.context.throwError (Errors::noMatchForFunctionCall (call.getDescription (name->path.toString())));
                        }

                        if (totalMatches > 1 || matchingGenerics.size() > 1)
                        {
                            ArrayWithPreallocation<AST::FunctionPtr, 4> functions;

                            for (auto& f : possibles)
                                functions.push_back (f.function);

                            SanityCheckPass::checkForDuplicateFunctions (functions);

                            call.context.throwError (Errors::ambiguousFunctionCall (call.getDescription (name->path.toString())));
                        }
                    }
                }
            }

            ++numFails;
            return call;
        }

        struct PossibleFunction
        {
            PossibleFunction() = delete;
            PossibleFunction (PossibleFunction&&) = default;

            PossibleFunction (AST::Function& f, ArrayView<Type> argTypes) : function (f)
            {
                for (size_t i = 0; i < argTypes.size(); ++i)
                {
                    Type targetParamType;

                    if (function.isGeneric() && ! function.parameters[i]->isResolved())
                    {
                        requiresGeneric = true;
                        continue;
                    }

                    targetParamType = function.parameters[i]->getType();

                    if (TypeRules::canPassAsArgumentTo (targetParamType, argTypes[i], true))
                        continue;

                    if (! TypeRules::canPassAsArgumentTo (targetParamType, argTypes[i], false))
                        isImpossible = true;

                    requiresCast = true;
                }
            }

            AST::Function& function;

            bool isImpossible = false;
            bool requiresCast = false;
            bool requiresGeneric = false;

            bool isExactMatch() const       { return ! (isImpossible || requiresCast || requiresGeneric); }
        };

        AST::ExpPtr resolveFunction (const PossibleFunction& f, AST::CallOrCast& call, bool ignoreErrorsInGenerics)
        {
            if (f.function.isRunFunction())
                call.context.throwError (Errors::cannotCallRunFunction());

            if (f.function.isGeneric())
                return createCallToGenericFunction (call, f.function, ignoreErrorsInGenerics);

            return allocator.allocate<AST::FunctionCall> (call.context, f.function, call.arguments, false);
        }

        virtual bool canResolveGenerics() const     { return false; }

        virtual AST::ExpPtr createCallToGenericFunction (AST::CallOrCast&, AST::Function&, bool)
        {
            ++numFails;
            return {};
        }

        ArrayWithPreallocation<PossibleFunction, 4> findAllPossibleFunctions (const AST::CallOrCast& call,
                                                                              const AST::QualifiedIdentifier& name)
        {
            auto argTypes = call.getArgumentTypes();

            AST::Scope::NameSearch search;
            search.partiallyQualifiedPath = name.path;
            search.stopAtFirstScopeWithResults = false;
            search.requiredNumFunctionArgs = (int) argTypes.size();
            search.findVariables = false;
            search.findTypes = false;
            search.findFunctions = true;
            search.findProcessorsAndNamespaces = false;
            search.findEndpoints = false;

            call.getParentScope()->performFullNameSearch (search, nullptr);

            if (name.path.isUnqualified())
            {
                search.partiallyQualifiedPath = owner.intrinsicsNamespacePath.withSuffix (search.partiallyQualifiedPath.getLastPart());
                call.getParentScope()->performFullNameSearch (search, nullptr);
            }

            ArrayWithPreallocation<PossibleFunction, 4> results;

            for (auto& i : search.itemsFound)
                if (auto f = cast<AST::Function> (i))
                    if (f->orginalGenericFunction == nullptr)
                        results.push_back (PossibleFunction (*f, argTypes));

            return results;
        }

        static size_t countNumberOfExactMatches (ArrayView<PossibleFunction> matches)
        {
            return (size_t) std::count_if (matches.begin(), matches.end(), [=] (const PossibleFunction& f) { return f.isExactMatch(); });
        }

        static size_t countNumberOfMatchesWithCast (ArrayView<PossibleFunction> matches)
        {
            return (size_t) std::count_if (matches.begin(), matches.end(), [=] (const PossibleFunction& f) { return f.requiresCast && ! f.isImpossible; });
        }

        void throwErrorForUnknownFunction (AST::CallOrCast& call, AST::QualifiedIdentifier& name)
        {
            AST::Scope::NameSearch search;
            search.partiallyQualifiedPath = name.path;
            search.stopAtFirstScopeWithResults = true;
            search.findVariables = true;
            search.findTypes = true;
            search.findFunctions = true;
            search.findProcessorsAndNamespaces = true;
            search.findEndpoints = true;

            if (auto scope = name.getParentScope())
                scope->performFullNameSearch (search, nullptr);

            if (name.path.isUnqualified())
            {
                search.partiallyQualifiedPath = owner.intrinsicsNamespacePath.withSuffix (search.partiallyQualifiedPath.getLastPart());
                call.getParentScope()->performFullNameSearch (search, nullptr);
            }

            size_t numFunctions = 0;

            for (auto& i : search.itemsFound)
                if (is_type<AST::Function> (i))
                    ++numFunctions;

            if (numFunctions > 0)
                name.context.throwError (Errors::noFunctionWithNumberOfArgs (name.path,
                                                                             std::to_string (call.getNumArguments())));

            if (! search.itemsFound.empty())
            {
                if (is_type<AST::Processor> (search.itemsFound.front()))
                    name.context.throwError (Errors::cannotUseProcessorAsFunction());

                if (is_type<AST::InputDeclaration> (search.itemsFound.front()))
                    name.context.throwError (Errors::cannotUseInputAsFunction());

                if (is_type<AST::OutputDeclaration> (search.itemsFound.front()))
                    name.context.throwError (Errors::cannotUseOutputAsFunction());
            }

            auto possibleFunction = findPossibleMisspeltFunction (name.path.getLastPart());

            if (! possibleFunction.empty())
                name.context.throwError (Errors::unknownFunctionWithSuggestion (name.path, possibleFunction));

            name.context.throwError (Errors::unknownFunction (name.path));
        }

        std::string findPossibleMisspeltFunction (const std::string& name)
        {
            std::string nearest;
            size_t lowestDistance = 5;

            AST::Scope* topLevelScope = std::addressof (module);

            while (topLevelScope->getParentScope() != nullptr)
                topLevelScope = topLevelScope->getParentScope();

            findLeastMisspeltFunction (*topLevelScope, name, nearest, lowestDistance);

            nearest = Program::stripRootNamespaceFromQualifiedPath (nearest);
            return TokenisedPathString::removeTopLevelNameIfPresent (nearest, getIntrinsicsNamespaceName());
        }

        static void findLeastMisspeltFunction (AST::Scope& scope, const std::string& name, std::string& nearest, size_t& lowestDistance)
        {
            for (auto& f : scope.getFunctions())
            {
                auto functionName = f->name.toString();
                auto distance = levenshteinDistance (name, functionName);

                if (distance < lowestDistance)
                {
                    lowestDistance = distance;
                    nearest = TokenisedPathString::join (scope.getFullyQualifiedPath().toString(), functionName);
                }
            }

            for (auto& sub : scope.getSubModules())
                findLeastMisspeltFunction (*sub, name, nearest, lowestDistance);
        }

        AST::ExpPtr createAdvanceCall (AST::CallOrCast& c)
        {
            if (c.isMethodCall)                            c.context.throwError (Errors::advanceIsNotAMethod());
            if (c.getNumArguments() != 0)                  c.context.throwError (Errors::advanceHasNoArgs());
            if (! c.getParentFunction()->isRunFunction())  c.context.throwError (Errors::advanceMustBeCalledInRun());

            return allocator.allocate<AST::AdvanceClock> (c.context);
        }

        AST::StaticAssertionPtr createStaticAssert (AST::CallOrCast& c)
        {
            auto numArgs = c.getNumArguments();

            if (numArgs != 1 && numArgs != 2)
                c.context.throwError (Errors::expected1or2Args());

            std::string error = "static_assert failed";

            if (numArgs == 2)
                error = getErrorMessageArgument (c.arguments->items[1]);

            return allocator.allocate<AST::StaticAssertion> (c.context, c.arguments->items.front(), error);
        }

        std::string getErrorMessageArgument (AST::ExpPtr e)
        {
            if (AST::isResolvedAsConstant (e))
                if (auto c = e->getAsConstant())
                    if (c->value.getType().isStringLiteral())
                        return allocator.stringDictionary.getStringForHandle (c->value.getStringLiteral());

            e->context.throwError (Errors::expectedStringLiteralAsArg2());
            return {};
        }

        AST::ExpPtr createAtCall (AST::CallOrCast& call)
        {
            if (call.getNumArguments() != 2)
                call.context.throwError (Errors::atMethodTakes1Arg());

            auto array = call.arguments->items[0];
            auto index = call.arguments->items[1];

            SanityCheckPass::expectSilentCastPossible (call.context, Type (PrimitiveType::int32), *index);

            if (array->kind == AST::ExpressionKind::endpoint)
            {
                SOUL_ASSERT (AST::isResolvedAsEndpoint (array));
                pool_ptr<AST::EndpointDeclaration> endpoint;

                if (auto i = cast<AST::InputEndpointRef> (array))
                    endpoint = i->input;

                if (auto o = cast<AST::OutputEndpointRef> (array))
                    endpoint = o->output;

                if (endpoint == nullptr)
                    array->context.throwError (Errors::cannotResolveSourceOfAtMethod());

                Type::BoundedIntSize arraySize = 0;

                if (endpoint->arraySize != nullptr)
                {
                    SOUL_ASSERT (AST::isResolvedAsConstant (endpoint->arraySize));
                    arraySize = static_cast<Type::BoundedIntSize> (TypeRules::checkAndGetArraySize (endpoint->arraySize->context,
                                                                                                    endpoint->arraySize->getAsConstant()->value));
                }

                if (arraySize == 0)
                    call.context.throwError (Errors::wrongTypeForAtMethod());
            }
            else
            {
                auto arrayType = array->getResultType();

                if (! arrayType.isArrayOrVector())
                    call.context.throwError (Errors::wrongTypeForAtMethod());
            }

            auto ref = allocator.allocate<AST::ArrayElementRef> (call.context, array, index, nullptr, false);
            ref->suppressWrapWarning = true;
            return ref;
        }

        AST::FunctionPtr visit (AST::Function& f) override
        {
            if (! f.isGeneric())
                return super::visit (f);

            return f;
        }

        AST::ExpPtr visit (AST::ArrayElementRef& s) override
        {
            super::visit (s);

            if (! ignoreErrors)
                SanityCheckPass::checkArraySubscript (s);

            return s;
        }
    };

    //==============================================================================
    struct GenericFunctionResolver  : public FunctionResolver
    {
        static inline constexpr const char* getPassName()  { return "GenericFunctionResolver"; }

        GenericFunctionResolver (ResolutionPass& rp, bool shouldIgnoreErrors)
            : FunctionResolver (rp, shouldIgnoreErrors)
        {
        }

        bool canResolveGenerics() const override        { return true; }

        AST::ExpPtr createCallToGenericFunction (AST::CallOrCast& call, AST::Function& genericFunction, bool shouldIgnoreErrors) override
        {
            SOUL_ASSERT (genericFunction.isGeneric());

            if (auto newFunction = getOrCreateSpecialisedFunction (call, genericFunction,
                                                                   allocator.get ("_" + genericFunction.name.toString()
                                                                                    + "_specialised_" + call.getIDStringForArgumentTypes()),
                                                                   call.getArgumentTypes(),
                                                                   shouldIgnoreErrors))
            {
                auto newCall = allocator.allocate<AST::FunctionCall> (call.context, *newFunction, call.arguments, call.isMethodCall);
                newFunction->originalCallLeadingToSpecialisation = newCall;
                return newCall;
            }

            return {};
        }

        AST::FunctionPtr getOrCreateSpecialisedFunction (AST::CallOrCast& call, AST::Function& genericFunction,
                                                         Identifier specialisedFunctionName, ArrayView<Type> callerArgumentTypes,
                                                         bool shouldIgnoreErrors)
        {
            auto parentScope = genericFunction.getParentScope();
            SOUL_ASSERT (parentScope != nullptr);

            for (auto& f : parentScope->getFunctions())
                if (f->name == specialisedFunctionName && f->orginalGenericFunction == genericFunction)
                    return *f;

            auto newFunction = StructuralParser::cloneFunction (allocator, genericFunction);
            newFunction->name = specialisedFunctionName;
            newFunction->orginalGenericFunction = genericFunction;

            SOUL_ASSERT (callerArgumentTypes.size() == newFunction->parameters.size());

            if (! resolveGenericFunctionTypes (call, genericFunction, *newFunction, callerArgumentTypes, shouldIgnoreErrors))
            {
                auto parentModule = dynamic_cast<AST::ModuleBase*> (genericFunction.getParentScope());
                SOUL_ASSERT (parentModule != nullptr);
                removeItem (*parentModule->getFunctionList(), newFunction);
                return {};
            }

            return newFunction;
        }

        bool resolveGenericFunctionTypes (AST::CallOrCast& call, AST::Function& originalFunction,
                                          AST::Function& function, ArrayView<Type> callerArgumentTypes,
                                          bool shouldIgnoreErrors)
        {
            while (! function.genericWildcards.empty())
            {
                auto wildcardToResolve = function.genericWildcards.back();
                SOUL_ASSERT (wildcardToResolve->path.isUnqualified());
                auto wildcardName = wildcardToResolve->path.getLastPart();
                function.genericWildcards.pop_back();
                Type resolvedType;

                for (size_t i = 0; i < function.parameters.size(); ++i)
                {
                    if (auto paramType = function.parameters[i]->declaredType)
                    {
                        bool anyReferencesInvolved = false;
                        auto newMatch = matchParameterAgainstWildcard (paramType, callerArgumentTypes[i],
                                                                       wildcardName, anyReferencesInvolved);

                        if (newMatch.isValid())
                        {
                            if (! newMatch.isReference())
                                newMatch = newMatch.removeConstIfPresent();

                            if (resolvedType.isValid())
                            {
                                if (! newMatch.isIdentical (resolvedType))
                                {
                                    if (! anyReferencesInvolved && TypeRules::canSilentlyCastTo (newMatch, resolvedType))
                                    {
                                        resolvedType = newMatch;
                                    }
                                    else if (anyReferencesInvolved || ! TypeRules::canSilentlyCastTo (resolvedType, newMatch))
                                    {
                                        if (! shouldIgnoreErrors)
                                            throwResolutionError (call, originalFunction, wildcardToResolve->context,
                                                                  "Could not find a value for " + quoteName (wildcardName) + " that satisfies all argument types");

                                        return false;
                                    }
                                }
                            }
                            else
                            {
                                resolvedType = newMatch;
                            }
                        }
                    }
                }

                if (! resolvedType.isValid())
                {
                    if (! shouldIgnoreErrors)
                        throwResolutionError (call, originalFunction, wildcardToResolve->context,
                                              "Failed to resolve generic parameter " + quoteName (wildcardName));
                    return false;
                }

                auto type = allocator.allocate<AST::ConcreteType> (AST::Context(), resolvedType);
                auto usingDecl = allocator.allocate<AST::UsingDeclaration> (wildcardToResolve->context, wildcardName, type);
                function.genericSpecialisations.push_back (usingDecl);
            }

            return true;
        }

        void throwResolutionError (AST::CallOrCast& call, AST::Function& function,
                                   const AST::Context& errorLocation, const std::string& errorMessage)
        {
            CompileMessageGroup messages;

            if (function.context.location.sourceCode->isInternal)
            {
                messages.messages.push_back ({ "Could not resolve argument types for function call " + call.getDescription (function.name),
                                               call.context.location, CompileMessage::Type::error });
            }
            else
            {
                messages.messages.push_back ({ "Failed to resolve generic function call " + call.getDescription (function.name),
                                               call.context.location, CompileMessage::Type::error });

                messages.messages.push_back ({ errorMessage, errorLocation.location, CompileMessage::Type::error });
            }

            soul::throwError (messages);
        }

        Type matchParameterAgainstWildcard (const AST::ExpPtr paramType,
                                            const Type& callerArgumentType,
                                            const Identifier& wildcardToFind,
                                            bool& anyReferencesInvolved)
        {
            if (auto unresolvedTypeName = cast<AST::QualifiedIdentifier> (paramType))
            {
                if (unresolvedTypeName->path.isUnqualifiedName (wildcardToFind))
                    return callerArgumentType;
            }
            else if (auto mf = cast<AST::TypeMetaFunction> (paramType))
            {
                if (mf->isMakingConst())
                    return matchParameterAgainstWildcard (mf->source, callerArgumentType.removeConstIfPresent(), wildcardToFind, anyReferencesInvolved);

                if (mf->isMakingReference())
                {
                    anyReferencesInvolved = true;
                    return matchParameterAgainstWildcard (mf->source, callerArgumentType.removeReferenceIfPresent(), wildcardToFind, anyReferencesInvolved);
                }
            }
            else if (auto sb = cast<AST::SubscriptWithBrackets> (paramType))
            {
                if (callerArgumentType.isArray() && sb->rhs == nullptr)
                    return matchParameterAgainstWildcard (sb->lhs, callerArgumentType.getElementType(), wildcardToFind, anyReferencesInvolved);

                if (callerArgumentType.isFixedSizeArray() && sb->rhs != nullptr)
                {
                    if (auto sizeConst = sb->rhs->getAsConstant())
                    {
                        if (sizeConst->value.getType().isPrimitiveInteger())
                        {
                            auto size = sizeConst->value.getAsInt64();

                            if (size == (int64_t) callerArgumentType.getArraySize())
                                return matchParameterAgainstWildcard (sb->lhs, callerArgumentType.getElementType(), wildcardToFind, anyReferencesInvolved);
                        }
                    }
                }
            }
            else if (auto sc = cast<AST::SubscriptWithChevrons> (paramType))
            {
                if (callerArgumentType.isVector())
                {
                    if (auto sizeConst = sc->rhs->getAsConstant())
                    {
                        if (sizeConst->value.getType().isPrimitiveInteger())
                        {
                            auto size = sizeConst->value.getAsInt64();

                            if (size == (int64_t) callerArgumentType.getVectorSize())
                                return matchParameterAgainstWildcard (sc->lhs, callerArgumentType.getElementType(), wildcardToFind, anyReferencesInvolved);
                        }
                    }
                }
            }

            return {};
        }
    };

    //==============================================================================
    struct FullResolver  : public RewritingASTVisitor
    {
        FullResolver (ResolutionPass& rp) : allocator (rp.allocator), module (rp.module) {}

        using super = RewritingASTVisitor;
        static inline constexpr const char* getPassName()  { return "FullResolver"; }

        AST::Allocator& allocator;
        AST::ModuleBase& module;

        AST::ExpPtr silentCastToType (const AST::Context& castLocation, AST::ExpPtr e, const Type& targetType)
        {
            if (e == nullptr)
                return {};

            SOUL_ASSERT (AST::isResolvedAsValue (e));

            auto srcType = e->getResultType();

            if (srcType.isIdentical (targetType))
                return e;

            SanityCheckPass::expectSilentCastPossible (castLocation, targetType, *e);

            if (auto c = e->getAsConstant())
            {
                SOUL_ASSERT (TypeRules::canSilentlyCastTo (targetType, c->value));
                return allocator.allocate<AST::Constant> (e->context, c->value.castToTypeExpectingSuccess (targetType));
            }

            return visitExpression (allocator.allocate<AST::TypeCast> (e->context, targetType, e));
        }

        AST::FunctionPtr visit (AST::Function& f) override
        {
            if (f.isGeneric())
                return f;

            return super::visit (f);
        }

        AST::ExpPtr visit (AST::QualifiedIdentifier& qi) override
        {
            super::visit (qi);
            qi.context.throwError (Errors::unresolvedSymbol (qi.path));
            return qi;
        }

        AST::ExpPtr visit (AST::CallOrCast& c) override
        {
            super::visit (c);
            c.context.throwError (Errors::cannotResolveFunctionOrCast());
            return c;
        }

        AST::StatementPtr visit (AST::ReturnStatement& r) override
        {
            super::visit (r);

            auto returnTypeExp = r.getParentFunction()->returnType;
            SOUL_ASSERT (AST::isResolvedAsType (returnTypeExp));
            auto returnType = returnTypeExp->resolveAsType();

            if (r.returnValue != nullptr)
                SanityCheckPass::expectSilentCastPossible (r.context, returnType, *r.returnValue);
            else if (! returnType.isVoid())
                r.context.throwError (Errors::voidFunctionCannotReturnValue());

            return r;
        }

        AST::StatementPtr visit (AST::IfStatement& i) override
        {
            super::visit (i);

            if (auto constant = i.condition->getAsConstant())
            {
                if (constant->value.getAsBool())
                    return i.trueBranch;

                if (i.falseBranch != nullptr)
                    return i.falseBranch;

                return allocator.allocate<AST::NoopStatement> (i.context);
            }

            return i;
        }

        AST::ExpPtr visit (AST::TernaryOp& t) override
        {
            super::visit (t);
            SanityCheckPass::throwErrorIfNotReadableValue (t.condition);
            SanityCheckPass::throwErrorIfNotReadableValue (t.trueBranch);
            SanityCheckPass::throwErrorIfNotReadableValue (t.falseBranch);
            SanityCheckPass::expectSilentCastPossible (t.context, Type (PrimitiveType::bool_), *t.condition);

            auto trueType  = t.trueBranch->getResultType();
            auto falseType = t.falseBranch->getResultType();

            if (trueType.isVoid() || falseType.isVoid())
                t.context.throwError (Errors::ternaryCannotBeVoid());

            if (! trueType.isIdentical (falseType))
            {
                bool castToTrue  = TypeRules::canSilentlyCastTo (trueType, falseType);
                bool castToFalse = TypeRules::canSilentlyCastTo (falseType, trueType);

                if (! (castToTrue || castToFalse))
                    t.context.throwError (Errors::ternaryTypesMustMatch (trueType.getDescription(),
                                                                         falseType.getDescription()));

                if (castToTrue)
                {
                    t.falseBranch = allocator.allocate<AST::TypeCast> (t.falseBranch->context, trueType, t.falseBranch);
                    ++itemsReplaced;
                }
                else
                {
                    t.trueBranch = allocator.allocate<AST::TypeCast> (t.trueBranch->context, falseType, t.trueBranch);
                    ++itemsReplaced;
                }
            }

            if (auto constant = t.condition->getAsConstant())
                return constant->value.getAsBool() ? t.trueBranch : t.falseBranch;

            return t;
        }

        AST::ExpPtr visit (AST::TypeCast& c) override
        {
            super::visit (c);

            SOUL_ASSERT (c.getNumArguments() != 0); // should have already been handled by the constant folder

            if (c.targetType.isUnsizedArray())
                c.context.throwError (Errors::notYetImplemented ("cast to unsized arrays"));

            size_t numArgs = 1;

            if (auto list = cast<AST::CommaSeparatedList> (c.source))
                numArgs = list->items.size();

            if (numArgs != 1)
                SanityCheckPass::throwErrorIfWrongNumberOfElements (c.context, c.targetType, numArgs);

            return c;
        }

        AST::ExpPtr visit (AST::BinaryOperator& b) override
        {
            super::visit (b);

            SanityCheckPass::throwErrorIfNotReadableValue (b.rhs);

            if (b.isOutputEndpoint())
                return b;

            SanityCheckPass::throwErrorIfNotReadableValue (b.lhs);

            auto operandType = b.getOperandType();

            if (! b.isResolved())
            {
                // If we fail to resolve the operator type based on its input types, see if there
                // are constants involved which do actually silently cast to a suitable type (e.g. '0' to '0.0f')
                auto lhsType = b.lhs->getResultType();
                auto rhsType = b.rhs->getResultType();

                if (! lhsType.isIdentical (rhsType))
                {
                    if (auto lhsConst = b.lhs->getAsConstant())
                    {
                        if (TypeRules::canSilentlyCastTo (rhsType, lhsConst->value))
                        {
                            b.lhs = allocator.allocate<AST::Constant> (b.lhs->context, lhsConst->value.castToTypeExpectingSuccess (rhsType));
                            operandType = b.getOperandType();
                        }
                    }

                    if (auto rhsConst = b.rhs->getAsConstant())
                    {
                        if (TypeRules::canSilentlyCastTo (lhsType, rhsConst->value))
                        {
                            b.rhs = allocator.allocate<AST::Constant> (b.rhs->context, rhsConst->value.castToTypeExpectingSuccess (lhsType));
                            operandType = b.getOperandType();
                        }

                        if (rhsConst->value.isZero())
                        {
                            if (b.operation == BinaryOp::Op::modulo)
                                b.rhs->context.throwError (Errors::moduloZero());

                            if (b.operation == BinaryOp::Op::divide)
                                b.rhs->context.throwError (Errors::divideByZero());
                        }
                    }
                }
            }
            else if (! operandType.isValid())
            {
                b.context.throwError (Errors::illegalTypesForBinaryOperator (getSymbol (b.operation),
                                                                             b.lhs->getResultType().getDescription(),
                                                                             b.rhs->getResultType().getDescription()));
            }

            return b;
        }

        static void checkPropertyValue (AST::Expression& value)
        {
            if (! value.isCompileTimeConstant())
                value.context.throwError (Errors::propertyMustBeConstant());

            if (auto constValue = value.getAsConstant())
            {
                auto type = constValue->getResultType();

                if (! (type.isPrimitiveFloat() || type.isPrimitiveInteger()
                        || type.isPrimitiveBool() || type.isStringLiteral()))
                    value.context.throwError (Errors::illegalPropertyType());
            }
        }

        void visit (AST::Annotation& a) override
        {
            super::visit (a);

            for (auto& property : a.properties)
                checkPropertyValue (*property.value);
        }

        AST::ExpPtr visit (AST::Assignment& a) override
        {
            super::visit (a);

            if (! a.target->isAssignable())
                a.context.throwError (Errors::operatorNeedsAssignableTarget ("="));

            SanityCheckPass::expectSilentCastPossible (a.context,
                                                       a.target->getResultType().removeReferenceIfPresent()
                                                                                .removeConstIfPresent(),
                                                       *a.newValue);
            return a;
        }

        static std::string getOperatorName (const AST::PreOrPostIncOrDec& p)
        {
            return p.isIncrement ? "++" : "--";
        }

        AST::ExpPtr visit (AST::PreOrPostIncOrDec& p) override
        {
            super::visit (p);

            if (! p.target->isAssignable())
                p.context.throwError (Errors::operatorNeedsAssignableTarget (getOperatorName (p)));

            auto type = p.target->getResultType();

            if (type.isBool() || ! (type.isPrimitive() || type.isBoundedInt()))
                p.context.throwError (Errors::illegalTypeForOperator (getOperatorName (p)));

            return p;
        }

        AST::ExpPtr visit (AST::ArrayElementRef& s) override
        {
            super::visit (s);

            Type lhsType;

            if (AST::isResolvedAsEndpoint (s.object))
            {
                if (auto outRef = cast<AST::OutputEndpointRef> (s.object))
                    lhsType = outRef->output->getSampleArrayTypes().front();
                else if (auto inRef = cast<AST::InputEndpointRef> (s.object))
                    lhsType = inRef->input->getSampleArrayTypes().front();
                else
                    SOUL_ASSERT_FALSE;
            }
            else
            {
                lhsType = s.object->getResultType();
            }

            if (! lhsType.isArrayOrVector())
            {
                if (AST::isResolvedAsEndpoint (s.object) || is_type<AST::InputEndpointRef> (s.object))
                    s.object->context.throwError (Errors::cannotUseBracketOnEndpoint());

                s.object->context.throwError (Errors::expectedArrayOrVectorForBracketOp());
            }

            if (auto startIndexConst = s.startIndex->getAsConstant())
            {
                auto startIndex = TypeRules::checkAndGetArrayIndex (s.startIndex->context, startIndexConst->value);

                if (! (lhsType.isUnsizedArray() || lhsType.isValidArrayOrVectorIndex (startIndex)))
                    s.startIndex->context.throwError (Errors::indexOutOfRange());

                if (s.isSlice)
                {
                    if (lhsType.isUnsizedArray())
                        s.startIndex->context.throwError (Errors::notYetImplemented ("Slices of dynamic arrays"));

                    if (! lhsType.getElementType().isPrimitive())
                        s.startIndex->context.throwError (Errors::notYetImplemented ("Slices of non-primitive arrays"));

                    if (s.endIndex != nullptr)
                    {
                        if (auto endIndexConst = s.endIndex->getAsConstant())
                        {
                            auto endIndex = TypeRules::checkAndGetArrayIndex (s.endIndex->context, endIndexConst->value);

                            if (! lhsType.isValidArrayOrVectorRange (startIndex, endIndex))
                                s.endIndex->context.throwError (Errors::illegalSliceSize());
                        }
                        else
                        {
                            s.endIndex->context.throwError (Errors::notYetImplemented ("Dynamic slice indexes"));
                        }
                    }
                }
            }
            else
            {
                if (s.isSlice)
                    s.startIndex->context.throwError (Errors::notYetImplemented ("Dynamic slice indexes"));

                auto indexType = s.startIndex->getResultType();

                if (lhsType.isUnsizedArray())
                {
                    if (! (indexType.isInteger() || indexType.isBoundedInt()))
                        s.startIndex->context.throwError (Errors::nonIntegerArrayIndex());
                }
                else
                {
                    SanityCheckPass::expectSilentCastPossible (s.startIndex->context,
                                                               Type (PrimitiveType::int32), *s.startIndex);
                }
            }

            return s;
        }

        AST::StatementPtr visit (AST::LoopStatement& s) override
        {
            if (s.numIterations != nullptr)
            {
                if (auto c = s.numIterations->getAsConstant())
                    if (c->value.getAsInt64() <= 0)
                        s.numIterations->context.throwError (Errors::negativeLoopCount());

                SanityCheckPass::expectSilentCastPossible (s.numIterations->context,
                                                           Type (PrimitiveType::int64), *s.numIterations);
            }

            return super::visit (s);
        }

        static AST::WriteToEndpoint& getTopLevelWriteToEndpoint (AST::WriteToEndpoint& ws)
        {
            if (auto chainedWrite = cast<AST::WriteToEndpoint> (ws.target))
                return getTopLevelWriteToEndpoint (*chainedWrite);

            return ws;
        }

        AST::ExpPtr visit (AST::WriteToEndpoint& w) override
        {
            super::visit (w);

            SanityCheckPass::throwErrorIfNotReadableValue (w.value);
            auto& topLevelWrite = getTopLevelWriteToEndpoint (w);

            // Either an OutputEndpointRef, or an ArrayElementRef of an OutputEndpointRef
            if (auto outputEndpoint = cast<AST::OutputEndpointRef> (topLevelWrite.target))
            {
                SanityCheckPass::expectSilentCastPossible (w.context, outputEndpoint->output->getSampleArrayTypes(), *w.value);
                return w;
            }

            if (auto arraySubscript = cast<AST::ArrayElementRef> (topLevelWrite.target))
            {
                if (auto outputEndpoint = cast<AST::OutputEndpointRef> (arraySubscript->object))
                {
                    SanityCheckPass::expectSilentCastPossible (w.context, outputEndpoint->output->getResolvedSampleTypes(), *w.value);
                    return w;
                }
            }

            w.context.throwError (Errors::targetMustBeOutput());
            return w;
        }

        AST::ProcessorInstancePtr visit (AST::ProcessorInstance& i) override
        {
            super::visit (i);

            if (i.clockMultiplierRatio != nullptr)
                validateClockRatio (i.clockMultiplierRatio);

            if (i.clockDividerRatio != nullptr)
                validateClockRatio (i.clockDividerRatio);

            return i;
        }

        static void validateClockRatio (AST::ExpPtr ratio)
        {
            if (auto c = ratio->getAsConstant())
                heart::getClockRatioFromValue (ratio->context, c->value);
            else
                ratio->context.throwError (Errors::ratioMustBeConstant());
        }
    };
};


} // namespace soul
