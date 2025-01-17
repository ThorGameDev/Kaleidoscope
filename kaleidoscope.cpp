#include "./include/KaleidoscopeJIT.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/Reassociate.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Value.h>
#include <map>
#include <memory>
#include <stack>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;
using namespace llvm::orc;


enum DataType {
    type_UNDECIDED = 999,
    type_bool = 0,
    type_i8 = 1,
    type_i16 = 2,
    type_i32 = 3,
    type_i64 = 3,
    type_float = 4,
    type_double = 5,
};

//Lexer

// The lexer returns tokens [0-255] if it is an unknown character, otherwise
// one of these for known things. It returns tokens greater than 255 for
// multi-part operators
enum Token {
    // Operators CURRENTLY WRONG
    op_eq = 15677,
    op_or = 31868,
    op_neq = 15649,
    op_geq = 15678,
    op_leq = 15676,
    op_shl = 15420,
    op_shr = 15934,

    tok_eof = -1,

    //commands
    tok_def = -2,
    tok_extern = -3,

    //primary
    tok_identifier = -4,
    tok_number = -5,
    tok_true = -10,
    tok_false = -11,

    // control
    tok_if = -12,
    tok_else = -13,
    tok_for = -14,

    //operators
    tok_binary = -15,
    tok_unary = -16,

    // var definition
    tok_dtype = -17,
};

static int optok(std::string op) {
    return (op[1] << 8) + op[0];
}
static std::string tokop(int op) {
    std::string ret;
    ret += (char)(op);
    char second = op >> 8;
    if (second != 0) {
        ret += second;
    }
    return ret;
}

static std::string IdentifierStr; //Filled in if tok_identifier
static double NumVal;             //Filled in if tok_number
static int64_t INumVal;             //Filled in if tok_number
static DataType TokenDataType;
static std::vector<int> longops;

// gettok - Return the next token from the standard input.
static int gettok() {
    static char LastChar = ' ';

    //Skip any white space
    while (isspace(LastChar))
        LastChar = getchar();

    if (isalpha(LastChar)) {
        IdentifierStr = LastChar;
        while (isalnum((LastChar = getchar())))
            IdentifierStr += LastChar;

        if (IdentifierStr == "def")
            return tok_def;
        if (IdentifierStr == "extern")
            return tok_extern;
        if (IdentifierStr == "if")
            return tok_if;
        if (IdentifierStr == "else")
            return tok_else;
        if (IdentifierStr == "for")
            return tok_for;
        if (IdentifierStr == "binary")
            return tok_binary;
        if (IdentifierStr == "unary")
            return tok_unary;
        if (IdentifierStr == "double"){
            TokenDataType = type_double;
            return tok_dtype;
        }
        if (IdentifierStr == "float"){
            TokenDataType = type_float;
            return tok_dtype;
        }
        if (IdentifierStr == "bool"){
            TokenDataType = type_bool;
            return tok_dtype;
        }
        if (IdentifierStr == "i64"){
            TokenDataType = type_i64;
            return tok_dtype;
        }
        if (IdentifierStr == "i32"){
            TokenDataType = type_i32;
            return tok_dtype;
        }
        if (IdentifierStr == "i16"){
            TokenDataType = type_i16;
            return tok_dtype;
        }
        if (IdentifierStr == "i8"){
            TokenDataType = type_i8;
            return tok_dtype;
        }
        if (IdentifierStr == "true")
            return tok_true;
        if (IdentifierStr == "false")
            return tok_false;

        return tok_identifier;
    }

    if (isdigit(LastChar) || LastChar == '.') {
        std::string NumStr;
        std::string INumStr;
        bool isInt = true;
        do {
            if (LastChar == '.')
                isInt = false;
            if (isInt)
                INumStr += LastChar;
            NumStr += LastChar;
            LastChar = getchar();
        } while (isdigit(LastChar) || LastChar == '.');


        NumVal = strtod(NumStr.c_str(), 0);
        INumVal = strtol(INumStr.c_str(), 0, 10);
        if (LastChar == ':'){
            LastChar = getchar();
            std::string ExplicitType;
            do {
                ExplicitType += LastChar;
                LastChar = getchar();
            } 
            while(isdigit(LastChar) || LastChar == 'i' || LastChar == 'f' || LastChar == 'd');
            if (ExplicitType == "i64")
                TokenDataType = type_i64;
            if (ExplicitType == "i64")
                TokenDataType = type_i64;
            if (ExplicitType == "i32")
                TokenDataType = type_i32;
            if (ExplicitType == "i16")
                TokenDataType = type_i16;
            if (ExplicitType == "i8")
                TokenDataType = type_i8;
            if (ExplicitType == "d")
                TokenDataType = type_double;
            if (ExplicitType == "f")
                TokenDataType = type_float;
        }
        else {
            if (NumVal != INumVal)
                TokenDataType = type_i32;
            else
                TokenDataType = type_double;
        }
        return tok_number;
    }

    if (LastChar == '#') {
        do
            LastChar = getchar();
        while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

        if (LastChar != EOF)
            return gettok();
    }

    // Check for end of file
    if (LastChar == EOF)
        return tok_eof;

    // Otherwise, just return the character as its ascii value.
    char ThisChar = LastChar;
    LastChar = getchar();

    // But first, check to make sure it isnt a multipart operator
    int value = (LastChar << 8) + ThisChar;
    for (int val = 0; val < longops.size(); val++) {
        if (longops[val] == value) {
            LastChar = getchar();
            return value;
        }
    }

    return ThisChar;
}

//AST

/// ExprAST - Base class for all expression nodes
class ExprAST { //To add types other than doubles, this would have a type field
    DataType dtype;
public:
    virtual ~ExprAST() = default;
    virtual Value *codegen() = 0;
    const DataType &getDatatype() const {
        return dtype;
    };
protected:
    ExprAST(DataType dtype): dtype(dtype) {};
};


class LineAST : public ExprAST {
    std::unique_ptr<ExprAST> Body;
    bool returns;

public:
    LineAST(std::unique_ptr<ExprAST> Body, bool returns)
        : Body(std::move(Body)), returns(returns), ExprAST(Body->getDatatype()) {}
    Value *codegen() override;
    const bool &getReturns() const {
        return returns;
    }
};

/// DoubleExprAST - Expression class for numeric literals like "1.0".
class DoubleExprAST : public ExprAST {
    double Val;

public:
    DoubleExprAST(double Val) : Val(Val), ExprAST(type_double) {}
    Value *codegen() override;
};

/// FloatExprAST - Expression class for numeric literals like "1.0".
class FloatExprAST : public ExprAST {
    float Val;

public:
    FloatExprAST(double Val) : Val(Val), ExprAST(type_float) {}
    Value *codegen() override;
};

/// I64ExprAST - Expression class for 32 bit integers
class I64ExprAST : public ExprAST {
    int64_t Val;

public:
    I64ExprAST(int64_t Val) : Val(Val), ExprAST(type_i64) {}
    Value *codegen() override;
};

/// I32ExprAST - Expression class for 32 bit integers
class I32ExprAST : public ExprAST {
    int32_t Val;

public:
    I32ExprAST(int32_t Val) : Val(Val), ExprAST(type_i32) {}
    Value *codegen() override;
};

/// I16ExprAST - Expression class for 16 bit integers
class I16ExprAST : public ExprAST {
    int16_t Val;

public:
    I16ExprAST(int16_t Val) : Val(Val), ExprAST(type_i16) {}
    Value *codegen() override;
};

/// I8ExprAST - Expression class for 8 bit integers
class I8ExprAST : public ExprAST {
    int8_t Val;

public:
    I8ExprAST(int8_t Val) : Val(Val), ExprAST(type_i8) {}
    Value *codegen() override;
};

// BoolExprAST - Expression class for bools
class BoolExprAST : public ExprAST {
    bool Val;

public:
    BoolExprAST(bool Val) : Val(Val), ExprAST(type_bool) {}
    Value *codegen() override;
};

/// VariableExprAST - Expression class for referencing a variable, like "a"
class VariableExprAST : public ExprAST {
    std::string Name;

public:
    VariableExprAST(const std::string &Name, DataType dtype) : Name(Name), ExprAST(dtype) {}
    Value *codegen() override;
    const std::string &getName() const {
        return Name;
    }
};

