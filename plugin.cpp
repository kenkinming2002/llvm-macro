#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Transforms/Utils/Cloning.h>

using namespace llvm;

struct MacroFilename
{
public:
  static inline std::optional<std::string> opt_value;

  static const std::string &get()
  {
    if(!opt_value)
      report_fatal_error("missing -macro <macrofilename> argument (pass -mllvm -macro <macrofilename> if invoked via clang)", false);

    return *opt_value;
  }

public:
  void operator=(const std::string &filename)
  {
    if(opt_value)
      report_fatal_error("too many -macro <macrofilename> arguments passed", false);

    opt_value = filename;
  }
};

static cl::opt<MacroFilename, false, cl::parser<std::string>> optMacroFilename("macro", cl::value_desc("macrofilename"), cl::desc("Load the specified macro"));

static bool functionIsDefined(Function& f)
{
  return !f.empty();
}

static bool functionCheckSignature(Function& f, Type *RetTy, ArrayRef<Type *> ParamTys)
{
  FunctionType *functionType = f.getFunctionType();

  if(functionType->getReturnType() != RetTy)
    return false;

  if(functionType->getNumParams() != ParamTys.size())
    return false;

  for(unsigned i=0; i<functionType->getNumParams(); ++i)
    if(functionType->getParamType(i) != ParamTys[i])
      return false;

  return true;
}

static const APInt *valueAsConstantInt(Value *value)
{
  ConstantInt *constant = dyn_cast<ConstantInt>(value);
  if(!constant)
    return NULL;

  return &constant->getValue();
}

static const int ERROR_NOT_CONSTANT = 1;
static const int ERROR_OVERFLOW = 2;

static int valueAsUint64(uint64_t &result, Value *value)
{
  ConstantInt *constant = dyn_cast<ConstantInt>(value);
  if(!constant)
    return ERROR_NOT_CONSTANT;

  const APInt& ap = constant->getValue();
  if(ap.ugt(UINT64_MAX))
    return ERROR_OVERFLOW;

  result = ap.getZExtValue();
  return 0;
}

