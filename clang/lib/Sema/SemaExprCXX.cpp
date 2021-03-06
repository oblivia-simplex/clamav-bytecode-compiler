//===--- SemaExprCXX.cpp - Semantic Analysis for Expressions --------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements semantic analysis for C++ expressions.
//
//===----------------------------------------------------------------------===//

#include "Sema.h"
#include "SemaInit.h"
#include "Lookup.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/CXXInheritance.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/PartialDiagnostic.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Parse/DeclSpec.h"
#include "clang/Parse/Template.h"
#include "llvm/ADT/STLExtras.h"
using namespace clang;

Action::TypeTy *Sema::getDestructorName(SourceLocation TildeLoc,
                                        IdentifierInfo &II, 
                                        SourceLocation NameLoc,
                                        Scope *S, const CXXScopeSpec &SS,
                                        TypeTy *ObjectTypePtr,
                                        bool EnteringContext) {
  // Determine where to perform name lookup.

  // FIXME: This area of the standard is very messy, and the current
  // wording is rather unclear about which scopes we search for the
  // destructor name; see core issues 399 and 555. Issue 399 in
  // particular shows where the current description of destructor name
  // lookup is completely out of line with existing practice, e.g.,
  // this appears to be ill-formed:
  //
  //   namespace N {
  //     template <typename T> struct S {
  //       ~S();
  //     };
  //   }
  //
  //   void f(N::S<int>* s) {
  //     s->N::S<int>::~S();
  //   }
  //
  // See also PR6358 and PR6359.
  QualType SearchType;
  DeclContext *LookupCtx = 0;
  bool isDependent = false;
  bool LookInScope = false;

  // If we have an object type, it's because we are in a
  // pseudo-destructor-expression or a member access expression, and
  // we know what type we're looking for.
  if (ObjectTypePtr)
    SearchType = GetTypeFromParser(ObjectTypePtr);

  if (SS.isSet()) {
    NestedNameSpecifier *NNS = (NestedNameSpecifier *)SS.getScopeRep();
    
    bool AlreadySearched = false;
    bool LookAtPrefix = true;
    if (!getLangOptions().CPlusPlus0x) {
      // C++ [basic.lookup.qual]p6:
      //   If a pseudo-destructor-name (5.2.4) contains a nested-name-specifier, 
      //   the type-names are looked up as types in the scope designated by the
      //   nested-name-specifier. In a qualified-id of the form:
      // 
      //     ::[opt] nested-name-specifier  ̃ class-name 
      //
      //   where the nested-name-specifier designates a namespace scope, and in
      //   a qualified-id of the form:
      //
      //     ::opt nested-name-specifier class-name ::  ̃ class-name 
      //
      //   the class-names are looked up as types in the scope designated by 
      //   the nested-name-specifier.
      //
      // Here, we check the first case (completely) and determine whether the
      // code below is permitted to look at the prefix of the 
      // nested-name-specifier (as we do in C++0x).
      DeclContext *DC = computeDeclContext(SS, EnteringContext);
      if (DC && DC->isFileContext()) {
        AlreadySearched = true;
        LookupCtx = DC;
        isDependent = false;
      } else if (DC && isa<CXXRecordDecl>(DC))
        LookAtPrefix = false;
    }
    
    // C++0x [basic.lookup.qual]p6:
    //   If a pseudo-destructor-name (5.2.4) contains a
    //   nested-name-specifier, the type-names are looked up as types
    //   in the scope designated by the nested-name-specifier. Similarly, in 
    //   a qualified-id of the form:
    //
    //     :: [opt] nested-name-specifier[opt] class-name :: ~class-name 
    //
    //   the second class-name is looked up in the same scope as the first.
    //
    // To implement this, we look at the prefix of the
    // nested-name-specifier we were given, and determine the lookup
    // context from that.
    //
    // We also fold in the second case from the C++03 rules quoted further 
    // above.
    NestedNameSpecifier *Prefix = 0;
    if (AlreadySearched) {
      // Nothing left to do.
    } else if (LookAtPrefix && (Prefix = NNS->getPrefix())) {
      CXXScopeSpec PrefixSS;
      PrefixSS.setScopeRep(Prefix);
      LookupCtx = computeDeclContext(PrefixSS, EnteringContext);
      isDependent = isDependentScopeSpecifier(PrefixSS);
    } else if (getLangOptions().CPlusPlus0x &&
               (LookupCtx = computeDeclContext(SS, EnteringContext))) {
      if (!LookupCtx->isTranslationUnit())
        LookupCtx = LookupCtx->getParent();
      isDependent = LookupCtx && LookupCtx->isDependentContext();
    } else if (ObjectTypePtr) {
      LookupCtx = computeDeclContext(SearchType);
      isDependent = SearchType->isDependentType();
    } else {
      LookupCtx = computeDeclContext(SS, EnteringContext);
      isDependent = LookupCtx && LookupCtx->isDependentContext();
    }
    
    LookInScope = false;
  } else if (ObjectTypePtr) {
    // C++ [basic.lookup.classref]p3:
    //   If the unqualified-id is ~type-name, the type-name is looked up
    //   in the context of the entire postfix-expression. If the type T
    //   of the object expression is of a class type C, the type-name is
    //   also looked up in the scope of class C. At least one of the
    //   lookups shall find a name that refers to (possibly
    //   cv-qualified) T.
    LookupCtx = computeDeclContext(SearchType);
    isDependent = SearchType->isDependentType();
    assert((isDependent || !SearchType->isIncompleteType()) && 
           "Caller should have completed object type");

    LookInScope = true;
  } else {
    // Perform lookup into the current scope (only).
    LookInScope = true;
  }

  LookupResult Found(*this, &II, NameLoc, LookupOrdinaryName);
  for (unsigned Step = 0; Step != 2; ++Step) {
    // Look for the name first in the computed lookup context (if we
    // have one) and, if that fails to find a match, in the sope (if
    // we're allowed to look there).
    Found.clear();
    if (Step == 0 && LookupCtx)
      LookupQualifiedName(Found, LookupCtx);
    else if (Step == 1 && LookInScope && S)
      LookupName(Found, S);
    else
      continue;

    // FIXME: Should we be suppressing ambiguities here?
    if (Found.isAmbiguous())
      return 0;

    if (TypeDecl *Type = Found.getAsSingle<TypeDecl>()) {
      QualType T = Context.getTypeDeclType(Type);
      // If we found the injected-class-name of a class template, retrieve the
      // type of that template.
      // FIXME: We really shouldn't need to do this.
      if (CXXRecordDecl *Record = dyn_cast<CXXRecordDecl>(Type))
        if (Record->isInjectedClassName())
          if (Record->getDescribedClassTemplate())
            T = Record->getDescribedClassTemplate()
                                           ->getInjectedClassNameType(Context);

      if (SearchType.isNull() || SearchType->isDependentType() ||
          Context.hasSameUnqualifiedType(T, SearchType)) {
        // We found our type!

        return T.getAsOpaquePtr();
      }
    }

    // If the name that we found is a class template name, and it is
    // the same name as the template name in the last part of the
    // nested-name-specifier (if present) or the object type, then
    // this is the destructor for that class.
    // FIXME: This is a workaround until we get real drafting for core
    // issue 399, for which there isn't even an obvious direction. 
    if (ClassTemplateDecl *Template = Found.getAsSingle<ClassTemplateDecl>()) {
      QualType MemberOfType;
      if (SS.isSet()) {
        if (DeclContext *Ctx = computeDeclContext(SS, EnteringContext)) {
          // Figure out the type of the context, if it has one.
          if (ClassTemplateSpecializationDecl *Spec
                          = dyn_cast<ClassTemplateSpecializationDecl>(Ctx))
            MemberOfType = Context.getTypeDeclType(Spec);
          else if (CXXRecordDecl *Record = dyn_cast<CXXRecordDecl>(Ctx)) {
            if (Record->getDescribedClassTemplate())
              MemberOfType = Record->getDescribedClassTemplate()
                                          ->getInjectedClassNameType(Context);
            else
              MemberOfType = Context.getTypeDeclType(Record);
          }
        }
      }
      if (MemberOfType.isNull())
        MemberOfType = SearchType;
      
      if (MemberOfType.isNull())
        continue;

      // We're referring into a class template specialization. If the
      // class template we found is the same as the template being
      // specialized, we found what we are looking for.
      if (const RecordType *Record = MemberOfType->getAs<RecordType>()) {
        if (ClassTemplateSpecializationDecl *Spec
              = dyn_cast<ClassTemplateSpecializationDecl>(Record->getDecl())) {
          if (Spec->getSpecializedTemplate()->getCanonicalDecl() ==
                Template->getCanonicalDecl())
            return MemberOfType.getAsOpaquePtr();
        }

        continue;
      }
      
      // We're referring to an unresolved class template
      // specialization. Determine whether we class template we found
      // is the same as the template being specialized or, if we don't
      // know which template is being specialized, that it at least
      // has the same name.
      if (const TemplateSpecializationType *SpecType
            = MemberOfType->getAs<TemplateSpecializationType>()) {
        TemplateName SpecName = SpecType->getTemplateName();

        // The class template we found is the same template being
        // specialized.
        if (TemplateDecl *SpecTemplate = SpecName.getAsTemplateDecl()) {
          if (SpecTemplate->getCanonicalDecl() == Template->getCanonicalDecl())
            return MemberOfType.getAsOpaquePtr();

          continue;
        }

        // The class template we found has the same name as the
        // (dependent) template name being specialized.
        if (DependentTemplateName *DepTemplate 
                                    = SpecName.getAsDependentTemplateName()) {
          if (DepTemplate->isIdentifier() &&
              DepTemplate->getIdentifier() == Template->getIdentifier())
            return MemberOfType.getAsOpaquePtr();

          continue;
        }
      }
    }
  }

  if (isDependent) {
    // We didn't find our type, but that's okay: it's dependent
    // anyway.
    NestedNameSpecifier *NNS = 0;
    SourceRange Range;
    if (SS.isSet()) {
      NNS = (NestedNameSpecifier *)SS.getScopeRep();
      Range = SourceRange(SS.getRange().getBegin(), NameLoc);
    } else {
      NNS = NestedNameSpecifier::Create(Context, &II);
      Range = SourceRange(NameLoc);
    }

    return CheckTypenameType(NNS, II, Range).getAsOpaquePtr();
  }

  if (ObjectTypePtr)
    Diag(NameLoc, diag::err_ident_in_pseudo_dtor_not_a_type)
      << &II;        
  else
    Diag(NameLoc, diag::err_destructor_class_name);

  return 0;
}

/// ActOnCXXTypeidOfType - Parse typeid( type-id ).
Action::OwningExprResult
Sema::ActOnCXXTypeid(SourceLocation OpLoc, SourceLocation LParenLoc,
                     bool isType, void *TyOrExpr, SourceLocation RParenLoc) {
  if (!StdNamespace)
    return ExprError(Diag(OpLoc, diag::err_need_header_before_typeid));

  if (isType) {
    // C++ [expr.typeid]p4:
    //   The top-level cv-qualifiers of the lvalue expression or the type-id 
    //   that is the operand of typeid are always ignored.
    // FIXME: Preserve type source info.
    // FIXME: Preserve the type before we stripped the cv-qualifiers?
    QualType T = GetTypeFromParser(TyOrExpr);
    if (T.isNull())
      return ExprError();
    
    // C++ [expr.typeid]p4:
    //   If the type of the type-id is a class type or a reference to a class 
    //   type, the class shall be completely-defined.
    QualType CheckT = T;
    if (const ReferenceType *RefType = CheckT->getAs<ReferenceType>())
      CheckT = RefType->getPointeeType();
    
    if (CheckT->getAs<RecordType>() &&
        RequireCompleteType(OpLoc, CheckT, diag::err_incomplete_typeid))
      return ExprError();
    
    TyOrExpr = T.getUnqualifiedType().getAsOpaquePtr();
  }

  IdentifierInfo *TypeInfoII = &PP.getIdentifierTable().get("type_info");
  LookupResult R(*this, TypeInfoII, SourceLocation(), LookupTagName);
  LookupQualifiedName(R, StdNamespace);
  RecordDecl *TypeInfoRecordDecl = R.getAsSingle<RecordDecl>();
  if (!TypeInfoRecordDecl)
    return ExprError(Diag(OpLoc, diag::err_need_header_before_typeid));

  QualType TypeInfoType = Context.getTypeDeclType(TypeInfoRecordDecl);

  if (!isType) {
    bool isUnevaluatedOperand = true;
    Expr *E = static_cast<Expr *>(TyOrExpr);
    if (E && !E->isTypeDependent()) {
      QualType T = E->getType();
      if (const RecordType *RecordT = T->getAs<RecordType>()) {
        CXXRecordDecl *RecordD = cast<CXXRecordDecl>(RecordT->getDecl());
        // C++ [expr.typeid]p3:
        //   [...] If the type of the expression is a class type, the class
        //   shall be completely-defined.
        if (RequireCompleteType(OpLoc, T, diag::err_incomplete_typeid))
          return ExprError();

        // C++ [expr.typeid]p3:
        //   When typeid is applied to an expression other than an lvalue of a
        //   polymorphic class type [...] [the] expression is an unevaluated
        //   operand. [...]
        if (RecordD->isPolymorphic() && E->isLvalue(Context) == Expr::LV_Valid)
          isUnevaluatedOperand = false;
      }

      // C++ [expr.typeid]p4:
      //   [...] If the type of the type-id is a reference to a possibly
      //   cv-qualified type, the result of the typeid expression refers to a 
      //   std::type_info object representing the cv-unqualified referenced 
      //   type.
      if (T.hasQualifiers()) {
        ImpCastExprToType(E, T.getUnqualifiedType(), CastExpr::CK_NoOp,
                          E->isLvalue(Context));
        TyOrExpr = E;
      }
    }

    // If this is an unevaluated operand, clear out the set of
    // declaration references we have been computing and eliminate any
    // temporaries introduced in its computation.
    if (isUnevaluatedOperand)
      ExprEvalContexts.back().Context = Unevaluated;
  }

  return Owned(new (Context) CXXTypeidExpr(isType, TyOrExpr,
                                           TypeInfoType.withConst(),
                                           SourceRange(OpLoc, RParenLoc)));
}

/// ActOnCXXBoolLiteral - Parse {true,false} literals.
Action::OwningExprResult
Sema::ActOnCXXBoolLiteral(SourceLocation OpLoc, tok::TokenKind Kind) {
  assert((Kind == tok::kw_true || Kind == tok::kw_false) &&
         "Unknown C++ Boolean value!");
  return Owned(new (Context) CXXBoolLiteralExpr(Kind == tok::kw_true,
                                                Context.BoolTy, OpLoc));
}

/// ActOnCXXNullPtrLiteral - Parse 'nullptr'.
Action::OwningExprResult
Sema::ActOnCXXNullPtrLiteral(SourceLocation Loc) {
  return Owned(new (Context) CXXNullPtrLiteralExpr(Context.NullPtrTy, Loc));
}

/// ActOnCXXThrow - Parse throw expressions.
Action::OwningExprResult
Sema::ActOnCXXThrow(SourceLocation OpLoc, ExprArg E) {
  Expr *Ex = E.takeAs<Expr>();
  if (Ex && !Ex->isTypeDependent() && CheckCXXThrowOperand(OpLoc, Ex))
    return ExprError();
  return Owned(new (Context) CXXThrowExpr(Ex, Context.VoidTy, OpLoc));
}

