//===------- CGObjCGNU.cpp - Emit LLVM Code from ASTs for a Module --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This provides Objective-C code generation targetting the GNU runtime.  The
// class in this file generates structures used by the GNU Objective-C runtime
// library.  These structures are defined in objc/objc.h and objc/objc-api.h in
// the GNU runtime distribution.
//
//===----------------------------------------------------------------------===//

#include "CGObjCRuntime.h"
#include "CodeGenModule.h"
#include "CodeGenFunction.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/StmtObjC.h"

#include "llvm/Intrinsics.h"
#include "llvm/Module.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Target/TargetData.h"

#include <map>


using namespace clang;
using namespace CodeGen;
using llvm::dyn_cast;

// The version of the runtime that this class targets.  Must match the version
// in the runtime.
static const int RuntimeVersion = 8;
static const int NonFragileRuntimeVersion = 9;
static const int ProtocolVersion = 2;

namespace {
class CGObjCGNU : public CodeGen::CGObjCRuntime {
private:
  CodeGen::CodeGenModule &CGM;
  llvm::Module &TheModule;
  const llvm::PointerType *SelectorTy;
  const llvm::PointerType *PtrToInt8Ty;
  const llvm::FunctionType *IMPTy;
  const llvm::PointerType *IdTy;
  QualType ASTIdTy;
  const llvm::IntegerType *IntTy;
  const llvm::PointerType *PtrTy;
  const llvm::IntegerType *LongTy;
  const llvm::PointerType *PtrToIntTy;
  llvm::GlobalAlias *ClassPtrAlias;
  llvm::GlobalAlias *MetaClassPtrAlias;
  std::vector<llvm::Constant*> Classes;
  std::vector<llvm::Constant*> Categories;
  std::vector<llvm::Constant*> ConstantStrings;
  llvm::Function *LoadFunction;
  llvm::StringMap<llvm::Constant*> ExistingProtocols;
  typedef std::pair<std::string, std::string> TypedSelector;
  std::map<TypedSelector, llvm::GlobalAlias*> TypedSelectors;
  llvm::StringMap<llvm::GlobalAlias*> UntypedSelectors;
  // Some zeros used for GEPs in lots of places.
  llvm::Constant *Zeros[2];
  llvm::Constant *NULLPtr;
  llvm::LLVMContext &VMContext;
private:
  llvm::Constant *GenerateIvarList(
      const llvm::SmallVectorImpl<llvm::Constant *>  &IvarNames,
      const llvm::SmallVectorImpl<llvm::Constant *>  &IvarTypes,
      const llvm::SmallVectorImpl<llvm::Constant *>  &IvarOffsets);
  llvm::Constant *GenerateMethodList(const std::string &ClassName,
      const std::string &CategoryName,
      const llvm::SmallVectorImpl<Selector>  &MethodSels,
      const llvm::SmallVectorImpl<llvm::Constant *>  &MethodTypes,
      bool isClassMethodList);
  llvm::Constant *GenerateEmptyProtocol(const std::string &ProtocolName);
  llvm::Constant *GenerateProtocolList(
      const llvm::SmallVectorImpl<std::string> &Protocols);
  llvm::Constant *GenerateClassStructure(
      llvm::Constant *MetaClass,
      llvm::Constant *SuperClass,
      unsigned info,
      const char *Name,
      llvm::Constant *Version,
      llvm::Constant *InstanceSize,
      llvm::Constant *IVars,
      llvm::Constant *Methods,
      llvm::Constant *Protocols);
  llvm::Constant *GenerateProtocolMethodList(
      const llvm::SmallVectorImpl<llvm::Constant *>  &MethodNames,
      const llvm::SmallVectorImpl<llvm::Constant *>  &MethodTypes);
  llvm::Constant *MakeConstantString(const std::string &Str, const std::string
      &Name="");
  llvm::Constant *MakeGlobal(const llvm::StructType *Ty,
      std::vector<llvm::Constant*> &V, const std::string &Name="");
  llvm::Constant *MakeGlobal(const llvm::ArrayType *Ty,
      std::vector<llvm::Constant*> &V, const std::string &Name="");
  llvm::GlobalVariable *ObjCIvarOffsetVariable(const ObjCInterfaceDecl *ID,
      const ObjCIvarDecl *Ivar);
  void EmitClassRef(const std::string &className);
public:
  CGObjCGNU(CodeGen::CodeGenModule &cgm);
  virtual llvm::Constant *GenerateConstantString(const ObjCStringLiteral *);
  virtual CodeGen::RValue
  GenerateMessageSend(CodeGen::CodeGenFunction &CGF,
                      QualType ResultType,
                      Selector Sel,
                      llvm::Value *Receiver,
                      bool IsClassMessage,
                      const CallArgList &CallArgs,
                      const ObjCMethodDecl *Method);
  virtual CodeGen::RValue
  GenerateMessageSendSuper(CodeGen::CodeGenFunction &CGF,
                           QualType ResultType,
                           Selector Sel,
                           const ObjCInterfaceDecl *Class,
                           bool isCategoryImpl,
                           llvm::Value *Receiver,
                           bool IsClassMessage,
                           const CallArgList &CallArgs);
  virtual llvm::Value *GetClass(CGBuilderTy &Builder,
                                const ObjCInterfaceDecl *OID);
  virtual llvm::Value *GetSelector(CGBuilderTy &Builder, Selector Sel);
  virtual llvm::Value *GetSelector(CGBuilderTy &Builder, const ObjCMethodDecl
      *Method);

  virtual llvm::Function *GenerateMethod(const ObjCMethodDecl *OMD,
                                         const ObjCContainerDecl *CD);
  virtual void GenerateCategory(const ObjCCategoryImplDecl *CMD);
  virtual void GenerateClass(const ObjCImplementationDecl *ClassDecl);
  virtual llvm::Value *GenerateProtocolRef(CGBuilderTy &Builder,
                                           const ObjCProtocolDecl *PD);
  virtual void GenerateProtocol(const ObjCProtocolDecl *PD);
  virtual llvm::Function *ModuleInitFunction();
  virtual llvm::Function *GetPropertyGetFunction();
  virtual llvm::Function *GetPropertySetFunction();
  virtual llvm::Constant *EnumerationMutationFunction();

  virtual void EmitTryOrSynchronizedStmt(CodeGen::CodeGenFunction &CGF,
                                         const Stmt &S);
  virtual void EmitThrowStmt(CodeGen::CodeGenFunction &CGF,
                             const ObjCAtThrowStmt &S);
  virtual llvm::Value * EmitObjCWeakRead(CodeGen::CodeGenFunction &CGF,
                                         llvm::Value *AddrWeakObj);
  virtual void EmitObjCWeakAssign(CodeGen::CodeGenFunction &CGF,
                                  llvm::Value *src, llvm::Value *dst);
  virtual void EmitObjCGlobalAssign(CodeGen::CodeGenFunction &CGF,
                                    llvm::Value *src, llvm::Value *dest);
  virtual void EmitObjCIvarAssign(CodeGen::CodeGenFunction &CGF,
                                    llvm::Value *src, llvm::Value *dest);
  virtual void EmitObjCStrongCastAssign(CodeGen::CodeGenFunction &CGF,
                                        llvm::Value *src, llvm::Value *dest);
  virtual void EmitGCMemmoveCollectable(CodeGen::CodeGenFunction &CGF,
                                        llvm::Value *DestPtr,
                                        llvm::Value *SrcPtr,
                                        unsigned long size);
  virtual LValue EmitObjCValueForIvar(CodeGen::CodeGenFunction &CGF,
                                      QualType ObjectTy,
                                      llvm::Value *BaseValue,
                                      const ObjCIvarDecl *Ivar,
                                      unsigned CVRQualifiers);
  virtual llvm::Value *EmitIvarOffset(CodeGen::CodeGenFunction &CGF,
                                      const ObjCInterfaceDecl *Interface,
                                      const ObjCIvarDecl *Ivar);
};
} // end anonymous namespace


/// Emits a reference to a dummy variable which is emitted with each class.
/// This ensures that a linker error will be generated when trying to link
/// together modules where a referenced class is not defined.
void CGObjCGNU::EmitClassRef(const std::string &className) {
  std::string symbolRef = "__objc_class_ref_" + className;
  // Don't emit two copies of the same symbol
  if (TheModule.getGlobalVariable(symbolRef))
    return;
  std::string symbolName = "__objc_class_name_" + className;
  llvm::GlobalVariable *ClassSymbol = TheModule.getGlobalVariable(symbolName);
  if (!ClassSymbol) {
    ClassSymbol = new llvm::GlobalVariable(TheModule, LongTy, false,
        llvm::GlobalValue::ExternalLinkage, 0, symbolName);
  }
  new llvm::GlobalVariable(TheModule, ClassSymbol->getType(), true,
    llvm::GlobalValue::WeakAnyLinkage, ClassSymbol, symbolRef);
}

static std::string SymbolNameForClass(const std::string &ClassName) {
  return "_OBJC_CLASS_" + ClassName;
}

static std::string SymbolNameForMethod(const std::string &ClassName, const
  std::string &CategoryName, const std::string &MethodName, bool isClassMethod)
{
  return "_OBJC_METHOD_" + ClassName + "("+CategoryName+")"+
            (isClassMethod ? "+" : "-") + MethodName;
}