/// BinaryExprAST - Expression class for a binary operator.
class BinaryExprAST : public ExprAST {
    int Op;
    std::unique_ptr<ExprAST> LHS, RHS;

public:
    BinaryExprAST(int Op, std::unique_ptr<ExprAST> LHS,
                  std::unique_ptr<ExprAST> RHS, DataType dtype)
        : Op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)), ExprAST(dtype) {}
    Value *codegen() override;
};

/// UnaryExprAST - Expression class for a binary operator.
class UnaryExprAST : public ExprAST {
    int Opcode;
    std::unique_ptr<ExprAST> Operand;

public:
    UnaryExprAST(int Opcode, std::unique_ptr<ExprAST> Operand)
        : Opcode(Opcode), Operand(std::move(Operand)), ExprAST(Operand->getDatatype() ) {}
    Value *codegen() override;
};

/// CallExprAST - Expression class for function calls.
// USES TEMPORARY DTYPE
class CallExprAST : public ExprAST {
    std::string Callee;
    std::vector<std::unique_ptr<ExprAST>> Args;

public:
    CallExprAST(const std::string &Callee,
                std::vector<std::unique_ptr<ExprAST>> Args)
        : Callee(Callee), Args(std::move(Args)), ExprAST(type_double) {}
    Value *codegen() override;
};

class BlockAST : public ExprAST {
    std::vector<std::unique_ptr<LineAST>> Lines;

public:
    std::vector<AllocaInst *> LocalVarAlloca;
    std::vector<std::pair<BasicBlock*, Value*>> ReturnFromPoints;
    std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames;
    BlockAST(std::vector<std::unique_ptr<LineAST>> Lines, DataType dtype)
        : Lines(std::move(Lines)), ExprAST(dtype) {}
    Value *codegen() override;
};

// USES TEMPORARY DTYPE
class IfExprAST :
    public ExprAST {
    std::unique_ptr<ExprAST> Cond;
    std::unique_ptr<LineAST> Then, Else;

public:
    IfExprAST(std::unique_ptr<ExprAST> Cond, std::unique_ptr<LineAST> Then,
              std::unique_ptr<LineAST> Else)
        : Cond(std::move(Cond)), Then(std::move(Then)), Else(std::move(Else)),
          ExprAST(type_double) {}
    Value *codegen() override;
};

// USES TEMPORARY DTYPE
class ForExprAST : public ExprAST {
    std::string VarName;
    std::unique_ptr<ExprAST> Start, End, Step, Body;

public:
    ForExprAST(const std::string &VarName, std::unique_ptr<ExprAST> Start,
               std::unique_ptr<ExprAST> End, std::unique_ptr<ExprAST> Step,
               std::unique_ptr<ExprAST> Body)
        : VarName(VarName), Start(std::move(Start)), End(std::move(End)),
          Step(std::move(Step)), Body(std::move(Body)), ExprAST(type_double) {}

    Value *codegen() override;
};

// USES TEMPORARY DTYPE
class VarExprAST : public ExprAST {
    std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames;

public:
    VarExprAST(std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames, DataType dtype)
        : VarNames(std::move(VarNames)), ExprAST(dtype) {}

    Value *codegen() override;
};

/// PrototypeAST - This class represents the "prototype" for a function,
/// which captures its name, and its argument names (thus implicitly the number
/// of arguments the function takes).
class PrototypeAST {
    std::string Name;
    DataType ReturnType;
    std::vector<std::pair<std::string, DataType>> Args;
    bool IsOperator;
    unsigned Precedence; //Precedence if a binary op.
    int OperatorName;

public:
    PrototypeAST(const std::string &Name, std::vector<std::pair<std::string, DataType>> Args,
                 DataType returnType, bool IsOperator = false, unsigned Prec = 0,
                 int OperatorName = 0)
        : Name(Name), Args(std::move(Args)), IsOperator(IsOperator),
          Precedence(Prec), OperatorName(OperatorName), ReturnType(returnType) {}

    Function *codegen();
    const std::string &getName() const {
        return Name;
    }

    bool isUnaryOp() const {
        return IsOperator && Args.size() == 1;
    }
    bool isBinaryOp() const {
        return IsOperator && Args.size() == 2;
    }

    int getOperatorName() const {
        assert(isUnaryOp() || isBinaryOp());
        return OperatorName;
    }

    unsigned getBinaryPrecedence() const {
        return Precedence;
    }
};

// FunctionAST - This class represents a function definition itself
class FunctionAST {
    std::unique_ptr<PrototypeAST> Proto;
    std::unique_ptr<ExprAST> Body;

public:
    FunctionAST(std::unique_ptr<PrototypeAST> Proto,
                std::unique_ptr<ExprAST> Body)
        : Proto(std::move(Proto)), Body(std::move(Body)) {}
    Function *codegen();
};

// Parser


/// CurTok/getNextToken - Provide a simple token buffer. CurTok is the current
/// token the parser is looking at. getNextToken reads another token from the
/// lexer and updates CurTok with its results.
static int CurTok;
static std::map<std::string, DataType> NamedValuesDatatype;
static std::map<std::pair<std::string, std::vector<DataType>>, DataType> FunctionDataTypes;

static int getNextToken() {
    return CurTok = gettok();
}

/// LogError* - These are little helper funcions for error handling.
std::unique_ptr<ExprAST> LogError(const char *Str) {
    fprintf(stderr, "Error: %s\n", Str);
    return nullptr;
}
std::unique_ptr<PrototypeAST> LogErrorP(const char *Str) {
    LogError(Str);
    return nullptr;
}

static std::unique_ptr<ExprAST> ParseExpression();
static std::unique_ptr<LineAST> ParseLine();

static std::unique_ptr<ExprAST> ParseNumberExpr() {
    if (TokenDataType == type_double){
        auto Result = std::make_unique<DoubleExprAST>(NumVal);
        getNextToken(); // consume the number
        return Result;
    } 
    if (TokenDataType == type_float){
        auto Result = std::make_unique<FloatExprAST>((float)NumVal);
        getNextToken(); // consume the number
        return Result;
    } 
    else if (TokenDataType == type_i64){
        auto Result = std::make_unique<I64ExprAST>(INumVal);
        getNextToken(); // consume the number
        return Result;
    }
    else if (TokenDataType == type_i32){
        auto Result = std::make_unique<I32ExprAST>((int32_t)INumVal);
        getNextToken(); // consume the number
        return Result;
    }
    else if (TokenDataType == type_i16){
        auto Result = std::make_unique<I16ExprAST>((int16_t)INumVal);
        getNextToken(); // consume the number
        return Result;
    }
    else if (TokenDataType == type_i8){
        auto Result = std::make_unique<I8ExprAST>((int8_t)INumVal);
        getNextToken(); // consume the number
        return Result;
    }

    return LogError("Invalid datatype");
}

static std::unique_ptr<ExprAST> ParseBoolExpr() {
    auto Result = std::make_unique<BoolExprAST>(CurTok == tok_true);
    getNextToken(); // Consume the truth statement
    return std::move(Result);
}

/// parenexpr ::= '(' expression ')'
static std::unique_ptr<ExprAST> ParseParenExpr() {
    getNextToken(); // eat (.
    auto V = ParseExpression();
    if (!V)
        return nullptr;
    if (CurTok != ')')
        return LogError("expected ')'");
    getNextToken(); //eat ).
    return V;
}

