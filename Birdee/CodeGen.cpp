
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/ADT/Triple.h"

#include "CompileError.h"
#include "AST.h"
#include <cassert>

using namespace llvm;
using Birdee::CompileError;
using std::unordered_map;
using namespace Birdee;

static LLVMContext context;
static IRBuilder<> builder(context);

static Module* module;
static std::unique_ptr<DIBuilder> DBuilder;

static llvm::Type* ty_int=IntegerType::getInt32Ty(context);
static llvm::Type* ty_long = IntegerType::getInt64Ty(context);

struct DebugInfo {
	DICompileUnit *cu;
	std::vector<DIScope *> LexicalBlocks;

	void emitLocation(Birdee::StatementAST *AST);
} dinfo;

namespace std
{

	template <>
	struct hash<Birdee::ResolvedType>
	{
		std::size_t operator()(const Birdee::ResolvedType& a) const
		{
			uintptr_t v = (((int)a.type) << 5) + a.index_level + (uintptr_t)a.proto_ast;
			hash<uintptr_t> has;
			return has(v);
		}
	};
}

typedef DIType* PDIType;
bool GenerateType(const Birdee::ResolvedType& type, PDIType& dtype, llvm::Type* & base);
struct LLVMHelper {
	Function* cur_llvm_func = nullptr;
	ClassAST* cur_class_ast = nullptr;

	unordered_map<Birdee::ResolvedType, llvm::Type*> typemap;
	unordered_map<Birdee::ResolvedType, DIType *> dtypemap;
	llvm::Type* GetType(const Birdee::ResolvedType& ty)
	{
		PDIType dtype;
		return  GetType(ty, dtype);
	}
	llvm::Type* GetType(const Birdee::ResolvedType& ty, PDIType& dtype)
	{
		auto itr = typemap.find(ty);
		if (itr != typemap.end())
			return itr->second;
		llvm::Type* ret;
		bool resolved = GenerateType(ty, dtype,ret);
		if (resolved)
		{
			typemap[ty] = ret;
			dtypemap[ty] = dtype;
		}
		return ret;
	}

	DIType* GetDType(const Birdee::ResolvedType& ty)
	{
		auto itr = dtypemap.find(ty);
		if (itr != dtypemap.end())
			return itr->second;
		assert(0 && "Cannot find the DIType");
		return nullptr;
	}

} helper;


bool GenerateType(const Birdee::ResolvedType& type, PDIType& dtype, llvm::Type* & base)
{
	bool resolved = true;
	switch (type.type)
	{
	case tok_boolean:
		base = llvm::Type::getInt1Ty(context);
		dtype = DBuilder->createBasicType("boolean", 1, dwarf::DW_ATE_boolean);
		break;
	case tok_uint:
		base = llvm::Type::getInt32Ty(context);
		dtype = DBuilder->createBasicType("uint", 32, dwarf::DW_ATE_unsigned);
		break;
	case tok_int:
		base= llvm::Type::getInt32Ty(context);
		dtype = DBuilder->createBasicType("int", 32, dwarf::DW_ATE_signed);
		break;
	case tok_long:
		base = llvm::Type::getInt64Ty(context);
		dtype = DBuilder->createBasicType("long", 64, dwarf::DW_ATE_signed);
		break;
	case tok_ulong:
		base= llvm::Type::getInt64Ty(context);
		dtype = DBuilder->createBasicType("ulong", 64, dwarf::DW_ATE_unsigned);
		break;
	case tok_float:
		base= llvm::Type::getFloatTy(context);
		dtype = DBuilder->createBasicType("float", 32, dwarf::DW_ATE_float);
		break;
	case tok_double:
		base = llvm::Type::getDoubleTy(context);
		dtype =DBuilder->createBasicType("double", 64, dwarf::DW_ATE_float);
		break;
	case tok_pointer:
		base = llvm::Type::getInt8PtrTy(context);
		dtype = DBuilder->createBasicType("pointer", 64, dwarf::DW_ATE_address);
		break;
	case tok_func:
		base = type.proto_ast->GenerateFunctionType();
		dtype = type.proto_ast->GenerateDebugType();
		break;
	case tok_void:
		base = llvm::Type::getVoidTy(context);
		dtype = DBuilder->createBasicType("void", 0, dwarf::DW_ATE_address);
		break;
	case tok_class:
		if (!type.class_ast->llvm_type)
		{
			resolved = false;
			base = StructType::create(context, type.class_ast->GetUniqueName())->getPointerTo();
			dtype = DBuilder->createPointerType(DBuilder->createUnspecifiedType(type.class_ast->GetUniqueName()),64);
		}
		else
		{
			base = type.class_ast->llvm_type->getPointerTo();
			dtype = DBuilder->createPointerType(type.class_ast->llvm_dtype, 64);
		}
		break;
	default:
		assert(0 && "Error type");
	}
	if (type.index_level > 0)
	{
		for (int i = 0; i < type.index_level; i++)
		{
			dtype=DBuilder->createPointerType(dtype, 64);
			base = base->getPointerTo();
		}	
	}
	return resolved;
}