/// CheckCXXThrowOperand - Validate the operand of a throw.
bool Sema::CheckCXXThrowOperand(SourceLocation ThrowLoc, Expr *&E) {
  // C++ [except.throw]p3:
  //   A throw-expression initializes a temporary object, called the exception
  //   object, the type of which is determined by removing any top-level
  //   cv-qualifiers from the static type of the operand of throw and adjusting
  //   the type from "array of T" or "function returning T" to "pointer to T" 
  //   or "pointer to function returning T", [...]
  if (E->getType().hasQualifiers())
    ImpCastExprToType(E, E->getType().getUnqualifiedType(), CastExpr::CK_NoOp,
                      E->isLvalue(Context) == Expr::LV_Valid);
  
  DefaultFunctionArrayConversion(E);

  //   If the type of the exception would be an incomplete type or a pointer
  //   to an incomplete type other than (cv) void the program is ill-formed.
  QualType Ty = E->getType();
  int isPointer = 0;
  if (const PointerType* Ptr = Ty->getAs<PointerType>()) {
    Ty = Ptr->getPointeeType();
    isPointer = 1;
  }
  if (!isPointer || !Ty->isVoidType()) {
    if (RequireCompleteType(ThrowLoc, Ty,
                            PDiag(isPointer ? diag::err_throw_incomplete_ptr
                                            : diag::err_throw_incomplete)
                              << E->getSourceRange()))
      return true;

    // FIXME: This is just a hack to mark the copy constructor referenced.
    // This should go away when the next FIXME is fixed.
    const RecordType *RT = Ty->getAs<RecordType>();
    if (!RT)
      return false;

    const CXXRecordDecl *RD = cast<CXXRecordDecl>(RT->getDecl());
    if (RD->hasTrivialCopyConstructor())
      return false;
    CXXConstructorDecl *CopyCtor = RD->getCopyConstructor(Context, 0);
    MarkDeclarationReferenced(ThrowLoc, CopyCtor);
  }

  // FIXME: Construct a temporary here.
  return false;
}

Action::OwningExprResult Sema::ActOnCXXThis(SourceLocation ThisLoc) {
  /// C++ 9.3.2: In the body of a non-static member function, the keyword this
  /// is a non-lvalue expression whose value is the address of the object for
  /// which the function is called.

  if (!isa<FunctionDecl>(CurContext))
    return ExprError(Diag(ThisLoc, diag::err_invalid_this_use));

  if (CXXMethodDecl *MD = dyn_cast<CXXMethodDecl>(CurContext))
    if (MD->isInstance())
      return Owned(new (Context) CXXThisExpr(ThisLoc,
                                             MD->getThisType(Context),
                                             /*isImplicit=*/false));

  return ExprError(Diag(ThisLoc, diag::err_invalid_this_use));
}

/// ActOnCXXTypeConstructExpr - Parse construction of a specified type.
/// Can be interpreted either as function-style casting ("int(x)")
/// or class type construction ("ClassType(x,y,z)")
/// or creation of a value-initialized type ("int()").
Action::OwningExprResult
Sema::ActOnCXXTypeConstructExpr(SourceRange TypeRange, TypeTy *TypeRep,
                                SourceLocation LParenLoc,
                                MultiExprArg exprs,
                                SourceLocation *CommaLocs,
                                SourceLocation RParenLoc) {
  if (!TypeRep)
    return ExprError();

  TypeSourceInfo *TInfo;
  QualType Ty = GetTypeFromParser(TypeRep, &TInfo);
  if (!TInfo)
    TInfo = Context.getTrivialTypeSourceInfo(Ty, SourceLocation());
  unsigned NumExprs = exprs.size();
  Expr **Exprs = (Expr**)exprs.get();
  SourceLocation TyBeginLoc = TypeRange.getBegin();
  SourceRange FullRange = SourceRange(TyBeginLoc, RParenLoc);

  if (Ty->isDependentType() ||
      CallExpr::hasAnyTypeDependentArguments(Exprs, NumExprs)) {
    exprs.release();

    return Owned(CXXUnresolvedConstructExpr::Create(Context,
                                                    TypeRange.getBegin(), Ty,
                                                    LParenLoc,
                                                    Exprs, NumExprs,
                                                    RParenLoc));
  }

  if (Ty->isArrayType())
    return ExprError(Diag(TyBeginLoc,
                          diag::err_value_init_for_array_type) << FullRange);
  if (!Ty->isVoidType() &&
      RequireCompleteType(TyBeginLoc, Ty,
                          PDiag(diag::err_invalid_incomplete_type_use)
                            << FullRange))
    return ExprError();
  
  if (RequireNonAbstractType(TyBeginLoc, Ty,
                             diag::err_allocation_of_abstract_type))
    return ExprError();


  // C++ [expr.type.conv]p1:
  // If the expression list is a single expression, the type conversion
  // expression is equivalent (in definedness, and if defined in meaning) to the
  // corresponding cast expression.
  //
  if (NumExprs == 1) {
    CastExpr::CastKind Kind = CastExpr::CK_Unknown;
    CXXMethodDecl *Method = 0;
    if (CheckCastTypes(TypeRange, Ty, Exprs[0], Kind, Method,
                       /*FunctionalStyle=*/true))
      return ExprError();

    exprs.release();
    if (Method) {
      OwningExprResult CastArg 
        = BuildCXXCastArgument(TypeRange.getBegin(), Ty.getNonReferenceType(), 
                               Kind, Method, Owned(Exprs[0]));
      if (CastArg.isInvalid())
        return ExprError();

      Exprs[0] = CastArg.takeAs<Expr>();
    }

    return Owned(new (Context) CXXFunctionalCastExpr(Ty.getNonReferenceType(),
                                                     TInfo, TyBeginLoc, Kind,
                                                     Exprs[0], RParenLoc));
  }

  if (const RecordType *RT = Ty->getAs<RecordType>()) {
    CXXRecordDecl *Record = cast<CXXRecordDecl>(RT->getDecl());

    if (NumExprs > 1 || !Record->hasTrivialConstructor() ||
        !Record->hasTrivialDestructor()) {
      InitializedEntity Entity = InitializedEntity::InitializeTemporary(Ty);
      InitializationKind Kind
        = NumExprs ? InitializationKind::CreateDirect(TypeRange.getBegin(), 
                                                      LParenLoc, RParenLoc)
                   : InitializationKind::CreateValue(TypeRange.getBegin(), 
                                                     LParenLoc, RParenLoc);
      InitializationSequence InitSeq(*this, Entity, Kind, Exprs, NumExprs);
      OwningExprResult Result = InitSeq.Perform(*this, Entity, Kind,
                                                move(exprs));

      // FIXME: Improve AST representation?
      return move(Result);
    }

    // Fall through to value-initialize an object of class type that
    // doesn't have a user-declared default constructor.
  }

  // C++ [expr.type.conv]p1:
  // If the expression list specifies more than a single value, the type shall
  // be a class with a suitably declared constructor.
  //
  if (NumExprs > 1)
    return ExprError(Diag(CommaLocs[0],
                          diag::err_builtin_func_cast_more_than_one_arg)
      << FullRange);

  assert(NumExprs == 0 && "Expected 0 expressions");
  // C++ [expr.type.conv]p2:
  // The expression T(), where T is a simple-type-specifier for a non-array
  // complete object type or the (possibly cv-qualified) void type, creates an
  // rvalue of the specified type, which is value-initialized.
  //
  exprs.release();
  return Owned(new (Context) CXXZeroInitValueExpr(Ty, TyBeginLoc, RParenLoc));
}


/// ActOnCXXNew - Parsed a C++ 'new' expression (C++ 5.3.4), as in e.g.:
/// @code new (memory) int[size][4] @endcode
/// or
/// @code ::new Foo(23, "hello") @endcode
/// For the interpretation of this heap of arguments, consult the base version.
Action::OwningExprResult
Sema::ActOnCXXNew(SourceLocation StartLoc, bool UseGlobal,
                  SourceLocation PlacementLParen, MultiExprArg PlacementArgs,
                  SourceLocation PlacementRParen, bool ParenTypeId,
                  Declarator &D, SourceLocation ConstructorLParen,
                  MultiExprArg ConstructorArgs,
                  SourceLocation ConstructorRParen) {
  Expr *ArraySize = 0;
  // If the specified type is an array, unwrap it and save the expression.
  if (D.getNumTypeObjects() > 0 &&
      D.getTypeObject(0).Kind == DeclaratorChunk::Array) {
    DeclaratorChunk &Chunk = D.getTypeObject(0);
    if (Chunk.Arr.hasStatic)
      return ExprError(Diag(Chunk.Loc, diag::err_static_illegal_in_new)
        << D.getSourceRange());
    if (!Chunk.Arr.NumElts)
      return ExprError(Diag(Chunk.Loc, diag::err_array_new_needs_size)
        << D.getSourceRange());

    if (ParenTypeId) {
      // Can't have dynamic array size when the type-id is in parentheses.
      Expr *NumElts = (Expr *)Chunk.Arr.NumElts;
      if (!NumElts->isTypeDependent() && !NumElts->isValueDependent() &&
          !NumElts->isIntegerConstantExpr(Context)) {
        Diag(D.getTypeObject(0).Loc, diag::err_new_paren_array_nonconst)
          << NumElts->getSourceRange();
        return ExprError();
      }
    }

    ArraySize = static_cast<Expr*>(Chunk.Arr.NumElts);
    D.DropFirstTypeObject();
  }

  // Every dimension shall be of constant size.
  if (ArraySize) {
    for (unsigned I = 0, N = D.getNumTypeObjects(); I < N; ++I) {
      if (D.getTypeObject(I).Kind != DeclaratorChunk::Array)
        break;

      DeclaratorChunk::ArrayTypeInfo &Array = D.getTypeObject(I).Arr;
      if (Expr *NumElts = (Expr *)Array.NumElts) {
        if (!NumElts->isTypeDependent() && !NumElts->isValueDependent() &&
            !NumElts->isIntegerConstantExpr(Context)) {
          Diag(D.getTypeObject(I).Loc, diag::err_new_array_nonconst)
            << NumElts->getSourceRange();
          return ExprError();
        }
      }
    }
  }

  //FIXME: Store TypeSourceInfo in CXXNew expression.
  TypeSourceInfo *TInfo = 0;
  QualType AllocType = GetTypeForDeclarator(D, /*Scope=*/0, &TInfo);
  if (D.isInvalidType())
    return ExprError();
    
  return BuildCXXNew(StartLoc, UseGlobal,
                     PlacementLParen,
                     move(PlacementArgs),
                     PlacementRParen,
                     ParenTypeId,
                     AllocType,
                     D.getSourceRange().getBegin(),
                     D.getSourceRange(),
                     Owned(ArraySize),
                     ConstructorLParen,
                     move(ConstructorArgs),
                     ConstructorRParen);
}

Sema::OwningExprResult
Sema::BuildCXXNew(SourceLocation StartLoc, bool UseGlobal,
                  SourceLocation PlacementLParen,
                  MultiExprArg PlacementArgs,
                  SourceLocation PlacementRParen,
                  bool ParenTypeId,
                  QualType AllocType,
                  SourceLocation TypeLoc,
                  SourceRange TypeRange,
                  ExprArg ArraySizeE,
                  SourceLocation ConstructorLParen,
                  MultiExprArg ConstructorArgs,
                  SourceLocation ConstructorRParen) {
  if (CheckAllocatedType(AllocType, TypeLoc, TypeRange))
    return ExprError();

  QualType ResultType = Context.getPointerType(AllocType);

  // That every array dimension except the first is constant was already
  // checked by the type check above.

  // C++ 5.3.4p6: "The expression in a direct-new-declarator shall have integral
  //   or enumeration type with a non-negative value."
  Expr *ArraySize = (Expr *)ArraySizeE.get();
  if (ArraySize && !ArraySize->isTypeDependent()) {
    QualType SizeType = ArraySize->getType();
    if (!SizeType->isIntegralType() && !SizeType->isEnumeralType())
      return ExprError(Diag(ArraySize->getSourceRange().getBegin(),
                            diag::err_array_size_not_integral)
        << SizeType << ArraySize->getSourceRange());
    // Let's see if this is a constant < 0. If so, we reject it out of hand.
    // We don't care about special rules, so we tell the machinery it's not
    // evaluated - it gives us a result in more cases.
    if (!ArraySize->isValueDependent()) {
      llvm::APSInt Value;
      if (ArraySize->isIntegerConstantExpr(Value, Context, 0, false)) {
        if (Value < llvm::APSInt(
                        llvm::APInt::getNullValue(Value.getBitWidth()), 
                                 Value.isUnsigned()))
          return ExprError(Diag(ArraySize->getSourceRange().getBegin(),
                           diag::err_typecheck_negative_array_size)
            << ArraySize->getSourceRange());
      }
    }
    
    ImpCastExprToType(ArraySize, Context.getSizeType(),
                      CastExpr::CK_IntegralCast);
  }

  FunctionDecl *OperatorNew = 0;
  FunctionDecl *OperatorDelete = 0;
  Expr **PlaceArgs = (Expr**)PlacementArgs.get();
  unsigned NumPlaceArgs = PlacementArgs.size();
  
  if (!AllocType->isDependentType() &&
      !Expr::hasAnyTypeDependentArguments(PlaceArgs, NumPlaceArgs) &&
      FindAllocationFunctions(StartLoc,
                              SourceRange(PlacementLParen, PlacementRParen),
                              UseGlobal, AllocType, ArraySize, PlaceArgs,
                              NumPlaceArgs, OperatorNew, OperatorDelete))
    return ExprError();
  llvm::SmallVector<Expr *, 8> AllPlaceArgs;
  if (OperatorNew) {
    // Add default arguments, if any.
    const FunctionProtoType *Proto = 
      OperatorNew->getType()->getAs<FunctionProtoType>();
    VariadicCallType CallType = 
      Proto->isVariadic() ? VariadicFunction : VariadicDoesNotApply;
    bool Invalid = GatherArgumentsForCall(PlacementLParen, OperatorNew,
                                          Proto, 1, PlaceArgs, NumPlaceArgs, 
                                          AllPlaceArgs, CallType);
    if (Invalid)
      return ExprError();
    
    NumPlaceArgs = AllPlaceArgs.size();
    if (NumPlaceArgs > 0)
      PlaceArgs = &AllPlaceArgs[0];
  }
  
  bool Init = ConstructorLParen.isValid();
  // --- Choosing a constructor ---
  CXXConstructorDecl *Constructor = 0;
  Expr **ConsArgs = (Expr**)ConstructorArgs.get();
  unsigned NumConsArgs = ConstructorArgs.size();
  ASTOwningVector<&ActionBase::DeleteExpr> ConvertedConstructorArgs(*this);

  if (!AllocType->isDependentType() &&
      !Expr::hasAnyTypeDependentArguments(ConsArgs, NumConsArgs)) {
    // C++0x [expr.new]p15:
    //   A new-expression that creates an object of type T initializes that
    //   object as follows:
    InitializationKind Kind
    //     - If the new-initializer is omitted, the object is default-
    //       initialized (8.5); if no initialization is performed,
    //       the object has indeterminate value
      = !Init? InitializationKind::CreateDefault(TypeLoc)
    //     - Otherwise, the new-initializer is interpreted according to the 
    //       initialization rules of 8.5 for direct-initialization.
             : InitializationKind::CreateDirect(TypeLoc,
                                                ConstructorLParen, 
                                                ConstructorRParen);
    
    InitializedEntity Entity
      = InitializedEntity::InitializeNew(StartLoc, AllocType);
    InitializationSequence InitSeq(*this, Entity, Kind, ConsArgs, NumConsArgs);
    OwningExprResult FullInit = InitSeq.Perform(*this, Entity, Kind, 
                                                move(ConstructorArgs));
    if (FullInit.isInvalid())
      return ExprError();
    
    // FullInit is our initializer; walk through it to determine if it's a 
    // constructor call, which CXXNewExpr handles directly.
    if (Expr *FullInitExpr = (Expr *)FullInit.get()) {
      if (CXXBindTemporaryExpr *Binder
            = dyn_cast<CXXBindTemporaryExpr>(FullInitExpr))
        FullInitExpr = Binder->getSubExpr();
      if (CXXConstructExpr *Construct
                    = dyn_cast<CXXConstructExpr>(FullInitExpr)) {
        Constructor = Construct->getConstructor();
        for (CXXConstructExpr::arg_iterator A = Construct->arg_begin(),
                                         AEnd = Construct->arg_end();
             A != AEnd; ++A)
          ConvertedConstructorArgs.push_back(A->Retain());
      } else {
        // Take the converted initializer.
        ConvertedConstructorArgs.push_back(FullInit.release());
      }
    } else {
      // No initialization required.
    }
    
    // Take the converted arguments and use them for the new expression.
    NumConsArgs = ConvertedConstructorArgs.size();
    ConsArgs = (Expr **)ConvertedConstructorArgs.take();
  }
  
  // Mark the new and delete operators as referenced.
  if (OperatorNew)
    MarkDeclarationReferenced(StartLoc, OperatorNew);
  if (OperatorDelete)
    MarkDeclarationReferenced(StartLoc, OperatorDelete);

  // FIXME: Also check that the destructor is accessible. (C++ 5.3.4p16)
  
  PlacementArgs.release();
  ConstructorArgs.release();
  ArraySizeE.release();
  return Owned(new (Context) CXXNewExpr(Context, UseGlobal, OperatorNew,
                                        PlaceArgs, NumPlaceArgs, ParenTypeId,
                                        ArraySize, Constructor, Init,
                                        ConsArgs, NumConsArgs, OperatorDelete,
                                        ResultType, StartLoc,
                                        Init ? ConstructorRParen :
                                               SourceLocation()));
}