CGObjCGNU::CGObjCGNU(CodeGen::CodeGenModule &cgm)
  : CGM(cgm), TheModule(CGM.getModule()), ClassPtrAlias(0),
    MetaClassPtrAlias(0), VMContext(cgm.getLLVMContext()) {
  IntTy = cast<llvm::IntegerType>(
      CGM.getTypes().ConvertType(CGM.getContext().IntTy));
  LongTy = cast<llvm::IntegerType>(
      CGM.getTypes().ConvertType(CGM.getContext().LongTy));

  Zeros[0] = llvm::ConstantInt::get(LongTy, 0);
  Zeros[1] = Zeros[0];
  NULLPtr = llvm::ConstantPointerNull::get(
    llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(VMContext)));
  // C string type.  Used in lots of places.
  PtrToInt8Ty =
    llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(VMContext));
  // Get the selector Type.
  SelectorTy = cast<llvm::PointerType>(
    CGM.getTypes().ConvertType(CGM.getContext().getObjCSelType()));

  PtrToIntTy = llvm::PointerType::getUnqual(IntTy);
  PtrTy = PtrToInt8Ty;

  // Object type
  ASTIdTy = CGM.getContext().getObjCIdType();
  IdTy = cast<llvm::PointerType>(CGM.getTypes().ConvertType(ASTIdTy));

  // IMP type
  std::vector<const llvm::Type*> IMPArgs;
  IMPArgs.push_back(IdTy);
  IMPArgs.push_back(SelectorTy);
  IMPTy = llvm::FunctionType::get(IdTy, IMPArgs, true);
}

// This has to perform the lookup every time, since posing and related
// techniques can modify the name -> class mapping.
llvm::Value *CGObjCGNU::GetClass(CGBuilderTy &Builder,
                                 const ObjCInterfaceDecl *OID) {
  llvm::Value *ClassName = CGM.GetAddrOfConstantCString(OID->getNameAsString());
  EmitClassRef(OID->getNameAsString());
  ClassName = Builder.CreateStructGEP(ClassName, 0);

  std::vector<const llvm::Type*> Params(1, PtrToInt8Ty);
  llvm::Constant *ClassLookupFn =
    CGM.CreateRuntimeFunction(llvm::FunctionType::get(IdTy,
                                                      Params,
                                                      true),
                              "objc_lookup_class");
  return Builder.CreateCall(ClassLookupFn, ClassName);
}

llvm::Value *CGObjCGNU::GetSelector(CGBuilderTy &Builder, Selector Sel) {
  llvm::GlobalAlias *&US = UntypedSelectors[Sel.getAsString()];
  if (US == 0)
    US = new llvm::GlobalAlias(llvm::PointerType::getUnqual(SelectorTy),
                               llvm::GlobalValue::PrivateLinkage,
                               ".objc_untyped_selector_alias"+Sel.getAsString(),
                               NULL, &TheModule);

  return Builder.CreateLoad(US);
}

llvm::Value *CGObjCGNU::GetSelector(CGBuilderTy &Builder, const ObjCMethodDecl
    *Method) {

  std::string SelName = Method->getSelector().getAsString();
  std::string SelTypes;
  CGM.getContext().getObjCEncodingForMethodDecl(Method, SelTypes);
  // Typed selectors
  TypedSelector Selector = TypedSelector(SelName,
          SelTypes);

  // If it's already cached, return it.
  if (TypedSelectors[Selector]) {
    return Builder.CreateLoad(TypedSelectors[Selector]);
  }

  // If it isn't, cache it.
  llvm::GlobalAlias *Sel = new llvm::GlobalAlias(
          llvm::PointerType::getUnqual(SelectorTy),
          llvm::GlobalValue::PrivateLinkage, ".objc_selector_alias" + SelName,
          NULL, &TheModule);
  TypedSelectors[Selector] = Sel;

  return Builder.CreateLoad(Sel);
}

llvm::Constant *CGObjCGNU::MakeConstantString(const std::string &Str,
                                              const std::string &Name) {
  llvm::Constant * ConstStr = llvm::ConstantArray::get(VMContext, Str);
  ConstStr = new llvm::GlobalVariable(TheModule, ConstStr->getType(), true,
                                      llvm::GlobalValue::InternalLinkage,
                                      ConstStr, Name);
  return llvm::ConstantExpr::getGetElementPtr(ConstStr, Zeros, 2);
}

llvm::Constant *CGObjCGNU::MakeGlobal(const llvm::StructType *Ty,
    std::vector<llvm::Constant*> &V, const std::string &Name) {
  llvm::Constant *C = llvm::ConstantStruct::get(Ty, V);
  return new llvm::GlobalVariable(TheModule, Ty, false,
      llvm::GlobalValue::InternalLinkage, C, Name);
}

llvm::Constant *CGObjCGNU::MakeGlobal(const llvm::ArrayType *Ty,
    std::vector<llvm::Constant*> &V, const std::string &Name) {
  llvm::Constant *C = llvm::ConstantArray::get(Ty, V);
  return new llvm::GlobalVariable(TheModule, Ty, false,
                                  llvm::GlobalValue::InternalLinkage, C, Name);
}

/// Generate an NSConstantString object.
//TODO: In case there are any crazy people still using the GNU runtime without
//an OpenStep implementation, this should let them select their own class for
//constant strings.
llvm::Constant *CGObjCGNU::GenerateConstantString(const ObjCStringLiteral *SL) {
  std::string Str(SL->getString()->getStrData(),
                  SL->getString()->getByteLength());
  std::vector<llvm::Constant*> Ivars;
  Ivars.push_back(NULLPtr);
  Ivars.push_back(MakeConstantString(Str));
  Ivars.push_back(llvm::ConstantInt::get(IntTy, Str.size()));
  llvm::Constant *ObjCStr = MakeGlobal(
    llvm::StructType::get(VMContext, PtrToInt8Ty, PtrToInt8Ty, IntTy, NULL),
    Ivars, ".objc_str");
  ConstantStrings.push_back(
      llvm::ConstantExpr::getBitCast(ObjCStr, PtrToInt8Ty));
  return ObjCStr;
}

///Generates a message send where the super is the receiver.  This is a message
///send to self with special delivery semantics indicating which class's method
///should be called.
CodeGen::RValue
CGObjCGNU::GenerateMessageSendSuper(CodeGen::CodeGenFunction &CGF,
                                    QualType ResultType,
                                    Selector Sel,
                                    const ObjCInterfaceDecl *Class,
                                    bool isCategoryImpl,
                                    llvm::Value *Receiver,
                                    bool IsClassMessage,
                                    const CallArgList &CallArgs) {
  llvm::Value *cmd = GetSelector(CGF.Builder, Sel);

  CallArgList ActualArgs;

  ActualArgs.push_back(
      std::make_pair(RValue::get(CGF.Builder.CreateBitCast(Receiver, IdTy)),
      ASTIdTy));
  ActualArgs.push_back(std::make_pair(RValue::get(cmd),
                                      CGF.getContext().getObjCSelType()));
  ActualArgs.insert(ActualArgs.end(), CallArgs.begin(), CallArgs.end());

  CodeGenTypes &Types = CGM.getTypes();
  const CGFunctionInfo &FnInfo = Types.getFunctionInfo(ResultType, ActualArgs);
  const llvm::FunctionType *impType = Types.GetFunctionType(FnInfo, false);

  llvm::Value *ReceiverClass = 0;
  if (isCategoryImpl) {
    llvm::Constant *classLookupFunction = 0;
    std::vector<const llvm::Type*> Params;
    Params.push_back(PtrTy);
    if (IsClassMessage)  {
      classLookupFunction = CGM.CreateRuntimeFunction(llvm::FunctionType::get(
            IdTy, Params, true), "objc_get_meta_class");
    } else {
      classLookupFunction = CGM.CreateRuntimeFunction(llvm::FunctionType::get(
            IdTy, Params, true), "objc_get_class");
    }
    ReceiverClass = CGF.Builder.CreateCall(classLookupFunction,
        MakeConstantString(Class->getNameAsString()));
  } else {
    // Set up global aliases for the metaclass or class pointer if they do not
    // already exist.  These will are forward-references which will be set to
    // pointers to the class and metaclass structure created for the runtime
    // load function.  To send a message to super, we look up the value of the
    // super_class pointer from either the class or metaclass structure.
    if (IsClassMessage)  {
      if (!MetaClassPtrAlias) {
        MetaClassPtrAlias = new llvm::GlobalAlias(IdTy,
            llvm::GlobalValue::InternalLinkage, ".objc_metaclass_ref" +
            Class->getNameAsString(), NULL, &TheModule);
      }
      ReceiverClass = MetaClassPtrAlias;
    } else {
      if (!ClassPtrAlias) {
        ClassPtrAlias = new llvm::GlobalAlias(IdTy,
            llvm::GlobalValue::InternalLinkage, ".objc_class_ref" +
            Class->getNameAsString(), NULL, &TheModule);
      }
      ReceiverClass = ClassPtrAlias;
    }
  }
  // Cast the pointer to a simplified version of the class structure
  ReceiverClass = CGF.Builder.CreateBitCast(ReceiverClass,
      llvm::PointerType::getUnqual(
        llvm::StructType::get(VMContext, IdTy, IdTy, NULL)));
  // Get the superclass pointer
  ReceiverClass = CGF.Builder.CreateStructGEP(ReceiverClass, 1);
  // Load the superclass pointer
  ReceiverClass = CGF.Builder.CreateLoad(ReceiverClass);
  // Construct the structure used to look up the IMP
  llvm::StructType *ObjCSuperTy = llvm::StructType::get(VMContext,
      Receiver->getType(), IdTy, NULL);
  llvm::Value *ObjCSuper = CGF.Builder.CreateAlloca(ObjCSuperTy);

  CGF.Builder.CreateStore(Receiver, CGF.Builder.CreateStructGEP(ObjCSuper, 0));
  CGF.Builder.CreateStore(ReceiverClass,
      CGF.Builder.CreateStructGEP(ObjCSuper, 1));

  // Get the IMP
  std::vector<const llvm::Type*> Params;
  Params.push_back(llvm::PointerType::getUnqual(ObjCSuperTy));
  Params.push_back(SelectorTy);
  llvm::Constant *lookupFunction =
    CGM.CreateRuntimeFunction(llvm::FunctionType::get(
          llvm::PointerType::getUnqual(impType), Params, true),
        "objc_msg_lookup_super");

  llvm::Value *lookupArgs[] = {ObjCSuper, cmd};
  llvm::Value *imp = CGF.Builder.CreateCall(lookupFunction, lookupArgs,
      lookupArgs+2);

  return CGF.EmitCall(FnInfo, imp, ActualArgs);
}