void Birdee::VariableSingleDefAST::PreGenerateForGlobal()
{
	DIType* ty;
	auto type=helper.GetType(resolved_type, ty);
	GlobalVariable* v = new GlobalVariable(*module, type,false,GlobalValue::CommonLinkage,
		Constant::getNullValue(type),cu.symbol_prefix+name);
	DIGlobalVariableExpression* D = DBuilder->createGlobalVariableExpression(
			dinfo.cu, cu.symbol_prefix + name, cu.symbol_prefix + name, dinfo.cu->getFile(), Pos.line, ty,
			true);
	llvm_value = v;
	
	v->addDebugInfo(D); //fix-me: check if this works
	//should look like https://releases.llvm.org/2.7/docs/SourceLevelDebugging.html#ccxx_global_variable
}

void Birdee::VariableSingleDefAST::PreGenerateForArgument(llvm::Value* init,int argno)
{
	DIType* ty;
	llvm_value = builder.CreateAlloca(helper.GetType(resolved_type, ty), nullptr, name);
	
	// Create a debug descriptor for the variable.
	DILocalVariable *D = DBuilder->createParameterVariable(
		dinfo.LexicalBlocks.back(), name, argno, dinfo.cu->getFile(), Pos.line, ty,
		true);
	
	DBuilder->insertDeclare(llvm_value, D, DBuilder->createExpression(),
		DebugLoc::get(Pos.line, Pos.pos, dinfo.LexicalBlocks.back()),
		builder.GetInsertBlock());

	builder.CreateStore(init, llvm_value);
}

llvm::Value* Birdee::VariableSingleDefAST::Generate()
{
	dinfo.emitLocation(this);
	if (!llvm_value)
	{
		DIType* ty;
		llvm_value = builder.CreateAlloca(helper.GetType(resolved_type, ty), nullptr, name);

		// Create a debug descriptor for the variable.
		DILocalVariable *D = DBuilder->createAutoVariable(dinfo.LexicalBlocks.back(), name, dinfo.cu->getFile(), Pos.line, ty,
			true);

		DBuilder->insertDeclare(llvm_value, D, DBuilder->createExpression(),
			DebugLoc::get(Pos.line, Pos.pos, dinfo.LexicalBlocks.back()),
			builder.GetInsertBlock());
	}
	if (val)
	{
		return builder.CreateStore(val->Generate(), llvm_value);
	}
	else
	{
		return builder.CreateStore(Constant::getNullValue(llvm_value->getType()->getPointerElementType()), llvm_value);
	}
}

DISubprogram * PrepareFunctionDebugInfo(Function* TheFunction,DISubroutineType* type,SourcePos pos)
{

	// Create a subprogram DIE for this function.
	DIFile *Unit = DBuilder->createFile(dinfo.cu->getFilename(),
		dinfo.cu->getDirectory());
	DIScope *FContext = Unit;
	unsigned LineNo = pos.line;
	unsigned ScopeLine = LineNo;
	DISubprogram *SP = DBuilder->createFunction(
		FContext, TheFunction->getName(), StringRef(), Unit, LineNo,
		type,
		false /* external linkage */, true /* definition */, ScopeLine,
		DINode::FlagPrototyped, false);
	TheFunction->setSubprogram(SP);
	return SP;
}