/// CheckAllocatedType - Checks that a type is suitable as the allocated type
/// in a new-expression.
/// dimension off and stores the size expression in ArraySize.
bool Sema::CheckAllocatedType(QualType AllocType, SourceLocation Loc,
                              SourceRange R) {
  // C++ 5.3.4p1: "[The] type shall be a complete object type, but not an
  //   abstract class type or array thereof.
  if (AllocType->isFunctionType())
    return Diag(Loc, diag::err_bad_new_type)
      << AllocType << 0 << R;
  else if (AllocType->isReferenceType())
    return Diag(Loc, diag::err_bad_new_type)
      << AllocType << 1 << R;
  else if (!AllocType->isDependentType() &&
           RequireCompleteType(Loc, AllocType,
                               PDiag(diag::err_new_incomplete_type)
                                 << R))
    return true;
  else if (RequireNonAbstractType(Loc, AllocType,
                                  diag::err_allocation_of_abstract_type))
    return true;

  return false;
}

/// \brief Determine whether the given function is a non-placement
/// deallocation function.
static bool isNonPlacementDeallocationFunction(FunctionDecl *FD) {
  if (FD->isInvalidDecl())
    return false;

  if (CXXMethodDecl *Method = dyn_cast<CXXMethodDecl>(FD))
    return Method->isUsualDeallocationFunction();

  return ((FD->getOverloadedOperator() == OO_Delete ||
           FD->getOverloadedOperator() == OO_Array_Delete) &&
          FD->getNumParams() == 1);
}

/// FindAllocationFunctions - Finds the overloads of operator new and delete
/// that are appropriate for the allocation.
bool Sema::FindAllocationFunctions(SourceLocation StartLoc, SourceRange Range,
                                   bool UseGlobal, QualType AllocType,
                                   bool IsArray, Expr **PlaceArgs,
                                   unsigned NumPlaceArgs,
                                   FunctionDecl *&OperatorNew,
                                   FunctionDecl *&OperatorDelete) {
  // --- Choosing an allocation function ---
  // C++ 5.3.4p8 - 14 & 18
  // 1) If UseGlobal is true, only look in the global scope. Else, also look
  //   in the scope of the allocated class.
  // 2) If an array size is given, look for operator new[], else look for
  //   operator new.
  // 3) The first argument is always size_t. Append the arguments from the
  //   placement form.

  llvm::SmallVector<Expr*, 8> AllocArgs(1 + NumPlaceArgs);
  // We don't care about the actual value of this argument.
  // FIXME: Should the Sema create the expression and embed it in the syntax
  // tree? Or should the consumer just recalculate the value?
  IntegerLiteral Size(llvm::APInt::getNullValue(
                      Context.Target.getPointerWidth(0)),
                      Context.getSizeType(),
                      SourceLocation());
  AllocArgs[0] = &Size;
  std::copy(PlaceArgs, PlaceArgs + NumPlaceArgs, AllocArgs.begin() + 1);

  // C++ [expr.new]p8:
  //   If the allocated type is a non-array type, the allocation
  //   function’s name is operator new and the deallocation function’s
  //   name is operator delete. If the allocated type is an array
  //   type, the allocation function’s name is operator new[] and the
  //   deallocation function’s name is operator delete[].
  DeclarationName NewName = Context.DeclarationNames.getCXXOperatorName(
                                        IsArray ? OO_Array_New : OO_New);
  DeclarationName DeleteName = Context.DeclarationNames.getCXXOperatorName(
                                        IsArray ? OO_Array_Delete : OO_Delete);

  if (AllocType->isRecordType() && !UseGlobal) {
    CXXRecordDecl *Record
      = cast<CXXRecordDecl>(AllocType->getAs<RecordType>()->getDecl());
    if (FindAllocationOverload(StartLoc, Range, NewName, &AllocArgs[0],
                          AllocArgs.size(), Record, /*AllowMissing=*/true,
                          OperatorNew))
      return true;
  }
  if (!OperatorNew) {
    // Didn't find a member overload. Look for a global one.
    DeclareGlobalNewDelete();
    DeclContext *TUDecl = Context.getTranslationUnitDecl();
    if (FindAllocationOverload(StartLoc, Range, NewName, &AllocArgs[0],
                          AllocArgs.size(), TUDecl, /*AllowMissing=*/false,
                          OperatorNew))
      return true;
  }

  // FindAllocationOverload can change the passed in arguments, so we need to
  // copy them back.
  if (NumPlaceArgs > 0)
    std::copy(&AllocArgs[1], AllocArgs.end(), PlaceArgs);

  // C++ [expr.new]p19:
  //
  //   If the new-expression begins with a unary :: operator, the
  //   deallocation function’s name is looked up in the global
  //   scope. Otherwise, if the allocated type is a class type T or an
  //   array thereof, the deallocation function’s name is looked up in
  //   the scope of T. If this lookup fails to find the name, or if
  //   the allocated type is not a class type or array thereof, the
  //   deallocation function’s name is looked up in the global scope.
  LookupResult FoundDelete(*this, DeleteName, StartLoc, LookupOrdinaryName);
  if (AllocType->isRecordType() && !UseGlobal) {
    CXXRecordDecl *RD
      = cast<CXXRecordDecl>(AllocType->getAs<RecordType>()->getDecl());
    LookupQualifiedName(FoundDelete, RD);
  }

  if (FoundDelete.empty()) {
    DeclareGlobalNewDelete();
    LookupQualifiedName(FoundDelete, Context.getTranslationUnitDecl());
  }

  FoundDelete.suppressDiagnostics();
  llvm::SmallVector<NamedDecl *, 4> Matches;
  if (NumPlaceArgs > 1) {
    // C++ [expr.new]p20:
    //   A declaration of a placement deallocation function matches the
    //   declaration of a placement allocation function if it has the
    //   same number of parameters and, after parameter transformations
    //   (8.3.5), all parameter types except the first are
    //   identical. [...]
    // 
    // To perform this comparison, we compute the function type that
    // the deallocation function should have, and use that type both
    // for template argument deduction and for comparison purposes.
    QualType ExpectedFunctionType;
    {
      const FunctionProtoType *Proto
        = OperatorNew->getType()->getAs<FunctionProtoType>();
      llvm::SmallVector<QualType, 4> ArgTypes;
      ArgTypes.push_back(Context.VoidPtrTy); 
      for (unsigned I = 1, N = Proto->getNumArgs(); I < N; ++I)
        ArgTypes.push_back(Proto->getArgType(I));

      ExpectedFunctionType
        = Context.getFunctionType(Context.VoidTy, ArgTypes.data(),
                                  ArgTypes.size(),
                                  Proto->isVariadic(),
                                  0, false, false, 0, 0, false, CC_Default);
    }

    for (LookupResult::iterator D = FoundDelete.begin(), 
                             DEnd = FoundDelete.end();
         D != DEnd; ++D) {
      FunctionDecl *Fn = 0;
      if (FunctionTemplateDecl *FnTmpl 
            = dyn_cast<FunctionTemplateDecl>((*D)->getUnderlyingDecl())) {
        // Perform template argument deduction to try to match the
        // expected function type.
        TemplateDeductionInfo Info(Context, StartLoc);
        if (DeduceTemplateArguments(FnTmpl, 0, ExpectedFunctionType, Fn, Info))
          continue;
      } else
        Fn = cast<FunctionDecl>((*D)->getUnderlyingDecl());

      if (Context.hasSameType(Fn->getType(), ExpectedFunctionType))
        Matches.push_back(Fn);
    }
  } else {
    // C++ [expr.new]p20:
    //   [...] Any non-placement deallocation function matches a
    //   non-placement allocation function. [...]
    for (LookupResult::iterator D = FoundDelete.begin(), 
                             DEnd = FoundDelete.end();
         D != DEnd; ++D) {
      if (FunctionDecl *Fn = dyn_cast<FunctionDecl>((*D)->getUnderlyingDecl()))
        if (isNonPlacementDeallocationFunction(Fn))
          Matches.push_back(*D);
    }
  }

  // C++ [expr.new]p20:
  //   [...] If the lookup finds a single matching deallocation
  //   function, that function will be called; otherwise, no
  //   deallocation function will be called.
  if (Matches.size() == 1) {
    // FIXME: Drops access, using-declaration info!
    OperatorDelete = cast<FunctionDecl>(Matches[0]->getUnderlyingDecl());

    // C++0x [expr.new]p20:
    //   If the lookup finds the two-parameter form of a usual
    //   deallocation function (3.7.4.2) and that function, considered
    //   as a placement deallocation function, would have been
    //   selected as a match for the allocation function, the program
    //   is ill-formed.
    if (NumPlaceArgs && getLangOptions().CPlusPlus0x &&
        isNonPlacementDeallocationFunction(OperatorDelete)) {
      Diag(StartLoc, diag::err_placement_new_non_placement_delete)
        << SourceRange(PlaceArgs[0]->getLocStart(), 
                       PlaceArgs[NumPlaceArgs - 1]->getLocEnd());
      Diag(OperatorDelete->getLocation(), diag::note_previous_decl)
        << DeleteName;
    }
  }

  return false;
}

/// FindAllocationOverload - Find an fitting overload for the allocation
/// function in the specified scope.
bool Sema::FindAllocationOverload(SourceLocation StartLoc, SourceRange Range,
                                  DeclarationName Name, Expr** Args,
                                  unsigned NumArgs, DeclContext *Ctx,
                                  bool AllowMissing, FunctionDecl *&Operator) {
  LookupResult R(*this, Name, StartLoc, LookupOrdinaryName);
  LookupQualifiedName(R, Ctx);
  if (R.empty()) {
    if (AllowMissing)
      return false;
    return Diag(StartLoc, diag::err_ovl_no_viable_function_in_call)
      << Name << Range;
  }

  // FIXME: handle ambiguity

  OverloadCandidateSet Candidates(StartLoc);
  for (LookupResult::iterator Alloc = R.begin(), AllocEnd = R.end(); 
       Alloc != AllocEnd; ++Alloc) {
    // Even member operator new/delete are implicitly treated as
    // static, so don't use AddMemberCandidate.

    if (FunctionTemplateDecl *FnTemplate = 
          dyn_cast<FunctionTemplateDecl>((*Alloc)->getUnderlyingDecl())) {
      AddTemplateOverloadCandidate(FnTemplate, Alloc.getAccess(),
                                   /*ExplicitTemplateArgs=*/0, Args, NumArgs,
                                   Candidates,
                                   /*SuppressUserConversions=*/false);
      continue;
    }

    FunctionDecl *Fn = cast<FunctionDecl>((*Alloc)->getUnderlyingDecl());
    AddOverloadCandidate(Fn, Alloc.getAccess(), Args, NumArgs, Candidates,
                         /*SuppressUserConversions=*/false);
  }

  // Do the resolution.
  OverloadCandidateSet::iterator Best;
  switch(BestViableFunction(Candidates, StartLoc, Best)) {
  case OR_Success: {
    // Got one!
    FunctionDecl *FnDecl = Best->Function;
    // The first argument is size_t, and the first parameter must be size_t,
    // too. This is checked on declaration and can be assumed. (It can't be
    // asserted on, though, since invalid decls are left in there.)
    // Whatch out for variadic allocator function.
    unsigned NumArgsInFnDecl = FnDecl->getNumParams();
    for (unsigned i = 0; (i < NumArgs && i < NumArgsInFnDecl); ++i) {
      if (PerformCopyInitialization(Args[i],
                                    FnDecl->getParamDecl(i)->getType(),
                                    AA_Passing))
        return true;
    }
    Operator = FnDecl;
    return false;
  }

  case OR_No_Viable_Function:
    Diag(StartLoc, diag::err_ovl_no_viable_function_in_call)
      << Name << Range;
    PrintOverloadCandidates(Candidates, OCD_AllCandidates, Args, NumArgs);
    return true;

  case OR_Ambiguous:
    Diag(StartLoc, diag::err_ovl_ambiguous_call)
      << Name << Range;
    PrintOverloadCandidates(Candidates, OCD_ViableCandidates, Args, NumArgs);
    return true;

  case OR_Deleted:
    Diag(StartLoc, diag::err_ovl_deleted_call)
      << Best->Function->isDeleted()
      << Name << Range;
    PrintOverloadCandidates(Candidates, OCD_AllCandidates, Args, NumArgs);
    return true;
  }
  assert(false && "Unreachable, bad result from BestViableFunction");
  return true;
}