/// Generate code for a message send expression.
CodeGen::RValue
CGObjCGNU::GenerateMessageSend(CodeGen::CodeGenFunction &CGF,
                               QualType ResultType,
                               Selector Sel,
                               llvm::Value *Receiver,
                               bool IsClassMessage,
                               const CallArgList &CallArgs,
                               const ObjCMethodDecl *Method) {
  IdTy = cast<llvm::PointerType>(CGM.getTypes().ConvertType(ASTIdTy));
  llvm::Value *cmd;
  if (Method)
    cmd = GetSelector(CGF.Builder, Method);
  else
    cmd = GetSelector(CGF.Builder, Sel);
  CallArgList ActualArgs;

  ActualArgs.push_back(
    std::make_pair(RValue::get(CGF.Builder.CreateBitCast(Receiver, IdTy)), ASTIdTy));
  ActualArgs.push_back(std::make_pair(RValue::get(cmd),
                                      CGF.getContext().getObjCSelType()));
  ActualArgs.insert(ActualArgs.end(), CallArgs.begin(), CallArgs.end());

  CodeGenTypes &Types = CGM.getTypes();
  const CGFunctionInfo &FnInfo = Types.getFunctionInfo(ResultType, ActualArgs);
  const llvm::FunctionType *impType = Types.GetFunctionType(FnInfo, false);

  llvm::Value *imp;
  std::vector<const llvm::Type*> Params;
  Params.push_back(Receiver->getType());
  Params.push_back(SelectorTy);
  // For sender-aware dispatch, we pass the sender as the third argument to a
  // lookup function.  When sending messages from C code, the sender is nil.
  // objc_msg_lookup_sender(id receiver, SEL selector, id sender);
  if (CGM.getContext().getLangOptions().ObjCSenderDispatch) {
    llvm::Value *self;

    if (isa<ObjCMethodDecl>(CGF.CurFuncDecl)) {
      self = CGF.LoadObjCSelf();
    } else {
      self = llvm::ConstantPointerNull::get(IdTy);
    }
    Params.push_back(self->getType());
    llvm::Constant *lookupFunction =
      CGM.CreateRuntimeFunction(llvm::FunctionType::get(
          llvm::PointerType::getUnqual(impType), Params, true),
        "objc_msg_lookup_sender");

    imp = CGF.Builder.CreateCall3(lookupFunction, Receiver, cmd, self);
  } else {
    llvm::Constant *lookupFunction =
    CGM.CreateRuntimeFunction(llvm::FunctionType::get(
        llvm::PointerType::getUnqual(impType), Params, true),
      "objc_msg_lookup");

    imp = CGF.Builder.CreateCall2(lookupFunction, Receiver, cmd);
  }

  return CGF.EmitCall(FnInfo, imp, ActualArgs);
}

/// Generates a MethodList.  Used in construction of a objc_class and
/// objc_category structures.
llvm::Constant *CGObjCGNU::GenerateMethodList(const std::string &ClassName,
                                              const std::string &CategoryName,
    const llvm::SmallVectorImpl<Selector> &MethodSels,
    const llvm::SmallVectorImpl<llvm::Constant *> &MethodTypes,
    bool isClassMethodList) {
  if (MethodSels.empty())
    return NULLPtr;
  // Get the method structure type.
  llvm::StructType *ObjCMethodTy = llvm::StructType::get(VMContext,
    PtrToInt8Ty, // Really a selector, but the runtime creates it us.
    PtrToInt8Ty, // Method types
    llvm::PointerType::getUnqual(IMPTy), //Method pointer
    NULL);
  std::vector<llvm::Constant*> Methods;
  std::vector<llvm::Constant*> Elements;
  for (unsigned int i = 0, e = MethodTypes.size(); i < e; ++i) {
    Elements.clear();
    if (llvm::Constant *Method =
      TheModule.getFunction(SymbolNameForMethod(ClassName, CategoryName,
                                                MethodSels[i].getAsString(),
                                                isClassMethodList))) {
      llvm::Constant *C =
        CGM.GetAddrOfConstantCString(MethodSels[i].getAsString());
      Elements.push_back(llvm::ConstantExpr::getGetElementPtr(C, Zeros, 2));
      Elements.push_back(
            llvm::ConstantExpr::getGetElementPtr(MethodTypes[i], Zeros, 2));
      Method = llvm::ConstantExpr::getBitCast(Method,
          llvm::PointerType::getUnqual(IMPTy));
      Elements.push_back(Method);
      Methods.push_back(llvm::ConstantStruct::get(ObjCMethodTy, Elements));
    }
  }

  // Array of method structures
  llvm::ArrayType *ObjCMethodArrayTy = llvm::ArrayType::get(ObjCMethodTy,
                                                            Methods.size());
  llvm::Constant *MethodArray = llvm::ConstantArray::get(ObjCMethodArrayTy,
                                                         Methods);

  // Structure containing list pointer, array and array count
  llvm::SmallVector<const llvm::Type*, 16> ObjCMethodListFields;
  llvm::PATypeHolder OpaqueNextTy = llvm::OpaqueType::get(VMContext);
  llvm::Type *NextPtrTy = llvm::PointerType::getUnqual(OpaqueNextTy);
  llvm::StructType *ObjCMethodListTy = llvm::StructType::get(VMContext,
      NextPtrTy,
      IntTy,
      ObjCMethodArrayTy,
      NULL);
  // Refine next pointer type to concrete type
  llvm::cast<llvm::OpaqueType>(
      OpaqueNextTy.get())->refineAbstractTypeTo(ObjCMethodListTy);
  ObjCMethodListTy = llvm::cast<llvm::StructType>(OpaqueNextTy.get());

  Methods.clear();
  Methods.push_back(llvm::ConstantPointerNull::get(
        llvm::PointerType::getUnqual(ObjCMethodListTy)));
  Methods.push_back(llvm::ConstantInt::get(llvm::Type::getInt32Ty(VMContext),
        MethodTypes.size()));
  Methods.push_back(MethodArray);

  // Create an instance of the structure
  return MakeGlobal(ObjCMethodListTy, Methods, ".objc_method_list");
}

/// Generates an IvarList.  Used in construction of a objc_class.
llvm::Constant *CGObjCGNU::GenerateIvarList(
    const llvm::SmallVectorImpl<llvm::Constant *>  &IvarNames,
    const llvm::SmallVectorImpl<llvm::Constant *>  &IvarTypes,
    const llvm::SmallVectorImpl<llvm::Constant *>  &IvarOffsets) {
  // Get the method structure type.
  llvm::StructType *ObjCIvarTy = llvm::StructType::get(VMContext,
    PtrToInt8Ty,
    PtrToInt8Ty,
    IntTy,
    NULL);
  std::vector<llvm::Constant*> Ivars;
  std::vector<llvm::Constant*> Elements;
  for (unsigned int i = 0, e = IvarNames.size() ; i < e ; i++) {
    Elements.clear();
    Elements.push_back( llvm::ConstantExpr::getGetElementPtr(IvarNames[i],
          Zeros, 2));
    Elements.push_back( llvm::ConstantExpr::getGetElementPtr(IvarTypes[i],
          Zeros, 2));
    Elements.push_back(IvarOffsets[i]);
    Ivars.push_back(llvm::ConstantStruct::get(ObjCIvarTy, Elements));
  }

  // Array of method structures
  llvm::ArrayType *ObjCIvarArrayTy = llvm::ArrayType::get(ObjCIvarTy,
      IvarNames.size());


  Elements.clear();
  Elements.push_back(llvm::ConstantInt::get(IntTy, (int)IvarNames.size()));
  Elements.push_back(llvm::ConstantArray::get(ObjCIvarArrayTy, Ivars));
  // Structure containing array and array count
  llvm::StructType *ObjCIvarListTy = llvm::StructType::get(VMContext, IntTy,
    ObjCIvarArrayTy,
    NULL);

  // Create an instance of the structure
  return MakeGlobal(ObjCIvarListTy, Elements, ".objc_ivar_list");
}

/// Generate a class structure
llvm::Constant *CGObjCGNU::GenerateClassStructure(
    llvm::Constant *MetaClass,
    llvm::Constant *SuperClass,
    unsigned info,
    const char *Name,
    llvm::Constant *Version,
    llvm::Constant *InstanceSize,
    llvm::Constant *IVars,
    llvm::Constant *Methods,
    llvm::Constant *Protocols) {
  // Set up the class structure
  // Note:  Several of these are char*s when they should be ids.  This is
  // because the runtime performs this translation on load.
  llvm::StructType *ClassTy = llvm::StructType::get(VMContext,
      PtrToInt8Ty,        // class_pointer
      PtrToInt8Ty,        // super_class
      PtrToInt8Ty,        // name
      LongTy,             // version
      LongTy,             // info
      LongTy,             // instance_size
      IVars->getType(),   // ivars
      Methods->getType(), // methods
      // These are all filled in by the runtime, so we pretend
      PtrTy,              // dtable
      PtrTy,              // subclass_list
      PtrTy,              // sibling_class
      PtrTy,              // protocols
      PtrTy,              // gc_object_type
      NULL);
  llvm::Constant *Zero = llvm::ConstantInt::get(LongTy, 0);
  llvm::Constant *NullP =
    llvm::ConstantPointerNull::get(PtrTy);
  // Fill in the structure
  std::vector<llvm::Constant*> Elements;
  Elements.push_back(llvm::ConstantExpr::getBitCast(MetaClass, PtrToInt8Ty));
  Elements.push_back(SuperClass);
  Elements.push_back(MakeConstantString(Name, ".class_name"));
  Elements.push_back(Zero);
  Elements.push_back(llvm::ConstantInt::get(LongTy, info));
  Elements.push_back(InstanceSize);
  Elements.push_back(IVars);
  Elements.push_back(Methods);
  Elements.push_back(NullP);
  Elements.push_back(NullP);
  Elements.push_back(NullP);
  Elements.push_back(llvm::ConstantExpr::getBitCast(Protocols, PtrTy));
  Elements.push_back(NullP);
  // Create an instance of the structure
  return MakeGlobal(ClassTy, Elements, SymbolNameForClass(Name));
}