void Birdee::CompileUnit::InitForGenerate()
{
	InitializeNativeTarget();
	InitializeNativeTargetAsmPrinter();
	InitializeNativeTargetAsmParser();

	module= new Module(name, context);

	// Add the current debug info version into the module.
	module->addModuleFlag(Module::Warning, "Debug Info Version",
		DEBUG_METADATA_VERSION);

	// Darwin only supports dwarf2.
	if (Triple(sys::getProcessTriple()).isOSDarwin())
		module->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 2);

	DBuilder = llvm::make_unique<DIBuilder>(*module);
	dinfo.cu = DBuilder->createCompileUnit(
		dwarf::DW_LANG_C, DBuilder->createFile(filename, "."),
		"Birdee Compiler", 0, "", 0);

	//first generate the classes, as the functions may reference them
	//this will generate the LLVM types for the classes
	for (auto& cls : classmap)
	{
		cls.second.get().PreGenerate();
	}

	//generate the function objects of the function. Not the time to generate the bodies, 
	//since functions may reference each other
	for (auto& cls : classmap)
	{
		cls.second.get().PreGenerateFuncs();
	}

	for (auto& func : funcmap)
	{
		func.second.get().PreGenerate();
	}

	for (auto& dim : dimmap)
	{
		dim.second.get().PreGenerateForGlobal();
	}

	FunctionType *FT =
		FunctionType::get(llvm::Type::getVoidTy(context),false);

	Function *F =Function::Create(FT, Function::ExternalLinkage, cu.symbol_prefix+"main", module);
	BasicBlock *BB = BasicBlock::Create(context, "entry", F);
	builder.SetInsertPoint(BB);
	//SmallVector<Metadata *, 8> dargs{ DBuilder->createBasicType("void", 0, dwarf::DW_ATE_address) };
	DISubroutineType* functy= DBuilder->createSubroutineType(DBuilder->getOrCreateTypeArray({ DBuilder->createBasicType("void", 0, dwarf::DW_ATE_address) }));
	auto dbginfo=PrepareFunctionDebugInfo(F, functy, SourcePos(1, 1));

	// Push the current scope.
	dinfo.LexicalBlocks.push_back(dbginfo);
	for (auto& stmt : toplevel)
	{
		stmt->Generate();
	}
	dinfo.LexicalBlocks.pop_back();

	// Finalize the debug info.
	DBuilder->finalize();

	// Print out all of the generated code.
	module->print(errs(), nullptr);
}

llvm::FunctionType * Birdee::PrototypeAST::GenerateFunctionType()
{
	std::vector<llvm::Type*> args;
	DIType* dummy;
	if (cls) {
		args.push_back(cls->llvm_type->getPointerTo());
	}
	for (auto& arg : resolved_args)
	{
		llvm::Type* argty = helper.GetType(arg->resolved_type, dummy);
		args.push_back(argty);
	}
	return FunctionType::get(helper.GetType(resolved_type, dummy),args,false);
}

DIType * Birdee::PrototypeAST::GenerateDebugType()
{
	SmallVector<Metadata *, 8> dargs;
	// Add the result type.
	dargs.push_back(helper.GetDType(resolved_type));
	
	if (cls) {
		dargs.push_back(DBuilder->createPointerType(cls->llvm_dtype,64));
	}
	for (auto& arg : resolved_args)
	{
		DIType* darg = helper.GetDType(arg->resolved_type);
		dargs.push_back(darg);
	}
	return DBuilder->createSubroutineType(DBuilder->getOrCreateTypeArray(dargs));
}

void Birdee::ClassAST::PreGenerate()
{
	vector<llvm::Type*> types;
	SmallVector<Metadata *, 8> dtypes;

	for (auto& field : fields)
	{
		DIType* dty;
		types.push_back(helper.GetType(field.decl->resolved_type, dty));
		dtypes.push_back(dty); 
	}
	llvm_type = StructType::create(context, types, GetUniqueName());
	DIFile *Unit = DBuilder->createFile(dinfo.cu->getFilename(),
		dinfo.cu->getDirectory());
	llvm_dtype=DBuilder->createStructType(dinfo.cu, GetUniqueName(), Unit, Pos.line, 64, 64, DINode::DIFlags::FlagZero, nullptr, DBuilder->getOrCreateArray(dtypes));
	//fix-me: the align and the size
}

void Birdee::ClassAST::PreGenerateFuncs()
{
	for (auto& func : funcs)
	{
		func.decl->PreGenerate();
	}
}
DIType* Birdee::FunctionAST::PreGenerate()
{
	if (llvm_func)
		return helper.dtypemap[resolved_type];
	string prefix = cu.symbol_prefix;
	if (Proto->cls)
		prefix += "." + Proto->cls->name + ".";
	llvm_func = Function::Create(Proto->GenerateFunctionType(), Function::ExternalLinkage, prefix + Proto->GetName(), module);
	DIType* ret = Proto->GenerateDebugType();
	helper.dtypemap[resolved_type] = ret;
	return ret;
}

void Birdee::ASTBasicBlock::Generate()
{
	for (auto& stmt : body)
	{
		stmt->Generate();
	}
}

llvm::Value * Birdee::ThisExprAST::Generate()
{
	dinfo.emitLocation(this);
	return helper.cur_llvm_func->args().begin();
}