/// DeclareGlobalNewDelete - Declare the global forms of operator new and
/// delete. These are:
/// @code
///   void* operator new(std::size_t) throw(std::bad_alloc);
///   void* operator new[](std::size_t) throw(std::bad_alloc);
///   void operator delete(void *) throw();
///   void operator delete[](void *) throw();
/// @endcode
/// Note that the placement and nothrow forms of new are *not* implicitly
/// declared. Their use requires including \<new\>.
void Sema::DeclareGlobalNewDelete() {
  if (GlobalNewDeleteDeclared)
    return;
  
  // C++ [basic.std.dynamic]p2:
  //   [...] The following allocation and deallocation functions (18.4) are 
  //   implicitly declared in global scope in each translation unit of a 
  //   program
  //   
  //     void* operator new(std::size_t) throw(std::bad_alloc);
  //     void* operator new[](std::size_t) throw(std::bad_alloc); 
  //     void  operator delete(void*) throw(); 
  //     void  operator delete[](void*) throw();
  //
  //   These implicit declarations introduce only the function names operator 
  //   new, operator new[], operator delete, operator delete[].
  //
  // Here, we need to refer to std::bad_alloc, so we will implicitly declare
  // "std" or "bad_alloc" as necessary to form the exception specification.
  // However, we do not make these implicit declarations visible to name
  // lookup.
  if (!StdNamespace) {
    // The "std" namespace has not yet been defined, so build one implicitly.
    StdNamespace = NamespaceDecl::Create(Context, 
                                         Context.getTranslationUnitDecl(),
                                         SourceLocation(),
                                         &PP.getIdentifierTable().get("std"));
    StdNamespace->setImplicit(true);
  }
  
  if (!StdBadAlloc) {
    // The "std::bad_alloc" class has not yet been declared, so build it
    // implicitly.
    StdBadAlloc = CXXRecordDecl::Create(Context, TagDecl::TK_class, 
                                        StdNamespace, 
                                        SourceLocation(), 
                                      &PP.getIdentifierTable().get("bad_alloc"), 
                                        SourceLocation(), 0);
    StdBadAlloc->setImplicit(true);
  }
  
  GlobalNewDeleteDeclared = true;

  QualType VoidPtr = Context.getPointerType(Context.VoidTy);
  QualType SizeT = Context.getSizeType();
  bool AssumeSaneOperatorNew = getLangOptions().AssumeSaneOperatorNew;

  DeclareGlobalAllocationFunction(
      Context.DeclarationNames.getCXXOperatorName(OO_New),
      VoidPtr, SizeT, AssumeSaneOperatorNew);
  DeclareGlobalAllocationFunction(
      Context.DeclarationNames.getCXXOperatorName(OO_Array_New),
      VoidPtr, SizeT, AssumeSaneOperatorNew);
  DeclareGlobalAllocationFunction(
      Context.DeclarationNames.getCXXOperatorName(OO_Delete),
      Context.VoidTy, VoidPtr);
  DeclareGlobalAllocationFunction(
      Context.DeclarationNames.getCXXOperatorName(OO_Array_Delete),
      Context.VoidTy, VoidPtr);
}

/// DeclareGlobalAllocationFunction - Declares a single implicit global
/// allocation function if it doesn't already exist.
void Sema::DeclareGlobalAllocationFunction(DeclarationName Name,
                                           QualType Return, QualType Argument,
                                           bool AddMallocAttr) {
  DeclContext *GlobalCtx = Context.getTranslationUnitDecl();

  // Check if this function is already declared.
  {
    DeclContext::lookup_iterator Alloc, AllocEnd;
    for (llvm::tie(Alloc, AllocEnd) = GlobalCtx->lookup(Name);
         Alloc != AllocEnd; ++Alloc) {
      // Only look at non-template functions, as it is the predefined,
      // non-templated allocation function we are trying to declare here.
      if (FunctionDecl *Func = dyn_cast<FunctionDecl>(*Alloc)) {
        QualType InitialParamType =
          Context.getCanonicalType(
            Func->getParamDecl(0)->getType().getUnqualifiedType());
        // FIXME: Do we need to check for default arguments here?
        if (Func->getNumParams() == 1 && InitialParamType == Argument)
          return;
      }
    }
  }

  QualType BadAllocType;
  bool HasBadAllocExceptionSpec 
    = (Name.getCXXOverloadedOperator() == OO_New ||
       Name.getCXXOverloadedOperator() == OO_Array_New);
  if (HasBadAllocExceptionSpec) {
    assert(StdBadAlloc && "Must have std::bad_alloc declared");
    BadAllocType = Context.getTypeDeclType(StdBadAlloc);
  }
  
  QualType FnType = Context.getFunctionType(Return, &Argument, 1, false, 0,
                                            true, false,
                                            HasBadAllocExceptionSpec? 1 : 0,
                                            &BadAllocType, false, CC_Default);
  FunctionDecl *Alloc =
    FunctionDecl::Create(Context, GlobalCtx, SourceLocation(), Name,
                         FnType, /*TInfo=*/0, FunctionDecl::None, false, true);
  Alloc->setImplicit();
  
  if (AddMallocAttr)
    Alloc->addAttr(::new (Context) MallocAttr());
  
  ParmVarDecl *Param = ParmVarDecl::Create(Context, Alloc, SourceLocation(),
                                           0, Argument, /*TInfo=*/0,
                                           VarDecl::None, 0);
  Alloc->setParams(&Param, 1);

  // FIXME: Also add this declaration to the IdentifierResolver, but
  // make sure it is at the end of the chain to coincide with the
  // global scope.
  ((DeclContext *)TUScope->getEntity())->addDecl(Alloc);
}

bool Sema::FindDeallocationFunction(SourceLocation StartLoc, CXXRecordDecl *RD,
                                    DeclarationName Name,
                                    FunctionDecl* &Operator) {
  LookupResult Found(*this, Name, StartLoc, LookupOrdinaryName);
  // Try to find operator delete/operator delete[] in class scope.
  LookupQualifiedName(Found, RD);
  
  if (Found.isAmbiguous())
    return true;

  for (LookupResult::iterator F = Found.begin(), FEnd = Found.end();
       F != FEnd; ++F) {
    if (CXXMethodDecl *Delete = dyn_cast<CXXMethodDecl>(*F))
      if (Delete->isUsualDeallocationFunction()) {
        Operator = Delete;
        return false;
      }
  }

  // We did find operator delete/operator delete[] declarations, but
  // none of them were suitable.
  if (!Found.empty()) {
    Diag(StartLoc, diag::err_no_suitable_delete_member_function_found)
      << Name << RD;
        
    for (LookupResult::iterator F = Found.begin(), FEnd = Found.end();
         F != FEnd; ++F) {
      Diag((*F)->getLocation(), 
           diag::note_delete_member_function_declared_here)
        << Name;
    }

    return true;
  }

  // Look for a global declaration.
  DeclareGlobalNewDelete();
  DeclContext *TUDecl = Context.getTranslationUnitDecl();
  
  CXXNullPtrLiteralExpr Null(Context.VoidPtrTy, SourceLocation());
  Expr* DeallocArgs[1];
  DeallocArgs[0] = &Null;
  if (FindAllocationOverload(StartLoc, SourceRange(), Name,
                             DeallocArgs, 1, TUDecl, /*AllowMissing=*/false,
                             Operator))
    return true;

  assert(Operator && "Did not find a deallocation function!");
  return false;
}

/// ActOnCXXDelete - Parsed a C++ 'delete' expression (C++ 5.3.5), as in:
/// @code ::delete ptr; @endcode
/// or
/// @code delete [] ptr; @endcode
Action::OwningExprResult
Sema::ActOnCXXDelete(SourceLocation StartLoc, bool UseGlobal,
                     bool ArrayForm, ExprArg Operand) {
  // C++ [expr.delete]p1:
  //   The operand shall have a pointer type, or a class type having a single
  //   conversion function to a pointer type. The result has type void.
  //
  // DR599 amends "pointer type" to "pointer to object type" in both cases.

  FunctionDecl *OperatorDelete = 0;

  Expr *Ex = (Expr *)Operand.get();
  if (!Ex->isTypeDependent()) {
    QualType Type = Ex->getType();

    if (const RecordType *Record = Type->getAs<RecordType>()) {
      llvm::SmallVector<CXXConversionDecl *, 4> ObjectPtrConversions;
      CXXRecordDecl *RD = cast<CXXRecordDecl>(Record->getDecl());
      const UnresolvedSetImpl *Conversions = RD->getVisibleConversionFunctions();
      
      for (UnresolvedSetImpl::iterator I = Conversions->begin(),
             E = Conversions->end(); I != E; ++I) {
        // Skip over templated conversion functions; they aren't considered.
        if (isa<FunctionTemplateDecl>(*I))
          continue;
        
        CXXConversionDecl *Conv = cast<CXXConversionDecl>(*I);
        
        QualType ConvType = Conv->getConversionType().getNonReferenceType();
        if (const PointerType *ConvPtrType = ConvType->getAs<PointerType>())
          if (ConvPtrType->getPointeeType()->isObjectType())
            ObjectPtrConversions.push_back(Conv);
      }
      if (ObjectPtrConversions.size() == 1) {
        // We have a single conversion to a pointer-to-object type. Perform
        // that conversion.
        Operand.release();
        if (!PerformImplicitConversion(Ex, 
                            ObjectPtrConversions.front()->getConversionType(), 
                                      AA_Converting)) {
          Operand = Owned(Ex);
          Type = Ex->getType();
        }
      }
      else if (ObjectPtrConversions.size() > 1) {
        Diag(StartLoc, diag::err_ambiguous_delete_operand)
              << Type << Ex->getSourceRange();
        for (unsigned i= 0; i < ObjectPtrConversions.size(); i++) {
          CXXConversionDecl *Conv = ObjectPtrConversions[i];
          NoteOverloadCandidate(Conv);
        }
        return ExprError();
      }
    }

    if (!Type->isPointerType())
      return ExprError(Diag(StartLoc, diag::err_delete_operand)
        << Type << Ex->getSourceRange());

    QualType Pointee = Type->getAs<PointerType>()->getPointeeType();
    if (Pointee->isFunctionType() || Pointee->isVoidType())
      return ExprError(Diag(StartLoc, diag::err_delete_operand)
        << Type << Ex->getSourceRange());
    else if (!Pointee->isDependentType() &&
             RequireCompleteType(StartLoc, Pointee,
                                 PDiag(diag::warn_delete_incomplete)
                                   << Ex->getSourceRange()))
      return ExprError();

    // C++ [expr.delete]p2:
    //   [Note: a pointer to a const type can be the operand of a 
    //   delete-expression; it is not necessary to cast away the constness 
    //   (5.2.11) of the pointer expression before it is used as the operand 
    //   of the delete-expression. ]
    ImpCastExprToType(Ex, Context.getPointerType(Context.VoidTy), 
                      CastExpr::CK_NoOp);
    
    // Update the operand.
    Operand.take();
    Operand = ExprArg(*this, Ex);
    
    DeclarationName DeleteName = Context.DeclarationNames.getCXXOperatorName(
                                      ArrayForm ? OO_Array_Delete : OO_Delete);

    if (const RecordType *RT = Pointee->getAs<RecordType>()) {
      CXXRecordDecl *RD = cast<CXXRecordDecl>(RT->getDecl());

      if (!UseGlobal && 
          FindDeallocationFunction(StartLoc, RD, DeleteName, OperatorDelete))
        return ExprError();
      
      if (!RD->hasTrivialDestructor())
        if (const CXXDestructorDecl *Dtor = RD->getDestructor(Context))
          MarkDeclarationReferenced(StartLoc,
                                    const_cast<CXXDestructorDecl*>(Dtor));
    }
    
    if (!OperatorDelete) {
      // Look for a global declaration.
      DeclareGlobalNewDelete();
      DeclContext *TUDecl = Context.getTranslationUnitDecl();
      if (FindAllocationOverload(StartLoc, SourceRange(), DeleteName,
                                 &Ex, 1, TUDecl, /*AllowMissing=*/false,
                                 OperatorDelete))
        return ExprError();
    }

    // FIXME: Check access and ambiguity of operator delete and destructor.
  }

  Operand.release();
  return Owned(new (Context) CXXDeleteExpr(Context.VoidTy, UseGlobal, ArrayForm,
                                           OperatorDelete, Ex, StartLoc));
}

/// \brief Check the use of the given variable as a C++ condition in an if,
/// while, do-while, or switch statement.
Action::OwningExprResult Sema::CheckConditionVariable(VarDecl *ConditionVar) {
  QualType T = ConditionVar->getType();
  
  // C++ [stmt.select]p2:
  //   The declarator shall not specify a function or an array.
  if (T->isFunctionType())
    return ExprError(Diag(ConditionVar->getLocation(), 
                          diag::err_invalid_use_of_function_type)
                       << ConditionVar->getSourceRange());
  else if (T->isArrayType())
    return ExprError(Diag(ConditionVar->getLocation(), 
                          diag::err_invalid_use_of_array_type)
                     << ConditionVar->getSourceRange());

  return Owned(DeclRefExpr::Create(Context, 0, SourceRange(), ConditionVar,
                                   ConditionVar->getLocation(), 
                                ConditionVar->getType().getNonReferenceType()));
}

/// CheckCXXBooleanCondition - Returns true if a conversion to bool is invalid.
bool Sema::CheckCXXBooleanCondition(Expr *&CondExpr) {
  // C++ 6.4p4:
  // The value of a condition that is an initialized declaration in a statement
  // other than a switch statement is the value of the declared variable
  // implicitly converted to type bool. If that conversion is ill-formed, the
  // program is ill-formed.
  // The value of a condition that is an expression is the value of the
  // expression, implicitly converted to bool.
  //
  return PerformContextuallyConvertToBool(CondExpr);
}

/// Helper function to determine whether this is the (deprecated) C++
/// conversion from a string literal to a pointer to non-const char or
/// non-const wchar_t (for narrow and wide string literals,
/// respectively).
bool
Sema::IsStringLiteralToNonConstPointerConversion(Expr *From, QualType ToType) {
  // Look inside the implicit cast, if it exists.
  if (ImplicitCastExpr *Cast = dyn_cast<ImplicitCastExpr>(From))
    From = Cast->getSubExpr();

  // A string literal (2.13.4) that is not a wide string literal can
  // be converted to an rvalue of type "pointer to char"; a wide
  // string literal can be converted to an rvalue of type "pointer
  // to wchar_t" (C++ 4.2p2).
  if (StringLiteral *StrLit = dyn_cast<StringLiteral>(From))
    if (const PointerType *ToPtrType = ToType->getAs<PointerType>())
      if (const BuiltinType *ToPointeeType
          = ToPtrType->getPointeeType()->getAs<BuiltinType>()) {
        // This conversion is considered only when there is an
        // explicit appropriate pointer target type (C++ 4.2p2).
        if (!ToPtrType->getPointeeType().hasQualifiers() &&
            ((StrLit->isWide() && ToPointeeType->isWideCharType()) ||
             (!StrLit->isWide() &&
              (ToPointeeType->getKind() == BuiltinType::Char_U ||
               ToPointeeType->getKind() == BuiltinType::Char_S))))
          return true;
      }

  return false;
}

/// PerformImplicitConversion - Perform an implicit conversion of the
/// expression From to the type ToType. Returns true if there was an
/// error, false otherwise. The expression From is replaced with the
/// converted expression. Flavor is the kind of conversion we're
/// performing, used in the error message. If @p AllowExplicit,
/// explicit user-defined conversions are permitted. @p Elidable should be true
/// when called for copies which may be elided (C++ 12.8p15). C++0x overload
/// resolution works differently in that case.
bool
Sema::PerformImplicitConversion(Expr *&From, QualType ToType,
                                AssignmentAction Action, bool AllowExplicit,
                                bool Elidable) {
  ImplicitConversionSequence ICS;
  return PerformImplicitConversion(From, ToType, Action, AllowExplicit, 
                                   Elidable, ICS);
}

