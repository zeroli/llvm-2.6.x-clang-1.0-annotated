//===--- ASTConsumers.cpp - ASTConsumer implementations -------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// AST Consumer Implementations.
//
//===----------------------------------------------------------------------===//

#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/DocumentXML.h"
#include "clang/Frontend/PathDiagnosticClients.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/FileManager.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "llvm/Module.h"
#include "llvm/Support/Streams.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/System/Path.h"
using namespace clang;

//===----------------------------------------------------------------------===//
/// ASTPrinter - Pretty-printer and dumper of ASTs

namespace {
  class ASTPrinter : public ASTConsumer {
    llvm::raw_ostream &Out;
    bool Dump;

  public:
    ASTPrinter(llvm::raw_ostream* o = NULL, bool Dump = false)
      : Out(o? *o : llvm::errs()), Dump(Dump) { }

    virtual void HandleTranslationUnit(ASTContext &Context) {
      PrintingPolicy Policy = Context.PrintingPolicy;
      Policy.Dump = Dump;
      Context.getTranslationUnitDecl()->print(Out, Policy);
    }
  };
} // end anonymous namespace

ASTConsumer *clang::CreateASTPrinter(llvm::raw_ostream* out) {
  return new ASTPrinter(out);
}

//===----------------------------------------------------------------------===//
/// ASTPrinterXML - XML-printer of ASTs

namespace {
  class ASTPrinterXML : public ASTConsumer {
    DocumentXML         Doc;

  public:
    ASTPrinterXML(llvm::raw_ostream& o) : Doc("CLANG_XML", o) {}

    void Initialize(ASTContext &Context) {
      Doc.initialize(Context);
    }

    virtual void HandleTranslationUnit(ASTContext &Ctx) {
      Doc.addSubNode("TranslationUnit");
      for (DeclContext::decl_iterator
             D = Ctx.getTranslationUnitDecl()->decls_begin(),
             DEnd = Ctx.getTranslationUnitDecl()->decls_end();
           D != DEnd;
           ++D)
      {
        Doc.PrintDecl(*D);
      }
      Doc.toParent();
      Doc.finalize();
    }
  };
} // end anonymous namespace


ASTConsumer *clang::CreateASTPrinterXML(llvm::raw_ostream* out) {
  return new ASTPrinterXML(out ? *out : llvm::outs());
}

ASTConsumer *clang::CreateASTDumper() {
  return new ASTPrinter(0, true);
}

//===----------------------------------------------------------------------===//
/// ASTViewer - AST Visualization

namespace {
  class ASTViewer : public ASTConsumer {
    ASTContext *Context;
  public:
    void Initialize(ASTContext &Context) {
      this->Context = &Context;
    }

    virtual void HandleTopLevelDecl(DeclGroupRef D) {
      for (DeclGroupRef::iterator I = D.begin(), E = D.end(); I != E; ++I)
        HandleTopLevelSingleDecl(*I);
    }

    void HandleTopLevelSingleDecl(Decl *D);
  };
}

void ASTViewer::HandleTopLevelSingleDecl(Decl *D) {
  if (FunctionDecl *FD = dyn_cast<FunctionDecl>(D)) {
    FD->print(llvm::errs());

    if (FD->getBodyIfAvailable()) {
      llvm::cerr << "\n";
      FD->getBodyIfAvailable()->viewAST();
      llvm::cerr << "\n";
    }
    return;
  }

  if (ObjCMethodDecl *MD = dyn_cast<ObjCMethodDecl>(D)) {
    MD->print(llvm::errs());

    if (MD->getBody()) {
      llvm::cerr << "\n";
      MD->getBody()->viewAST();
      llvm::cerr << "\n";
    }
  }
}


ASTConsumer *clang::CreateASTViewer() { return new ASTViewer(); }

//===----------------------------------------------------------------------===//
/// DeclContextPrinter - Decl and DeclContext Visualization

namespace {

class DeclContextPrinter : public ASTConsumer {
  llvm::raw_ostream& Out;
public:
  DeclContextPrinter() : Out(llvm::errs()) {}

  void HandleTranslationUnit(ASTContext &C) {
    PrintDeclContext(C.getTranslationUnitDecl(), 4);
  }

  void PrintDeclContext(const DeclContext* DC, unsigned Indentation);
};
}  // end anonymous namespace