// USES PLACEHOLDER DTYPE
struct ParserBlockStackData {
    DataType blockDtype = type_UNDECIDED;
    std::map<std::string, DataType> outerVariables;
    std::vector<std::string> localVariables;
};
static std::stack<ParserBlockStackData*> ParseBlockStack;
static std::unique_ptr<ExprAST> ParseBlock() {
    getNextToken(); // Eat {
    std::vector<std::unique_ptr<LineAST>> lines;
    ParserBlockStackData data = ParserBlockStackData();
    ParseBlockStack.push(&data);
    while (CurTok != '}') {
        std::unique_ptr<LineAST> line = ParseLine();
        if(line->getReturns()) {
            if (data.blockDtype == type_UNDECIDED)
                data.blockDtype = line->getDatatype();
            else if (data.blockDtype != line->getDatatype())
                return LogError("Block can not have multiple return types");
        }
        lines.push_back(std::move(line));
    }
    ParseBlockStack.pop();
    getNextToken(); // Eat '}'
    // Remove all local variables from scope
    for (int i = 0; i < data.localVariables.size(); i++){
        if(data.outerVariables.count(data.localVariables[i]) != 0)
            NamedValuesDatatype[data.localVariables[i]] = data.outerVariables[data.localVariables[i]];
        else
            NamedValuesDatatype.erase(data.localVariables[i]);
    }
    if (data.blockDtype == type_UNDECIDED)
        data.blockDtype = type_double;
    return std::make_unique<BlockAST>(std::move(lines), data.blockDtype);
}

/// identifierexpr
/// ::=identifier
/// ::=identifier '(' expression* ')'
static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
    std::string IdName = IdentifierStr;

    getNextToken(); // eat identifier.
    if (CurTok != '('){ // Simple variable ref.
        if (NamedValuesDatatype.count(IdName) == 0){
            return LogError("Variable does not exist!");
        }
        return std::make_unique<VariableExprAST>(IdName, NamedValuesDatatype[IdName]);
    }

    // Call.
    getNextToken(); //eat (
    std::vector<std::unique_ptr<ExprAST>> Args;
    if (CurTok != ')') {
        while (true) {
            if (auto Arg = ParseExpression())
                Args.push_back(std::move(Arg));
            else
                return nullptr;

            if (CurTok == ')')
                break;

            if (CurTok != ',')
                return LogError("Expecetd ')' or ',' in argument list") ;
            getNextToken();
        }
    }

    // Eat the ')'
    getNextToken();

    std::vector<DataType> argsig;
    for (int i = 0; i < Args.size(); i++){
        argsig.push_back(Args[i]->getDatatype());
    }
    if (FunctionDataTypes.count(std::make_pair(IdName, argsig)) == 0){
        return LogError("No function of signature exists");
    }

    return std::make_unique<CallExprAST>(IdName, std::move(Args));
}

static std::unique_ptr<ExprAST> ParseIfExpr() {
    getNextToken(); // eat the if.

    if (CurTok != '(')
        return LogError("Expected '('");
    getNextToken(); // Eat the '('

    //condition.
    auto Cond = ParseExpression();
    if (!Cond)
        return nullptr;

    if (CurTok != ')')
        return LogError("Expected ')'");
    getNextToken(); // Eat the ')'

    std::unique_ptr<LineAST> Then = ParseLine();
    if (!Then)
        return nullptr;

    if(Then->getReturns() && !ParseBlockStack.empty()) {
        if (ParseBlockStack.top()->blockDtype == type_UNDECIDED)
            ParseBlockStack.top()->blockDtype = Then->getDatatype();
        else if (ParseBlockStack.top()->blockDtype != Then->getDatatype())
            return LogError("Block can not have multiple return types");
    }

    if (CurTok != tok_else)
        return LogError("Expected else");

    getNextToken();
    std::unique_ptr<LineAST> Else = ParseLine();
    if (!Else)
        return nullptr;

    if(Else->getReturns() && !ParseBlockStack.empty()) {
        if (ParseBlockStack.top()->blockDtype == type_UNDECIDED)
            ParseBlockStack.top()->blockDtype = Else->getDatatype();
        else if (ParseBlockStack.top()->blockDtype != Else->getDatatype())
            return LogError("Block can not have multiple return types");
    }

    return std::make_unique<IfExprAST>(std::move(Cond), std::move(Then), std::move(Else));
}

// forexper ::= 'for' identifier '=' expr ',' exper (',' expr)? 'in' expression
static std::unique_ptr<ExprAST> ParseForExpr() {
    getNextToken(); // eat the for.

    if (CurTok != '(')
        return LogError("Expected '('");
    getNextToken(); // Eat the '('

    if (CurTok != tok_identifier)
        return LogError("expected identifier after for");

    std::string IdName = IdentifierStr;
    getNextToken(); //eat identifier.

    if (CurTok != '=')
        return LogError("expected = after for");
    getNextToken(); // eat '='.

    auto Start = ParseExpression();
    if (!Start)
        return nullptr;
    if (CurTok != ';')
        return LogError("expected ';' after for start value");
    getNextToken();

    auto End = ParseExpression();
    if (!End)
        return nullptr;

    // The step value is optional.
    std::unique_ptr<ExprAST> Step;
    if (CurTok == ';') {
        getNextToken();
        Step = ParseExpression();
        if (!Step)
            return nullptr;
    }

    if (CurTok != ')')
        return LogError("Expected ')'");
    getNextToken(); // Eat the ')'

    std::unique_ptr<ExprAST> Body = ParseExpression();
    if (!Body)
        return nullptr;

    if (CurTok == ';')
        getNextToken();
    else
        return LogError("expected ';' at end of for loop");

    return std::make_unique<ForExprAST>(IdName, std::move(Start),
                                        std::move(End), std::move(Step),std::move(Body));
}

/// varexpr :: 'var' identifier ('=' expression)?
///                 (',' identifier ('=' expression)?)* 'in' expression
static std::unique_ptr<ExprAST> ParseVarExpr() {
    if (ParseBlockStack.size() == 0) {
        return LogError("Variable must be contained in a block");
    }

    DataType dtype = type_UNDECIDED;
    if (CurTok == tok_dtype)
        dtype = TokenDataType;
    else
        return LogError("Invalid datatype passed to 'ParseVarExpr()'");
    getNextToken(); // eat the var.

    std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames;

    // At least one variable name is required
    if (CurTok != tok_identifier)
        return LogError("expected identifier after var");

    while (true) {
        std::string Name = IdentifierStr;
        ParseBlockStack.top()->localVariables.push_back(Name);
        if (NamedValuesDatatype.count(Name) == 0){
            NamedValuesDatatype[Name] = dtype;
        }
        else {
            ParseBlockStack.top()->outerVariables[Name] = NamedValuesDatatype[Name];
        }
        getNextToken(); // eat identifier.

        // Read the optional initializer.
        std::unique_ptr<ExprAST> Init;
        if (CurTok == '=') {
            getNextToken(); // eat the '='.

            Init = ParseExpression();
            if (!Init) return nullptr;
        }

        VarNames.push_back(std::make_pair(Name, std::move(Init)));

        // End of var list, exit loop.
        if (CurTok != ',') break;
        getNextToken(); // eat the ','.

        if (CurTok != tok_identifier)
            return LogError("expected identifier list after var");
    }

    // Check and consume In omitted

    return std::make_unique<VarExprAST>(std::move(VarNames), dtype);
}

/// primary
///     ::= identifierexpr
///     ::= numberexpr
///     ::= parenexpr
///     ::= ifexpr
///     ::= forexpr
///     ::= varexpr
static std::unique_ptr<ExprAST> ParsePrimary() {
    switch(CurTok) {
    default:
        return LogError("Unknown token when expecting an expression");
    case tok_identifier:
        return ParseIdentifierExpr();
    case tok_number:
        return ParseNumberExpr();
    case tok_true:
    case tok_false:
        return ParseBoolExpr();
    case '{':
        return ParseBlock();
    case '(':
        return ParseParenExpr();
    case tok_if:
        return ParseIfExpr();
    case tok_for:
        return ParseForExpr();
    case tok_dtype:
        return ParseVarExpr();
    }
}

/// BinopProperties - This holds the precedence for each binary operator that is
/// defined.
struct BinopProperty{
    int Precedence;
    std::map<std::pair<DataType, DataType>, DataType> CompatibilityChart;
};
static std::map<int, BinopProperty> BinopProperties;

/// GetTokPrecedence - Get the precedence of the pending binary operator token.
static int GetTokPrecedence() {
    if (CurTok < 0)
        return -1;

    //Make sure it is a declared binop.
    if (BinopProperties.find(CurTok) == BinopProperties.end()) return -1;
    return BinopProperties[CurTok].Precedence;
}