bool
Sema::PerformImplicitConversion(Expr *&From, QualType ToType,
                                AssignmentAction Action, bool AllowExplicit,
                                bool Elidable,
                                ImplicitConversionSequence& ICS) {
  ICS.setBad(BadConversionSequence::no_conversion, From, ToType);
  if (Elidable && getLangOptions().CPlusPlus0x) {
    ICS = TryImplicitConversion(From, ToType,
                                /*SuppressUserConversions=*/false,
                                AllowExplicit,
                                /*ForceRValue=*/true,
                                /*InOverloadResolution=*/false);
  }
  if (ICS.isBad()) {
    ICS = TryImplicitConversion(From, ToType,
                                /*SuppressUserConversions=*/false,
                                AllowExplicit,
                                /*ForceRValue=*/false,
                                /*InOverloadResolution=*/false);
  }
  return PerformImplicitConversion(From, ToType, ICS, Action);
}

/// PerformImplicitConversion - Perform an implicit conversion of the
/// expression From to the type ToType using the pre-computed implicit
/// conversion sequence ICS. Returns true if there was an error, false
/// otherwise. The expression From is replaced with the converted
/// expression. Action is the kind of conversion we're performing,
/// used in the error message.
bool
Sema::PerformImplicitConversion(Expr *&From, QualType ToType,
                                const ImplicitConversionSequence &ICS,
                                AssignmentAction Action, bool IgnoreBaseAccess) {
  switch (ICS.getKind()) {
  case ImplicitConversionSequence::StandardConversion:
    if (PerformImplicitConversion(From, ToType, ICS.Standard, Action,
                                  IgnoreBaseAccess))
      return true;
    break;

  case ImplicitConversionSequence::UserDefinedConversion: {
    
      FunctionDecl *FD = ICS.UserDefined.ConversionFunction;
      CastExpr::CastKind CastKind = CastExpr::CK_Unknown;
      QualType BeforeToType;
      if (const CXXConversionDecl *Conv = dyn_cast<CXXConversionDecl>(FD)) {
        CastKind = CastExpr::CK_UserDefinedConversion;
        
        // If the user-defined conversion is specified by a conversion function,
        // the initial standard conversion sequence converts the source type to
        // the implicit object parameter of the conversion function.
        BeforeToType = Context.getTagDeclType(Conv->getParent());
      } else if (const CXXConstructorDecl *Ctor = 
                  dyn_cast<CXXConstructorDecl>(FD)) {
        CastKind = CastExpr::CK_ConstructorConversion;
        // Do no conversion if dealing with ... for the first conversion.
        if (!ICS.UserDefined.EllipsisConversion) {
          // If the user-defined conversion is specified by a constructor, the 
          // initial standard conversion sequence converts the source type to the
          // type required by the argument of the constructor
          BeforeToType = Ctor->getParamDecl(0)->getType().getNonReferenceType();
        }
      }    
      else
        assert(0 && "Unknown conversion function kind!");
      // Whatch out for elipsis conversion.
      if (!ICS.UserDefined.EllipsisConversion) {
        if (PerformImplicitConversion(From, BeforeToType, 
                                      ICS.UserDefined.Before, AA_Converting,
                                      IgnoreBaseAccess))
          return true;
      }
    
      OwningExprResult CastArg 
        = BuildCXXCastArgument(From->getLocStart(),
                               ToType.getNonReferenceType(),
                               CastKind, cast<CXXMethodDecl>(FD), 
                               Owned(From));

      if (CastArg.isInvalid())
        return true;

      From = CastArg.takeAs<Expr>();

      return PerformImplicitConversion(From, ToType, ICS.UserDefined.After,
                                       AA_Converting, IgnoreBaseAccess);
  }

  case ImplicitConversionSequence::AmbiguousConversion:
    DiagnoseAmbiguousConversion(ICS, From->getExprLoc(),
                          PDiag(diag::err_typecheck_ambiguous_condition)
                            << From->getSourceRange());
     return true;
      
  case ImplicitConversionSequence::EllipsisConversion:
    assert(false && "Cannot perform an ellipsis conversion");
    return false;

  case ImplicitConversionSequence::BadConversion:
    return true;
  }

  // Everything went well.
  return false;
}

/// PerformImplicitConversion - Perform an implicit conversion of the
/// expression From to the type ToType by following the standard
/// conversion sequence SCS. Returns true if there was an error, false
/// otherwise. The expression From is replaced with the converted
/// expression. Flavor is the context in which we're performing this
/// conversion, for use in error messages.
bool
Sema::PerformImplicitConversion(Expr *&From, QualType ToType,
                                const StandardConversionSequence& SCS,
                                AssignmentAction Action, bool IgnoreBaseAccess) {
  // Overall FIXME: we are recomputing too many types here and doing far too
  // much extra work. What this means is that we need to keep track of more
  // information that is computed when we try the implicit conversion initially,
  // so that we don't need to recompute anything here.
  QualType FromType = From->getType();

  if (SCS.CopyConstructor) {
    // FIXME: When can ToType be a reference type?
    assert(!ToType->isReferenceType());
    if (SCS.Second == ICK_Derived_To_Base) {
      ASTOwningVector<&ActionBase::DeleteExpr> ConstructorArgs(*this);
      if (CompleteConstructorCall(cast<CXXConstructorDecl>(SCS.CopyConstructor),
                                  MultiExprArg(*this, (void **)&From, 1),
                                  /*FIXME:ConstructLoc*/SourceLocation(), 
                                  ConstructorArgs))
        return true;
      OwningExprResult FromResult =
        BuildCXXConstructExpr(/*FIXME:ConstructLoc*/SourceLocation(),
                              ToType, SCS.CopyConstructor,
                              move_arg(ConstructorArgs));
      if (FromResult.isInvalid())
        return true;
      From = FromResult.takeAs<Expr>();
      return false;
    }
    OwningExprResult FromResult =
      BuildCXXConstructExpr(/*FIXME:ConstructLoc*/SourceLocation(),
                            ToType, SCS.CopyConstructor,
                            MultiExprArg(*this, (void**)&From, 1));

    if (FromResult.isInvalid())
      return true;

    From = FromResult.takeAs<Expr>();
    return false;
  }

  // Perform the first implicit conversion.
  switch (SCS.First) {
  case ICK_Identity:
  case ICK_Lvalue_To_Rvalue:
    // Nothing to do.
    break;

  case ICK_Array_To_Pointer:
    FromType = Context.getArrayDecayedType(FromType);
    ImpCastExprToType(From, FromType, CastExpr::CK_ArrayToPointerDecay);
    break;

  case ICK_Function_To_Pointer:
    if (Context.getCanonicalType(FromType) == Context.OverloadTy) {
      FunctionDecl *Fn = ResolveAddressOfOverloadedFunction(From, ToType, true);
      if (!Fn)
        return true;

      if (DiagnoseUseOfDecl(Fn, From->getSourceRange().getBegin()))
        return true;

      From = FixOverloadedFunctionReference(From, Fn);
      FromType = From->getType();
        
      // If there's already an address-of operator in the expression, we have
      // the right type already, and the code below would just introduce an
      // invalid additional pointer level.
      if (FromType->isPointerType() || FromType->isMemberFunctionPointerType())
        break;
    }
    FromType = Context.getPointerType(FromType);
    ImpCastExprToType(From, FromType, CastExpr::CK_FunctionToPointerDecay);
    break;

  default:
    assert(false && "Improper first standard conversion");
    break;
  }

  // Perform the second implicit conversion
  switch (SCS.Second) {
  case ICK_Identity:
    // If both sides are functions (or pointers/references to them), there could
    // be incompatible exception declarations.
    if (CheckExceptionSpecCompatibility(From, ToType))
      return true;
    // Nothing else to do.
    break;

  case ICK_NoReturn_Adjustment:
    // If both sides are functions (or pointers/references to them), there could
    // be incompatible exception declarations.
    if (CheckExceptionSpecCompatibility(From, ToType))
      return true;      
      
    ImpCastExprToType(From, Context.getNoReturnType(From->getType(), false),
                      CastExpr::CK_NoOp);
    break;
      
  case ICK_Integral_Promotion:
  case ICK_Integral_Conversion:
    ImpCastExprToType(From, ToType, CastExpr::CK_IntegralCast);
    break;

  case ICK_Floating_Promotion:
  case ICK_Floating_Conversion:
    ImpCastExprToType(From, ToType, CastExpr::CK_FloatingCast);
    break;

  case ICK_Complex_Promotion:
  case ICK_Complex_Conversion:
    ImpCastExprToType(From, ToType, CastExpr::CK_Unknown);
    break;

  case ICK_Floating_Integral:
    if (ToType->isFloatingType())
      ImpCastExprToType(From, ToType, CastExpr::CK_IntegralToFloating);
    else
      ImpCastExprToType(From, ToType, CastExpr::CK_FloatingToIntegral);
    break;

  case ICK_Complex_Real:
    ImpCastExprToType(From, ToType, CastExpr::CK_Unknown);
    break;

  case ICK_Compatible_Conversion:
    ImpCastExprToType(From, ToType, CastExpr::CK_NoOp);
    break;

  case ICK_Pointer_Conversion: {
    if (SCS.IncompatibleObjC) {
      // Diagnose incompatible Objective-C conversions
      Diag(From->getSourceRange().getBegin(),
           diag::ext_typecheck_convert_incompatible_pointer)
        << From->getType() << ToType << Action
        << From->getSourceRange();
    }

    
    CastExpr::CastKind Kind = CastExpr::CK_Unknown;
    if (CheckPointerConversion(From, ToType, Kind, IgnoreBaseAccess))
      return true;
    ImpCastExprToType(From, ToType, Kind);
    break;
  }
  
  case ICK_Pointer_Member: {
    CastExpr::CastKind Kind = CastExpr::CK_Unknown;
    if (CheckMemberPointerConversion(From, ToType, Kind, IgnoreBaseAccess))
      return true;
    if (CheckExceptionSpecCompatibility(From, ToType))
      return true;
    ImpCastExprToType(From, ToType, Kind);
    break;
  }
  case ICK_Boolean_Conversion: {
    CastExpr::CastKind Kind = CastExpr::CK_Unknown;
    if (FromType->isMemberPointerType())
      Kind = CastExpr::CK_MemberPointerToBoolean;
    
    ImpCastExprToType(From, Context.BoolTy, Kind);
    break;
  }

  case ICK_Derived_To_Base:
    if (CheckDerivedToBaseConversion(From->getType(), 
                                     ToType.getNonReferenceType(),
                                     From->getLocStart(),
                                     From->getSourceRange(),
                                     IgnoreBaseAccess))
      return true;
    ImpCastExprToType(From, ToType.getNonReferenceType(), 
                      CastExpr::CK_DerivedToBase);
    break;
      
  default:
    assert(false && "Improper second standard conversion");
    break;
  }

  switch (SCS.Third) {
  case ICK_Identity:
    // Nothing to do.
    break;

  case ICK_Qualification:
    // FIXME: Not sure about lvalue vs rvalue here in the presence of rvalue
    // references.
    ImpCastExprToType(From, ToType.getNonReferenceType(),
                      CastExpr::CK_NoOp,
                      ToType->isLValueReferenceType());

    if (SCS.DeprecatedStringLiteralToCharPtr)
      Diag(From->getLocStart(), diag::warn_deprecated_string_literal_conversion)
        << ToType.getNonReferenceType();

    break;
      
  default:
    assert(false && "Improper second standard conversion");
    break;
  }

  return false;
}

Sema::OwningExprResult Sema::ActOnUnaryTypeTrait(UnaryTypeTrait OTT,
                                                 SourceLocation KWLoc,
                                                 SourceLocation LParen,
                                                 TypeTy *Ty,
                                                 SourceLocation RParen) {
  QualType T = GetTypeFromParser(Ty);

  // According to http://gcc.gnu.org/onlinedocs/gcc/Type-Traits.html
  // all traits except __is_class, __is_enum and __is_union require a the type
  // to be complete.
  if (OTT != UTT_IsClass && OTT != UTT_IsEnum && OTT != UTT_IsUnion) {
    if (RequireCompleteType(KWLoc, T,
                            diag::err_incomplete_type_used_in_type_trait_expr))
      return ExprError();
  }

  // There is no point in eagerly computing the value. The traits are designed
  // to be used from type trait templates, so Ty will be a template parameter
  // 99% of the time.
  return Owned(new (Context) UnaryTypeTraitExpr(KWLoc, OTT, T,
                                                RParen, Context.BoolTy));
}

QualType Sema::CheckPointerToMemberOperands(
  Expr *&lex, Expr *&rex, SourceLocation Loc, bool isIndirect) {
  const char *OpSpelling = isIndirect ? "->*" : ".*";
  // C++ 5.5p2
  //   The binary operator .* [p3: ->*] binds its second operand, which shall
  //   be of type "pointer to member of T" (where T is a completely-defined
  //   class type) [...]
  QualType RType = rex->getType();
  const MemberPointerType *MemPtr = RType->getAs<MemberPointerType>();
  if (!MemPtr) {
    Diag(Loc, diag::err_bad_memptr_rhs)
      << OpSpelling << RType << rex->getSourceRange();
    return QualType();
  }

  QualType Class(MemPtr->getClass(), 0);

  // C++ 5.5p2
  //   [...] to its first operand, which shall be of class T or of a class of
  //   which T is an unambiguous and accessible base class. [p3: a pointer to
  //   such a class]
  QualType LType = lex->getType();
  if (isIndirect) {
    if (const PointerType *Ptr = LType->getAs<PointerType>())
      LType = Ptr->getPointeeType().getNonReferenceType();
    else {
      Diag(Loc, diag::err_bad_memptr_lhs)
        << OpSpelling << 1 << LType
        << CodeModificationHint::CreateReplacement(SourceRange(Loc), ".*");
      return QualType();
    }
  }

  if (!Context.hasSameUnqualifiedType(Class, LType)) {
    CXXBasePaths Paths(/*FindAmbiguities=*/true, /*RecordPaths=*/false,
                       /*DetectVirtual=*/false);
    // FIXME: Would it be useful to print full ambiguity paths, or is that
    // overkill?
    if (!IsDerivedFrom(LType, Class, Paths) ||
        Paths.isAmbiguous(Context.getCanonicalType(Class))) {
      Diag(Loc, diag::err_bad_memptr_lhs) << OpSpelling
        << (int)isIndirect << lex->getType();
      return QualType();
    }
    // Cast LHS to type of use.
    QualType UseType = isIndirect ? Context.getPointerType(Class) : Class;
    bool isLValue = !isIndirect && lex->isLvalue(Context) == Expr::LV_Valid;
    ImpCastExprToType(lex, UseType, CastExpr::CK_DerivedToBase, isLValue);
  }

  if (isa<CXXZeroInitValueExpr>(rex->IgnoreParens())) {
    // Diagnose use of pointer-to-member type which when used as
    // the functional cast in a pointer-to-member expression.
    Diag(Loc, diag::err_pointer_to_member_type) << isIndirect;
     return QualType();
  }
  // C++ 5.5p2
  //   The result is an object or a function of the type specified by the
  //   second operand.
  // The cv qualifiers are the union of those in the pointer and the left side,
  // in accordance with 5.5p5 and 5.2.5.
  // FIXME: This returns a dereferenced member function pointer as a normal
  // function type. However, the only operation valid on such functions is
  // calling them. There's also a GCC extension to get a function pointer to the
  // thing, which is another complication, because this type - unlike the type
  // that is the result of this expression - takes the class as the first
  // argument.
  // We probably need a "MemberFunctionClosureType" or something like that.
  QualType Result = MemPtr->getPointeeType();
  Result = Context.getCVRQualifiedType(Result, LType.getCVRQualifiers());
  return Result;
}