llvm::Constant *CGObjCGNU::GenerateProtocolMethodList(
    const llvm::SmallVectorImpl<llvm::Constant *>  &MethodNames,
    const llvm::SmallVectorImpl<llvm::Constant *>  &MethodTypes) {
  // Get the method structure type.
  llvm::StructType *ObjCMethodDescTy = llvm::StructType::get(VMContext,
    PtrToInt8Ty, // Really a selector, but the runtime does the casting for us.
    PtrToInt8Ty,
    NULL);
  std::vector<llvm::Constant*> Methods;
  std::vector<llvm::Constant*> Elements;
  for (unsigned int i = 0, e = MethodTypes.size() ; i < e ; i++) {
    Elements.clear();
    Elements.push_back(llvm::ConstantExpr::getGetElementPtr(MethodNames[i],
          Zeros, 2));
    Elements.push_back(
          llvm::ConstantExpr::getGetElementPtr(MethodTypes[i], Zeros, 2));
    Methods.push_back(llvm::ConstantStruct::get(ObjCMethodDescTy, Elements));
  }
  llvm::ArrayType *ObjCMethodArrayTy = llvm::ArrayType::get(ObjCMethodDescTy,
      MethodNames.size());
  llvm::Constant *Array = llvm::ConstantArray::get(ObjCMethodArrayTy,
                                                   Methods);
  llvm::StructType *ObjCMethodDescListTy = llvm::StructType::get(VMContext,
      IntTy, ObjCMethodArrayTy, NULL);
  Methods.clear();
  Methods.push_back(llvm::ConstantInt::get(IntTy, MethodNames.size()));
  Methods.push_back(Array);
  return MakeGlobal(ObjCMethodDescListTy, Methods, ".objc_method_list");
}

// Create the protocol list structure used in classes, categories and so on
llvm::Constant *CGObjCGNU::GenerateProtocolList(
    const llvm::SmallVectorImpl<std::string> &Protocols) {
  llvm::ArrayType *ProtocolArrayTy = llvm::ArrayType::get(PtrToInt8Ty,
      Protocols.size());
  llvm::StructType *ProtocolListTy = llvm::StructType::get(VMContext,
      PtrTy, //Should be a recurisve pointer, but it's always NULL here.
      LongTy,//FIXME: Should be size_t
      ProtocolArrayTy,
      NULL);
  std::vector<llvm::Constant*> Elements;
  for (const std::string *iter = Protocols.begin(), *endIter = Protocols.end();
      iter != endIter ; iter++) {
    llvm::Constant *protocol = ExistingProtocols[*iter];
    if (!protocol)
      protocol = GenerateEmptyProtocol(*iter);
    llvm::Constant *Ptr = llvm::ConstantExpr::getBitCast(protocol,
                                                           PtrToInt8Ty);
    Elements.push_back(Ptr);
  }
  llvm::Constant * ProtocolArray = llvm::ConstantArray::get(ProtocolArrayTy,
      Elements);
  Elements.clear();
  Elements.push_back(NULLPtr);
  Elements.push_back(llvm::ConstantInt::get(LongTy, Protocols.size()));
  Elements.push_back(ProtocolArray);
  return MakeGlobal(ProtocolListTy, Elements, ".objc_protocol_list");
}

llvm::Value *CGObjCGNU::GenerateProtocolRef(CGBuilderTy &Builder,
                                            const ObjCProtocolDecl *PD) {
  llvm::Value *protocol = ExistingProtocols[PD->getNameAsString()];
  const llvm::Type *T =
    CGM.getTypes().ConvertType(CGM.getContext().getObjCProtoType());
  return Builder.CreateBitCast(protocol, llvm::PointerType::getUnqual(T));
}

llvm::Constant *CGObjCGNU::GenerateEmptyProtocol(
  const std::string &ProtocolName) {
  llvm::SmallVector<std::string, 0> EmptyStringVector;
  llvm::SmallVector<llvm::Constant*, 0> EmptyConstantVector;

  llvm::Constant *ProtocolList = GenerateProtocolList(EmptyStringVector);
  llvm::Constant *InstanceMethodList =
    GenerateProtocolMethodList(EmptyConstantVector, EmptyConstantVector);
  llvm::Constant *ClassMethodList =
    GenerateProtocolMethodList(EmptyConstantVector, EmptyConstantVector);
  // Protocols are objects containing lists of the methods implemented and
  // protocols adopted.
  llvm::StructType *ProtocolTy = llvm::StructType::get(VMContext, IdTy,
      PtrToInt8Ty,
      ProtocolList->getType(),
      InstanceMethodList->getType(),
      ClassMethodList->getType(),
      NULL);
  std::vector<llvm::Constant*> Elements;
  // The isa pointer must be set to a magic number so the runtime knows it's
  // the correct layout.
  Elements.push_back(llvm::ConstantExpr::getIntToPtr(
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(VMContext), ProtocolVersion), IdTy));
  Elements.push_back(MakeConstantString(ProtocolName, ".objc_protocol_name"));
  Elements.push_back(ProtocolList);
  Elements.push_back(InstanceMethodList);
  Elements.push_back(ClassMethodList);
  return MakeGlobal(ProtocolTy, Elements, ".objc_protocol");
}

void CGObjCGNU::GenerateProtocol(const ObjCProtocolDecl *PD) {
  ASTContext &Context = CGM.getContext();
  std::string ProtocolName = PD->getNameAsString();
  llvm::SmallVector<std::string, 16> Protocols;
  for (ObjCProtocolDecl::protocol_iterator PI = PD->protocol_begin(),
       E = PD->protocol_end(); PI != E; ++PI)
    Protocols.push_back((*PI)->getNameAsString());
  llvm::SmallVector<llvm::Constant*, 16> InstanceMethodNames;
  llvm::SmallVector<llvm::Constant*, 16> InstanceMethodTypes;
  for (ObjCProtocolDecl::instmeth_iterator iter = PD->instmeth_begin(),
       E = PD->instmeth_end(); iter != E; iter++) {
    std::string TypeStr;
    Context.getObjCEncodingForMethodDecl(*iter, TypeStr);
    InstanceMethodNames.push_back(
        CGM.GetAddrOfConstantCString((*iter)->getSelector().getAsString()));
    InstanceMethodTypes.push_back(CGM.GetAddrOfConstantCString(TypeStr));
  }
  // Collect information about class methods:
  llvm::SmallVector<llvm::Constant*, 16> ClassMethodNames;
  llvm::SmallVector<llvm::Constant*, 16> ClassMethodTypes;
  for (ObjCProtocolDecl::classmeth_iterator
         iter = PD->classmeth_begin(), endIter = PD->classmeth_end();
       iter != endIter ; iter++) {
    std::string TypeStr;
    Context.getObjCEncodingForMethodDecl((*iter),TypeStr);
    ClassMethodNames.push_back(
        CGM.GetAddrOfConstantCString((*iter)->getSelector().getAsString()));
    ClassMethodTypes.push_back(CGM.GetAddrOfConstantCString(TypeStr));
  }

  llvm::Constant *ProtocolList = GenerateProtocolList(Protocols);
  llvm::Constant *InstanceMethodList =
    GenerateProtocolMethodList(InstanceMethodNames, InstanceMethodTypes);
  llvm::Constant *ClassMethodList =
    GenerateProtocolMethodList(ClassMethodNames, ClassMethodTypes);
  // Protocols are objects containing lists of the methods implemented and
  // protocols adopted.
  llvm::StructType *ProtocolTy = llvm::StructType::get(VMContext, IdTy,
      PtrToInt8Ty,
      ProtocolList->getType(),
      InstanceMethodList->getType(),
      ClassMethodList->getType(),
      NULL);
  std::vector<llvm::Constant*> Elements;
  // The isa pointer must be set to a magic number so the runtime knows it's
  // the correct layout.
  Elements.push_back(llvm::ConstantExpr::getIntToPtr(
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(VMContext), ProtocolVersion), IdTy));
  Elements.push_back(MakeConstantString(ProtocolName, ".objc_protocol_name"));
  Elements.push_back(ProtocolList);
  Elements.push_back(InstanceMethodList);
  Elements.push_back(ClassMethodList);
  ExistingProtocols[ProtocolName] =
    llvm::ConstantExpr::getBitCast(MakeGlobal(ProtocolTy, Elements,
          ".objc_protocol"), IdTy);
}