void DeclContextPrinter::PrintDeclContext(const DeclContext* DC,
                                          unsigned Indentation) {
  // Print DeclContext name.
  switch (DC->getDeclKind()) {
  case Decl::TranslationUnit:
    Out << "[translation unit] " << DC;
    break;
  case Decl::Namespace: {
    Out << "[namespace] ";
    const NamespaceDecl* ND = cast<NamespaceDecl>(DC);
    Out << ND->getNameAsString();
    break;
  }
  case Decl::Enum: {
    const EnumDecl* ED = cast<EnumDecl>(DC);
    if (ED->isDefinition())
      Out << "[enum] ";
    else
      Out << "<enum> ";
    Out << ED->getNameAsString();
    break;
  }
  case Decl::Record: {
    const RecordDecl* RD = cast<RecordDecl>(DC);
    if (RD->isDefinition())
      Out << "[struct] ";
    else
      Out << "<struct> ";
    Out << RD->getNameAsString();
    break;
  }
  case Decl::CXXRecord: {
    const CXXRecordDecl* RD = cast<CXXRecordDecl>(DC);
    if (RD->isDefinition())
      Out << "[class] ";
    else
      Out << "<class> ";
    Out << RD->getNameAsString() << " " << DC;
    break;
  }
  case Decl::ObjCMethod:
    Out << "[objc method]";
    break;
  case Decl::ObjCInterface:
    Out << "[objc interface]";
    break;
  case Decl::ObjCCategory:
    Out << "[objc category]";
    break;
  case Decl::ObjCProtocol:
    Out << "[objc protocol]";
    break;
  case Decl::ObjCImplementation:
    Out << "[objc implementation]";
    break;
  case Decl::ObjCCategoryImpl:
    Out << "[objc categoryimpl]";
    break;
  case Decl::LinkageSpec:
    Out << "[linkage spec]";
    break;
  case Decl::Block:
    Out << "[block]";
    break;
  case Decl::Function: {
    const FunctionDecl* FD = cast<FunctionDecl>(DC);
    if (FD->isThisDeclarationADefinition())
      Out << "[function] ";
    else
      Out << "<function> ";
    Out << FD->getNameAsString();
    // Print the parameters.
    Out << "(";
    bool PrintComma = false;
    for (FunctionDecl::param_const_iterator I = FD->param_begin(),
           E = FD->param_end(); I != E; ++I) {
      if (PrintComma)
        Out << ", ";
      else
        PrintComma = true;
      Out << (*I)->getNameAsString();
    }
    Out << ")";
    break;
  }
  case Decl::CXXMethod: {
    const CXXMethodDecl* D = cast<CXXMethodDecl>(DC);
    if (D->isOutOfLine())
      Out << "[c++ method] ";
    else if (D->isImplicit())
      Out << "(c++ method) ";
    else
      Out << "<c++ method> ";
    Out << D->getNameAsString();
    // Print the parameters.
    Out << "(";
    bool PrintComma = false;
    for (FunctionDecl::param_const_iterator I = D->param_begin(),
           E = D->param_end(); I != E; ++I) {
      if (PrintComma)
        Out << ", ";
      else
        PrintComma = true;
      Out << (*I)->getNameAsString();
    }
    Out << ")";

    // Check the semantic DeclContext.
    const DeclContext* SemaDC = D->getDeclContext();
    const DeclContext* LexicalDC = D->getLexicalDeclContext();
    if (SemaDC != LexicalDC)
      Out << " [[" << SemaDC << "]]";

    break;
  }
  case Decl::CXXConstructor: {
    const CXXConstructorDecl* D = cast<CXXConstructorDecl>(DC);
    if (D->isOutOfLine())
      Out << "[c++ ctor] ";
    else if (D->isImplicit())
      Out << "(c++ ctor) ";
    else
      Out << "<c++ ctor> ";
    Out << D->getNameAsString();
    // Print the parameters.
    Out << "(";
    bool PrintComma = false;
    for (FunctionDecl::param_const_iterator I = D->param_begin(),
           E = D->param_end(); I != E; ++I) {
      if (PrintComma)
        Out << ", ";
      else
        PrintComma = true;
      Out << (*I)->getNameAsString();
    }
    Out << ")";

    // Check the semantic DC.
    const DeclContext* SemaDC = D->getDeclContext();
    const DeclContext* LexicalDC = D->getLexicalDeclContext();
    if (SemaDC != LexicalDC)
      Out << " [[" << SemaDC << "]]";
    break;
  }
  case Decl::CXXDestructor: {
    const CXXDestructorDecl* D = cast<CXXDestructorDecl>(DC);
    if (D->isOutOfLine())
      Out << "[c++ dtor] ";
    else if (D->isImplicit())
      Out << "(c++ dtor) ";
    else
      Out << "<c++ dtor> ";
    Out << D->getNameAsString();
    // Check the semantic DC.
    const DeclContext* SemaDC = D->getDeclContext();
    const DeclContext* LexicalDC = D->getLexicalDeclContext();
    if (SemaDC != LexicalDC)
      Out << " [[" << SemaDC << "]]";
    break;
  }
  case Decl::CXXConversion: {
    const CXXConversionDecl* D = cast<CXXConversionDecl>(DC);
    if (D->isOutOfLine())
      Out << "[c++ conversion] ";
    else if (D->isImplicit())
      Out << "(c++ conversion) ";
    else
      Out << "<c++ conversion> ";
    Out << D->getNameAsString();
    // Check the semantic DC.
    const DeclContext* SemaDC = D->getDeclContext();
    const DeclContext* LexicalDC = D->getLexicalDeclContext();
    if (SemaDC != LexicalDC)
      Out << " [[" << SemaDC << "]]";
    break;
  }

  default:
    assert(0 && "a decl that inherits DeclContext isn't handled");
  }

  Out << "\n";

  // Print decls in the DeclContext.
  for (DeclContext::decl_iterator I = DC->decls_begin(), E = DC->decls_end();
       I != E; ++I) {
    for (unsigned i = 0; i < Indentation; ++i)
      Out << "  ";

    Decl::Kind DK = I->getKind();
    switch (DK) {
    case Decl::Namespace:
    case Decl::Enum:
    case Decl::Record:
    case Decl::CXXRecord:
    case Decl::ObjCMethod:
    case Decl::ObjCInterface:
    case Decl::ObjCCategory:
    case Decl::ObjCProtocol:
    case Decl::ObjCImplementation:
    case Decl::ObjCCategoryImpl:
    case Decl::LinkageSpec:
    case Decl::Block:
    case Decl::Function:
    case Decl::CXXMethod:
    case Decl::CXXConstructor:
    case Decl::CXXDestructor:
    case Decl::CXXConversion:
    {
      DeclContext* DC = cast<DeclContext>(*I);
      PrintDeclContext(DC, Indentation+2);
      break;
    }
    case Decl::Field: {
      FieldDecl* FD = cast<FieldDecl>(*I);
      Out << "<field> " << FD->getNameAsString() << "\n";
      break;
    }
    case Decl::Typedef: {
      TypedefDecl* TD = cast<TypedefDecl>(*I);
      Out << "<typedef> " << TD->getNameAsString() << "\n";
      break;
    }
    case Decl::EnumConstant: {
      EnumConstantDecl* ECD = cast<EnumConstantDecl>(*I);
      Out << "<enum constant> " << ECD->getNameAsString() << "\n";
      break;
    }
    case Decl::Var: {
      VarDecl* VD = cast<VarDecl>(*I);
      Out << "<var> " << VD->getNameAsString() << "\n";
      break;
    }
    case Decl::ImplicitParam: {
      ImplicitParamDecl* IPD = cast<ImplicitParamDecl>(*I);
      Out << "<implicit parameter> " << IPD->getNameAsString() << "\n";
      break;
    }
    case Decl::ParmVar: {
      ParmVarDecl* PVD = cast<ParmVarDecl>(*I);
      Out << "<parameter> " << PVD->getNameAsString() << "\n";
      break;
    }
    case Decl::OriginalParmVar: {
      OriginalParmVarDecl* OPVD = cast<OriginalParmVarDecl>(*I);
      Out << "<original parameter> " << OPVD->getNameAsString() << "\n";
      break;
    }
    case Decl::ObjCProperty: {
      ObjCPropertyDecl* OPD = cast<ObjCPropertyDecl>(*I);
      Out << "<objc property> " << OPD->getNameAsString() << "\n";
      break;
    }
    default:
      fprintf(stderr, "DeclKind: %d \"%s\"\n", DK, I->getDeclKindName());
      assert(0 && "decl unhandled");
    }
  }
}
ASTConsumer *clang::CreateDeclContextPrinter() {
  return new DeclContextPrinter();
}

//===----------------------------------------------------------------------===//
/// InheritanceViewer - C++ Inheritance Visualization

namespace {
class InheritanceViewer : public ASTConsumer {
  const std::string clsname;
public:
  InheritanceViewer(const std::string& cname) : clsname(cname) {}

  void HandleTranslationUnit(ASTContext &C) {
    for (ASTContext::type_iterator I=C.types_begin(),E=C.types_end(); I!=E; ++I)
      if (RecordType *T = dyn_cast<RecordType>(*I)) {
        if (CXXRecordDecl *D = dyn_cast<CXXRecordDecl>(T->getDecl())) {
          // FIXME: This lookup needs to be generalized to handle namespaces and
          // (when we support them) templates.
          if (D->getNameAsString() == clsname) {
            D->viewInheritance(C);
          }
        }
      }
  }
};
}

ASTConsumer *clang::CreateInheritanceViewer(const std::string& clsname) {
  return new InheritanceViewer(clsname);
}