/// \brief Get the target type of a standard or user-defined conversion.
static QualType TargetType(const ImplicitConversionSequence &ICS) {
  switch (ICS.getKind()) {
  case ImplicitConversionSequence::StandardConversion:
    return ICS.Standard.getToType(2);
  case ImplicitConversionSequence::UserDefinedConversion:
    return ICS.UserDefined.After.getToType(2);
  case ImplicitConversionSequence::AmbiguousConversion:
    return ICS.Ambiguous.getToType();

  case ImplicitConversionSequence::EllipsisConversion:
  case ImplicitConversionSequence::BadConversion:
    llvm_unreachable("function not valid for ellipsis or bad conversions");
  }
  return QualType(); // silence warnings
}

/// \brief Try to convert a type to another according to C++0x 5.16p3.
///
/// This is part of the parameter validation for the ? operator. If either
/// value operand is a class type, the two operands are attempted to be
/// converted to each other. This function does the conversion in one direction.
/// It emits a diagnostic and returns true only if it finds an ambiguous
/// conversion.
static bool TryClassUnification(Sema &Self, Expr *From, Expr *To,
                                SourceLocation QuestionLoc,
                                ImplicitConversionSequence &ICS) {
  // C++0x 5.16p3
  //   The process for determining whether an operand expression E1 of type T1
  //   can be converted to match an operand expression E2 of type T2 is defined
  //   as follows:
  //   -- If E2 is an lvalue:
  if (To->isLvalue(Self.Context) == Expr::LV_Valid) {
    //   E1 can be converted to match E2 if E1 can be implicitly converted to
    //   type "lvalue reference to T2", subject to the constraint that in the
    //   conversion the reference must bind directly to E1.
    if (!Self.CheckReferenceInit(From,
                            Self.Context.getLValueReferenceType(To->getType()),
                                 To->getLocStart(),
                                 /*SuppressUserConversions=*/false,
                                 /*AllowExplicit=*/false,
                                 /*ForceRValue=*/false,
                                 &ICS))
    {
      assert((ICS.isStandard() || ICS.isUserDefined()) &&
             "expected a definite conversion");
      bool DirectBinding =
        ICS.isStandard() ? ICS.Standard.DirectBinding
                         : ICS.UserDefined.After.DirectBinding;
      if (DirectBinding)
        return false;
    }
  }

  //   -- If E2 is an rvalue, or if the conversion above cannot be done:
  //      -- if E1 and E2 have class type, and the underlying class types are
  //         the same or one is a base class of the other:
  QualType FTy = From->getType();
  QualType TTy = To->getType();
  const RecordType *FRec = FTy->getAs<RecordType>();
  const RecordType *TRec = TTy->getAs<RecordType>();
  bool FDerivedFromT = FRec && TRec && Self.IsDerivedFrom(FTy, TTy);
  if (FRec && TRec && (FRec == TRec ||
        FDerivedFromT || Self.IsDerivedFrom(TTy, FTy))) {
    //         E1 can be converted to match E2 if the class of T2 is the
    //         same type as, or a base class of, the class of T1, and
    //         [cv2 > cv1].
    if (FRec == TRec || FDerivedFromT) {
      if (TTy.isAtLeastAsQualifiedAs(FTy)) {
        // Could still fail if there's no copy constructor.
        // FIXME: Is this a hard error then, or just a conversion failure? The
        // standard doesn't say.
        ICS = Self.TryCopyInitialization(From, TTy,
                                         /*SuppressUserConversions=*/false,
                                         /*ForceRValue=*/false,
                                         /*InOverloadResolution=*/false);
      } else {
        ICS.setBad(BadConversionSequence::bad_qualifiers, From, TTy);
      }
    } else {
      // Can't implicitly convert FTy to a derived class TTy.
      // TODO: more specific error for this.
      ICS.setBad(BadConversionSequence::no_conversion, From, TTy);
    }
  } else {
    //     -- Otherwise: E1 can be converted to match E2 if E1 can be
    //        implicitly converted to the type that expression E2 would have
    //        if E2 were converted to an rvalue.
    // First find the decayed type.
    if (TTy->isFunctionType())
      TTy = Self.Context.getPointerType(TTy);
    else if (TTy->isArrayType())
      TTy = Self.Context.getArrayDecayedType(TTy);

    // Now try the implicit conversion.
    // FIXME: This doesn't detect ambiguities.
    ICS = Self.TryImplicitConversion(From, TTy,
                                     /*SuppressUserConversions=*/false,
                                     /*AllowExplicit=*/false,
                                     /*ForceRValue=*/false,
                                     /*InOverloadResolution=*/false);
  }
  return false;
}

/// \brief Try to find a common type for two according to C++0x 5.16p5.
///
/// This is part of the parameter validation for the ? operator. If either
/// value operand is a class type, overload resolution is used to find a
/// conversion to a common type.
static bool FindConditionalOverload(Sema &Self, Expr *&LHS, Expr *&RHS,
                                    SourceLocation Loc) {
  Expr *Args[2] = { LHS, RHS };
  OverloadCandidateSet CandidateSet(Loc);
  Self.AddBuiltinOperatorCandidates(OO_Conditional, Loc, Args, 2, CandidateSet);

  OverloadCandidateSet::iterator Best;
  switch (Self.BestViableFunction(CandidateSet, Loc, Best)) {
    case OR_Success:
      // We found a match. Perform the conversions on the arguments and move on.
      if (Self.PerformImplicitConversion(LHS, Best->BuiltinTypes.ParamTypes[0],
                                         Best->Conversions[0], Sema::AA_Converting) ||
          Self.PerformImplicitConversion(RHS, Best->BuiltinTypes.ParamTypes[1],
                                         Best->Conversions[1], Sema::AA_Converting))
        break;
      return false;

    case OR_No_Viable_Function:
      Self.Diag(Loc, diag::err_typecheck_cond_incompatible_operands)
        << LHS->getType() << RHS->getType()
        << LHS->getSourceRange() << RHS->getSourceRange();
      return true;

    case OR_Ambiguous:
      Self.Diag(Loc, diag::err_conditional_ambiguous_ovl)
        << LHS->getType() << RHS->getType()
        << LHS->getSourceRange() << RHS->getSourceRange();
      // FIXME: Print the possible common types by printing the return types of
      // the viable candidates.
      break;

    case OR_Deleted:
      assert(false && "Conditional operator has only built-in overloads");
      break;
  }
  return true;
}

/// \brief Perform an "extended" implicit conversion as returned by
/// TryClassUnification.
///
/// TryClassUnification generates ICSs that include reference bindings.
/// PerformImplicitConversion is not suitable for this; it chokes if the
/// second part of a standard conversion is ICK_DerivedToBase. This function
/// handles the reference binding specially.
static bool ConvertForConditional(Sema &Self, Expr *&E,
                                  const ImplicitConversionSequence &ICS) {
  if (ICS.isStandard() && ICS.Standard.ReferenceBinding) {
    assert(ICS.Standard.DirectBinding &&
           "TryClassUnification should never generate indirect ref bindings");
    // FIXME: CheckReferenceInit should be able to reuse the ICS instead of
    // redoing all the work.
    return Self.CheckReferenceInit(E, Self.Context.getLValueReferenceType(
                                        TargetType(ICS)),
                                   /*FIXME:*/E->getLocStart(),
                                   /*SuppressUserConversions=*/false,
                                   /*AllowExplicit=*/false,
                                   /*ForceRValue=*/false);
  }
  if (ICS.isUserDefined() && ICS.UserDefined.After.ReferenceBinding) {
    assert(ICS.UserDefined.After.DirectBinding &&
           "TryClassUnification should never generate indirect ref bindings");
    return Self.CheckReferenceInit(E, Self.Context.getLValueReferenceType(
                                        TargetType(ICS)),
                                   /*FIXME:*/E->getLocStart(),
                                   /*SuppressUserConversions=*/false,
                                   /*AllowExplicit=*/false,
                                   /*ForceRValue=*/false);
  }
  if (Self.PerformImplicitConversion(E, TargetType(ICS), ICS, Sema::AA_Converting))
    return true;
  return false;
}

/// \brief Check the operands of ?: under C++ semantics.
///
/// See C++ [expr.cond]. Note that LHS is never null, even for the GNU x ?: y
/// extension. In this case, LHS == Cond. (But they're not aliases.)
QualType Sema::CXXCheckConditionalOperands(Expr *&Cond, Expr *&LHS, Expr *&RHS,
                                           SourceLocation QuestionLoc) {
  // FIXME: Handle C99's complex types, vector types, block pointers and Obj-C++
  // interface pointers.

  // C++0x 5.16p1
  //   The first expression is contextually converted to bool.
  if (!Cond->isTypeDependent()) {
    if (CheckCXXBooleanCondition(Cond))
      return QualType();
  }

  // Either of the arguments dependent?
  if (LHS->isTypeDependent() || RHS->isTypeDependent())
    return Context.DependentTy;

  CheckSignCompare(LHS, RHS, QuestionLoc, diag::warn_mixed_sign_conditional);

  // C++0x 5.16p2
  //   If either the second or the third operand has type (cv) void, ...
  QualType LTy = LHS->getType();
  QualType RTy = RHS->getType();
  bool LVoid = LTy->isVoidType();
  bool RVoid = RTy->isVoidType();
  if (LVoid || RVoid) {
    //   ... then the [l2r] conversions are performed on the second and third
    //   operands ...
    DefaultFunctionArrayLvalueConversion(LHS);
    DefaultFunctionArrayLvalueConversion(RHS);
    LTy = LHS->getType();
    RTy = RHS->getType();

    //   ... and one of the following shall hold:
    //   -- The second or the third operand (but not both) is a throw-
    //      expression; the result is of the type of the other and is an rvalue.
    bool LThrow = isa<CXXThrowExpr>(LHS);
    bool RThrow = isa<CXXThrowExpr>(RHS);
    if (LThrow && !RThrow)
      return RTy;
    if (RThrow && !LThrow)
      return LTy;

    //   -- Both the second and third operands have type void; the result is of
    //      type void and is an rvalue.
    if (LVoid && RVoid)
      return Context.VoidTy;

    // Neither holds, error.
    Diag(QuestionLoc, diag::err_conditional_void_nonvoid)
      << (LVoid ? RTy : LTy) << (LVoid ? 0 : 1)
      << LHS->getSourceRange() << RHS->getSourceRange();
    return QualType();
  }

  // Neither is void.

  // C++0x 5.16p3
  //   Otherwise, if the second and third operand have different types, and
  //   either has (cv) class type, and attempt is made to convert each of those
  //   operands to the other.
  if (Context.getCanonicalType(LTy) != Context.getCanonicalType(RTy) &&
      (LTy->isRecordType() || RTy->isRecordType())) {
    ImplicitConversionSequence ICSLeftToRight, ICSRightToLeft;
    // These return true if a single direction is already ambiguous.
    if (TryClassUnification(*this, LHS, RHS, QuestionLoc, ICSLeftToRight))
      return QualType();
    if (TryClassUnification(*this, RHS, LHS, QuestionLoc, ICSRightToLeft))
      return QualType();

    bool HaveL2R = !ICSLeftToRight.isBad();
    bool HaveR2L = !ICSRightToLeft.isBad();
    //   If both can be converted, [...] the program is ill-formed.
    if (HaveL2R && HaveR2L) {
      Diag(QuestionLoc, diag::err_conditional_ambiguous)
        << LTy << RTy << LHS->getSourceRange() << RHS->getSourceRange();
      return QualType();
    }

    //   If exactly one conversion is possible, that conversion is applied to
    //   the chosen operand and the converted operands are used in place of the
    //   original operands for the remainder of this section.
    if (HaveL2R) {
      if (ConvertForConditional(*this, LHS, ICSLeftToRight))
        return QualType();
      LTy = LHS->getType();
    } else if (HaveR2L) {
      if (ConvertForConditional(*this, RHS, ICSRightToLeft))
        return QualType();
      RTy = RHS->getType();
    }
  }

  // C++0x 5.16p4
  //   If the second and third operands are lvalues and have the same type,
  //   the result is of that type [...]
  bool Same = Context.getCanonicalType(LTy) == Context.getCanonicalType(RTy);
  if (Same && LHS->isLvalue(Context) == Expr::LV_Valid &&
      RHS->isLvalue(Context) == Expr::LV_Valid)
    return LTy;

  // C++0x 5.16p5
  //   Otherwise, the result is an rvalue. If the second and third operands
  //   do not have the same type, and either has (cv) class type, ...
  if (!Same && (LTy->isRecordType() || RTy->isRecordType())) {
    //   ... overload resolution is used to determine the conversions (if any)
    //   to be applied to the operands. If the overload resolution fails, the
    //   program is ill-formed.
    if (FindConditionalOverload(*this, LHS, RHS, QuestionLoc))
      return QualType();
  }

  // C++0x 5.16p6
  //   LValue-to-rvalue, array-to-pointer, and function-to-pointer standard
  //   conversions are performed on the second and third operands.
  DefaultFunctionArrayLvalueConversion(LHS);
  DefaultFunctionArrayLvalueConversion(RHS);
  LTy = LHS->getType();
  RTy = RHS->getType();

  //   After those conversions, one of the following shall hold:
  //   -- The second and third operands have the same type; the result
  //      is of that type.
  if (Context.getCanonicalType(LTy) == Context.getCanonicalType(RTy))
    return LTy;

  //   -- The second and third operands have arithmetic or enumeration type;
  //      the usual arithmetic conversions are performed to bring them to a
  //      common type, and the result is of that type.
  if (LTy->isArithmeticType() && RTy->isArithmeticType()) {
    UsualArithmeticConversions(LHS, RHS);
    return LHS->getType();
  }

  //   -- The second and third operands have pointer type, or one has pointer
  //      type and the other is a null pointer constant; pointer conversions
  //      and qualification conversions are performed to bring them to their
  //      composite pointer type. The result is of the composite pointer type.
  //   -- The second and third operands have pointer to member type, or one has
  //      pointer to member type and the other is a null pointer constant;
  //      pointer to member conversions and qualification conversions are
  //      performed to bring them to a common type, whose cv-qualification
  //      shall match the cv-qualification of either the second or the third
  //      operand. The result is of the common type.
  bool NonStandardCompositeType = false;
  QualType Composite = FindCompositePointerType(LHS, RHS,
                              isSFINAEContext()? 0 : &NonStandardCompositeType);
  if (!Composite.isNull()) {
    if (NonStandardCompositeType)
      Diag(QuestionLoc, 
           diag::ext_typecheck_cond_incompatible_operands_nonstandard)
        << LTy << RTy << Composite
        << LHS->getSourceRange() << RHS->getSourceRange();
      
    return Composite;
  }
  
  // Similarly, attempt to find composite type of twp objective-c pointers.
  Composite = FindCompositeObjCPointerType(LHS, RHS, QuestionLoc);
  if (!Composite.isNull())
    return Composite;

  Diag(QuestionLoc, diag::err_typecheck_cond_incompatible_operands)
    << LHS->getType() << RHS->getType()
    << LHS->getSourceRange() << RHS->getSourceRange();
  return QualType();
}