llvm::Value * Birdee::FunctionAST::Generate()
{
	DISubroutineType* functy=(DISubroutineType*)PreGenerate();

	auto dbginfo = PrepareFunctionDebugInfo(llvm_func, functy, Pos);

	if (!isDeclare)
	{
		auto IP = builder.saveIP();
		auto func_backup = helper.cur_llvm_func;
		BasicBlock *BB = BasicBlock::Create(context, "entry", llvm_func);
		builder.SetInsertPoint(BB);

		dinfo.emitLocation(this);
		int i = 0;
		auto itr = llvm_func->args().begin();
		if (Proto->cls)
			itr++;
		for (;itr!= llvm_func->args().end();itr++)
		{
			Proto->resolved_args[i]->PreGenerateForArgument(itr, i+1);
			i++;
		}

		dinfo.LexicalBlocks.push_back(dbginfo);
		Body.Generate();
		dinfo.LexicalBlocks.pop_back();
		builder.restoreIP(IP);
		helper.cur_llvm_func=func_backup;
	}
	return llvm_func;
}

llvm::Value * Birdee::IdentifierExprAST::Generate()
{
	return impl->Generate();
}

llvm::Value * Birdee::ResolvedFuncExprAST::Generate()
{
	dinfo.emitLocation(this);
	return def->llvm_func;
}

StructType* GetStringType()
{
	static StructType* cls_string = nullptr;
	if (cls_string)
		return cls_string;
	string str = "string";
	return cu.classmap.find(str)->second.get().llvm_type;
}

llvm::Value * Birdee::StringLiteralAST::Generate()
{
	static unordered_map<reference_wrapper<const string>, GlobalVariable*> stringpool;
	dinfo.emitLocation(this);
	auto itr = stringpool.find(Val);
	if (itr != stringpool.end())
	{
		return builder.CreateLoad(itr->second);
	}
	Constant * str = ConstantDataArray::getString(context, Val);
	GlobalVariable* vstr = new GlobalVariable(str->getType(), true, GlobalValue::PrivateLinkage, str);
	vstr->setAlignment(1);

	std::vector<Constant*> const_ptr_5_indices;
	ConstantInt* const_int64_6 = ConstantInt::get(context, APInt(64, 0));
	const_ptr_5_indices.push_back(const_int64_6);
	const_ptr_5_indices.push_back(const_int64_6);
	Constant* const_ptr_5 = ConstantExpr::getGetElementPtr(llvm::Type::getInt8PtrTy(context),vstr, const_ptr_5_indices);


	Constant * obj= llvm::ConstantStruct::get(GetStringType(),{
		const_ptr_5,
		ConstantInt::get(llvm::Type::getInt32Ty(context),APInt(32,Val.length(),true))
		});
	GlobalVariable* vobj = new GlobalVariable(GetStringType(), true, GlobalValue::PrivateLinkage, obj);
	stringpool[Val] = vobj;
	return builder.CreateLoad(vobj);
}

llvm::Value * Birdee::LocalVarExprAST::Generate()
{
	dinfo.emitLocation(this);
	return builder.CreateLoad(def->llvm_value);
}

llvm::Value * Birdee::AddressOfExprAST::Generate()
{
	dinfo.emitLocation(this);
	return builder.CreateBitOrPointerCast(expr->Generate(), llvm::Type::getInt8PtrTy(context));
}

llvm::Value * Birdee::VariableMultiDefAST::Generate()
{
	assert(0 && "Encounter VariableMultiDefAST generation");
	return nullptr;
}

Value * Birdee::NumberExprAST::Generate()
{
	dinfo.emitLocation(this);
	switch (Val.type)
	{
	case tok_int:
		return ConstantInt::get(context, APInt(32, (uint64_t)Val.v_int, true));
		break;
	case tok_long:
		return ConstantInt::get(context, APInt(64, Val.v_ulong, true));
		break;
	case tok_uint:
		return ConstantInt::get(context, APInt(32, (uint64_t)Val.v_uint, false));
		break;
	case tok_ulong:
		return ConstantInt::get(context, APInt(64, Val.v_ulong, false));
		break;
	case tok_float:
		return ConstantFP::get(context, APFloat((float)Val.v_double));
		break;
	case tok_double:
		return ConstantFP::get(context, APFloat(Val.v_double));
		break;
	default:
		assert(0 && "Error type of constant");
	}
	return nullptr;
}


Value * Birdee::ReturnAST::Generate()
{
	dinfo.emitLocation(this);
	Value* ret = Val->Generate();
	return builder.CreateRet(ret);
}