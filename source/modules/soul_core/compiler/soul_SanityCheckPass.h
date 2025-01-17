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
    Provides various types of sanity-check on some AST
*/
struct SanityCheckPass  final
{
    /** Does some high-level checks after an initial parse and before name resolution, */
    static void runPreResolution (AST::ModuleBase& module)
    {
        checkOverallStructure (module);
    }

    /** After the AST is resolved, this pass checks for more subtle errors */
    static void runPostResolution (AST::ModuleBase& module)
    {
        runDuplicateNameChecker (module);
        PostResolutionChecks().visitObject (module);
        PreAndPostIncOperatorCheck().ASTVisitor::visitObject (module);
    }

    static void runDuplicateNameChecker (AST::ModuleBase& module)
    {
        DuplicateNameChecker().visitObject (module);
    }

    struct RecursiveTypeDeclVisitStack
    {
        ArrayWithPreallocation<AST::TypeDeclarationBase*, 8> stack;

        void push (AST::TypeDeclarationBase& t)
        {
            if (contains (stack, &t))
            {
                if (stack.back() == &t)
                    t.context.throwError (Errors::typeContainsItself (t.name));

                t.context.throwError (Errors::typesReferToEachOther (t.name, stack.back()->name));
            }

            stack.push_back (&t);
        }

        void pop()
        {
            stack.pop_back();
        }
    };

    static void throwErrorIfNotReadableValue (AST::ExpPtr e)
    {
        if (! AST::isResolvedAsValue (e))
        {
            if (is_type<AST::OutputEndpointRef> (e))
                e->context.throwError (Errors::cannotReadFromOutput());

            if (is_type<AST::ProcessorRef> (e))
                e->context.throwError (Errors::cannotUseProcessorAsOutput());

            e->context.throwError (Errors::expectedValue());
        }
    }

    static void throwErrorIfNotArrayOrVector (AST::ExpPtr e)
    {
        throwErrorIfNotReadableValue (e);

        if (! e->getResultType().isArrayOrVector())
            e->context.throwError (Errors::expectedArrayOrVector());
    }

    static void throwErrorIfNotReadableType (AST::ExpPtr e)
    {
        if (! AST::isResolvedAsType (e))
        {
            if (is_type<AST::ProcessorRef> (e))
                e->context.throwError (Errors::cannotUseProcessorAsType());

            e->context.throwError (Errors::expectedType());
        }
    }

    static void expectCastPossible (const AST::Context& context, const Type& targetType, const Type& sourceType)
    {
        if (! TypeRules::canCastTo (targetType, sourceType))
            context.throwError (Errors::cannotCastBetween (sourceType.getDescription(), targetType.getDescription()));
    }

    static void expectSilentCastPossible (const AST::Context& context, const Type& targetType, AST::Expression& source)
    {
        if (auto list = cast<AST::CommaSeparatedList> (source))
        {
            throwErrorIfWrongNumberOfElements (context, targetType, list->items.size());

            if (targetType.isArrayOrVector())
            {
                auto elementType = targetType.getElementType();

                for (auto& i : list->items)
                    expectSilentCastPossible (i->context, elementType, *i);

                return;
            }

            if (targetType.isStruct())
            {
                auto& s = targetType.getStructRef();

                for (size_t i = 0; i < list->items.size(); ++i)
                    expectSilentCastPossible (list->items[i]->context, s.members[i].type, *list->items[i]);

                return;
            }

            context.throwError (Errors::cannotCastListToType (targetType.getDescription()));
        }

        if (! source.canSilentlyCastTo (targetType))
        {
            if (auto c = source.getAsConstant())
                if (c->getResultType().isPrimitive())
                    context.throwError (Errors::cannotImplicitlyCastValue (c->value.getDescription(),
                                                                           c->value.getType().getDescription(),
                                                                           targetType.getDescription()));

            context.throwError (Errors::cannotImplicitlyCastType (source.getResultType().getDescription(),
                                                                  targetType.getDescription()));
        }
    }

    static void expectSilentCastPossible (const AST::Context& context, ArrayView<Type> targetTypes, AST::Expression& source)
    {
        int matches = 0;

        for (auto& type : targetTypes)
        {
            // If we have an exact match, it doesn't matter how many other types could be used silently
            if (source.getResultType().isEqual (type, Type::ignoreVectorSize1))
                return;

            if (source.canSilentlyCastTo (type))
                ++matches;
        }

        if (matches == 0)
            context.throwError (Errors::cannotImplicitlyCastType (source.getResultType().getDescription(),
                                                                  getTypesDescription (targetTypes)));

        if (matches > 1)
            context.throwError (Errors::ambiguousCastBetween (source.getResultType().getDescription(),
                                                              getTypesDescription (targetTypes)));
    }

