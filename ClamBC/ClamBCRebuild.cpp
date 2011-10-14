/*
 *  Compile LLVM bytecode to ClamAV bytecode.
 *
 *  Copyright (C) 2011 Sourcefire, Inc.
 *
 *  Authors: Török Edvin
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */
#include "llvm/System/DataTypes.h"
#include "ClamBCModule.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/BasicBlock.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/Module.h"
#include "llvm/Pass.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/InstVisitor.h"
#include "llvm/Support/IRBuilder.h"
#include "llvm/Support/TargetFolder.h"

using namespace llvm;

class ClamBCRebuild : public ModulePass, public InstVisitor<ClamBCRebuild> {
public:
  static char ID;
  explicit ClamBCRebuild() : ModulePass(&ID) {}
  virtual const char *getPassName() const { return "ClamAV bytecode backend rebuilder"; }

  void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addRequired<TargetData>();
  }

  bool runOnModule(Module &M) {
      FMap.clear();
      TD = &getAnalysis<TargetData>();
      Context = &M.getContext();

      std::vector<Function*> functions;
      for (Module::iterator I=M.begin(),E=M.end(); I != E; ++I) {
	  Function *F = &*I;
	  if (F->isDeclaration())
	      continue;
	  functions.push_back(F);
      }
      for (std::vector<Function*>::iterator I=functions.begin(),
	   E=functions.end(); I != E; ++I) {
	  FMap[*I] = createFunction(*I, &M);
      }
      for (std::vector<Function*>::iterator I=functions.begin(),
	   E=functions.end(); I != E; ++I) {
	  Function *F = *I;
	  runOnFunction(F);
	  F->deleteBody();
      }
      for (std::vector<Function*>::iterator I=functions.begin(),
	   E=functions.end(); I != E; ++I) {
	  Function *F = *I;
	  F->eraseFromParent();
      }
      return true;
  }