void CGObjCGNU::GenerateCategory(const ObjCCategoryImplDecl *OCD) {
  std::string ClassName = OCD->getClassInterface()->getNameAsString();
  std::string CategoryName = OCD->getNameAsString();
  // Collect information about instance methods
  llvm::SmallVector<Selector, 16> InstanceMethodSels;
  llvm::SmallVector<llvm::Constant*, 16> InstanceMethodTypes;
  for (ObjCCategoryImplDecl::instmeth_iterator
         iter = OCD->instmeth_begin(), endIter = OCD->instmeth_end();
       iter != endIter ; iter++) {
    InstanceMethodSels.push_back((*iter)->getSelector());
    std::string TypeStr;
    CGM.getContext().getObjCEncodingForMethodDecl(*iter,TypeStr);
    InstanceMethodTypes.push_back(CGM.GetAddrOfConstantCString(TypeStr));
  }

  // Collect information about class methods
  llvm::SmallVector<Selector, 16> ClassMethodSels;
  llvm::SmallVector<llvm::Constant*, 16> ClassMethodTypes;
  for (ObjCCategoryImplDecl::classmeth_iterator
         iter = OCD->classmeth_begin(), endIter = OCD->classmeth_end();
       iter != endIter ; iter++) {
    ClassMethodSels.push_back((*iter)->getSelector());
    std::string TypeStr;
    CGM.getContext().getObjCEncodingForMethodDecl(*iter,TypeStr);
    ClassMethodTypes.push_back(CGM.GetAddrOfConstantCString(TypeStr));
  }

  // Collect the names of referenced protocols
  llvm::SmallVector<std::string, 16> Protocols;
  const ObjCInterfaceDecl *ClassDecl = OCD->getClassInterface();
  const ObjCList<ObjCProtocolDecl> &Protos =ClassDecl->getReferencedProtocols();
  for (ObjCList<ObjCProtocolDecl>::iterator I = Protos.begin(),
       E = Protos.end(); I != E; ++I)
    Protocols.push_back((*I)->getNameAsString());

  std::vector<llvm::Constant*> Elements;
  Elements.push_back(MakeConstantString(CategoryName));
  Elements.push_back(MakeConstantString(ClassName));
  // Instance method list
  Elements.push_back(llvm::ConstantExpr::getBitCast(GenerateMethodList(
          ClassName, CategoryName, InstanceMethodSels, InstanceMethodTypes,
          false), PtrTy));
  // Class method list
  Elements.push_back(llvm::ConstantExpr::getBitCast(GenerateMethodList(
          ClassName, CategoryName, ClassMethodSels, ClassMethodTypes, true),
        PtrTy));
  // Protocol list
  Elements.push_back(llvm::ConstantExpr::getBitCast(
        GenerateProtocolList(Protocols), PtrTy));
  Categories.push_back(llvm::ConstantExpr::getBitCast(
        MakeGlobal(llvm::StructType::get(VMContext, PtrToInt8Ty, PtrToInt8Ty,
            PtrTy, PtrTy, PtrTy, NULL), Elements), PtrTy));
}

void CGObjCGNU::GenerateClass(const ObjCImplementationDecl *OID) {
  ASTContext &Context = CGM.getContext();

  // Get the superclass name.
  const ObjCInterfaceDecl * SuperClassDecl =
    OID->getClassInterface()->getSuperClass();
  std::string SuperClassName;
  if (SuperClassDecl) {
    SuperClassName = SuperClassDecl->getNameAsString();
    EmitClassRef(SuperClassName);
  }

  // Get the class name
  ObjCInterfaceDecl *ClassDecl =
    const_cast<ObjCInterfaceDecl *>(OID->getClassInterface());
  std::string ClassName = ClassDecl->getNameAsString();
  // Emit the symbol that is used to generate linker errors if this class is
  // referenced in other modules but not declared.
  std::string classSymbolName = "__objc_class_name_" + ClassName;
  if (llvm::GlobalVariable *symbol =
      TheModule.getGlobalVariable(classSymbolName)) {
    symbol->setInitializer(llvm::ConstantInt::get(LongTy, 0));
  } else {
    new llvm::GlobalVariable(TheModule, LongTy, false,
    llvm::GlobalValue::ExternalLinkage, llvm::ConstantInt::get(LongTy, 0),
    classSymbolName);
  }

  // Get the size of instances.
  int instanceSize = Context.getASTObjCImplementationLayout(OID).getSize() / 8;

  // Collect information about instance variables.
  llvm::SmallVector<llvm::Constant*, 16> IvarNames;
  llvm::SmallVector<llvm::Constant*, 16> IvarTypes;
  llvm::SmallVector<llvm::Constant*, 16> IvarOffsets;

  int superInstanceSize = !SuperClassDecl ? 0 :
    Context.getASTObjCInterfaceLayout(SuperClassDecl).getSize() / 8;
  // For non-fragile ivars, set the instance size to 0 - {the size of just this
  // class}.  The runtime will then set this to the correct value on load.
  if (CGM.getContext().getLangOptions().ObjCNonFragileABI) {
    instanceSize = 0 - (instanceSize - superInstanceSize);
  }
  for (ObjCInterfaceDecl::ivar_iterator iter = ClassDecl->ivar_begin(),
      endIter = ClassDecl->ivar_end() ; iter != endIter ; iter++) {
      // Store the name
      IvarNames.push_back(CGM.GetAddrOfConstantCString((*iter)
                                                         ->getNameAsString()));
      // Get the type encoding for this ivar
      std::string TypeStr;
      Context.getObjCEncodingForType((*iter)->getType(), TypeStr);
      IvarTypes.push_back(CGM.GetAddrOfConstantCString(TypeStr));
      // Get the offset
      uint64_t Offset;
      if (CGM.getContext().getLangOptions().ObjCNonFragileABI) {
		Offset = ComputeIvarBaseOffset(CGM, ClassDecl, *iter) -
			superInstanceSize;
        ObjCIvarOffsetVariable(ClassDecl, *iter);
      } else {
        Offset = ComputeIvarBaseOffset(CGM, ClassDecl, *iter);
      }
      IvarOffsets.push_back(
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(VMContext), Offset));
  }

  // Collect information about instance methods
  llvm::SmallVector<Selector, 16> InstanceMethodSels;
  llvm::SmallVector<llvm::Constant*, 16> InstanceMethodTypes;
  for (ObjCImplementationDecl::instmeth_iterator
         iter = OID->instmeth_begin(), endIter = OID->instmeth_end();
       iter != endIter ; iter++) {
    InstanceMethodSels.push_back((*iter)->getSelector());
    std::string TypeStr;
    Context.getObjCEncodingForMethodDecl((*iter),TypeStr);
    InstanceMethodTypes.push_back(CGM.GetAddrOfConstantCString(TypeStr));
  }
  for (ObjCImplDecl::propimpl_iterator
         iter = OID->propimpl_begin(), endIter = OID->propimpl_end();
       iter != endIter ; iter++) {
    ObjCPropertyDecl *property = (*iter)->getPropertyDecl();
    if (ObjCMethodDecl *getter = property->getGetterMethodDecl()) {
      InstanceMethodSels.push_back(getter->getSelector());
      std::string TypeStr;
      Context.getObjCEncodingForMethodDecl(getter,TypeStr);
      InstanceMethodTypes.push_back(CGM.GetAddrOfConstantCString(TypeStr));
    }
    if (ObjCMethodDecl *setter = property->getSetterMethodDecl()) {
      InstanceMethodSels.push_back(setter->getSelector());
      std::string TypeStr;
      Context.getObjCEncodingForMethodDecl(setter,TypeStr);
      InstanceMethodTypes.push_back(CGM.GetAddrOfConstantCString(TypeStr));
    }
  }

  // Collect information about class methods
  llvm::SmallVector<Selector, 16> ClassMethodSels;
  llvm::SmallVector<llvm::Constant*, 16> ClassMethodTypes;
  for (ObjCImplementationDecl::classmeth_iterator
         iter = OID->classmeth_begin(), endIter = OID->classmeth_end();
       iter != endIter ; iter++) {
    ClassMethodSels.push_back((*iter)->getSelector());
    std::string TypeStr;
    Context.getObjCEncodingForMethodDecl((*iter),TypeStr);
    ClassMethodTypes.push_back(CGM.GetAddrOfConstantCString(TypeStr));
  }
  // Collect the names of referenced protocols
  llvm::SmallVector<std::string, 16> Protocols;
  const ObjCList<ObjCProtocolDecl> &Protos =ClassDecl->getReferencedProtocols();
  for (ObjCList<ObjCProtocolDecl>::iterator I = Protos.begin(),
       E = Protos.end(); I != E; ++I)
    Protocols.push_back((*I)->getNameAsString());



  // Get the superclass pointer.
  llvm::Constant *SuperClass;
  if (!SuperClassName.empty()) {
    SuperClass = MakeConstantString(SuperClassName, ".super_class_name");
  } else {
    SuperClass = llvm::ConstantPointerNull::get(PtrToInt8Ty);
  }
  // Empty vector used to construct empty method lists
  llvm::SmallVector<llvm::Constant*, 1>  empty;
  // Generate the method and instance variable lists
  llvm::Constant *MethodList = GenerateMethodList(ClassName, "",
      InstanceMethodSels, InstanceMethodTypes, false);
  llvm::Constant *ClassMethodList = GenerateMethodList(ClassName, "",
      ClassMethodSels, ClassMethodTypes, true);
  llvm::Constant *IvarList = GenerateIvarList(IvarNames, IvarTypes,
      IvarOffsets);
  //Generate metaclass for class methods
  llvm::Constant *MetaClassStruct = GenerateClassStructure(NULLPtr,
      NULLPtr, 0x2L, /*name*/"", 0, Zeros[0], GenerateIvarList(
        empty, empty, empty), ClassMethodList, NULLPtr);

  // Generate the class structure
  llvm::Constant *ClassStruct =
    GenerateClassStructure(MetaClassStruct, SuperClass, 0x1L,
                           ClassName.c_str(), 0,
      llvm::ConstantInt::get(LongTy, instanceSize), IvarList,
      MethodList, GenerateProtocolList(Protocols));

  // Resolve the class aliases, if they exist.
  if (ClassPtrAlias) {
    ClassPtrAlias->setAliasee(
        llvm::ConstantExpr::getBitCast(ClassStruct, IdTy));
    ClassPtrAlias = 0;
  }
  if (MetaClassPtrAlias) {
    MetaClassPtrAlias->setAliasee(
        llvm::ConstantExpr::getBitCast(MetaClassStruct, IdTy));
    MetaClassPtrAlias = 0;
  }

  // Add class structure to list to be added to the symtab later
  ClassStruct = llvm::ConstantExpr::getBitCast(ClassStruct, PtrToInt8Ty);
  Classes.push_back(ClassStruct);
}