    static void throwErrorIfMultidimensionalArray (const AST::Context& location, const Type& type)
    {
        if (type.isArray())
        {
            auto elementType = type.getArrayElementType();

            if (elementType.isArray())
                location.throwError (Errors::notYetImplemented ("Multi-dimensional arrays"));

            throwErrorIfMultidimensionalArray (location, elementType);
        }

        if (type.isStruct())
            for (auto& m : type.getStructRef().members)
                throwErrorIfMultidimensionalArray (location, m.type);
    }

    static std::string getTypesDescription (ArrayView<Type> types)
    {
        if (types.size() == 1)
            return types.front().getDescription();

        std::vector<std::string> descriptions;

        for (auto& type : types)
            descriptions.push_back (type.getDescription());

        return "(" + soul::joinStrings (descriptions, ", ") + ")";
    }

    static void checkArraySubscript (AST::ArrayElementRef& s)
    {
        if (! s.object->isOutputEndpoint())
            throwErrorIfNotArrayOrVector (s.object);
    }

    static void throwErrorIfWrongNumberOfElements (const AST::Context& c, const Type& type, size_t numberAvailable)
    {
        if (type.isFixedSizeAggregate() && type.getNumAggregateElements() != numberAvailable)
            c.throwError (Errors::wrongNumArgsForAggregate (type.getDescription()));
    }

    static int64_t checkDelayLineLength (const AST::Context& context, const Value& v)
    {
        if (! v.getType().isPrimitiveInteger())
            context.throwError (Errors::delayLineMustHaveIntLength());

        auto value = v.getAsInt64();

        if (value < 1)
            context.throwError (Errors::delayLineHasZeroLength());

        if (value > (int64_t) AST::maxDelayLineLength)
            context.throwError (Errors::delayLineIllegalLength());

        return value;
    }

    static void checkForDuplicateFunctions (ArrayView<AST::FunctionPtr> functions)
    {
        std::vector<std::string> functionSigs;
        functionSigs.reserve (functions.size());

        for (auto& f : functions)
        {
            if (! f->isGeneric())
            {
                auto newSig = f->getSignatureID();

                if (contains (functionSigs, newSig))
                    f->context.throwError (Errors::duplicateFunction());

                functionSigs.push_back (newSig);
            }
        }
    }

private:
    //==============================================================================
    static void checkOverallStructure (pool_ptr<AST::ModuleBase> module)
    {
        if (auto p = cast<AST::ProcessorBase> (module))
            checkOverallStructureOfProcessor (*p);

        for (auto m : module->getSubModules())
            checkOverallStructure (*m);
    }

    static void checkOverallStructureOfProcessor (pool_ptr<AST::ProcessorBase> processorOrGraph)
    {
        if (processorOrGraph->outputs.empty())
            processorOrGraph->context.throwError (Errors::processorNeedsAnOutput());

        if (auto processor = cast<AST::Processor> (processorOrGraph))
        {
            int numRunFunctions = 0;

            for (auto& f : processor->getFunctions())
            {
                if (f->isRunFunction())
                {
                    if (! f->returnType->resolveAsType().isVoid())
                        f->context.throwError (Errors::runFunctionMustBeVoid());

                    if (! f->parameters.empty())
                        f->context.throwError (Errors::runFunctionHasParams());

                    ++numRunFunctions;
                }
            }

            if (numRunFunctions == 0)
                processor->context.throwError (Errors::processorNeedsRunFunction());

            if (numRunFunctions > 1)
                processor->context.throwError (Errors::multipleRunFunctions());
        }
    }

    //==============================================================================
    struct DuplicateNameChecker  : public ASTVisitor
    {
        using super = ASTVisitor;

        void visit (AST::Processor& p) override
        {
            super::visit (p);

            soul::DuplicateNameChecker duplicateNameChecker;

            for (auto& io : p.inputs)          duplicateNameChecker.check (io->name, io->context);
            for (auto& io : p.outputs)         duplicateNameChecker.check (io->name, io->context);
            for (auto& v : p.stateVariables)   duplicateNameChecker.check (v->name, v->context);
            for (auto& s : p.structures)       duplicateNameChecker.check (s->name, s->context);
            for (auto& u : p.usings)           duplicateNameChecker.check (u->name, u->context);

            // (functions must be scanned last)
            for (auto& f : p.functions)
            {
                if (f->isEventFunction())
                {
                    bool nameFound = false;

                    for (auto& io : p.inputs)
                        if (io->name == f->name)
                            nameFound = true;

                    if (! nameFound)
                        f->context.throwError (Errors::noSuchInputEvent (f->name));
                }
                else
                {
                    duplicateNameChecker.checkWithoutAdding (f->name, f->nameLocation);
                }
            }

            for (auto& m : p.getSubModules())
                duplicateNameChecker.check (m->name, m->context);
        }

