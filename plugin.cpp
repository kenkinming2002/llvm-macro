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

static bool functionIsVoid(Function& f)
{
  assert(f.getReturnType());
  return f.getReturnType()->isVoidTy() && f.arg_size() == 0;
}

class MacroModulePass : public PassInfoMixin<MacroModulePass>
{
public:
  PreservedAnalyses run(Module &module, ModuleAnalysisManager &)
  {
    LLVMContext &context = module.getContext();

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

    for(Function &function: macroFunctions)
    {
      StringRef name = function.getName();
      if(!macroDef && name == "macro_def")
      {
        macroDef = &function;
        if(!(functionIsDefined(*macroDef) && functionIsVoid(*macroDef)))
          report_fatal_error("invalid definition of macro_def: macro_def must be a defined function with the following signature (without name mangling): void macro_def(void)", false);
      }
      else if(!macroCall && name == "macro_call")
      {
        macroCall = &function;
        if(!(!functionIsDefined(*macroCall) && functionIsVoid(*macroCall)))
          report_fatal_error("invalid definition of macro_call: macro_call must be a external function with the following signature (without name mangling): void macro_call(void)", false);
      }
    }

    if(!macroDef)
      report_fatal_error("missing definition of void macro_def(void) (without name mangling)", false);

    Type *voidType = Type::getVoidTy(context);
    PointerType *opaquePointerType = PointerType::get(context, 0);
    IntegerType *int32Type = IntegerType::getInt32Ty(context);

    Function *newMacroDef = nullptr;

    SmallDenseMap<Function *, Function *> macroFunctionsMap;
    for(Function &function: macroFunctions)
      if(functionIsDefined(function))
      {
        FunctionType *functionType = function.getFunctionType();

        SmallVector<Type *> newParams;
        newParams.push_back(opaquePointerType);
        newParams.push_back(opaquePointerType);
        newParams.insert(newParams.end(), functionType->param_begin(), functionType->param_end());

        FunctionType *newFunctionType = FunctionType::get(functionType->getReturnType(), newParams, functionType->isVarArg());
        Function *newFunction = Function::Create(newFunctionType, GlobalValue::LinkageTypes::InternalLinkage, function.getName() + ".clone", module);

        ValueToValueMapTy VMap;
        for(size_t i=0; i<function.arg_size(); ++i)
          VMap.insert({ function.getArg(i), newFunction->getArg(i+2) });

        SmallVector<ReturnInst *> Returns;
        CloneFunctionInto(newFunction, &function, VMap, CloneFunctionChangeType::LocalChangesOnly, Returns);

        macroFunctionsMap.insert({&function, newFunction});
        if(&function == macroDef)
          newMacroDef = newFunction;
      }

    // We have already found a definition for macro_def() above (or else we
    // would have erred out) so this should never happen.
    assert(newMacroDef);

    FunctionType *lambdaFunctionType = FunctionType::get(voidType, opaquePointerType, false);

    for(auto [function, newFunction]: macroFunctionsMap)
    {
      for(BasicBlock &block : *function)
        for(Instruction &instr : make_early_inc_range(make_range(block.begin(), --block.end())))
          if(CallInst *callInstr = dyn_cast<CallInst>(&instr))
            if(callInstr->getCalledFunction() == macroCall)
            {
              IRBuilder<> builder(callInstr);

              CallInst *newCallInstr = builder.CreateIntrinsic(Intrinsic::trap, {}, {});
              callInstr->replaceAllUsesWith(newCallInstr);
              callInstr->eraseFromParent();
            }

      for(BasicBlock &block : *newFunction)
      {
        Value *lambdaFunction = newFunction->getArg(0);
        Value *lambdaContext = newFunction->getArg(1);

        for(Instruction &instr : make_early_inc_range(make_range(block.begin(), --block.end())))
          if(CallInst *callInstr = dyn_cast<CallInst>(&instr))
          {
            if(callInstr->getCalledFunction() == macroCall)
            {
              IRBuilder<> builder(callInstr);

              CallInst *newCallInstr = builder.CreateCall(lambdaFunctionType, lambdaFunction, lambdaContext);
              callInstr->replaceAllUsesWith(newCallInstr);
              callInstr->eraseFromParent();
            }
            else if(auto it = macroFunctionsMap.find(callInstr->getCalledFunction()); it != macroFunctionsMap.end())
            {
              IRBuilder<> builder(callInstr);

              SmallVector<Value *> newArgs;
              newArgs.push_back(lambdaFunction);
              newArgs.push_back(lambdaContext);
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

          builder.CreateCall(newMacroDef->getFunctionType(), newMacroDef, {lambdaFunction, lambdaContext});

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