/// unary
///     ::= primary
///     ::= '!' unary
static std::unique_ptr<ExprAST> ParseUnary() {
    // If the current token is not an operator, it must be a primary expr.
    if (CurTok < 0 || CurTok == '(' || CurTok == ',' || CurTok == '{')
        return ParsePrimary();

    // If this is a unary operator, read it.
    int Opc = CurTok;
    getNextToken();
    if (auto Operand = ParseUnary())
        return std::make_unique<UnaryExprAST>(Opc, std::move(Operand));
    return nullptr;
}

///binoprhs
///     ::= ('+' primary)*
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
        std::unique_ptr<ExprAST> LHS) {
    // If this is a binop, find its precedence.
    while (true) {
        int TokPrec = GetTokPrecedence();

        // If this is a binop that binds at least as tightly as the current binop,
        // consume it, otherwise we are done.
        if (TokPrec < ExprPrec)
            return LHS;


        // Okay, we know this is a binop.
        int BinOp = CurTok;
        getNextToken(); // eat binop

        //Parse the unary expression after the binary operator.
        auto RHS = ParseUnary();
        if (!RHS)
            return nullptr;

        // If BinOp binds less tightly with RHS than the operator after RHS, let
        // the pending operator take RHS as its LHS.
        int NextPrec = GetTokPrecedence();
        if (TokPrec < NextPrec) {
            RHS = ParseBinOpRHS(TokPrec+1, std::move(RHS));
            if(!RHS)
                return nullptr;
        }
        //Merge LHS/RHS.

        std::pair<DataType, DataType> OperationTyping = std::make_pair(LHS->getDatatype(), RHS->getDatatype());

        if(BinopProperties[BinOp].CompatibilityChart.count(OperationTyping) == 0) {
            return LogError("Can not perform operation between those types");
        }
        DataType returnType = BinopProperties[BinOp].CompatibilityChart[OperationTyping];

        LHS = std::make_unique<BinaryExprAST>(BinOp, std::move(LHS), std::move(RHS), returnType);
    }
}


static std::unique_ptr<ExprAST> ParseExpression() {
    auto LHS = ParseUnary();
    if (!LHS) {
        return nullptr;
    }

    return ParseBinOpRHS(0, std::move(LHS));
}

static std::unique_ptr<LineAST> ParseLine() {
    bool returns = true;
    // Prevent double semicolon possibility
    if (CurTok == tok_def || CurTok == tok_for || CurTok == tok_if || CurTok == tok_extern) {
        returns = false;
    }
    auto body = ParseExpression();

    if (CurTok == ';') {
        getNextToken(); // Eat ;
        returns = false;
    }
    return std::make_unique<LineAST>(std::move(body), returns);
}

/// prototype
/// ::= id '(' id* ')'
/// ::= binary LETTER number? (id, id)
/// ::= unary LETTER (id)
static std::unique_ptr<PrototypeAST> ParsePrototype() {
    std::string FnName;
    DataType ReturnType;
    
    if(CurTok == tok_dtype)
        ReturnType = TokenDataType;
    else 
        return LogErrorP("No type specified in prototype");
    getNextToken(); // Eat datatype

    unsigned Kind = 0; // 0 = identifier, 1 = unary, 2 = binary.
    unsigned BinaryPrecedence = 30;
    std::string FnSufix;
    int OperatorName;
    switch (CurTok) {
    default:
        return LogErrorP("Expected function name in prototype");
    case tok_identifier:
        FnName = IdentifierStr;
        Kind = 0;
        getNextToken();
        break;
    case tok_unary:
        getNextToken();
        if (CurTok < 0)
            return LogErrorP("Expected unary operator");
        FnName = "unary";
        FnSufix = (char)CurTok;
        Kind = 1;
        getNextToken();
        if (isascii(CurTok) && CurTok != '(') {
            FnSufix += (char)CurTok;
            longops.push_back(optok(FnSufix));
            getNextToken();
        }
        FnName += FnSufix;
        OperatorName = optok(FnSufix);

        break;
    case tok_binary:
        getNextToken();
        if (CurTok < 0)
            return LogErrorP("Expected binary operator");
        FnName = "binary";
        FnSufix = (char)CurTok;
        Kind = 2;
        getNextToken();
        if (isascii(CurTok) && CurTok != '(') {
            FnSufix += (char)CurTok;
            longops.push_back(optok(FnSufix));
            getNextToken();
        }
        FnName += FnSufix;
        OperatorName = optok(FnSufix);


        // Read the precedence if present.
        if (CurTok == tok_number) {
            if (INumVal < 1 || INumVal > 100)
                return LogErrorP("Invalid precedence: must be 1..100");
            BinaryPrecedence = (unsigned)INumVal;
            getNextToken();
        }
        break;
    }


    if (CurTok != '(')
        return LogErrorP("Expected '(' in prototype");
    getNextToken(); // Eat '('

    std::vector<std::pair<std::string, DataType>> Arguments;
    std::vector<DataType> argsig;
    while (true){
        DataType dtype;
        if (CurTok == tok_dtype)
            dtype = TokenDataType;
        else
            break;
        getNextToken(); // Eat datatype

        if (CurTok != tok_identifier){
            return LogErrorP("Expected name after variable datatype");
        }
        Arguments.push_back(std::make_pair(IdentifierStr, dtype));
        argsig.push_back(dtype);
        NamedValuesDatatype[IdentifierStr] = dtype;
        getNextToken(); // Eat name

        if (CurTok != ',')
            break;
        getNextToken();
    }
    if (CurTok != ')')
        return LogErrorP("Expected ')' in prototype");

    //success.
    getNextToken(); // eat ')'.

    // Verify right number of names for operator.
    if (Kind && Arguments.size() != Kind)
        return LogErrorP("Invalid number of operands for operator");

    FunctionDataTypes[std::make_pair(FnName, std::move(argsig))] = ReturnType;

    return std::make_unique<PrototypeAST>(FnName, std::move(Arguments), ReturnType, Kind != 0, BinaryPrecedence, OperatorName);
}

/// definition ::= 'def' prototype expression
static std::unique_ptr<FunctionAST> ParseDefinition() {
    getNextToken(); // eat def.
    auto Proto = ParsePrototype();
    if (!Proto) return nullptr;

    if (auto Body = ParseLine())
        return std::make_unique<FunctionAST>(std::move(Proto), std::move(Body));
    return nullptr;
}

/// external ::= 'extern' prototype
static std::unique_ptr<PrototypeAST> ParseExtern() {
    getNextToken(); // eat extern.
    std::unique_ptr<PrototypeAST> body = ParsePrototype();
    if (CurTok == ';')
        getNextToken();
    return std::move(body);
}

/// toplevelexpr ::= expression
static std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
    if (auto E = ParseLine()) {
        // Make an anonymous proto.
        auto Proto = std::make_unique<PrototypeAST>("__anon_expr", std::vector<std::pair<std::string, DataType>>(), type_double);
        return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
    }
    return nullptr;
}

// Code Generation


static std::unique_ptr<LLVMContext> TheContext;
static std::unique_ptr<IRBuilder<>> Builder;
static std::unique_ptr<Module> TheModule;
static std::map<std::string, AllocaInst*> NamedValues;
static std::unique_ptr<KaleidoscopeJIT> TheJIT;
static std::unique_ptr<FunctionPassManager> TheFPM;
static std::unique_ptr<LoopAnalysisManager> TheLAM;
static std::unique_ptr<FunctionAnalysisManager> TheFAM;
static std::unique_ptr<CGSCCAnalysisManager> TheCGAM;
static std::unique_ptr<ModuleAnalysisManager> TheMAM;
static std::unique_ptr<PassInstrumentationCallbacks> ThePIC;
static std::unique_ptr<StandardInstrumentations> TheSI;
static std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;
static ExitOnError ExitOnErr;

static AllocaInst* CreateEntryBlockAlloca(Function* TheFunction,
        StringRef VarName, Type* dtype) {
    IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                     TheFunction->getEntryBlock().begin());
    return TmpB.CreateAlloca(dtype, nullptr,
                             VarName);
}

Value *LogErrorV(const char *Str) {
    LogError(Str);
    return nullptr;
}