        void visit (AST::Annotation& a) override
        {
            super::visit (a);
            soul::DuplicateNameChecker duplicateNameChecker;

            for (auto& p : a.properties)
                duplicateNameChecker.check (p.name->path.toString(), p.name->context);
        }

        void visit (AST::Graph& g) override
        {
            soul::DuplicateNameChecker duplicateNameChecker;

            for (auto& io : g.inputs)       duplicateNameChecker.check (io->name, io->context);
            for (auto& io : g.outputs)      duplicateNameChecker.check (io->name, io->context);
        }

        void visit (AST::Namespace& n) override
        {
            soul::DuplicateNameChecker duplicateNameChecker;

            for (auto& s : n.structures)    duplicateNameChecker.check (s->name, s->context);
            for (auto& u : n.usings)        duplicateNameChecker.check (u->name, u->context);
            for (auto& m : n.subModules)    duplicateNameChecker.check (m->name, m->context);

            // (functions must be scanned last)
            for (auto& f : n.functions)     duplicateNameChecker.checkWithoutAdding (f->name, f->nameLocation);
        }

        void visit (AST::Block& b) override
        {
            super::visit (b);
            soul::DuplicateNameChecker duplicateNameChecker;

            for (auto& s : b.statements)
                if (auto v = cast<AST::VariableDeclaration> (s))
                    duplicateNameChecker.check (v->name, v->context);
        }

        void visit (AST::Function& f) override
        {
            super::visit (f);
            soul::DuplicateNameChecker duplicateNameChecker;

            for (auto& param : f.parameters)
                duplicateNameChecker.check (param->name, param->context);
        }

        void visit (AST::StructDeclaration& s) override
        {
            super::visit (s);
            soul::DuplicateNameChecker duplicateNameChecker;

            for (auto& m : s.getMembers())
                duplicateNameChecker.check (m.name, s.context);
        }
    };

    //==============================================================================
    struct PostResolutionChecks  : public ASTVisitor
    {
        using super = ASTVisitor;

        void visit (AST::VariableDeclaration& v) override
        {
            super::visit (v);

            if (v.declaredType == nullptr)
                throwErrorIfNotReadableValue (v.initialValue);
            else
                throwErrorIfNotReadableType (v.declaredType);

            auto type = v.getType();
            auto context = (v.declaredType != nullptr ? v.declaredType->context : v.context);

            if (type.isVoid())
                context.throwError (Errors::variableCannotBeVoid());

            if (type.isPackedSizeTooBig())
                context.throwError (Errors::typeTooBig (getReadableDescriptionOfByteSize (v.getType().getPackedSizeInBytes()),
                                                        getReadableDescriptionOfByteSize (Type::maxPackedObjectSize)));

            throwErrorIfMultidimensionalArray (context, type);
        }

        void visit (AST::Processor& p) override
        {
            super::visit (p);
            checkForDuplicateFunctions (p.functions);

            for (auto input : p.inputs)
                input->checkSampleTypesValid();

            for (auto output : p.outputs)
                output->checkSampleTypesValid();

            for (auto& v : p.stateVariables)
                if (v->initialValue != nullptr && ! v->initialValue->isCompileTimeConstant())
                    v->initialValue->context.throwError (Errors::expectedConstant());
        }

        void visit (AST::Graph& g) override
        {
            super::visit (g);

            for (auto input : g.inputs)
                input->checkSampleTypesValid();

            for (auto output : g.outputs)
                output->checkSampleTypesValid();

            AST::Graph::RecursiveGraphDetector::check (g);
            AST::Graph::CycleDetector (g).check();
        }

        void visit (AST::Namespace& n) override
        {
            super::visit (n);
            checkForDuplicateFunctions (n.functions);

            for (auto& v : n.constants)
                if (! v->isCompileTimeConstant())
                    v->context.throwError (Errors::nonConstInNamespace());
        }

        void visit (AST::Function& f) override
        {
            if (! f.isGeneric())
            {
                for (auto& p : f.parameters)
                    if (p->getType().isVoid())
                        p->context.throwError (Errors::parameterCannotBeVoid());

                super::visit (f);
            }
        }

        void visit (AST::StructDeclaration& s) override
        {
            recursiveTypeDeclVisitStack.push (s);
            super::visit (s);
            recursiveTypeDeclVisitStack.pop();

            for (auto& m : s.getMembers())
                if (m.type->getConstness() == AST::Constness::definitelyConst)
                    m.type->context.throwError (Errors::memberCannotBeConst());
        }