llvm::Function *CGObjCGNU::ModuleInitFunction() {
  // Only emit an ObjC load function if no Objective-C stuff has been called
  if (Classes.empty() && Categories.empty() && ConstantStrings.empty() &&
      ExistingProtocols.empty() && TypedSelectors.empty() &&
      UntypedSelectors.empty())
    return NULL;

  const llvm::StructType *SelStructTy = dyn_cast<llvm::StructType>(
          SelectorTy->getElementType());
  const llvm::Type *SelStructPtrTy = SelectorTy;
  bool isSelOpaque = false;
  if (SelStructTy == 0) {
    SelStructTy = llvm::StructType::get(VMContext, PtrToInt8Ty,
                                        PtrToInt8Ty, NULL);
    SelStructPtrTy = llvm::PointerType::getUnqual(SelStructTy);
    isSelOpaque = true;
  }

  // Name the ObjC types to make the IR a bit easier to read
  TheModule.addTypeName(".objc_selector", SelStructPtrTy);
  TheModule.addTypeName(".objc_id", IdTy);
  TheModule.addTypeName(".objc_imp", IMPTy);

  std::vector<llvm::Constant*> Elements;
  llvm::Constant *Statics = NULLPtr;
  // Generate statics list:
  if (ConstantStrings.size()) {
    llvm::ArrayType *StaticsArrayTy = llvm::ArrayType::get(PtrToInt8Ty,
        ConstantStrings.size() + 1);
    ConstantStrings.push_back(NULLPtr);
    Elements.push_back(MakeConstantString("NSConstantString",
          ".objc_static_class_name"));
    Elements.push_back(llvm::ConstantArray::get(StaticsArrayTy,
       ConstantStrings));
    llvm::StructType *StaticsListTy =
      llvm::StructType::get(VMContext, PtrToInt8Ty, StaticsArrayTy, NULL);
    llvm::Type *StaticsListPtrTy =
      llvm::PointerType::getUnqual(StaticsListTy);
    Statics = MakeGlobal(StaticsListTy, Elements, ".objc_statics");
    llvm::ArrayType *StaticsListArrayTy =
      llvm::ArrayType::get(StaticsListPtrTy, 2);
    Elements.clear();
    Elements.push_back(Statics);
    Elements.push_back(llvm::Constant::getNullValue(StaticsListPtrTy));
    Statics = MakeGlobal(StaticsListArrayTy, Elements, ".objc_statics_ptr");
    Statics = llvm::ConstantExpr::getBitCast(Statics, PtrTy);
  }
  // Array of classes, categories, and constant objects
  llvm::ArrayType *ClassListTy = llvm::ArrayType::get(PtrToInt8Ty,
      Classes.size() + Categories.size()  + 2);
  llvm::StructType *SymTabTy = llvm::StructType::get(VMContext,
                                                     LongTy, SelStructPtrTy,
                                                     llvm::Type::getInt16Ty(VMContext),
                                                     llvm::Type::getInt16Ty(VMContext),
                                                     ClassListTy, NULL);

  Elements.clear();
  // Pointer to an array of selectors used in this module.
  std::vector<llvm::Constant*> Selectors;
  for (std::map<TypedSelector, llvm::GlobalAlias*>::iterator
     iter = TypedSelectors.begin(), iterEnd = TypedSelectors.end();
     iter != iterEnd ; ++iter) {
    Elements.push_back(MakeConstantString(iter->first.first, ".objc_sel_name"));
    Elements.push_back(MakeConstantString(iter->first.second,
                                          ".objc_sel_types"));
    Selectors.push_back(llvm::ConstantStruct::get(SelStructTy, Elements));
    Elements.clear();
  }
  for (llvm::StringMap<llvm::GlobalAlias*>::iterator
      iter = UntypedSelectors.begin(), iterEnd = UntypedSelectors.end();
      iter != iterEnd; ++iter) {
    Elements.push_back(
        MakeConstantString(iter->getKeyData(), ".objc_sel_name"));
    Elements.push_back(NULLPtr);
    Selectors.push_back(llvm::ConstantStruct::get(SelStructTy, Elements));
    Elements.clear();
  }
  Elements.push_back(NULLPtr);
  Elements.push_back(NULLPtr);
  Selectors.push_back(llvm::ConstantStruct::get(SelStructTy, Elements));
  Elements.clear();
  // Number of static selectors
  Elements.push_back(llvm::ConstantInt::get(LongTy, Selectors.size() ));
  llvm::Constant *SelectorList = MakeGlobal(
          llvm::ArrayType::get(SelStructTy, Selectors.size()), Selectors,
          ".objc_selector_list");
  Elements.push_back(llvm::ConstantExpr::getBitCast(SelectorList,
    SelStructPtrTy));

  // Now that all of the static selectors exist, create pointers to them.
  int index = 0;
  for (std::map<TypedSelector, llvm::GlobalAlias*>::iterator
     iter=TypedSelectors.begin(), iterEnd =TypedSelectors.end();
     iter != iterEnd; ++iter) {
    llvm::Constant *Idxs[] = {Zeros[0],
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(VMContext), index++), Zeros[0]};
    llvm::Constant *SelPtr = new llvm::GlobalVariable(TheModule, SelStructPtrTy,
        true, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantExpr::getGetElementPtr(SelectorList, Idxs, 2),
        ".objc_sel_ptr");
    // If selectors are defined as an opaque type, cast the pointer to this
    // type.
    if (isSelOpaque) {
      SelPtr = llvm::ConstantExpr::getBitCast(SelPtr,
        llvm::PointerType::getUnqual(SelectorTy));
    }
    (*iter).second->setAliasee(SelPtr);
  }
  for (llvm::StringMap<llvm::GlobalAlias*>::iterator
      iter=UntypedSelectors.begin(), iterEnd = UntypedSelectors.end();
      iter != iterEnd; iter++) {
    llvm::Constant *Idxs[] = {Zeros[0],
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(VMContext), index++), Zeros[0]};
    llvm::Constant *SelPtr = new llvm::GlobalVariable
      (TheModule, SelStructPtrTy,
       true, llvm::GlobalValue::InternalLinkage,
       llvm::ConstantExpr::getGetElementPtr(SelectorList, Idxs, 2),
       ".objc_sel_ptr");
    // If selectors are defined as an opaque type, cast the pointer to this
    // type.
    if (isSelOpaque) {
      SelPtr = llvm::ConstantExpr::getBitCast(SelPtr,
        llvm::PointerType::getUnqual(SelectorTy));
    }
    (*iter).second->setAliasee(SelPtr);
  }
  // Number of classes defined.
  Elements.push_back(llvm::ConstantInt::get(llvm::Type::getInt16Ty(VMContext),
        Classes.size()));
  // Number of categories defined
  Elements.push_back(llvm::ConstantInt::get(llvm::Type::getInt16Ty(VMContext),
        Categories.size()));
  // Create an array of classes, then categories, then static object instances
  Classes.insert(Classes.end(), Categories.begin(), Categories.end());
  //  NULL-terminated list of static object instances (mainly constant strings)
  Classes.push_back(Statics);
  Classes.push_back(NULLPtr);
  llvm::Constant *ClassList = llvm::ConstantArray::get(ClassListTy, Classes);
  Elements.push_back(ClassList);
  // Construct the symbol table
  llvm::Constant *SymTab= MakeGlobal(SymTabTy, Elements);

  // The symbol table is contained in a module which has some version-checking
  // constants
  llvm::StructType * ModuleTy = llvm::StructType::get(VMContext, LongTy, LongTy,
      PtrToInt8Ty, llvm::PointerType::getUnqual(SymTabTy), NULL);
  Elements.clear();
  // Runtime version used for compatibility checking.
  if (CGM.getContext().getLangOptions().ObjCNonFragileABI) {
	Elements.push_back(llvm::ConstantInt::get(LongTy,
        NonFragileRuntimeVersion));
  } else {
    Elements.push_back(llvm::ConstantInt::get(LongTy, RuntimeVersion));
  }
  // sizeof(ModuleTy)
  llvm::TargetData td = llvm::TargetData(&TheModule);
  Elements.push_back(llvm::ConstantInt::get(LongTy,
                     td.getTypeSizeInBits(ModuleTy)/8));
  //FIXME: Should be the path to the file where this module was declared
  Elements.push_back(NULLPtr);
  Elements.push_back(SymTab);
  llvm::Value *Module = MakeGlobal(ModuleTy, Elements);

  // Create the load function calling the runtime entry point with the module
  // structure
  llvm::Function * LoadFunction = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(VMContext), false),
      llvm::GlobalValue::InternalLinkage, ".objc_load_function",
      &TheModule);
  llvm::BasicBlock *EntryBB =
      llvm::BasicBlock::Create(VMContext, "entry", LoadFunction);
  CGBuilderTy Builder(VMContext);
  Builder.SetInsertPoint(EntryBB);

  std::vector<const llvm::Type*> Params(1,
      llvm::PointerType::getUnqual(ModuleTy));
  llvm::Value *Register = CGM.CreateRuntimeFunction(llvm::FunctionType::get(
        llvm::Type::getVoidTy(VMContext), Params, true), "__objc_exec_class");
  Builder.CreateCall(Register, Module);
  Builder.CreateRetVoid();

  return LoadFunction;
}

llvm::Function *CGObjCGNU::GenerateMethod(const ObjCMethodDecl *OMD,
                                          const ObjCContainerDecl *CD) {
  const ObjCCategoryImplDecl *OCD =
    dyn_cast<ObjCCategoryImplDecl>(OMD->getDeclContext());
  std::string CategoryName = OCD ? OCD->getNameAsString() : "";
  std::string ClassName = OMD->getClassInterface()->getNameAsString();
  std::string MethodName = OMD->getSelector().getAsString();
  bool isClassMethod = !OMD->isInstanceMethod();

  CodeGenTypes &Types = CGM.getTypes();
  const llvm::FunctionType *MethodTy =
    Types.GetFunctionType(Types.getFunctionInfo(OMD), OMD->isVariadic());
  std::string FunctionName = SymbolNameForMethod(ClassName, CategoryName,
      MethodName, isClassMethod);

  llvm::Function *Method = llvm::Function::Create(MethodTy,
      llvm::GlobalValue::InternalLinkage,
      FunctionName,
      &TheModule);
  return Method;
}