Function *getFunction(std::string Name) {
    // First, see if the function has already been added to the current module.
    if (auto *F = TheModule->getFunction(Name))
        return F;

    // If not, check whether we can codegen the declaration from some existing
    // prototype.
    auto FI = FunctionProtos.find(Name);
    if (FI != FunctionProtos.end())
        return FI->second->codegen();

    // If no existing prototype exists, return null.
    return nullptr;
}

Type *getType(DataType dtype){
    if (dtype == type_double){
        return Type::getDoubleTy(*TheContext);
    }
    if (dtype == type_float){
        return Type::getFloatTy(*TheContext);
    }
    else if (dtype == type_bool){
        return Type::getInt1Ty(*TheContext);
    }
    else if (dtype == type_i8){
        return Type::getInt8Ty(*TheContext);
    }
    else if (dtype == type_i16){
        return Type::getInt16Ty(*TheContext);
    }
    else if (dtype == type_i32){
        return Type::getInt32Ty(*TheContext);
    }
    else if (dtype == type_i64){
        return Type::getInt64Ty(*TheContext);
    }
    LogError("No type exists!");
    abort();
    return nullptr;
}

Value *DoubleExprAST::codegen() {
    return ConstantFP::get(*TheContext, APFloat(Val));
}

/// FIX AND TURN INTO FLOATING POINT INSTEAD OF DOUBLE.
Value *FloatExprAST::codegen() {
    return ConstantFP::get(*TheContext, APFloat(Val));
}

Value *I64ExprAST::codegen() {
    return ConstantInt::get(*TheContext, APInt(64, Val));
}

Value *I32ExprAST::codegen() {
    return ConstantInt::get(*TheContext, APInt(32, Val));
}

Value *I16ExprAST::codegen() {
    return ConstantInt::get(*TheContext, APInt(16, Val));
}

Value *I8ExprAST::codegen() {
    return ConstantInt::get(*TheContext, APInt(8, Val));
}

Value *BoolExprAST::codegen() {
    return ConstantInt::get(*TheContext, APInt(1, Val));
}

Value *VariableExprAST::codegen() {
    // Look this variable up in the funcion
    AllocaInst *A = NamedValues[Name];
    if (!A)
        LogErrorV("Unknown variable name");

    return Builder->CreateLoad(A->getAllocatedType(), A, Name.c_str());
}

namespace BinOps {
DataType priorities[] = {type_i64, type_double,type_i32, type_float,type_i16, type_i8, type_bool};

Value *toBool(Value* input) {
    return Builder->CreateFCmpONE(input,ConstantFP::get(*TheContext, APFloat(0.0)), "tobool");
}
Value* toDouble(Value* input) {
    return Builder->CreateUIToFP(input, Type::getDoubleTy(*TheContext), "tofloat");
};

bool isSigned(DataType dtype){
    if (dtype == type_bool) { return false; }
    else { return true; }
}

Value* expandDataType(Value* input, DataType target, DataType prior){
    if (prior == target){
        return input;
    }
    if (target == type_double){
        if(prior != type_float){
            if (isSigned(prior)){
                return Builder->CreateSIToFP(input, getType(target));
            }
            else {
                return Builder->CreateUIToFP(input, getType(target));
            }
        } 
        else {
            return Builder->CreateFPExt(input, getType(target));
        }
    }
    else if (target == type_float) {
        return Builder->CreateSIToFP(input, getType(target));
    }
    else {
        return Builder->CreateSExt(input, getType(target));
    }
};

std::pair<Value*, Value*> expandOperation(DataType LHS, DataType RHS, Value* L, Value* R){
    DataType biggerType;
    for(int i = 0; i < std::size(priorities); i++){
        if (LHS == priorities[i] || RHS == priorities[i]){
            biggerType = priorities[i];
            break;
        }
    }
    Value* LExt = expandDataType(L, biggerType, LHS);
    Value* RExt = expandDataType(R, biggerType, RHS);
    return std::make_pair(LExt, RExt);
};

/// 0: Or
/// 1: Xor
/// 2: And
Value* LogicGate(DataType LHS, DataType RHS, Value* L, Value* R, int gate) {
    std::pair<Value*, Value*> parts = expandOperation(LHS, RHS, L, R);

    if (gate == 0)
        return Builder->CreateOr(parts.first, parts.second, "ortmp");
    else if (gate == 1)
        return Builder->CreateXor(parts.first, parts.second, "xortmp");
    else if (gate == 2)
        return Builder->CreateAnd(parts.first, parts.second, "andtmp");

    return LogErrorV("In issue has occured wiht the logic gate.");
};

Value* EqualityCheck(DataType LHS, DataType RHS, Value* L, Value* R, int Op) {
    std::pair<Value*, Value*> parts = expandOperation(LHS, RHS, L, R);

    if (Op == '<')
        return Builder->CreateFCmpULT(parts.first, parts.second, "tlttmp");
    else if (Op == '>')
        return Builder->CreateFCmpUGT(parts.first, parts.second, "tgttmp");
    else if (Op == op_eq)
        return Builder->CreateFCmpUEQ(parts.first, parts.second, "teqtmp");
    else if (Op == op_geq)
        return Builder->CreateFCmpUGE(parts.first, parts.second, "tgetmp");
    else if (Op == op_leq)
        return Builder->CreateFCmpULE(parts.first, parts.second, "tletmp");
    else if (Op == op_neq)
        return Builder->CreateFCmpUNE(parts.first, parts.second, "tnetmp");


    return LogErrorV("Equality check did not exist");
};

Value* Add(DataType LHS, DataType RHS, Value* L, Value* R){
    std::pair<Value*, Value*> parts = expandOperation(LHS, RHS, L, R);

    if (LHS == type_double || LHS == type_float || RHS == type_double ||
            RHS == type_float){
        return Builder->CreateFAdd(parts.first, parts.second, "addtmp");
    }
    else{
        return Builder->CreateAdd(parts.first, parts.second, "addtmp");
    }
};
Value* Sub(DataType LHS, DataType RHS, Value* L, Value* R){
    std::pair<Value*, Value*> parts = expandOperation(LHS, RHS, L, R);

    if (LHS == type_double || LHS == type_float || RHS == type_double ||
            RHS == type_float){
        return Builder->CreateFSub(parts.first, parts.second, "subtmp");
    }
    else{
        return Builder->CreateSub(parts.first, parts.second, "subtmp");
    }
};
Value* Mul(DataType LHS, DataType RHS, Value* L, Value* R){
    std::pair<Value*, Value*> parts = expandOperation(LHS, RHS, L, R);

    if (LHS == type_double || LHS == type_float || RHS == type_double ||
            RHS == type_float){
        return Builder->CreateFMul(parts.first, parts.second, "multmp");
    }
    else{
        return Builder->CreateMul(parts.first, parts.second, "multmp");
    }
};
Value* Div(DataType LHS, DataType RHS, Value* L, Value* R){
    std::pair<Value*, Value*> parts = expandOperation(LHS, RHS, L, R);

    if (LHS == type_double || LHS == type_float || RHS == type_double ||
            RHS == type_float){
        return Builder->CreateFDiv(parts.first, parts.second, "divtmp");
    }
    else{
        return Builder->CreateSDiv(parts.first, parts.second, "divtmp");
    }
};

Value* Neg(DataType dtype, Value* input){
    if (dtype == type_float){
        return Builder->CreateFNeg(input, "negtmp");
    }
    else{
        return Builder->CreateNeg(input, "negtmp");
    }
};
}