/// \brief Find a merged pointer type and convert the two expressions to it.
///
/// This finds the composite pointer type (or member pointer type) for @p E1
/// and @p E2 according to C++0x 5.9p2. It converts both expressions to this
/// type and returns it.
/// It does not emit diagnostics.
///
/// If \p NonStandardCompositeType is non-NULL, then we are permitted to find
/// a non-standard (but still sane) composite type to which both expressions
/// can be converted. When such a type is chosen, \c *NonStandardCompositeType
/// will be set true.
QualType Sema::FindCompositePointerType(Expr *&E1, Expr *&E2,
                                        bool *NonStandardCompositeType) {
  if (NonStandardCompositeType)
    *NonStandardCompositeType = false;
  
  assert(getLangOptions().CPlusPlus && "This function assumes C++");
  QualType T1 = E1->getType(), T2 = E2->getType();

  if (!T1->isAnyPointerType() && !T1->isMemberPointerType() &&
      !T2->isAnyPointerType() && !T2->isMemberPointerType())
   return QualType();

  // C++0x 5.9p2
  //   Pointer conversions and qualification conversions are performed on
  //   pointer operands to bring them to their composite pointer type. If
  //   one operand is a null pointer constant, the composite pointer type is
  //   the type of the other operand.
  if (E1->isNullPointerConstant(Context, Expr::NPC_ValueDependentIsNull)) {
    if (T2->isMemberPointerType())
      ImpCastExprToType(E1, T2, CastExpr::CK_NullToMemberPointer);
    else
      ImpCastExprToType(E1, T2, CastExpr::CK_IntegralToPointer);
    return T2;
  }
  if (E2->isNullPointerConstant(Context, Expr::NPC_ValueDependentIsNull)) {
    if (T1->isMemberPointerType())
      ImpCastExprToType(E2, T1, CastExpr::CK_NullToMemberPointer);
    else
      ImpCastExprToType(E2, T1, CastExpr::CK_IntegralToPointer);
    return T1;
  }

  // Now both have to be pointers or member pointers.
  if ((!T1->isPointerType() && !T1->isMemberPointerType()) ||
      (!T2->isPointerType() && !T2->isMemberPointerType()))
    return QualType();

  //   Otherwise, of one of the operands has type "pointer to cv1 void," then
  //   the other has type "pointer to cv2 T" and the composite pointer type is
  //   "pointer to cv12 void," where cv12 is the union of cv1 and cv2.
  //   Otherwise, the composite pointer type is a pointer type similar to the
  //   type of one of the operands, with a cv-qualification signature that is
  //   the union of the cv-qualification signatures of the operand types.
  // In practice, the first part here is redundant; it's subsumed by the second.
  // What we do here is, we build the two possible composite types, and try the
  // conversions in both directions. If only one works, or if the two composite
  // types are the same, we have succeeded.
  // FIXME: extended qualifiers?
  typedef llvm::SmallVector<unsigned, 4> QualifierVector;
  QualifierVector QualifierUnion;
  typedef llvm::SmallVector<std::pair<const Type *, const Type *>, 4>
      ContainingClassVector;
  ContainingClassVector MemberOfClass;
  QualType Composite1 = Context.getCanonicalType(T1),
           Composite2 = Context.getCanonicalType(T2);
  unsigned NeedConstBefore = 0;  
  do {
    const PointerType *Ptr1, *Ptr2;
    if ((Ptr1 = Composite1->getAs<PointerType>()) &&
        (Ptr2 = Composite2->getAs<PointerType>())) {
      Composite1 = Ptr1->getPointeeType();
      Composite2 = Ptr2->getPointeeType();
      
      // If we're allowed to create a non-standard composite type, keep track
      // of where we need to fill in additional 'const' qualifiers. 
      if (NonStandardCompositeType &&
          Composite1.getCVRQualifiers() != Composite2.getCVRQualifiers())
        NeedConstBefore = QualifierUnion.size();
      
      QualifierUnion.push_back(
                 Composite1.getCVRQualifiers() | Composite2.getCVRQualifiers());
      MemberOfClass.push_back(std::make_pair((const Type *)0, (const Type *)0));
      continue;
    }

    const MemberPointerType *MemPtr1, *MemPtr2;
    if ((MemPtr1 = Composite1->getAs<MemberPointerType>()) &&
        (MemPtr2 = Composite2->getAs<MemberPointerType>())) {
      Composite1 = MemPtr1->getPointeeType();
      Composite2 = MemPtr2->getPointeeType();
      
      // If we're allowed to create a non-standard composite type, keep track
      // of where we need to fill in additional 'const' qualifiers. 
      if (NonStandardCompositeType &&
          Composite1.getCVRQualifiers() != Composite2.getCVRQualifiers())
        NeedConstBefore = QualifierUnion.size();
      
      QualifierUnion.push_back(
                 Composite1.getCVRQualifiers() | Composite2.getCVRQualifiers());
      MemberOfClass.push_back(std::make_pair(MemPtr1->getClass(),
                                             MemPtr2->getClass()));
      continue;
    }

    // FIXME: block pointer types?

    // Cannot unwrap any more types.
    break;
  } while (true);

  if (NeedConstBefore && NonStandardCompositeType) {
    // Extension: Add 'const' to qualifiers that come before the first qualifier
    // mismatch, so that our (non-standard!) composite type meets the 
    // requirements of C++ [conv.qual]p4 bullet 3.
    for (unsigned I = 0; I != NeedConstBefore; ++I) {
      if ((QualifierUnion[I] & Qualifiers::Const) == 0) {
        QualifierUnion[I] = QualifierUnion[I] | Qualifiers::Const;
        *NonStandardCompositeType = true;
      }
    }
  }
  
  // Rewrap the composites as pointers or member pointers with the union CVRs.
  ContainingClassVector::reverse_iterator MOC
    = MemberOfClass.rbegin();
  for (QualifierVector::reverse_iterator
         I = QualifierUnion.rbegin(),
         E = QualifierUnion.rend();
       I != E; (void)++I, ++MOC) {
    Qualifiers Quals = Qualifiers::fromCVRMask(*I);
    if (MOC->first && MOC->second) {
      // Rebuild member pointer type
      Composite1 = Context.getMemberPointerType(
                                    Context.getQualifiedType(Composite1, Quals),
                                    MOC->first);
      Composite2 = Context.getMemberPointerType(
                                    Context.getQualifiedType(Composite2, Quals),
                                    MOC->second);
    } else {
      // Rebuild pointer type
      Composite1
        = Context.getPointerType(Context.getQualifiedType(Composite1, Quals));
      Composite2
        = Context.getPointerType(Context.getQualifiedType(Composite2, Quals));
    }
  }

  ImplicitConversionSequence E1ToC1 =
    TryImplicitConversion(E1, Composite1,
                          /*SuppressUserConversions=*/false,
                          /*AllowExplicit=*/false,
                          /*ForceRValue=*/false,
                          /*InOverloadResolution=*/false);
  ImplicitConversionSequence E2ToC1 =
    TryImplicitConversion(E2, Composite1,
                          /*SuppressUserConversions=*/false,
                          /*AllowExplicit=*/false,
                          /*ForceRValue=*/false,
                          /*InOverloadResolution=*/false);

  bool ToC2Viable = false;
  ImplicitConversionSequence E1ToC2, E2ToC2;
  if (Context.getCanonicalType(Composite1) !=
      Context.getCanonicalType(Composite2)) {
    E1ToC2 = TryImplicitConversion(E1, Composite2,
                                   /*SuppressUserConversions=*/false,
                                   /*AllowExplicit=*/false,
                                   /*ForceRValue=*/false,
                                   /*InOverloadResolution=*/false);
    E2ToC2 = TryImplicitConversion(E2, Composite2,
                                   /*SuppressUserConversions=*/false,
                                   /*AllowExplicit=*/false,
                                   /*ForceRValue=*/false,
                                   /*InOverloadResolution=*/false);
    ToC2Viable = !E1ToC2.isBad() && !E2ToC2.isBad();
  }

  bool ToC1Viable = !E1ToC1.isBad() && !E2ToC1.isBad();
  if (ToC1Viable && !ToC2Viable) {
    if (!PerformImplicitConversion(E1, Composite1, E1ToC1, Sema::AA_Converting) &&
        !PerformImplicitConversion(E2, Composite1, E2ToC1, Sema::AA_Converting))
      return Composite1;
  }
  if (ToC2Viable && !ToC1Viable) {
    if (!PerformImplicitConversion(E1, Composite2, E1ToC2, Sema::AA_Converting) &&
        !PerformImplicitConversion(E2, Composite2, E2ToC2, Sema::AA_Converting))
      return Composite2;
  }
  return QualType();
}

Sema::OwningExprResult Sema::MaybeBindToTemporary(Expr *E) {
  if (!Context.getLangOptions().CPlusPlus)
    return Owned(E);

  assert(!isa<CXXBindTemporaryExpr>(E) && "Double-bound temporary?");

  const RecordType *RT = E->getType()->getAs<RecordType>();
  if (!RT)
    return Owned(E);

  // If this is the result of a call expression, our source might
  // actually be a reference, in which case we shouldn't bind.
  if (CallExpr *CE = dyn_cast<CallExpr>(E)) {
    QualType Ty = CE->getCallee()->getType();
    if (const PointerType *PT = Ty->getAs<PointerType>())
      Ty = PT->getPointeeType();
    else if (const BlockPointerType *BPT = Ty->getAs<BlockPointerType>())
      Ty = BPT->getPointeeType();

    const FunctionType *FTy = Ty->getAs<FunctionType>();
    if (FTy->getResultType()->isReferenceType())
      return Owned(E);
  }

  // That should be enough to guarantee that this type is complete.
  // If it has a trivial destructor, we can avoid the extra copy.
  CXXRecordDecl *RD = cast<CXXRecordDecl>(RT->getDecl());
  if (RD->hasTrivialDestructor())
    return Owned(E);

  CXXTemporary *Temp = CXXTemporary::Create(Context,
                                            RD->getDestructor(Context));
  ExprTemporaries.push_back(Temp);
  if (CXXDestructorDecl *Destructor =
        const_cast<CXXDestructorDecl*>(RD->getDestructor(Context)))
    MarkDeclarationReferenced(E->getExprLoc(), Destructor);
  // FIXME: Add the temporary to the temporaries vector.
  return Owned(CXXBindTemporaryExpr::Create(Context, Temp, E));
}

Expr *Sema::MaybeCreateCXXExprWithTemporaries(Expr *SubExpr) {
  assert(SubExpr && "sub expression can't be null!");

  unsigned FirstTemporary = ExprEvalContexts.back().NumTemporaries;
  assert(ExprTemporaries.size() >= FirstTemporary);
  if (ExprTemporaries.size() == FirstTemporary)
    return SubExpr;

  Expr *E = CXXExprWithTemporaries::Create(Context, SubExpr,
                                           &ExprTemporaries[FirstTemporary],
                                       ExprTemporaries.size() - FirstTemporary);
  ExprTemporaries.erase(ExprTemporaries.begin() + FirstTemporary,
                        ExprTemporaries.end());

  return E;
}

Sema::OwningExprResult 
Sema::MaybeCreateCXXExprWithTemporaries(OwningExprResult SubExpr) {
  if (SubExpr.isInvalid())
    return ExprError();
  
  return Owned(MaybeCreateCXXExprWithTemporaries(SubExpr.takeAs<Expr>()));
}

FullExpr Sema::CreateFullExpr(Expr *SubExpr) {
  unsigned FirstTemporary = ExprEvalContexts.back().NumTemporaries;
  assert(ExprTemporaries.size() >= FirstTemporary);
  
  unsigned NumTemporaries = ExprTemporaries.size() - FirstTemporary;
  CXXTemporary **Temporaries = 
    NumTemporaries == 0 ? 0 : &ExprTemporaries[FirstTemporary];
  
  FullExpr E = FullExpr::Create(Context, SubExpr, Temporaries, NumTemporaries);

  ExprTemporaries.erase(ExprTemporaries.begin() + FirstTemporary,
                        ExprTemporaries.end());

  return E;
}

Sema::OwningExprResult
Sema::ActOnStartCXXMemberReference(Scope *S, ExprArg Base, SourceLocation OpLoc,
                                   tok::TokenKind OpKind, TypeTy *&ObjectType,
                                   bool &MayBePseudoDestructor) {
  // Since this might be a postfix expression, get rid of ParenListExprs.
  Base = MaybeConvertParenListExprToParenExpr(S, move(Base));

  Expr *BaseExpr = (Expr*)Base.get();
  assert(BaseExpr && "no record expansion");

  QualType BaseType = BaseExpr->getType();
  MayBePseudoDestructor = false;
  if (BaseType->isDependentType()) {
    // If we have a pointer to a dependent type and are using the -> operator,
    // the object type is the type that the pointer points to. We might still
    // have enough information about that type to do something useful.
    if (OpKind == tok::arrow)
      if (const PointerType *Ptr = BaseType->getAs<PointerType>())
        BaseType = Ptr->getPointeeType();
    
    ObjectType = BaseType.getAsOpaquePtr();
    MayBePseudoDestructor = true;
    return move(Base);
  }

  // C++ [over.match.oper]p8:
  //   [...] When operator->returns, the operator-> is applied  to the value
  //   returned, with the original second operand.
  if (OpKind == tok::arrow) {
    // The set of types we've considered so far.
    llvm::SmallPtrSet<CanQualType,8> CTypes;
    llvm::SmallVector<SourceLocation, 8> Locations;
    CTypes.insert(Context.getCanonicalType(BaseType));
    
    while (BaseType->isRecordType()) {
      Base = BuildOverloadedArrowExpr(S, move(Base), OpLoc);
      BaseExpr = (Expr*)Base.get();
      if (BaseExpr == NULL)
        return ExprError();
      if (CXXOperatorCallExpr *OpCall = dyn_cast<CXXOperatorCallExpr>(BaseExpr))
        Locations.push_back(OpCall->getDirectCallee()->getLocation());
      BaseType = BaseExpr->getType();
      CanQualType CBaseType = Context.getCanonicalType(BaseType);
      if (!CTypes.insert(CBaseType)) {
        Diag(OpLoc, diag::err_operator_arrow_circular);
        for (unsigned i = 0; i < Locations.size(); i++)
          Diag(Locations[i], diag::note_declared_at);
        return ExprError();
      }
    }

    if (BaseType->isPointerType())
      BaseType = BaseType->getPointeeType();
  }

  // We could end up with various non-record types here, such as extended
  // vector types or Objective-C interfaces. Just return early and let
  // ActOnMemberReferenceExpr do the work.
  if (!BaseType->isRecordType()) {
    // C++ [basic.lookup.classref]p2:
    //   [...] If the type of the object expression is of pointer to scalar
    //   type, the unqualified-id is looked up in the context of the complete
    //   postfix-expression.
    //
    // This also indicates that we should be parsing a
    // pseudo-destructor-name.
    ObjectType = 0;
    MayBePseudoDestructor = true;
    return move(Base);
  }

  // The object type must be complete (or dependent).
  if (!BaseType->isDependentType() &&
      RequireCompleteType(OpLoc, BaseType, 
                          PDiag(diag::err_incomplete_member_access)))
    return ExprError();
  
  // C++ [basic.lookup.classref]p2:
  //   If the id-expression in a class member access (5.2.5) is an
  //   unqualified-id, and the type of the object expression is of a class
  //   type C (or of pointer to a class type C), the unqualified-id is looked
  //   up in the scope of class C. [...]
  ObjectType = BaseType.getAsOpaquePtr();
  return move(Base);
}