class MacroModulePass : public PassInfoMixin<MacroModulePass>
{
public:
  PreservedAnalyses run(Module &module, ModuleAnalysisManager &)
  {
    LLVMContext &context = module.getContext();
    const DataLayout &dataLayout = module.getDataLayout();

    Type *voidType = Type::getVoidTy(context);
    PointerType *opaquePointerType = PointerType::get(context, 0);
    IntegerType *int32Type = IntegerType::getInt32Ty(context);
    IntegerType *int8Type = IntegerType::getInt8Ty(context);

    unsigned int sizeWidth = dataLayout.getPointerSizeInBits();
    IntegerType *sizeType = IntegerType::getIntNTy(context, sizeWidth);

    SMDiagnostic err;

    std::unique_ptr<Module> macroModule = parseIRFile(MacroFilename::get(), err, context);
    if(!macroModule)
      report_fatal_error("failed to load macro module:" + Twine(MacroFilename::get()) + " (note: textual IR is not supported when invoked via clang, this may or may not be the issue)", false);

    auto moduleSplitIt = --module.end();
    if(Linker::linkModules(module, std::move(macroModule)))
      report_fatal_error("failed to link macro module:" + Twine(MacroFilename::get()), false);
    ++moduleSplitIt;

    SmallVector<std::reference_wrapper<Function>> mainFunctions(module.begin(), moduleSplitIt);
    SmallVector<std::reference_wrapper<Function>> macroFunctions(moduleSplitIt, module.end());

    Function *macroDef = nullptr;
    Function *macroCall = nullptr;
    Function *macroCount = nullptr;
    Function *macroIndex = nullptr;
    Function *macroArray = nullptr;

    for(Function &function: macroFunctions)
    {
      StringRef name = function.getName();
      if(!macroDef && name == "macro_def")
      {
        macroDef = &function;
        if(!functionIsDefined(*macroDef) || !functionCheckSignature(*macroDef, voidType, {}))
          report_fatal_error("invalid definition of macro_def: macro_def must be a defined function with the following signature (without name mangling): void macro_def(void)", false);
      }
      else if(!macroCall && name == "macro_call")
      {
        macroCall = &function;
        if(functionIsDefined(*macroCall) || !functionCheckSignature(*macroCall, voidType, {}))
          report_fatal_error("invalid definition of macro_call: macro_call must be a external function with the following signature (without name mangling): void macro_call(void)", false);
      }
      else if(!macroCount && name == "macro_count")
      {
        macroCount = &function;
        if(functionIsDefined(*macroCount) || !functionCheckSignature(*macroCount, sizeType, {}))
          report_fatal_error("invalid definition of macro_count: macro_count must be a external function with the following signature (without name mangling): size_t macro_count(void)", false);
      }
      else if(!macroIndex && name == "macro_index")
      {
        macroIndex = &function;
        if(functionIsDefined(*macroIndex) || !functionCheckSignature(*macroIndex, sizeType, {}))
          report_fatal_error("invalid definition of macro_index: macro_index must be a external function with the following signature (without name mangling): size_t macro_index(void)", false);
      }
      else if(!macroArray && name == "macro_array")
      {
        macroArray = &function;
        if(functionIsDefined(*macroArray) || !functionCheckSignature(*macroArray, opaquePointerType, {sizeType, sizeType, sizeType}))
          report_fatal_error("invalid definition of macro_array: macro_array must be a external function with the following signature (without name mangling): void *macro_array(size_t id, size_t size, size_t alignment)", false);
      }
    }

    if(!macroDef)
      report_fatal_error("missing definition of void macro_def(void) (without name mangling)", false);

    Function *newMacroDef = nullptr;

    SmallDenseMap<Function *, Function *> macroFunctionsMap;
    for(Function &function: macroFunctions)
      if(functionIsDefined(function))
      {
        FunctionType *functionType = function.getFunctionType();

        SmallVector<Type *> newParams;
        newParams.push_back(opaquePointerType);
        newParams.push_back(opaquePointerType);
        newParams.push_back(sizeType);
        newParams.insert(newParams.end(), functionType->param_begin(), functionType->param_end());

        FunctionType *newFunctionType = FunctionType::get(functionType->getReturnType(), newParams, functionType->isVarArg());
        Function *newFunction = Function::Create(newFunctionType, GlobalValue::LinkageTypes::InternalLinkage, function.getName() + ".clone", module);

        ValueToValueMapTy VMap;
        for(size_t i=0; i<function.arg_size(); ++i)
          VMap.insert({ function.getArg(i), newFunction->getArg(i+3) });

        SmallVector<ReturnInst *> Returns;
        CloneFunctionInto(newFunction, &function, VMap, CloneFunctionChangeType::LocalChangesOnly, Returns);

        macroFunctionsMap.insert({&function, newFunction});
        if(&function == macroDef)
          newMacroDef = newFunction;
      }

    // We have already found a definition for macro_def() above (or else we
    // would have erred out) so this should never happen.
    assert(newMacroDef);

    struct ArraySpec
    {
      uint64_t size;
      uint64_t alignment;
    };

    SmallDenseMap<uint64_t, ArraySpec> arraySpecs;

    FunctionType *lambdaFunctionType = FunctionType::get(voidType, opaquePointerType, false);

    for(auto [function, newFunction]: macroFunctionsMap)
    {
      for(BasicBlock &block : *function)
        for(Instruction &instr : make_early_inc_range(make_range(block.begin(), --block.end())))
          if(CallInst *callInstr = dyn_cast<CallInst>(&instr))
          {
            Function *calledFunction = callInstr->getCalledFunction();
            if(calledFunction == macroCall)
            {
              IRBuilder<> builder(callInstr);

              CallInst *newCallInstr = builder.CreateIntrinsic(Intrinsic::trap, {}, {});
              callInstr->replaceAllUsesWith(newCallInstr);
              callInstr->eraseFromParent();
            }
            else if(calledFunction == macroIndex)
            {
              IRBuilder<> builder(callInstr);

              CallInst *newCallInstr = builder.CreateIntrinsic(Intrinsic::trap, {}, {});
              callInstr->replaceAllUsesWith(builder.getIntN(sizeWidth, 0));
              callInstr->eraseFromParent();
            }
          }

      for(BasicBlock &block : *newFunction)
      {
        Value *lambdaFunction = newFunction->getArg(0);
        Value *lambdaContext = newFunction->getArg(1);
        Value *lambdaIndex = newFunction->getArg(2);

        for(Instruction &instr : make_early_inc_range(make_range(block.begin(), --block.end())))
          if(CallInst *callInstr = dyn_cast<CallInst>(&instr))
          {
            Function *calledFunction = callInstr->getCalledFunction();
            if(calledFunction == macroCall)
            {
              IRBuilder<> builder(callInstr);

              CallInst *newCallInstr = builder.CreateCall(lambdaFunctionType, lambdaFunction, lambdaContext);
              callInstr->replaceAllUsesWith(newCallInstr);
              callInstr->eraseFromParent();
            }
            else if(calledFunction == macroIndex)
            {
              callInstr->replaceAllUsesWith(lambdaIndex);
              callInstr->eraseFromParent();
            }
            else if(calledFunction == macroArray)
            {
              // Note: We kinda relied on the frontend to do all necessary
              //       constant folding.
              //
              // Note: While we are checking for overflow, size_t is always
              //       smaller than uint64_t until the day we start using
              //       128-bit computer, so the overflow should never happen.

              uint64_t id;
              switch(valueAsUint64(id, callInstr->getOperand(0)))
              {
              case ERROR_NOT_CONSTANT:
                report_fatal_error("id argument passed to void *macro_array(size_t id, size_t size, size_t alignment) must be a constant");
              case ERROR_OVERFLOW:
                report_fatal_error("overflow on id argument passed to void *macro_array(size_t id, size_t size, size_t alignment)");
              }

              uint64_t size;
              switch(valueAsUint64(size, callInstr->getOperand(1)))
              {
              case ERROR_NOT_CONSTANT:
                report_fatal_error("size argument passed to void *macro_array(size_t id, size_t size, size_t alignment) must be a constant");
              case ERROR_OVERFLOW:
                report_fatal_error("overflow on size argument passed to void *macro_array(size_t id, size_t size, size_t alignment)");
              }

              uint64_t alignment;
              switch(valueAsUint64(alignment, callInstr->getOperand(2)))
              {
              case ERROR_NOT_CONSTANT:
                report_fatal_error("alignment argument passed to void *macro_array(size_t id, size_t size, size_t alignment) must be a constant");
              case ERROR_OVERFLOW:
                report_fatal_error("overflow on alignment argument passed to void *macro_array(size_t id, size_t size, size_t alignment)");
              }

              if(alignment == 0)
                report_fatal_error("alignment argument passed to void *macro_array(size_t id, size_t size, size_t alignment) must be a non-zero");

              if(size % alignment != 0)
                report_fatal_error("alignment argument passed to void *macro_array(size_t id, size_t size, size_t alignment) must divide size");

              auto [it, success] = arraySpecs.insert({id, { .size = size, .alignment = alignment, }});
              if(!success && (it->second.size != size || it->second.alignment != alignment))
                report_fatal_error("multiple call to void *macro_array(size_t id, size_t size, size_t alignment) with same id but different size or alignment");
            }
            else if(auto it = macroFunctionsMap.find(calledFunction); it != macroFunctionsMap.end())
            {
              IRBuilder<> builder(callInstr);

              SmallVector<Value *> newArgs;
              newArgs.push_back(lambdaFunction);
              newArgs.push_back(lambdaContext);
              newArgs.push_back(lambdaIndex);
              newArgs.insert(newArgs.end(), callInstr->arg_begin(), callInstr->arg_end());

              CallInst *newCallInstr = builder.CreateCall(it->second->getFunctionType(), it->second, newArgs);
              callInstr->replaceAllUsesWith(newCallInstr);
              callInstr->eraseFromParent();
            }
          }
      }
    }

    // According to LLVM documentation
    // https://llvm.org/docs/LangRef.html#variable-argument-handling-intrinsics,
    // the type for va_list is { ptr } except in the case of Unix x86_64
    // platforms in which case the type for va_list is { i32, i32, ptr, ptr }.
    //
    // FIXME: We check for whether we are on Unix by checking if we are not on
    //        Windows. This is technically not correct but should work in most
    //        of the cases.
    Type *vaListType;
    if(llvm::Triple triple(module.getTargetTriple()); triple.getArch() == Triple::ArchType::x86_64 && !triple.isOSWindows())
      vaListType = StructType::get(context, { int32Type, int32Type, opaquePointerType, opaquePointerType, });
    else
      vaListType = StructType::get(context, opaquePointerType);

    size_t lambdaCount = 0;
    for(Function &function: mainFunctions)
      if(functionIsDefined(function))
      {
        ///////////////////////////////////////////////////////////////////////////
        // Create the type for lambda context out of original function signature //
        ///////////////////////////////////////////////////////////////////////////
        FunctionType *functionType = function.getFunctionType();

        SmallVector<Type *> elementTypes;
        elementTypes.assign(functionType->param_begin(), functionType->param_end());

        size_t vaListIndex;
        if(functionType->isVarArg())
        {
          vaListIndex = elementTypes.size();
          elementTypes.push_back(vaListType);
        }

        size_t returnStorageIndex;
        if(!functionType->getReturnType()->isVoidTy())
        {
          returnStorageIndex = elementTypes.size();
          elementTypes.push_back(functionType->getReturnType());
        }

        StructType *lambdaContextType = StructType::get(context, elementTypes);

        ///////////////////////////////////////////////////////////
        // Create the lambda function from the original function //
        ///////////////////////////////////////////////////////////
        Function *lambdaFunction = Function::Create(lambdaFunctionType, GlobalValue::LinkageTypes::InternalLinkage, function.getName() + ".lambda", module);
        BasicBlock *lambdaEntryBlock = BasicBlock::Create(context, "", lambdaFunction);
        {
          IRBuilder<> builder(lambdaEntryBlock);

          Argument *lambdaContext = lambdaFunction->getArg(0);

          for(size_t i=0; i<functionType->getNumParams(); ++i)
          {
            Value *argValuePointer = builder.CreateGEP(lambdaContextType, lambdaContext, { builder.getInt32(0), builder.getInt32(i) });
            Value *argValue = builder.CreateLoad(functionType->getParamType(i), argValuePointer);
            function.getArg(i)->replaceAllUsesWith(argValue);
          }

          Value *vaListPointer;
          if(functionType->isVarArg())
            vaListPointer = builder.CreateGEP(lambdaContextType, lambdaContext, { builder.getInt32(0), builder.getInt32(vaListIndex) });

          Value *returnStoragePointer;
          if(!functionType->getReturnType()->isVoidTy())
            returnStoragePointer = builder.CreateGEP(lambdaContextType, lambdaContext, { builder.getInt32(0), builder.getInt32(returnStorageIndex) });

          for(BasicBlock& block : function)
          {
            if(functionType->isVarArg())
              for(Instruction &instr : make_early_inc_range(make_range(block.begin(), --block.end())))
                if(VAStartInst *vaStartInstr = dyn_cast<VAStartInst>(&instr))
                {
                  IRBuilder<> builder(vaStartInstr);
                  builder.CreateBinaryIntrinsic(Intrinsic::vacopy, vaStartInstr->getArgList(), vaListPointer);
                  vaStartInstr->eraseFromParent();
                }

            if(!functionType->getReturnType()->isVoidTy())
              if(ReturnInst *returnInstr = dyn_cast<ReturnInst>(&block.back()))
              {
                IRBuilder<> builder(returnInstr);
                builder.CreateStore(returnInstr->getReturnValue(), returnStoragePointer);
                builder.CreateRetVoid();
                returnInstr->eraseFromParent();
              }
          }

          builder.CreateBr(&*function.begin());
        }
        lambdaFunction->splice(lambdaFunction->end(), &function, function.begin(), function.end());

        /////////////////////////////////////////////////////
        // Replace the original function with a trampoline //
        /////////////////////////////////////////////////////
        BasicBlock *trampolineBlock = BasicBlock::Create(context, "", &function);
        {
          IRBuilder<> builder(trampolineBlock);

          Value *lambdaContext = builder.CreateAlloca(lambdaContextType);

          for(size_t i=0; i<function.arg_size(); ++i)
          {
            Value *argValuePointer = builder.CreateGEP(lambdaContextType, lambdaContext, { builder.getInt32(0), builder.getInt32(i) });
            builder.CreateStore(function.getArg(i), argValuePointer);
          }

          Value *vaListPointer;
          if(functionType->isVarArg())
            vaListPointer = builder.CreateGEP(lambdaContextType, lambdaContext, { builder.getInt32(0), builder.getInt32(vaListIndex) });

          if(functionType->isVarArg())
            builder.CreateUnaryIntrinsic(Intrinsic::vastart, vaListPointer);

          Value *lambdaIndex = builder.getIntN(sizeWidth, lambdaCount++);

          builder.CreateCall(newMacroDef->getFunctionType(), newMacroDef, {lambdaFunction, lambdaContext, lambdaIndex});

          if(functionType->isVarArg())
            builder.CreateUnaryIntrinsic(Intrinsic::vaend, vaListPointer);

          if(!functionType->getReturnType()->isVoidTy())
          {
            Value *returnStoragePointer = builder.CreateGEP(lambdaContextType, lambdaContext, { builder.getInt32(0), builder.getInt32(returnStorageIndex) });
            Value *returnStorage = builder.CreateLoad(functionType->getReturnType(), returnStoragePointer);
            builder.CreateRet(returnStorage);
          }
          else
            builder.CreateRetVoid();
        }
      }

    if(macroCount)
    {
      Function *newMacroCount = Function::Create(macroCount->getFunctionType(), llvm::GlobalValue::InternalLinkage, "macro_count.def", &module);

      BasicBlock *block = BasicBlock::Create(context, "", newMacroCount);
      IRBuilder<> builder(block);
      builder.CreateRet(builder.getIntN(sizeWidth, lambdaCount));

      macroCount->replaceAllUsesWith(newMacroCount);
      macroCount->eraseFromParent();
    }

    if(macroArray)
    {
      Function *newMacroArray = Function::Create(macroArray->getFunctionType(), llvm::GlobalValue::InternalLinkage, "macro_array.def", &module);

      BasicBlock *switchBlock = BasicBlock::Create(context, "", newMacroArray);
      BasicBlock *failedBlock = BasicBlock::Create(context, "", newMacroArray);

      {
        IRBuilder<> builder(failedBlock);
        builder.CreateIntrinsic(Intrinsic::trap, {}, {});
        builder.CreateUnreachable();
      }

      {
        IRBuilder<> builder(switchBlock);
        SwitchInst *switchInstr = builder.CreateSwitch(newMacroArray->getArg(0), failedBlock, arraySpecs.size());
        for(auto&& [id, arraySpec]: arraySpecs)
        {
          BasicBlock *successBlock = BasicBlock::Create(context, "", newMacroArray);
          {
            IRBuilder<> builder(successBlock);

            // FIXME: Check for overflow.
            ArrayType *arrayType = ArrayType::get(Type::getInt8Ty(context), arraySpec.size * lambdaCount);
            GlobalVariable *globalVariable = new GlobalVariable(module, arrayType, false, llvm::GlobalValue::InternalLinkage, ConstantAggregateZero::get(arrayType));
            globalVariable->setAlignment(Align(arraySpec.alignment));

            builder.CreateRet(globalVariable);
          }
          switchInstr->addCase(builder.getIntN(sizeWidth, id), successBlock);
        }
      }

      macroArray->replaceAllUsesWith(newMacroArray);
      macroArray->eraseFromParent();
    }

    return PreservedAnalyses::none();
  }
};

extern "C" PassPluginLibraryInfo llvmGetPassPluginInfo()
{
  return {
    .APIVersion = LLVM_PLUGIN_API_VERSION,
    .PluginName = "Macro",
    .PluginVersion = "v0.1",
    .RegisterPassBuilderCallbacks = [](PassBuilder &PB) {
      PB.registerPipelineParsingCallback([](StringRef Name, ModulePassManager &PM, ArrayRef<PassBuilder::PipelineElement>) -> bool {
          if(Name != "macro-module-pass")
            return false;

          PM.addPass(MacroModulePass());
          return true;
      });
      PB.registerPipelineStartEPCallback([](ModulePassManager &PM, OptimizationLevel) {
          PM.addPass(MacroModulePass());
      });
    },
  };
}