Value *BinaryExprAST::codegen() {
    // Special case '=' because we don't want to emit the LHS as an expression.
    if (Op == '=') {
        // This assumes we're building without RTTI because LLVM builds that way by
        // default. If you build LLVM with RTTI this can be changed to a
        // dynamic_cast for automatic error checking.
        VariableExprAST *LHSE = static_cast<VariableExprAST *>(LHS.get());
        if (!LHSE)
            return LogErrorV("destination of '=' must be a variable");

        //Codegen the RHS.
        Value *Val = RHS->codegen();
        if (!Val)
            return nullptr;

        // Look up the name.
        Value *Variable = NamedValues[LHSE->getName()];
        if (!Variable)
            return LogErrorV("Unknown variable name");

        Builder->CreateStore(Val, Variable);
        return Val;
    }
    Value *L = LHS->codegen();
    Value *R = RHS->codegen();
    DataType LT = LHS->getDatatype();
    DataType RT = RHS->getDatatype();
    if(!L || !R)
        return nullptr;

    switch (Op) {
    case '+':
        return BinOps::Add(LT, RT, L, R);
    case '-':
        return BinOps::Sub(LT, RT, L, R);
    case '*':
        return BinOps::Mul(LT, RT, L, R);
    case '/':
        return BinOps::Div(LT, RT, L, R);
    case '<':
    case '>':
    case op_eq:
    case op_geq:
    case op_leq:
    case op_neq:
        return BinOps::EqualityCheck(LT, RT, L, R, Op);
    case '|':
        return BinOps::LogicGate(LT, RT, L, R, 1);
    case op_or:
        return BinOps::LogicGate(LT, RT, L, R, 0);
    case '&':
        return BinOps::LogicGate(LT, RT, L, R, 2);
    default:
        break;
    }
    // If it wasn't a builtin binary operator, it must be a user defined one. Emit
    // a call to it.
    Function *F = getFunction(std::string("binary") + tokop(Op));
    assert(F && "binary operator not found!");

    Value *Ops[2] = { L, R };
    return Builder->CreateCall(F, Ops, "binop");
}

Value *UnaryExprAST::codegen() {
    Value *OperandV = Operand->codegen();
    DataType DT = Operand->getDatatype();
    if (!OperandV)
        return nullptr;

    switch(Opcode) {
    case '-':
        return BinOps::Neg(DT, OperandV);
    case '!':
        OperandV = Builder->CreateFCmpONE(
                       OperandV, ConstantFP::get(*TheContext, APFloat(0.0)), "loopcond");
        OperandV = Builder->CreateNot(OperandV);
        return Builder->CreateUIToFP(OperandV, Type::getDoubleTy(*TheContext));
    default:
        break;
    }

    Function *F = getFunction(std::string("unary") + tokop(Opcode));
    if (!F)
        return LogErrorV("Unknown unary operator");

    return Builder->CreateCall(F, OperandV, "unop");
}

Value *CallExprAST::codegen() {
    //Look up the name in the global module table.
    Function *CalleeF = getFunction(Callee);
    if (!CalleeF)
        return LogErrorV("Unknown function refrenced");

    // If argument mismatch error.
    if (CalleeF->arg_size() != Args.size())
        return LogErrorV("Incorrect # arguments passed");

    std::vector<Value *> ArgsV;
    for (unsigned i = 0, e = Args.size(); i != e; i++) {
        ArgsV.push_back(Args[i]->codegen());
        if (!ArgsV.back())
            return nullptr;
    }

    return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
}

static std::stack<BlockAST*> BlockStack;
Value *BlockAST::codegen() {
    std::string name = "block" + std::to_string(BlockStack.size());
    // Add self to block stack, so that content code can access it
    BlockStack.push(this);

    // Create blocks
    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    BasicBlock *CurrentBlock = BasicBlock::Create(*TheContext, name, TheFunction);
    BasicBlock *AfterBB = BasicBlock::Create(*TheContext, name + "end");

    // Allow the flow to enter current block
    Builder->CreateBr(CurrentBlock);

    // Create a return value at every return point
    // Create block and fill with lines
    Builder->SetInsertPoint(CurrentBlock);

    Value *RetVal = Constant::getNullValue(Type::getDoubleTy(*TheContext));
    bool hasImmediateReturn = false;
    for (unsigned i = 0, e = Lines.size(); i != e; i++) {
        Value *Line = Lines[i]->codegen();
        if (!Line)
            return nullptr;
        if (Lines[i]->getReturns()) {
            RetVal = Line;
            hasImmediateReturn = true;
            break; // Do not generate unreachable code
        }
    }
    CurrentBlock = Builder->GetInsertBlock();
    // Pass return value to PHI node

    // Exit block
    TheFunction->insert(TheFunction->end(), AfterBB);
    Builder->CreateBr(AfterBB);
    Builder->SetInsertPoint(AfterBB);

    // Remove self from block stack
    BlockStack.pop();

    // Pop all local variables from scope.
    for (unsigned i = 0, e = VarNames.size(); i != e; i++)
        NamedValues[VarNames[i].first] = LocalVarAlloca[i];

    if (hasImmediateReturn || ReturnFromPoints.size() > 0){

        // Get the return type
        Type* retType = RetVal->getType();
        if (!hasImmediateReturn) {
            retType = ReturnFromPoints[0].second->getType();
            RetVal = Constant::getNullValue(ReturnFromPoints[0].second->getType());
        }

        // Create the PHI node to store return values
        PHINode *PN = Builder->CreatePHI(retType, ReturnFromPoints.size() + 1, "retval");

        // Create a return value at every return point
        for (int i = 0; i < ReturnFromPoints.size(); i++) {
            Builder->SetInsertPoint(ReturnFromPoints[i].first);
            PN->addIncoming(ReturnFromPoints[i].second, ReturnFromPoints[i].first);
            Builder->CreateBr(AfterBB);
        }
        Builder->SetInsertPoint(AfterBB);
        PN->addIncoming(RetVal, CurrentBlock);

        return PN;
    }
    return Constant::getNullValue( Type::getDoubleTy(*TheContext));
}

Value *LineAST::codegen() {
    Value *body = Body->codegen();
    if (returns == true) {
        return body;
    } else {
        return Constant::getNullValue(Type::getDoubleTy(*TheContext));
    }
}

// USES TEMPORARY DTYPE
Value *IfExprAST::codegen() {
    if (Cond->getDatatype() != type_bool) {
        return LogErrorV("If condition should be a boolean value!");
    }

    Value *CondV = Cond->codegen();
    if (!CondV)
        return nullptr;

    Function *TheFunction = Builder->GetInsertBlock()->getParent();

    // Create blocks for the then and else cases. Insert the then block at the
    // end of the function.
    BasicBlock *ThenBB =
        BasicBlock::Create(*TheContext, "then", TheFunction);
    BasicBlock *ElseBB = BasicBlock::Create(*TheContext, "else");
    BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "ifcont");

    Builder->CreateCondBr(CondV, ThenBB, ElseBB);

    // Emit then value.
    Builder->SetInsertPoint(ThenBB);
    Value *ThenV = Then->codegen();
    if (!ThenV)
        return nullptr;

    // Codegen of the 'Then' can change the current block, update ThenBB for the PHI.
    ThenBB = Builder->GetInsertBlock();

    // If "then" block does not have a semicolon, then if it is called, it should trigger a block return
    if (Then->getReturns() && BlockStack.size() > 0) {
        BlockStack.top()->ReturnFromPoints.push_back(std::pair<BasicBlock*, Value*>(ThenBB, ThenV));
    } else {
        Builder->CreateBr(MergeBB);
    }

    // Emit else block.
    TheFunction->insert(TheFunction->end(), ElseBB);
    Builder->SetInsertPoint(ElseBB);

    Value *ElseV = Else->codegen();
    if (!ElseV)
        return nullptr;

    // Codegen of 'Else' can change the current block, update ElseBB for the PHI.
    ElseBB = Builder->GetInsertBlock();

    // If "else" block does not have a semicolon, then if it is called, it should trigger a block return
    if (Else->getReturns() && BlockStack.size() > 0) {
        BlockStack.top()->ReturnFromPoints.push_back(std::pair<BasicBlock*, Value*>(ElseBB, ElseV));
    } else {
        Builder->CreateBr(MergeBB);
    }

    // Emit merge block.
    TheFunction->insert(TheFunction->end(), MergeBB);
    Builder->SetInsertPoint(MergeBB);
    return Constant::getNullValue(Type::getDoubleTy(*TheContext));
}