Sema::OwningExprResult Sema::DiagnoseDtorReference(SourceLocation NameLoc,
                                                   ExprArg MemExpr) {
  Expr *E = (Expr *) MemExpr.get();
  SourceLocation ExpectedLParenLoc = PP.getLocForEndOfToken(NameLoc);
  Diag(E->getLocStart(), diag::err_dtor_expr_without_call)
    << isa<CXXPseudoDestructorExpr>(E)
    << CodeModificationHint::CreateInsertion(ExpectedLParenLoc, "()");
  
  return ActOnCallExpr(/*Scope*/ 0,
                       move(MemExpr),
                       /*LPLoc*/ ExpectedLParenLoc,
                       Sema::MultiExprArg(*this, 0, 0),
                       /*CommaLocs*/ 0,
                       /*RPLoc*/ ExpectedLParenLoc);
}

Sema::OwningExprResult Sema::BuildPseudoDestructorExpr(ExprArg Base,
                                                       SourceLocation OpLoc,
                                                       tok::TokenKind OpKind,
                                                       const CXXScopeSpec &SS,
                                                 TypeSourceInfo *ScopeTypeInfo,
                                                       SourceLocation CCLoc,
                                                       SourceLocation TildeLoc,
                                         PseudoDestructorTypeStorage Destructed,
                                                       bool HasTrailingLParen) {
  TypeSourceInfo *DestructedTypeInfo = Destructed.getTypeSourceInfo();
  
  // C++ [expr.pseudo]p2:
  //   The left-hand side of the dot operator shall be of scalar type. The 
  //   left-hand side of the arrow operator shall be of pointer to scalar type.
  //   This scalar type is the object type. 
  Expr *BaseE = (Expr *)Base.get();
  QualType ObjectType = BaseE->getType();
  if (OpKind == tok::arrow) {
    if (const PointerType *Ptr = ObjectType->getAs<PointerType>()) {
      ObjectType = Ptr->getPointeeType();
    } else if (!BaseE->isTypeDependent()) {
      // The user wrote "p->" when she probably meant "p."; fix it.
      Diag(OpLoc, diag::err_typecheck_member_reference_suggestion)
        << ObjectType << true
        << CodeModificationHint::CreateReplacement(OpLoc, ".");
      if (isSFINAEContext())
        return ExprError();
      
      OpKind = tok::period;
    }
  }
  
  if (!ObjectType->isDependentType() && !ObjectType->isScalarType()) {
    Diag(OpLoc, diag::err_pseudo_dtor_base_not_scalar)
      << ObjectType << BaseE->getSourceRange();
    return ExprError();
  }

  // C++ [expr.pseudo]p2:
  //   [...] The cv-unqualified versions of the object type and of the type 
  //   designated by the pseudo-destructor-name shall be the same type.
  if (DestructedTypeInfo) {
    QualType DestructedType = DestructedTypeInfo->getType();
    SourceLocation DestructedTypeStart
      = DestructedTypeInfo->getTypeLoc().getSourceRange().getBegin();
    if (!DestructedType->isDependentType() && !ObjectType->isDependentType() &&
        !Context.hasSameUnqualifiedType(DestructedType, ObjectType)) {
      Diag(DestructedTypeStart, diag::err_pseudo_dtor_type_mismatch)
        << ObjectType << DestructedType << BaseE->getSourceRange()
        << DestructedTypeInfo->getTypeLoc().getSourceRange();
      
      // Recover by setting the destructed type to the object type.
      DestructedType = ObjectType;
      DestructedTypeInfo = Context.getTrivialTypeSourceInfo(ObjectType,
                                                           DestructedTypeStart);
      Destructed = PseudoDestructorTypeStorage(DestructedTypeInfo);
    }
  }
  
  // C++ [expr.pseudo]p2:
  //   [...] Furthermore, the two type-names in a pseudo-destructor-name of the
  //   form
  //
  //     ::[opt] nested-name-specifier[opt] type-name :: ~ type-name 
  //
  //   shall designate the same scalar type.
  if (ScopeTypeInfo) {
    QualType ScopeType = ScopeTypeInfo->getType();
    if (!ScopeType->isDependentType() && !ObjectType->isDependentType() &&
        !Context.hasSameType(ScopeType, ObjectType)) {
      
      Diag(ScopeTypeInfo->getTypeLoc().getSourceRange().getBegin(),
           diag::err_pseudo_dtor_type_mismatch)
        << ObjectType << ScopeType << BaseE->getSourceRange()
        << ScopeTypeInfo->getTypeLoc().getSourceRange();
  
      ScopeType = QualType();
      ScopeTypeInfo = 0;
    }
  }
  
  OwningExprResult Result
    = Owned(new (Context) CXXPseudoDestructorExpr(Context, 
                                                  Base.takeAs<Expr>(),
                                                  OpKind == tok::arrow,
                                                  OpLoc,
                                       (NestedNameSpecifier *) SS.getScopeRep(),
                                                  SS.getRange(),
                                                  ScopeTypeInfo,
                                                  CCLoc,
                                                  TildeLoc,
                                                  Destructed));
            
  if (HasTrailingLParen)
    return move(Result);
  
  return DiagnoseDtorReference(Destructed.getLocation(), move(Result));
}

Sema::OwningExprResult Sema::ActOnPseudoDestructorExpr(Scope *S, ExprArg Base,
                                                       SourceLocation OpLoc,
                                                       tok::TokenKind OpKind,
                                                       const CXXScopeSpec &SS,
                                                  UnqualifiedId &FirstTypeName,
                                                       SourceLocation CCLoc,
                                                       SourceLocation TildeLoc,
                                                 UnqualifiedId &SecondTypeName,
                                                       bool HasTrailingLParen) {
  assert((FirstTypeName.getKind() == UnqualifiedId::IK_TemplateId ||
          FirstTypeName.getKind() == UnqualifiedId::IK_Identifier) &&
         "Invalid first type name in pseudo-destructor");
  assert((SecondTypeName.getKind() == UnqualifiedId::IK_TemplateId ||
          SecondTypeName.getKind() == UnqualifiedId::IK_Identifier) &&
         "Invalid second type name in pseudo-destructor");

  Expr *BaseE = (Expr *)Base.get();
  
  // C++ [expr.pseudo]p2:
  //   The left-hand side of the dot operator shall be of scalar type. The 
  //   left-hand side of the arrow operator shall be of pointer to scalar type.
  //   This scalar type is the object type. 
  QualType ObjectType = BaseE->getType();
  if (OpKind == tok::arrow) {
    if (const PointerType *Ptr = ObjectType->getAs<PointerType>()) {
      ObjectType = Ptr->getPointeeType();
    } else if (!ObjectType->isDependentType()) {
      // The user wrote "p->" when she probably meant "p."; fix it.
      Diag(OpLoc, diag::err_typecheck_member_reference_suggestion)
        << ObjectType << true
        << CodeModificationHint::CreateReplacement(OpLoc, ".");
      if (isSFINAEContext())
        return ExprError();
      
      OpKind = tok::period;
    }
  }

  // Compute the object type that we should use for name lookup purposes. Only
  // record types and dependent types matter.
  void *ObjectTypePtrForLookup = 0;
  if (!SS.isSet()) {
    ObjectTypePtrForLookup = (void *)ObjectType->getAs<RecordType>();
    if (!ObjectTypePtrForLookup && ObjectType->isDependentType())
      ObjectTypePtrForLookup = Context.DependentTy.getAsOpaquePtr();
  }
  
  // Convert the name of the type being destructed (following the ~) into a 
  // type (with source-location information).
  QualType DestructedType;
  TypeSourceInfo *DestructedTypeInfo = 0;
  PseudoDestructorTypeStorage Destructed;
  if (SecondTypeName.getKind() == UnqualifiedId::IK_Identifier) {
    TypeTy *T = getTypeName(*SecondTypeName.Identifier, 
                            SecondTypeName.StartLocation,
                            S, &SS, true, ObjectTypePtrForLookup);
    if (!T && 
        ((SS.isSet() && !computeDeclContext(SS, false)) ||
         (!SS.isSet() && ObjectType->isDependentType()))) {
      // The name of the type being destroyed is a dependent name, and we 
      // couldn't find anything useful in scope. Just store the identifier and
      // it's location, and we'll perform (qualified) name lookup again at
      // template instantiation time.
      Destructed = PseudoDestructorTypeStorage(SecondTypeName.Identifier,
                                               SecondTypeName.StartLocation);
    } else if (!T) {
      Diag(SecondTypeName.StartLocation, 
           diag::err_pseudo_dtor_destructor_non_type)
        << SecondTypeName.Identifier << ObjectType;
      if (isSFINAEContext())
        return ExprError();
      
      // Recover by assuming we had the right type all along.
      DestructedType = ObjectType;
    } else
      DestructedType = GetTypeFromParser(T, &DestructedTypeInfo);
  } else {
    // Resolve the template-id to a type.
    TemplateIdAnnotation *TemplateId = SecondTypeName.TemplateId;
    ASTTemplateArgsPtr TemplateArgsPtr(*this,
                                       TemplateId->getTemplateArgs(),
                                       TemplateId->NumArgs);
    TypeResult T = ActOnTemplateIdType(TemplateTy::make(TemplateId->Template),
                                       TemplateId->TemplateNameLoc,
                                       TemplateId->LAngleLoc,
                                       TemplateArgsPtr,
                                       TemplateId->RAngleLoc);
    if (T.isInvalid() || !T.get()) {
      // Recover by assuming we had the right type all along.
      DestructedType = ObjectType;
    } else
      DestructedType = GetTypeFromParser(T.get(), &DestructedTypeInfo);
  }
  
  // If we've performed some kind of recovery, (re-)build the type source 
  // information.
  if (!DestructedType.isNull()) {
    if (!DestructedTypeInfo)
      DestructedTypeInfo = Context.getTrivialTypeSourceInfo(DestructedType,
                                                  SecondTypeName.StartLocation);
    Destructed = PseudoDestructorTypeStorage(DestructedTypeInfo);
  }
  
  // Convert the name of the scope type (the type prior to '::') into a type.
  TypeSourceInfo *ScopeTypeInfo = 0;
  QualType ScopeType;
  if (FirstTypeName.getKind() == UnqualifiedId::IK_TemplateId || 
      FirstTypeName.Identifier) {
    if (FirstTypeName.getKind() == UnqualifiedId::IK_Identifier) {
      TypeTy *T = getTypeName(*FirstTypeName.Identifier, 
                              FirstTypeName.StartLocation,
                              S, &SS, false, ObjectTypePtrForLookup);
      if (!T) {
        Diag(FirstTypeName.StartLocation, 
             diag::err_pseudo_dtor_destructor_non_type)
          << FirstTypeName.Identifier << ObjectType;
        
        if (isSFINAEContext())
          return ExprError();
        
        // Just drop this type. It's unnecessary anyway.
        ScopeType = QualType();
      } else
        ScopeType = GetTypeFromParser(T, &ScopeTypeInfo);
    } else {
      // Resolve the template-id to a type.
      TemplateIdAnnotation *TemplateId = FirstTypeName.TemplateId;
      ASTTemplateArgsPtr TemplateArgsPtr(*this,
                                         TemplateId->getTemplateArgs(),
                                         TemplateId->NumArgs);
      TypeResult T = ActOnTemplateIdType(TemplateTy::make(TemplateId->Template),
                                         TemplateId->TemplateNameLoc,
                                         TemplateId->LAngleLoc,
                                         TemplateArgsPtr,
                                         TemplateId->RAngleLoc);
      if (T.isInvalid() || !T.get()) {
        // Recover by dropping this type.
        ScopeType = QualType();
      } else
        ScopeType = GetTypeFromParser(T.get(), &ScopeTypeInfo);      
    }
  }
      
  if (!ScopeType.isNull() && !ScopeTypeInfo)
    ScopeTypeInfo = Context.getTrivialTypeSourceInfo(ScopeType,
                                                  FirstTypeName.StartLocation);

    
  return BuildPseudoDestructorExpr(move(Base), OpLoc, OpKind, SS,
                                   ScopeTypeInfo, CCLoc, TildeLoc,
                                   Destructed, HasTrailingLParen);
}

CXXMemberCallExpr *Sema::BuildCXXMemberCallExpr(Expr *Exp, 
                                                CXXMethodDecl *Method) {
  if (PerformObjectArgumentInitialization(Exp, /*Qualifier=*/0, Method))
    assert(0 && "Calling BuildCXXMemberCallExpr with invalid call?");

  MemberExpr *ME = 
      new (Context) MemberExpr(Exp, /*IsArrow=*/false, Method, 
                               SourceLocation(), Method->getType());
  QualType ResultType = Method->getResultType().getNonReferenceType();
  MarkDeclarationReferenced(Exp->getLocStart(), Method);
  CXXMemberCallExpr *CE =
    new (Context) CXXMemberCallExpr(Context, ME, 0, 0, ResultType,
                                    Exp->getLocEnd());
  return CE;
}

Sema::OwningExprResult Sema::BuildCXXCastArgument(SourceLocation CastLoc,
                                                  QualType Ty,
                                                  CastExpr::CastKind Kind,
                                                  CXXMethodDecl *Method,
                                                  ExprArg Arg) {
  Expr *From = Arg.takeAs<Expr>();

  switch (Kind) {
  default: assert(0 && "Unhandled cast kind!");
  case CastExpr::CK_ConstructorConversion: {
    ASTOwningVector<&ActionBase::DeleteExpr> ConstructorArgs(*this);
    
    if (CompleteConstructorCall(cast<CXXConstructorDecl>(Method),
                                MultiExprArg(*this, (void **)&From, 1),
                                CastLoc, ConstructorArgs))
      return ExprError();
    
    OwningExprResult Result = 
      BuildCXXConstructExpr(CastLoc, Ty, cast<CXXConstructorDecl>(Method), 
                            move_arg(ConstructorArgs));
    if (Result.isInvalid())
      return ExprError();
    
    return MaybeBindToTemporary(Result.takeAs<Expr>());
  }

  case CastExpr::CK_UserDefinedConversion: {
    assert(!From->getType()->isPointerType() && "Arg can't have pointer type!");

    // Create an implicit call expr that calls it.
    CXXMemberCallExpr *CE = BuildCXXMemberCallExpr(From, Method);
    return MaybeBindToTemporary(CE);
  }
  }
}    

Sema::OwningExprResult Sema::ActOnFinishFullExpr(ExprArg Arg) {
  Expr *FullExpr = Arg.takeAs<Expr>();
  if (FullExpr)
    FullExpr = MaybeCreateCXXExprWithTemporaries(FullExpr);

  return Owned(FullExpr);
}