private:
  typedef DenseMap<const Function*, Function*> FMapTy;
  typedef DenseMap<const BasicBlock*, BasicBlock*> BBMapTy;
  typedef DenseMap<const Value*, Value*> ValueMapTy;
  typedef SmallVector<std::pair<const Value*, int64_t>,4 > IndicesVectorTy;

  FMapTy FMap;
  BBMapTy BBMap;
  ValueMapTy VMap;
  TargetData *TD;
  LLVMContext *Context;
  IRBuilder<true,TargetFolder> *Builder;

  void stop(const std::string &Msg, const llvm::Instruction *I) {
    ClamBCModule::stop(Msg, I);
  }
  friend class InstVisitor<ClamBCRebuild>;

  const Type *getInnerElementType(const CompositeType *CTy)
  {
      const Type *ETy;
      // get pointer to first element
      do {
	  assert(CTy->indexValid(0u));
	  ETy = CTy->getTypeAtIndex(0u);
	  CTy = dyn_cast<CompositeType>(ETy);
      } while (CTy);
      assert(ETy->isIntegerTy());
      return ETy;
  }

  const Type *rebuildType(const Type *Ty)
  {
      if (Ty->isIntegerTy() || Ty->isVoidTy())
	  return Ty;
      if (const PointerType *PTy = dyn_cast<PointerType>(Ty))
	  return PointerType::getUnqual(getInnerElementType(PTy));
      if (const CompositeType *CTy = dyn_cast<CompositeType>(Ty)) {
	  const Type *ETy = getInnerElementType(CTy);
	  unsigned bytes = TD->getTypeAllocSize(CTy);
	  unsigned esize = TD->getTypeAllocSize(ETy);
	  unsigned n = bytes / esize;
	  assert(!(bytes % esize));
	  return ArrayType::get(ETy, n);
      }
      llvm_unreachable("unknown type");
  }

  ConstantInt *u64const(uint64_t n)
  {
      return ConstantInt::get(Type::getInt64Ty(*Context), n);
  }

  ConstantInt *i32const(int32_t n)
  {
      return ConstantInt::get(Type::getInt32Ty(*Context), n, true);
  }

  void visitAllocaInst(AllocaInst &AI) {
      if (!isa<ConstantInt>(AI.getArraySize()))
	  stop("VLA not supported", &AI);
      uint64_t n = cast<ConstantInt>(AI.getArraySize())->getZExtValue();
      const Type *Ty = rebuildType(AI.getAllocatedType());
      if (const ArrayType *ATy = dyn_cast<ArrayType>(Ty)) {
	  Ty = ATy->getElementType();
	  //TODO: check for overflow
	  n *= ATy->getNumElements();
      }
      Value *V = Builder->CreateAlloca(Ty, n == 1 ? 0 : u64const(n), AI.getName());
      VMap[&AI] = Builder->CreatePointerCast(V, AI.getType(), AI.getName());
  }

  Constant *mapConstant(Constant *C)
  {
      //TODO: compute any gep exprs here
      return C;
  }

  Value *mapValue(Value *V)
  {
      if (Constant *C = dyn_cast<Constant>(V))
	  return mapConstant(C);
      Value *NV = VMap[V];
      assert(NV);
      return NV;
  }

  Value *mapPointer(Value *P)
  {
      Value *PV = mapValue(P);
      PV = Builder->CreatePointerCast(PV, P->getType(), "rbcast");
      return PV;
  }

  BasicBlock *mapBlock(const BasicBlock *BB)
  {
      BasicBlock *NBB =  BBMap[BB];
      assert(NBB);
      return NBB;
  }

  void visitReturnInst(ReturnInst &I) {
      Value *V = I.getReturnValue();
      if (!V)
	  Builder->CreateRetVoid();
      else
	  Builder->CreateRet(mapValue(V));
  }

  void visitBranchInst(BranchInst &I) {
      if (I.isConditional()) {
	  Builder->CreateCondBr(mapValue(I.getCondition()),
				mapBlock(I.getSuccessor(0)),
				mapBlock(I.getSuccessor(1)));
      } else
	  Builder->CreateBr(mapBlock(I.getSuccessor(0)));
  }

  void visitSwitchInst(SwitchInst &I) {
      SwitchInst *SI = Builder->CreateSwitch(mapValue(I.getCondition()),
					     mapBlock(I.getDefaultDest()),
					     I.getNumCases());
      for (unsigned i=1;i<I.getNumCases();i++) {
	  BasicBlock *BB = mapBlock(I.getSuccessor(i));
	  SI->addCase(I.getCaseValue(i), BB);
      }
  }

  void visitUnreachableInst(UnreachableInst &I) {
      Builder->CreateUnreachable();
  }

  void visitICmpInst(ICmpInst &I) {
      VMap[&I] = Builder->CreateICmp(I.getPredicate(),
				     mapValue(I.getOperand(0)),
				     mapValue(I.getOperand(1)), I.getName());
  }

  void visitLoadInst(LoadInst &I) {
      VMap[&I] = Builder->CreateLoad(mapPointer(I.getPointerOperand()),
				     I.getName());
  }

  void visitStoreInst(StoreInst &I) {
      Builder->CreateStore(mapValue(I.getOperand(0)),
			   mapPointer(I.getPointerOperand()));
  }

  void visitGetElementPtrInst(GetElementPtrInst &II)
  {
      if (II.hasAllZeroIndices()) {
	  //just a bitcast
	  Value *V = mapValue(II.getOperand(0));
	  const Type *Ty = rebuildType(II.getType());
	  VMap[&II] = Builder->CreatePointerCast(V, Ty, "rbcast");
	  return;
      }
      //TODO: create inbounds GEPs if original one is inbounds

      int64_t BaseOffs;
      IndicesVectorTy VarIndices;
      const Type *i8pTy = PointerType::getUnqual(Type::getInt8Ty(*Context));
      const Type *i32Ty = Type::getInt32Ty(*Context);

      Value *P = const_cast<Value*>(DecomposeGEPExpression(&II, BaseOffs, VarIndices, TD));
      P = mapValue(P);
      const PointerType *PTy = cast<PointerType>(P->getType());
      unsigned divisor = TD->getTypeAllocSize(PTy->getElementType());
      if (BaseOffs % divisor)
	  P = Builder->CreateConstGEP1_64(P, BaseOffs / divisor, "rb.base");
      else {
	  P = Builder->CreatePointerCast(P, i8pTy);
	  P = Builder->CreateConstGEP1_64(P, BaseOffs, "rb.base8");
      }
      //TODO:add varindices too
      bool allDivisible = true;
      for (IndicesVectorTy::iterator I=VarIndices.begin(),E=VarIndices.end();
	   I != E; ++I) {
	  if (I->second % divisor) {
	      allDivisible = false;
	      break;
	  }
      }
      if (!allDivisible) {
	  divisor = 1;
	  P = Builder->CreatePointerCast(P, i8pTy);
      }
      Value *Sum = 0;
      for (IndicesVectorTy::iterator I=VarIndices.begin(),E=VarIndices.end();
	   I != E; ++I) {
	  int64_t m = I->second / divisor;
	  int32_t m2 = m;
	  assert((int64_t)m2 == m);
	  Value *V = Builder->CreateTruncOrBitCast(const_cast<Value*>(I->first), i32Ty);
	  if (m2 != 1)
	      V = Builder->CreateNSWMul(i32const(m), V);
	  if (Sum)
	      Sum = Builder->CreateNSWAdd(Sum, V);
	  else
	      Sum = V;
      }
      if (Sum)
	  P = Builder->CreateGEP(P, Sum);
      P = Builder->CreatePointerCast(P, II.getType(), II.getName());
      VMap[&II] = P;
  }


  Value *mapPHIValue(Value *V)
  {
      Value *NV;
      if (isa<PHINode>(V)) {
	  NV = VMap[V];
	  if (!NV) // break recursion
	      VMap[V] = NV = Builder->CreatePHI(V->getType());
	  return NV;
      }
      return mapValue(V);
  }

  void visitPHINode(PHINode &I)
  {
      PHINode *PN;
      if (Value *VV = VMap[&I]) {
	  PN = cast<PHINode>(VV);
      } else {
	  VMap[&I] = PN = Builder->CreatePHI(I.getType());
      }
      PN->reserveOperandSpace(I.getNumIncomingValues());
      for (unsigned i=0;i<PN->getNumIncomingValues();i++) {
	  Value *V = mapPHIValue(PN->getIncomingValue(i));
	  BasicBlock *BB = mapBlock(PN->getIncomingBlock(i));
	  PN->setIncomingBlock(i, BB);
	  PN->setIncomingValue(i, V);
      }
  }

  void visitCastInst(CastInst &I)
  {
      VMap[&I] = Builder->CreateCast(I.getOpcode(),
				     mapValue(I.getOperand(0)),
				     rebuildType(I.getType()),
				     I.getName());
  }

  void visitSelectInst(SelectInst &I)
  {
      VMap[&I] = Builder->CreateSelect(mapValue(I.getCondition()),
				       mapValue(I.getTrueValue()),
				       mapValue(I.getFalseValue()),
				       I.getName());
  }

  void visitCallInst(CallInst &I)
  {
      std::vector<Value*> params;
      Function *F = I.getCalledFunction();
      const FunctionType *FTy = F->getFunctionType();
      if (F->isDeclaration()) {
	  //APIcall, types preserved, no mapping of F
	  assert(!FTy->isVarArg());
	  for (unsigned i=0;i<FTy->getNumParams();i++) {
	      Value *V = mapValue(I.getOperand(i+1));
	      const Type *Ty = FTy->getParamType(i);
	      if (V->getType() != Ty)
		  V = Builder->CreateBitCast(V, Ty);
	      params.push_back(V);
	  }
	  VMap[&I] = Builder->CreateCall(F, params.begin(), params.end(),
					 I.getName());
	  return;
      }
      F = FMap[F];
      assert(F);
      for (unsigned i=0;i<FTy->getNumParams();i++) {
	  params.push_back(mapValue(I.getOperand(i+1)));
      }
      VMap[&I] = Builder->CreateCall(F, params.begin(), params.end(), I.getName());
  }

  void visitBinaryOperator(BinaryOperator &I)
  {
      VMap[&I] = Builder->CreateBinOp(I.getOpcode(),
				      mapValue(I.getOperand(0)),
				      mapValue(I.getOperand(1)),
				      I.getName());
  }

  void visitInstruction(Instruction &I) {
    stop("ClamAV bytecode backend rebuilder does not know about ", &I);
  }

  void runOnFunction(Function *F)
  {
      Function *NF = FMap[F];
      assert(NF);
      VMap.clear();
      BBMap.clear();
      TargetFolder TF(TD);
      for (Function::iterator I=F->begin(),E=F->end(); I != E; ++I) {
	  BasicBlock *BB = &*I;
	  BBMap[BB] = BasicBlock::Create(BB->getContext(), BB->getName(), NF, 0);
      }
      for (Function::iterator I=F->begin(),E=F->end(); I != E; ++I) {
	  BasicBlock *BB = &*I;
	  BasicBlock *NBB = BBMap[BB];
	  assert(NBB);
	  Builder = new IRBuilder<true,TargetFolder>(NBB, TF);

	  visit(BB);
	  delete Builder;
      }
  }

  Function* createFunction(const Function *F, Module *M)
  {
      unsigned i;
      std::vector<const Type*> params;
      const FunctionType *FTy = F->getFunctionType();
      assert(!F->isVarArg());
      for (i=0;i<FTy->getNumParams();i++) {
	  params.push_back(rebuildType(FTy->getParamType(i)));
      }

      FTy = FunctionType::get(rebuildType(FTy->getReturnType()), params, false);

      return Function::Create(FTy, GlobalValue::InternalLinkage, F->getName(),
			      M);
  }
};
char ClamBCRebuild::ID = 0;

llvm::ModulePass *createClamBCRebuild(void)
{
    return new ClamBCRebuild();
}