// USES TEMPORARY DTYPE
Value *ForExprAST::codegen() {
    if (End->getDatatype() != type_bool) {
        return LogErrorV("For loop condition should be bool type");
    }

    Function *TheFunction = Builder->GetInsertBlock()->getParent();

    //Create an alloca for the variable in the entry block.
    AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName, Type::getDoubleTy(*TheContext));

    // Emit the start code first, without 'variable' in scope.
    Value *StartVal = Start->codegen();
    if (!StartVal)
        return nullptr;

    //Store the value into the alloca.
    Builder->CreateStore(StartVal, Alloca);

    // Make the new basic block for the loop header, inserting after current block
    BasicBlock *LoopBB =
        BasicBlock::Create(*TheContext, "loop", TheFunction);


    // Insert an explicit fall through from the current block to the LoopBB.
    Builder->CreateBr(LoopBB);

    // Start insertion in LoopBB.
    Builder->SetInsertPoint(LoopBB);

    // Within the loop, the variable is defined equal to the PHI node. If it
    // shadows an existing variable, we have to restore it, so save it now.
    AllocaInst *OldVal = NamedValues[VarName];
    NamedValues[VarName] = Alloca;

    // Emit the body of the loop. This, like any other expr, can change the
    // current BB. Note that we ignore the value computed by the body, but don't
    // allow an error.
    Value* BodyV = Body->codegen();
    if (!BodyV)
        return nullptr;


    // Emit the step value.
    Value *StepVal = nullptr;
    if (Step) {
        StepVal = Step->codegen();
        if(!StepVal)
            return nullptr;
    } else {
        // If not specified, use 1.0
        StepVal = ConstantFP::get(*TheContext, APFloat(1.0));
    }

    // Compute the end condition
    Value *EndCond = End->codegen();
    if (!EndCond)
        return nullptr;

    // Reload, increment, and restore the alloca. This handles the case where
    // the body of the loop mutates the variable
    Value *CurVar =
        Builder->CreateLoad(Alloca->getAllocatedType(), Alloca, VarName.c_str());
    Value *NextVar = Builder->CreateFAdd(CurVar, StepVal, "nextvar");
    Builder->CreateStore(NextVar, Alloca);

    // Create the "after loop" block and insert it.
    BasicBlock *AfterBB =
        BasicBlock::Create(*TheContext, "afterloop", TheFunction);

    // Insert the conditional branch into the end of LoopEndBB.
    Builder->CreateCondBr(EndCond, LoopBB, AfterBB);

    // Any new code will be inserted in AfterBB.
    Builder->SetInsertPoint(AfterBB);

    //Restore the unshadowed variable.
    if (OldVal)
        NamedValues[VarName] = OldVal;
    else
        NamedValues.erase(VarName);

    // for expr always returns 0.0.
    return Constant::getNullValue(Type::getDoubleTy(*TheContext));
}

Value *VarExprAST::codegen() {
    std::vector<AllocaInst *> OldBindings;

    Function *TheFunction = Builder->GetInsertBlock()->getParent();

    // Register all variables and emit their initializer.
    for (unsigned i = 0, e = VarNames.size(); i != e; i++) {
        const std::string &VarName = VarNames[i].first;
        ExprAST *Init = VarNames[i].second.get();

        /// Emit the initializer before adding the variable to scope, this prevents
        /// the initializer from referencing the variable itself, and permits stuff
        /// like this:
        /// var a = 1 in
        /// var a = a in... # refers to outer 'a'.
        Value *InitVal;
        if (Init) {
            InitVal = Init->codegen();
            if (!InitVal)
                return nullptr;
        } else {
            // If not specified, use 0.0
            InitVal = ConstantFP::get(*TheContext, APFloat(0.0));
        }

        AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName, InitVal->getType());
        Builder->CreateStore(InitVal, Alloca);

        // Remember the old variable binding so that we can restore the binding when
        // we unrecurse.
        OldBindings.push_back(NamedValues[VarName]);

        // Remember this binding.
        NamedValues[VarName] = Alloca;
    }

    // Feed deepest level current local variables
    BlockStack.top()->LocalVarAlloca.insert(std::end(BlockStack.top()->LocalVarAlloca),
                                            std::begin(OldBindings), std::end(OldBindings));
    for (int i = VarNames.size() - 1; i >= 0; i--) {
        BlockStack.top()->VarNames.push_back(std::move(VarNames[i]));
    }

    // Return nothing
    return Constant::getNullValue(Type::getDoubleTy(*TheContext));
}

Function *PrototypeAST::codegen() {
    // Make the function type: double(double,double) etc.
    std::vector<Type*> TypeVector;
    for (int i = 0; i < Args.size(); i++){
        TypeVector.push_back(getType(Args[i].second));
    }

    FunctionType *FT =
        FunctionType::get(getType(ReturnType), TypeVector, false);

    Function *F =
        Function::Create(FT, Function::ExternalLinkage, Name, TheModule.get());

    // Set names for all arguments.
    unsigned Idx = 0;
    for (auto &Arg : F->args())
        Arg.setName(Args[Idx++].first);

    return F;
}

// USES TEMPORARY DTYPE
Function *FunctionAST::codegen() { // Might have an error, details are in the tutorial
    // Transfer ownership of the protype to the FunctionProtos map, but keep a
    // reference to it for use below.
    auto &P = *Proto;
    FunctionProtos[Proto->getName()] = std::move(Proto);
    Function *TheFunction = getFunction(P.getName());
    if (!TheFunction)
        return nullptr;

    // If this is an operator, install it.
    if (P.isBinaryOp())
        BinopProperties[P.getOperatorName()].Precedence = P.getBinaryPrecedence();

    // Create a new basic block to start insertion into.
    BasicBlock *BB = BasicBlock::Create(*TheContext, "entry", TheFunction);
    Builder->SetInsertPoint(BB);

    // Record the function arguments in the NamedValues map.
    NamedValues.clear();
    for (auto &Arg : TheFunction->args()) {
        // Create an alloca for this variable.
        AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, Arg.getName(), Arg.getType());

        // Store the initial value into the alloca.
        Builder->CreateStore(&Arg, Alloca);

        // Add arguments to variable symbol table.
        NamedValues[std::string(Arg.getName())] = Alloca;
    }

    if (Value *RetVal = Body->codegen()) {
        //Finish off the function.
        Builder->CreateRet(RetVal);

        //Validate the generated code, checking for consistency.
        verifyFunction(*TheFunction);

        //Optimize the function
        TheFPM->run(*TheFunction, *TheFAM);

        return TheFunction;
    }

    // Error reading body, remove function
    TheFunction->eraseFromParent();
    return nullptr;
}

// Top level parsing

