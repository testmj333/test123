//===- GenerateCubinAccessors.cpp - MLIR GPU lowering passes --------------===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================
//
// This file implements a pass to generate LLVMIR functions that return the
// data stored in nvvm.cubin char* blob.
//
//===----------------------------------------------------------------------===//

#include "mlir/Conversion/GPUToCUDA/GPUToCUDAPass.h"
#include "mlir/Dialect/GPU/GPUDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/Identifier.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/StandardTypes.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassRegistry.h"

#include "llvm/ADT/STLExtras.h"

namespace mlir {
namespace {

// TODO(herhut): Move to shared location.
constexpr const char *kCubinAnnotation = "nvvm.cubin";
constexpr const char *kCubinGetterAnnotation = "nvvm.cubingetter";
constexpr const char *kCubinGetterSuffix = "_cubin";
constexpr const char *kCubinStorageSuffix = "_cubin_cst";

/// A pass which moves cubin from function attributes in nested modules
/// to global strings and generates getter functions.
///
/// The GpuKernelToCubinPass annotates kernels functions with compiled device
/// code blobs. These functions reside in nested modules generated by
/// GpuKernelOutliningPass. This pass consumes these modules and moves the cubin
/// blobs back to the parent module as global strings and generates accessor
/// functions for them. The external kernel functions (also generated by the
/// outlining pass) are annotated with the symbol of the cubin accessor.
class GpuGenerateCubinAccessorsPass
    : public ModulePass<GpuGenerateCubinAccessorsPass> {
private:
  LLVM::LLVMType getIndexType() {
    unsigned bits =
        llvmDialect->getLLVMModule().getDataLayout().getPointerSizeInBits();
    return LLVM::LLVMType::getIntNTy(llvmDialect, bits);
  }

  // Inserts a global constant string containing `blob` into the parent module
  // of `kernelFunc` and generates the function that returns the address of the
  // first character of this string.
  // TODO(herhut): consider fusing this pass with launch-func-to-cuda.
  void generate(FuncOp kernelFunc, StringAttr blob) {
    auto stubFunc = getModule().lookupSymbol<FuncOp>(kernelFunc.getName());
    if (!stubFunc) {
      kernelFunc.emitError(
          "corresponding external function not found in parent module");
      return signalPassFailure();
    }

    Location loc = stubFunc.getLoc();
    SmallString<128> nameBuffer(stubFunc.getName());
    auto module = stubFunc.getParentOfType<ModuleOp>();
    assert(module && "function must belong to a module");

    // Insert the getter function just after the original function.
    OpBuilder moduleBuilder(module.getBody(), module.getBody()->begin());
    moduleBuilder.setInsertionPointAfter(stubFunc.getOperation());
    auto getterType = moduleBuilder.getFunctionType(
        llvm::None, LLVM::LLVMType::getInt8PtrTy(llvmDialect));
    nameBuffer.append(kCubinGetterSuffix);
    auto result = moduleBuilder.create<FuncOp>(
        loc, StringRef(nameBuffer), getterType, ArrayRef<NamedAttribute>());
    Block *entryBlock = result.addEntryBlock();

    // Drop the getter suffix before appending the storage suffix.
    nameBuffer.resize(stubFunc.getName().size());
    nameBuffer.append(kCubinStorageSuffix);

    // Obtain the address of the first character of the global string containing
    // the cubin and return from the getter.
    OpBuilder builder(entryBlock);
    Value *startPtr = LLVM::createGlobalString(
        loc, builder, StringRef(nameBuffer), blob.getValue(), llvmDialect);
    builder.create<LLVM::ReturnOp>(loc, startPtr);

    // Store the name of the getter on the function for easier lookup.
    stubFunc.setAttr(kCubinGetterAnnotation, builder.getSymbolRefAttr(result));
  }

public:
  void runOnModule() override {
    llvmDialect = getContext().getRegisteredDialect<LLVM::LLVMDialect>();

    auto modules = getModule().getOps<ModuleOp>();
    for (auto module : llvm::make_early_inc_range(modules)) {
      if (!module.getAttrOfType<UnitAttr>(
              gpu::GPUDialect::getKernelModuleAttrName()))
        continue;
      for (auto func : module.getOps<FuncOp>()) {
        if (StringAttr blob = func.getAttrOfType<StringAttr>(kCubinAnnotation))
          generate(func, blob);
      }
      module.erase();
    }
  }

private:
  LLVM::LLVMDialect *llvmDialect;
};

} // anonymous namespace

std::unique_ptr<OpPassBase<ModuleOp>> createGenerateCubinAccessorPass() {
  return std::make_unique<GpuGenerateCubinAccessorsPass>();
}

static PassRegistration<GpuGenerateCubinAccessorsPass>
    pass("generate-cubin-accessors",
         "Generate LLVMIR functions that give access to cubin data");

} // namespace mlir