llvm::Function *CGObjCGNU::GetPropertyGetFunction() {
	std::vector<const llvm::Type*> Params;
	const llvm::Type *BoolTy =
		CGM.getTypes().ConvertType(CGM.getContext().BoolTy);
	Params.push_back(IdTy);
	Params.push_back(SelectorTy);
	// FIXME: Using LongTy for ptrdiff_t is probably broken on Win64
	Params.push_back(LongTy);
	Params.push_back(BoolTy);
	// void objc_getProperty (id, SEL, ptrdiff_t, bool)
	const llvm::FunctionType *FTy =
		llvm::FunctionType::get(IdTy, Params, false);
	return cast<llvm::Function>(CGM.CreateRuntimeFunction(FTy,
				"objc_getProperty"));
}

llvm::Function *CGObjCGNU::GetPropertySetFunction() {
	std::vector<const llvm::Type*> Params;
	const llvm::Type *BoolTy =
		CGM.getTypes().ConvertType(CGM.getContext().BoolTy);
	Params.push_back(IdTy);
	Params.push_back(SelectorTy);
	// FIXME: Using LongTy for ptrdiff_t is probably broken on Win64
	Params.push_back(LongTy);
	Params.push_back(IdTy);
	Params.push_back(BoolTy);
	Params.push_back(BoolTy);
	// void objc_setProperty (id, SEL, ptrdiff_t, id, bool, bool)
	const llvm::FunctionType *FTy =
		llvm::FunctionType::get(llvm::Type::getVoidTy(VMContext), Params, false);
	return cast<llvm::Function>(CGM.CreateRuntimeFunction(FTy,
				"objc_setProperty"));
}

llvm::Constant *CGObjCGNU::EnumerationMutationFunction() {
  CodeGen::CodeGenTypes &Types = CGM.getTypes();
  ASTContext &Ctx = CGM.getContext();
  // void objc_enumerationMutation (id)
  llvm::SmallVector<QualType,16> Params;
  Params.push_back(ASTIdTy);
  const llvm::FunctionType *FTy =
    Types.GetFunctionType(Types.getFunctionInfo(Ctx.VoidTy, Params), false);
  return CGM.CreateRuntimeFunction(FTy, "objc_enumerationMutation");
}

void CGObjCGNU::EmitTryOrSynchronizedStmt(CodeGen::CodeGenFunction &CGF,
                                          const Stmt &S) {
  // Pointer to the personality function
  llvm::Constant *Personality =
    CGM.CreateRuntimeFunction(llvm::FunctionType::get(llvm::Type::getInt32Ty(VMContext),
          true),
        "__gnu_objc_personality_v0");
  Personality = llvm::ConstantExpr::getBitCast(Personality, PtrTy);
  std::vector<const llvm::Type*> Params;
  Params.push_back(PtrTy);
  llvm::Value *RethrowFn =
    CGM.CreateRuntimeFunction(llvm::FunctionType::get(llvm::Type::getVoidTy(VMContext),
          Params, false), "_Unwind_Resume_or_Rethrow");

  bool isTry = isa<ObjCAtTryStmt>(S);
  llvm::BasicBlock *TryBlock = CGF.createBasicBlock("try");
  llvm::BasicBlock *PrevLandingPad = CGF.getInvokeDest();
  llvm::BasicBlock *TryHandler = CGF.createBasicBlock("try.handler");
  llvm::BasicBlock *CatchInCatch = CGF.createBasicBlock("catch.rethrow");
  llvm::BasicBlock *FinallyBlock = CGF.createBasicBlock("finally");
  llvm::BasicBlock *FinallyRethrow = CGF.createBasicBlock("finally.throw");
  llvm::BasicBlock *FinallyEnd = CGF.createBasicBlock("finally.end");

  // GNU runtime does not currently support @synchronized()
  if (!isTry) {
    std::vector<const llvm::Type*> Args(1, IdTy);
    llvm::FunctionType *FTy =
      llvm::FunctionType::get(llvm::Type::getVoidTy(VMContext), Args, false);
    llvm::Value *SyncEnter = CGM.CreateRuntimeFunction(FTy, "objc_sync_enter");
    llvm::Value *SyncArg =
      CGF.EmitScalarExpr(cast<ObjCAtSynchronizedStmt>(S).getSynchExpr());
    SyncArg = CGF.Builder.CreateBitCast(SyncArg, IdTy);
    CGF.Builder.CreateCall(SyncEnter, SyncArg);
  }


  // Push an EH context entry, used for handling rethrows and jumps
  // through finally.
  CGF.PushCleanupBlock(FinallyBlock);

  // Emit the statements in the @try {} block
  CGF.setInvokeDest(TryHandler);

  CGF.EmitBlock(TryBlock);
  CGF.EmitStmt(isTry ? cast<ObjCAtTryStmt>(S).getTryBody()
                     : cast<ObjCAtSynchronizedStmt>(S).getSynchBody());

  // Jump to @finally if there is no exception
  CGF.EmitBranchThroughCleanup(FinallyEnd);

  // Emit the handlers
  CGF.EmitBlock(TryHandler);

  // Get the correct versions of the exception handling intrinsics
  llvm::TargetData td = llvm::TargetData(&TheModule);
  int PointerWidth = td.getTypeSizeInBits(PtrTy);
  assert((PointerWidth == 32 || PointerWidth == 64) &&
    "Can't yet handle exceptions if pointers are not 32 or 64 bits");
  llvm::Value *llvm_eh_exception =
    CGF.CGM.getIntrinsic(llvm::Intrinsic::eh_exception);
  llvm::Value *llvm_eh_selector = PointerWidth == 32 ?
    CGF.CGM.getIntrinsic(llvm::Intrinsic::eh_selector_i32) :
    CGF.CGM.getIntrinsic(llvm::Intrinsic::eh_selector_i64);
  llvm::Value *llvm_eh_typeid_for = PointerWidth == 32 ?
    CGF.CGM.getIntrinsic(llvm::Intrinsic::eh_typeid_for_i32) :
    CGF.CGM.getIntrinsic(llvm::Intrinsic::eh_typeid_for_i64);

  // Exception object
  llvm::Value *Exc = CGF.Builder.CreateCall(llvm_eh_exception, "exc");
  llvm::Value *RethrowPtr = CGF.CreateTempAlloca(Exc->getType(), "_rethrow");

  llvm::SmallVector<llvm::Value*, 8> ESelArgs;
  llvm::SmallVector<std::pair<const ParmVarDecl*, const Stmt*>, 8> Handlers;

  ESelArgs.push_back(Exc);
  ESelArgs.push_back(Personality);

  bool HasCatchAll = false;
  // Only @try blocks are allowed @catch blocks, but both can have @finally
  if (isTry) {
    if (const ObjCAtCatchStmt* CatchStmt =
      cast<ObjCAtTryStmt>(S).getCatchStmts())  {
      CGF.setInvokeDest(CatchInCatch);

      for (; CatchStmt; CatchStmt = CatchStmt->getNextCatchStmt()) {
        const ParmVarDecl *CatchDecl = CatchStmt->getCatchParamDecl();
        Handlers.push_back(std::make_pair(CatchDecl,
                                          CatchStmt->getCatchBody()));

        // @catch() and @catch(id) both catch any ObjC exception
        if (!CatchDecl || CatchDecl->getType()->isObjCIdType()
            || CatchDecl->getType()->isObjCQualifiedIdType()) {
          // Use i8* null here to signal this is a catch all, not a cleanup.
          ESelArgs.push_back(NULLPtr);
          HasCatchAll = true;
          // No further catches after this one will ever by reached
          break;
        }

        // All other types should be Objective-C interface pointer types.
        const ObjCObjectPointerType *OPT =
          CatchDecl->getType()->getAsObjCObjectPointerType();
        assert(OPT && "Invalid @catch type.");
        const ObjCInterfaceType *IT =
          OPT->getPointeeType()->getAsObjCInterfaceType();
        assert(IT && "Invalid @catch type.");
        llvm::Value *EHType =
          MakeConstantString(IT->getDecl()->getNameAsString());
        ESelArgs.push_back(EHType);
      }
    }
  }

  // We use a cleanup unless there was already a catch all.
  if (!HasCatchAll) {
    ESelArgs.push_back(llvm::ConstantInt::get(llvm::Type::getInt32Ty(VMContext), 0));
    Handlers.push_back(std::make_pair((const ParmVarDecl*) 0, (const Stmt*) 0));
  }

  // Find which handler was matched.
  llvm::Value *ESelector = CGF.Builder.CreateCall(llvm_eh_selector,
      ESelArgs.begin(), ESelArgs.end(), "selector");

  for (unsigned i = 0, e = Handlers.size(); i != e; ++i) {
    const ParmVarDecl *CatchParam = Handlers[i].first;
    const Stmt *CatchBody = Handlers[i].second;

    llvm::BasicBlock *Next = 0;

    // The last handler always matches.
    if (i + 1 != e) {
      assert(CatchParam && "Only last handler can be a catch all.");

      // Test whether this block matches the type for the selector and branch
      // to Match if it does, or to the next BB if it doesn't.
      llvm::BasicBlock *Match = CGF.createBasicBlock("match");
      Next = CGF.createBasicBlock("catch.next");
      llvm::Value *Id = CGF.Builder.CreateCall(llvm_eh_typeid_for,
          CGF.Builder.CreateBitCast(ESelArgs[i+2], PtrTy));
      CGF.Builder.CreateCondBr(CGF.Builder.CreateICmpEQ(ESelector, Id), Match,
          Next);

      CGF.EmitBlock(Match);
    }

    if (CatchBody) {
      llvm::Value *ExcObject = CGF.Builder.CreateBitCast(Exc,
          CGF.ConvertType(CatchParam->getType()));

      // Bind the catch parameter if it exists.
      if (CatchParam) {
        // CatchParam is a ParmVarDecl because of the grammar
        // construction used to handle this, but for codegen purposes
        // we treat this as a local decl.
        CGF.EmitLocalBlockVarDecl(*CatchParam);
        CGF.Builder.CreateStore(ExcObject, CGF.GetAddrOfLocalVar(CatchParam));
      }

      CGF.ObjCEHValueStack.push_back(ExcObject);
      CGF.EmitStmt(CatchBody);
      CGF.ObjCEHValueStack.pop_back();

      CGF.EmitBranchThroughCleanup(FinallyEnd);

      if (Next)
        CGF.EmitBlock(Next);
    } else {
      assert(!Next && "catchup should be last handler.");

      CGF.Builder.CreateStore(Exc, RethrowPtr);
      CGF.EmitBranchThroughCleanup(FinallyRethrow);
    }
  }
  // The @finally block is a secondary landing pad for any exceptions thrown in
  // @catch() blocks
  CGF.EmitBlock(CatchInCatch);
  Exc = CGF.Builder.CreateCall(llvm_eh_exception, "exc");
  ESelArgs.clear();
  ESelArgs.push_back(Exc);
  ESelArgs.push_back(Personality);
  ESelArgs.push_back(llvm::ConstantInt::get(llvm::Type::getInt32Ty(VMContext), 0));
  CGF.Builder.CreateCall(llvm_eh_selector, ESelArgs.begin(), ESelArgs.end(),
      "selector");
  CGF.Builder.CreateCall(llvm_eh_typeid_for,
      CGF.Builder.CreateIntToPtr(ESelArgs[2], PtrTy));
  CGF.Builder.CreateStore(Exc, RethrowPtr);
  CGF.EmitBranchThroughCleanup(FinallyRethrow);

  CodeGenFunction::CleanupBlockInfo Info = CGF.PopCleanupBlock();

  CGF.setInvokeDest(PrevLandingPad);

  CGF.EmitBlock(FinallyBlock);


  if (isTry) {
    if (const ObjCAtFinallyStmt* FinallyStmt =
        cast<ObjCAtTryStmt>(S).getFinallyStmt())
      CGF.EmitStmt(FinallyStmt->getFinallyBody());
  } else {
    // Emit 'objc_sync_exit(expr)' as finally's sole statement for
    // @synchronized.
    std::vector<const llvm::Type*> Args(1, IdTy);
    llvm::FunctionType *FTy =
      llvm::FunctionType::get(llvm::Type::getVoidTy(VMContext), Args, false);
    llvm::Value *SyncExit = CGM.CreateRuntimeFunction(FTy, "objc_sync_exit");
    llvm::Value *SyncArg =
      CGF.EmitScalarExpr(cast<ObjCAtSynchronizedStmt>(S).getSynchExpr());
    SyncArg = CGF.Builder.CreateBitCast(SyncArg, IdTy);
    CGF.Builder.CreateCall(SyncExit, SyncArg);
  }

  if (Info.SwitchBlock)
    CGF.EmitBlock(Info.SwitchBlock);
  if (Info.EndBlock)
    CGF.EmitBlock(Info.EndBlock);

  // Branch around the rethrow code.
  CGF.EmitBranch(FinallyEnd);

  CGF.EmitBlock(FinallyRethrow);
  CGF.Builder.CreateCall(RethrowFn, CGF.Builder.CreateLoad(RethrowPtr));
  CGF.Builder.CreateUnreachable();

  CGF.EmitBlock(FinallyEnd);

}