static void InitializeBinopPrecedence() {
    // Install standard binary operators.
    // 1 is lowest precedence.
    BinopProperties['='] =
        {10, std::map<std::pair<DataType, DataType>, DataType>()};
    BinopProperties['|'] =
        {20, std::map<std::pair<DataType, DataType>, DataType>()}; 
    BinopProperties[optok("||")] = 
        {20, std::map<std::pair<DataType, DataType>, DataType>()};
    BinopProperties['&'] =
        {30, std::map<std::pair<DataType, DataType>, DataType>()};
    BinopProperties['>'] =
        {40, std::map<std::pair<DataType, DataType>, DataType>()};
    BinopProperties['<'] =
        {40, std::map<std::pair<DataType, DataType>, DataType>()};
    BinopProperties[optok("==")] = 
        {40, std::map<std::pair<DataType, DataType>, DataType>()};
    BinopProperties[optok("!=")] = 
        {40, std::map<std::pair<DataType, DataType>, DataType>()};
    BinopProperties[optok(">=")] =
        {40, std::map<std::pair<DataType, DataType>, DataType>()};
    BinopProperties[optok("<=")] =
        {40, std::map<std::pair<DataType, DataType>, DataType>()};
    BinopProperties['+'] =
        {50, std::map<std::pair<DataType, DataType>, DataType>()};
    BinopProperties['-'] =
        {50, std::map<std::pair<DataType, DataType>, DataType>()};
    BinopProperties['*'] =
        {60, std::map<std::pair<DataType, DataType>, DataType>()};
    BinopProperties['/'] =
        {60, std::map<std::pair<DataType, DataType>, DataType>()};

    for(int i = 0; i < std::size(BinOps::priorities); i++){
        DataType Bigger = BinOps::priorities[i];
        for (int j = i; j < std::size(BinOps::priorities); j++){
            DataType Smaller = BinOps::priorities[j];
            std::pair<DataType, DataType> pair1 = std::make_pair(Bigger, Smaller);
            std::pair<DataType, DataType> pair2 = std::make_pair(Smaller, Bigger);
            BinopProperties['='].CompatibilityChart[pair1] = Bigger;

            if(!(Bigger == type_double || Bigger == type_float 
                    || Smaller == type_double || Smaller == type_float)) {
                // If not a floating point operator, install operators
                BinopProperties['|'].CompatibilityChart[pair1] = Bigger;
                BinopProperties['|'].CompatibilityChart[pair2] = Bigger;
                BinopProperties[optok("||")].CompatibilityChart[pair1] = Bigger;
                BinopProperties[optok("||")].CompatibilityChart[pair2] = Bigger;
                BinopProperties['&'].CompatibilityChart[pair1] = Bigger;
                BinopProperties['&'].CompatibilityChart[pair2] = Bigger;
            } else {
                // Otherwise, skip all operations that will result in data loss.
                if (Smaller == type_double && j < i)
                    continue;
                else if (Smaller == type_float && j < i && Bigger != type_double)
                    continue;
            }

            BinopProperties[optok("==")].CompatibilityChart[pair1] = type_bool;
            BinopProperties[optok("==")].CompatibilityChart[pair2] = type_bool;
            BinopProperties[optok("!=")].CompatibilityChart[pair1] = type_bool;
            BinopProperties[optok("!=")].CompatibilityChart[pair2] = type_bool;
            BinopProperties['+'].CompatibilityChart[pair1] = Bigger;
            BinopProperties['+'].CompatibilityChart[pair2] = Bigger;
            BinopProperties['-'].CompatibilityChart[pair1] = Bigger;
            BinopProperties['-'].CompatibilityChart[pair2] = Bigger;
            BinopProperties['*'].CompatibilityChart[pair1] = Bigger;
            BinopProperties['*'].CompatibilityChart[pair2] = Bigger;
            BinopProperties['/'].CompatibilityChart[pair1] = Bigger;
            BinopProperties['/'].CompatibilityChart[pair2] = Bigger;
            // Do not check for bools.
            if (i != std::size(BinOps::priorities)) {
                BinopProperties['>'].CompatibilityChart[pair1] = type_bool;
                BinopProperties['>'].CompatibilityChart[pair2] = type_bool;
                BinopProperties['<'].CompatibilityChart[pair1] = type_bool;
                BinopProperties['<'].CompatibilityChart[pair2] = type_bool;
                BinopProperties[optok(">=")].CompatibilityChart[pair1] = type_bool;
                BinopProperties[optok(">=")].CompatibilityChart[pair2] = type_bool;
                BinopProperties[optok("<=")].CompatibilityChart[pair1] = type_bool;
                BinopProperties[optok("<=")].CompatibilityChart[pair2] = type_bool;
            }
        }
    }

    longops.push_back(optok("||"));
    longops.push_back(optok("=="));
    longops.push_back(optok("!="));
    longops.push_back(optok(">="));
    longops.push_back(optok("<="));
}

static void InitializeModuleAndManagers() {
    //Open a new context and module
    TheContext = std::make_unique<LLVMContext>();
    TheModule = std::make_unique<Module>("KaleidoscopeJIT", *TheContext);
    TheModule->setDataLayout(TheJIT->getDataLayout());

    //Create a builder for the module
    Builder = std::make_unique<IRBuilder<>>(*TheContext);

    // Create new pass and analysis manager
    TheFPM = std::make_unique<FunctionPassManager>();
    TheLAM = std::make_unique<LoopAnalysisManager>();
    TheFAM = std::make_unique<FunctionAnalysisManager>();
    TheCGAM = std::make_unique<CGSCCAnalysisManager>();
    TheMAM = std::make_unique<ModuleAnalysisManager>();
    ThePIC = std::make_unique<PassInstrumentationCallbacks>();
    TheSI = std::make_unique<StandardInstrumentations>(*TheContext,
            /*DebugLogging*/ true);
    TheSI->registerCallbacks(*ThePIC, TheMAM.get());

    // Add transform passes
    // Promote allocas to registers
    TheFPM->addPass(PromotePass());
    // Do simple peephole optimizations and bit twiddling optimizations.
    TheFPM->addPass(InstCombinePass());
    // reassociate expressions.
    TheFPM->addPass(ReassociatePass());
    // Eliminate Common SubExpressions.
    TheFPM->addPass(GVNPass());
    // Simplify the control flow graph (deleting unreachable blocks etc.)
    TheFPM->addPass(SimplifyCFGPass());

    // Register analysis passes used in these transform passes.
    PassBuilder PB;
    PB.registerModuleAnalyses(*TheMAM);
    PB.registerFunctionAnalyses(*TheFAM);
    PB.crossRegisterProxies(*TheLAM, *TheFAM, *TheCGAM, *TheMAM);
}

static void HandleDefinition() {
    if (auto FnAST = ParseDefinition()) {
        if (auto *FnIR = FnAST->codegen()) {
            fprintf(stderr, "Parsed a function definition.\n");
            FnIR->print(errs());
            fprintf(stderr, "\n");
            ExitOnErr(TheJIT->addModule(
                          ThreadSafeModule(std::move(TheModule), std::move(TheContext))));
            InitializeModuleAndManagers();
        }
    } else {
        // Skip token for error recovery.
        getNextToken();
    }
}

static void HandleExtern() {
    if (auto ProtoAST = ParseExtern()) {
        if (auto *FnIR = ProtoAST->codegen()) {
            fprintf(stderr, "Parsed an extern\n");
            FnIR->print(errs());
            fprintf(stderr, "\n");
            FunctionProtos[ProtoAST->getName()] = std::move(ProtoAST);
        }
    } else {
        // Skip token for error recovery
        getNextToken();
    }
}

static void HandleTopLevelExpression() {
    // Evaluate a top-level expression into an anonymous function.
    if (auto FnAST = ParseTopLevelExpr()) {
        if (FnAST->codegen()) {
            // Create a Resource Tracker to track JIT'd memory allocated to our
            // anonymous expression -- that way we can free it after executing.
            auto RT = TheJIT->getMainJITDylib().createResourceTracker();

            auto TSM = ThreadSafeModule(std::move(TheModule), std::move(TheContext));
            ExitOnErr(TheJIT->addModule(std::move(TSM), RT));
            InitializeModuleAndManagers();

            // Search the JIT for the __anon_expr symbol.
            auto ExprSymbol = ExitOnErr(TheJIT->lookup("__anon_expr"));
            //assert(ExprSymbol && "Function not found");

            // Get the symbol's address and cast it into the right type (takes no
            // arguments, returns a double) so we can call it as a native function.

            double (*FP)() = ExprSymbol.getAddress().toPtr<double (*)()>();
            bool (*TF)() = ExprSymbol.getAddress().toPtr<bool (*)()>();
            fprintf(stderr, "Evaluated to %f\n", FP());
            fprintf(stderr, "Evaluated to %d\n", TF());

            // Delete the anonymous expression module from the JIT
            ExitOnErr(RT->remove());

        }
    } else {
        // Skip token for error recovery
        getNextToken();
    }
}

/// top ::= definition | external | expression | ';'
static void MainLoop() {
    while (true) {
        fprintf(stderr, "\nready> ");
        switch (CurTok) {
        case tok_eof:
            return;
        case '_': //ignore placeholder exec-char
            getNextToken();
            break;
        case tok_def:
            HandleDefinition();
            break;
        case tok_extern:
            HandleExtern();
            break;
        default:
            HandleTopLevelExpression();
            break;
        }
    }
}

// "Library" functions that can be "extern'd" from user code.

#ifdef _WIN32
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

/// putchard - putchar that takes a double and returns 0.
extern "C" DLLEXPORT double putchard(double X) {
    fputc((char)X, stderr);
    return 0;
}

/// printd - printf that takes a double prints it as "%f\n", returning 0.
extern "C" DLLEXPORT double printd(double X) {
    fprintf(stderr, "%f\n", X);
    return 0;
}

int main() {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    InitializeBinopPrecedence();

    // Prime the first token.
    fprintf(stderr, "ready> ");
    getNextToken();

    TheJIT = ExitOnErr(KaleidoscopeJIT::Create());

    // Make the module, which holds all the code
    InitializeModuleAndManagers();

    // Run the main "interpreter loop" now.
    MainLoop();
    return 0;
}
