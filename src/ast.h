#pragma once

#include "token.h"
#include <memory>
#include <string>
#include <vector>

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

// --- Expressions ---

struct Expr {
    int line = 0;
    virtual ~Expr() = default;
};

struct NumberExpr : Expr {
    double value;
};

struct StringExpr : Expr {
    std::string value;
};

struct BoolExpr : Expr {
    bool value;
};

struct NilExpr : Expr {};

struct IdentifierExpr : Expr {
    std::string name;
};

struct UnaryExpr : Expr {
    TokenType op;
    ExprPtr operand;
};

struct BinaryExpr : Expr {
    ExprPtr left;
    TokenType op;
    ExprPtr right;
};

struct PostfixExpr : Expr {
    ExprPtr operand;
    TokenType op;
};

struct AssignExpr : Expr {
    std::string name;
    ExprPtr value;
};

struct CallExpr : Expr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
};

// Parts alternate: string literal, expression, string literal, expression, ..., string literal
struct InterpolatedStringExpr : Expr {
    std::vector<ExprPtr> parts;
};

// lam{ params in body }
struct LambdaExpr : Expr {
    std::vector<std::string> params;
    std::vector<StmtPtr> body; // statements inside the lambda
};

struct ArrayLiteralExpr : Expr {
    std::vector<ExprPtr> elements;
};

struct IndexExpr : Expr {
    ExprPtr object;
    ExprPtr index;
};

struct IndexAssignExpr : Expr {
    ExprPtr object;
    ExprPtr index;
    ExprPtr value;
};

struct MapLiteralExpr : Expr {
    std::vector<std::string> keys;
    std::vector<ExprPtr> values;
};

struct DotExpr : Expr {
    ExprPtr object;
    std::string field;
};

struct DotAssignExpr : Expr {
    ExprPtr object;
    std::string field;
    ExprPtr value;
};

// a |> f becomes f(a), a |> f(x) becomes f(a, x)
struct PipeExpr : Expr {
    ExprPtr left;
    ExprPtr right;  // function or call expression
};

struct AsyncExpr : Expr {
    ExprPtr expr;  // the call expression to run async
};

struct AwaitExpr : Expr {
    ExprPtr expr;  // the future to await
};

struct ThisExpr : Expr {};

struct SuperExpr : Expr {
    std::string method; // super.method
};

// --- Statements ---

struct Stmt {
    int line = 0;
    virtual ~Stmt() = default;
};

struct ExprStmt : Stmt {
    ExprPtr expr;
};

struct LetStmt : Stmt {
    std::string name;
    ExprPtr initializer; // nullptr when uninitialized (= nil)
};

struct BlockStmt : Stmt {
    std::vector<StmtPtr> statements;
};

struct IfStmt : Stmt {
    ExprPtr condition;
    StmtPtr thenBranch;

    struct ElifBranch {
        ExprPtr condition;
        StmtPtr body;
    };
    std::vector<ElifBranch> elifBranches;

    StmtPtr elseBranch; // nullptr if no else
};

struct WhileStmt : Stmt {
    ExprPtr condition;
    StmtPtr body;
};

// for (varName in start..end) { body }
struct ForStmt : Stmt {
    std::string varName;
    ExprPtr start;
    ExprPtr end;
    StmtPtr body;
};

// for (varName in iterable) { body }
struct ForInStmt : Stmt {
    std::string varName;
    ExprPtr iterable;
    StmtPtr body;
};

struct FuncStmt : Stmt {
    std::string name;
    std::vector<std::string> params;
    StmtPtr body;
};

struct ReturnStmt : Stmt {
    ExprPtr value; // nullptr for bare return
};

struct ClassMethod {
    std::string name;
    std::vector<std::string> params;
    std::vector<StmtPtr> body;
    int line = 0;
};

struct ClassStmt : Stmt {
    std::string name;
    std::string superclass; // empty if no superclass
    std::vector<ClassMethod> methods;
};

struct BreakStmt : Stmt {};
struct ContinueStmt : Stmt {};

struct ThrowStmt : Stmt {
    ExprPtr value;
};

// try { body } catch (varName) { handler }
struct TryCatchStmt : Stmt {
    StmtPtr tryBody;
    std::string errorVar;
    StmtPtr catchBody;
};

// ensure (condition) else { body }
struct EnsureStmt : Stmt {
    ExprPtr condition;
    StmtPtr elseBody;
};

// use "path"  — imports a grain, binds to variable named after the file
struct UseStmt : Stmt {
    std::string path;    // the string literal from use "path"
    std::string alias;   // derived variable name (e.g. "math" from "utils/math")
};

// export { name1, name2, ... }
struct ExportStmt : Stmt {
    std::vector<std::string> names;
};