void CGObjCGNU::EmitThrowStmt(CodeGen::CodeGenFunction &CGF,
                              const ObjCAtThrowStmt &S) {
  llvm::Value *ExceptionAsObject;

  std::vector<const llvm::Type*> Args(1, IdTy);
  llvm::FunctionType *FTy =
    llvm::FunctionType::get(llvm::Type::getVoidTy(VMContext), Args, false);
  llvm::Value *ThrowFn =
    CGM.CreateRuntimeFunction(FTy, "objc_exception_throw");

  if (const Expr *ThrowExpr = S.getThrowExpr()) {
    llvm::Value *Exception = CGF.EmitScalarExpr(ThrowExpr);
    ExceptionAsObject = Exception;
  } else {
    assert((!CGF.ObjCEHValueStack.empty() && CGF.ObjCEHValueStack.back()) &&
           "Unexpected rethrow outside @catch block.");
    ExceptionAsObject = CGF.ObjCEHValueStack.back();
  }
  ExceptionAsObject =
      CGF.Builder.CreateBitCast(ExceptionAsObject, IdTy, "tmp");

  // Note: This may have to be an invoke, if we want to support constructs like:
  // @try {
  //  @throw(obj);
  // }
  // @catch(id) ...
  //
  // This is effectively turning @throw into an incredibly-expensive goto, but
  // it may happen as a result of inlining followed by missed optimizations, or
  // as a result of stupidity.
  llvm::BasicBlock *UnwindBB = CGF.getInvokeDest();
  if (!UnwindBB) {
    CGF.Builder.CreateCall(ThrowFn, ExceptionAsObject);
    CGF.Builder.CreateUnreachable();
  } else {
    CGF.Builder.CreateInvoke(ThrowFn, UnwindBB, UnwindBB, &ExceptionAsObject,
        &ExceptionAsObject+1);
  }
  // Clear the insertion point to indicate we are in unreachable code.
  CGF.Builder.ClearInsertionPoint();
}

llvm::Value * CGObjCGNU::EmitObjCWeakRead(CodeGen::CodeGenFunction &CGF,
                                          llvm::Value *AddrWeakObj)
{
  return 0;
}

void CGObjCGNU::EmitObjCWeakAssign(CodeGen::CodeGenFunction &CGF,
                                   llvm::Value *src, llvm::Value *dst)
{
  return;
}

void CGObjCGNU::EmitObjCGlobalAssign(CodeGen::CodeGenFunction &CGF,
                                     llvm::Value *src, llvm::Value *dst)
{
  return;
}

void CGObjCGNU::EmitObjCIvarAssign(CodeGen::CodeGenFunction &CGF,
                                   llvm::Value *src, llvm::Value *dst)
{
  return;
}

void CGObjCGNU::EmitObjCStrongCastAssign(CodeGen::CodeGenFunction &CGF,
                                         llvm::Value *src, llvm::Value *dst)
{
  return;
}

void CGObjCGNU::EmitGCMemmoveCollectable(CodeGen::CodeGenFunction &CGF,
                                         llvm::Value *DestPtr,
                                         llvm::Value *SrcPtr,
                                         unsigned long size) {
  return;
}

llvm::GlobalVariable *CGObjCGNU::ObjCIvarOffsetVariable(
                              const ObjCInterfaceDecl *ID,
                              const ObjCIvarDecl *Ivar) {
  const std::string Name = "__objc_ivar_offset_" + ID->getNameAsString()
    + '.' + Ivar->getNameAsString();
  // Emit the variable and initialize it with what we think the correct value
  // is.  This allows code compiled with non-fragile ivars to work correctly
  // when linked against code which isn't (most of the time).
  llvm::GlobalVariable *IvarOffsetGV = CGM.getModule().getGlobalVariable(Name);
  if (!IvarOffsetGV) {
    uint64_t Offset = ComputeIvarBaseOffset(CGM, ID, Ivar);
    llvm::ConstantInt *OffsetGuess =
      llvm::ConstantInt::get(LongTy, Offset, "ivar");
    IvarOffsetGV = new llvm::GlobalVariable(TheModule, LongTy, false,
        llvm::GlobalValue::WeakAnyLinkage, OffsetGuess, Name);
  }
  return IvarOffsetGV;
}

LValue CGObjCGNU::EmitObjCValueForIvar(CodeGen::CodeGenFunction &CGF,
                                       QualType ObjectTy,
                                       llvm::Value *BaseValue,
                                       const ObjCIvarDecl *Ivar,
                                       unsigned CVRQualifiers) {
  const ObjCInterfaceDecl *ID = ObjectTy->getAsObjCInterfaceType()->getDecl();
  return EmitValueForIvarAtOffset(CGF, ID, BaseValue, Ivar, CVRQualifiers,
                                  EmitIvarOffset(CGF, ID, Ivar));
}

static const ObjCInterfaceDecl *FindIvarInterface(ASTContext &Context,
                                                  const ObjCInterfaceDecl *OID,
                                                  const ObjCIvarDecl *OIVD) {
  llvm::SmallVector<ObjCIvarDecl*, 16> Ivars;
  Context.ShallowCollectObjCIvars(OID, Ivars);
  for (unsigned k = 0, e = Ivars.size(); k != e; ++k) {
    if (OIVD == Ivars[k])
      return OID;
  }

  // Otherwise check in the super class.
  if (const ObjCInterfaceDecl *Super = OID->getSuperClass())
    return FindIvarInterface(Context, Super, OIVD);

  return 0;
}

llvm::Value *CGObjCGNU::EmitIvarOffset(CodeGen::CodeGenFunction &CGF,
                         const ObjCInterfaceDecl *Interface,
                         const ObjCIvarDecl *Ivar) {
  if (CGF.getContext().getLangOptions().ObjCNonFragileABI) {
    Interface = FindIvarInterface(CGM.getContext(), Interface, Ivar);
    return CGF.Builder.CreateLoad(ObjCIvarOffsetVariable(Interface, Ivar),
                                  false, "ivar");
  }
  uint64_t Offset = ComputeIvarBaseOffset(CGF.CGM, Interface, Ivar);
  return llvm::ConstantInt::get(LongTy, Offset, "ivar");
}

CodeGen::CGObjCRuntime *
CodeGen::CreateGNUObjCRuntime(CodeGen::CodeGenModule &CGM) {
  return new CGObjCGNU(CGM);
}
