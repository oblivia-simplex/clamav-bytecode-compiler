set(LLVM_NO_RTTI 1)

add_clang_library(clangChecker
  AdjustedReturnValueChecker.cpp
  ArrayBoundChecker.cpp
  AttrNonNullChecker.cpp
  BasicConstraintManager.cpp
  BasicObjCFoundationChecks.cpp
  BasicStore.cpp
  BasicValueFactory.cpp
  BugReporter.cpp
  BugReporterVisitors.cpp
  BuiltinFunctionChecker.cpp
  CFRefCount.cpp
  CallAndMessageChecker.cpp
  CallInliner.cpp
  CastToStructChecker.cpp
  CheckDeadStores.cpp
  CheckObjCDealloc.cpp
  CheckObjCInstMethSignature.cpp
  CheckSecuritySyntaxOnly.cpp
  CheckSizeofPointer.cpp
  Checker.cpp
  CocoaConventions.cpp
  DereferenceChecker.cpp
  DivZeroChecker.cpp
  Environment.cpp
  ExplodedGraph.cpp
  FixedAddressChecker.cpp
  FlatStore.cpp
  GRBlockCounter.cpp
  GRCoreEngine.cpp
  GRExprEngine.cpp
  GRExprEngineExperimentalChecks.cpp
  GRState.cpp
  LLVMConventionsChecker.cpp
  MacOSXAPIChecker.cpp
  MallocChecker.cpp
  ManagerRegistry.cpp
  MemRegion.cpp
  NSAutoreleasePoolChecker.cpp
  NSErrorChecker.cpp
  NoReturnFunctionChecker.cpp
  OSAtomicChecker.cpp
  ObjCUnusedIVarsChecker.cpp
  PathDiagnostic.cpp
  PointerArithChecker.cpp
  PointerSubChecker.cpp
  PthreadLockChecker.cpp
  RangeConstraintManager.cpp
  RegionStore.cpp
  ReturnPointerRangeChecker.cpp
  ReturnStackAddressChecker.cpp
  ReturnUndefChecker.cpp
  SVals.cpp
  SValuator.cpp
  SimpleConstraintManager.cpp
  SimpleSValuator.cpp
  Store.cpp
  SymbolManager.cpp
  UndefBranchChecker.cpp
  UndefCapturedBlockVarChecker.cpp
  UndefResultChecker.cpp
  UndefinedArraySubscriptChecker.cpp
  UndefinedAssignmentChecker.cpp
  UnixAPIChecker.cpp
  VLASizeChecker.cpp
  ValueManager.cpp
  )