        void visit (AST::UsingDeclaration& u) override
        {
            recursiveTypeDeclVisitStack.push (u);
            super::visit (u);
            recursiveTypeDeclVisitStack.pop();
        }

        RecursiveTypeDeclVisitStack recursiveTypeDeclVisitStack;

        void visit (AST::InputDeclaration& io) override
        {
            super::visit (io);
            checkArraySize (io.arraySize, AST::maxEndpointArraySize);
        }

        void visit (AST::OutputDeclaration& io) override
        {
            super::visit (io);
            checkArraySize (io.arraySize, AST::maxEndpointArraySize);
        }

        void visit (AST::ProcessorInstance& i) override
        {
            super::visit (i);
            checkArraySize (i.arrayArgument, AST::maxProcessorArraySize);
        }

        void visit (AST::Connection& c) override
        {
            super::visit (c);

            if (c.delayLength != nullptr)
            {
                throwErrorIfNotReadableValue (c.delayLength);

                if (auto cv = c.delayLength->getAsConstant())
                    checkDelayLineLength (cv->context, cv->value);
            }
        }

        void visit (AST::UnaryOperator& u) override
        {
            super::visit (u);

            if (! UnaryOp::isTypeSuitable (u.operation, u.source->getResultType()))
                u.source->context.throwError (Errors::wrongTypeForUnary());
        }

        void visit (AST::BinaryOperator& b) override
        {
            super::visit (b);

            if (BinaryOp::isComparisonOperator (b.operation))
            {
                auto lhsConst = b.lhs->getAsConstant();
                auto rhsConst = b.rhs->getAsConstant();
                int result = 0;

                if (lhsConst != nullptr && rhsConst == nullptr)
                    result = BinaryOp::getResultOfComparisonWithBoundedType (b.operation, lhsConst->value, b.rhs->getResultType());

                if (lhsConst == nullptr && rhsConst != nullptr)
                    result = BinaryOp::getResultOfComparisonWithBoundedType (b.operation, b.lhs->getResultType(), rhsConst->value);

                if (result != 0)
                    b.context.throwError (result > 0 ? Errors::comparisonAlwaysTrue()
                                                     : Errors::comparisonAlwaysFalse());
            }
        }

        void checkArraySize (const AST::ExpPtr& arraySize, int64_t maxSize)
        {
            if (arraySize != nullptr)
            {
                if (auto c = arraySize->getAsConstant())
                {
                    // Should only be an integer, and must be >= 1
                    if (c->getResultType().isInteger())
                    {
                        auto size = c->value.getAsInt64();

                        if (size < 1 || size > maxSize)
                            arraySize->context.throwError (Errors::illegalArraySize());
                    }
                    else
                    {
                        arraySize->context.throwError (Errors::nonIntegerArraySize());
                    }
                }
                else
                {
                    arraySize->context.throwError (Errors::nonConstArraySize());
                }
            }
        }
    };

    //==============================================================================
    struct PreAndPostIncOperatorCheck  : public ASTVisitor
    {
        using super = ASTVisitor;
        using VariableList = ArrayWithPreallocation<AST::VariableDeclarationPtr, 16>;
        VariableList* variablesModified = nullptr;
        VariableList* variablesReferenced = nullptr;
        bool isInsidePreIncOp = false;

        void visitObject (AST::Statement& s) override
        {
            VariableList modified, referenced;
            auto oldMod = variablesModified;
            auto oldRef = variablesReferenced;
            variablesModified = &modified;
            variablesReferenced = &referenced;
            super::visitObject (s);
            variablesModified = oldMod;
            variablesReferenced = oldRef;
        }

        void visit (AST::VariableRef& v) override
        {
            if (variablesModified != nullptr)
            {
                throwIfVariableFound (*variablesModified, v);
                variablesReferenced->push_back (v.variable);
            }

            super::visit (v);
        }

        void visit (AST::PreOrPostIncOrDec& p) override
        {
            if (auto v = cast<AST::VariableRef> (p.target))
            {
                SOUL_ASSERT (variablesModified != nullptr);

                if (variablesModified != nullptr)
                {
                    throwIfVariableFound (*variablesReferenced, *v);
                    variablesModified->push_back (v->variable);
                    variablesReferenced->push_back (v->variable);
                }
            }
            else
            {
                super::visit (p);
            }
        }

        void throwIfVariableFound (VariableList& list, AST::VariableRef& v)
        {
            if (contains (list, v.variable))
                v.context.throwError (Errors::preIncDecCollision());
        }
    };
};

} // namespace soul
