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
    double floatValue = 0;
    int64_t intValue = 0;
    bool isInt = false;
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
    std::vector<std::string> argNames; // parallel to args; empty string = positional
};

// Parts alternate: string literal, expression, string literal, expression, ..., string literal
struct InterpolatedStringExpr : Expr {
    std::vector<ExprPtr> parts;
};

// lam{ params in body }
struct LambdaExpr : Expr {
    std::vector<std::string> params;
    std::vector<ExprPtr> defaults; // parallel to params, nullptr if no default
    std::string restParam;         // empty = no rest param
    std::vector<StmtPtr> body;
    bool isGenerator = false;
};

struct TernaryExpr : Expr {
    ExprPtr condition;
    ExprPtr thenExpr;
    ExprPtr elseExpr;
};

struct SpreadExpr : Expr {
    ExprPtr expr;
};

struct ArrayLiteralExpr : Expr {
    std::vector<ExprPtr> elements; // may contain SpreadExpr nodes
};

struct IndexExpr : Expr {
    ExprPtr object;
    ExprPtr index;
    bool isOptional = false;
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
    bool isOptional = false;
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

struct YieldExpr : Expr {
    ExprPtr value;  // nullptr = yield nil
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

// Destructuring pattern element
struct PatternEntry {
    std::string name;       // variable to bind
    std::string key;        // for maps: the key to extract (empty = same as name)
    bool isRest = false;    // ...rest
};

struct LetStmt : Stmt {
    // Simple: name is set, pattern is empty
    // Array destructuring: pattern has entries, isArrayPattern = true
    // Map destructuring: pattern has entries, isArrayPattern = false
    std::string name;
    std::vector<PatternEntry> pattern;
    bool isArrayPattern = false;
    ExprPtr initializer;
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

struct MatchStmt : Stmt {
    ExprPtr subject;
    struct CaseBranch {
        ExprPtr pattern; // nullptr = default (_)
        StmtPtr body;
    };
    std::vector<CaseBranch> cases;
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
// for ({key, value} in iterable) { body }  — map destructuring
struct ForInStmt : Stmt {
    std::string varName;                    // simple: "entry"
    std::vector<std::string> destructureKeys; // destructuring: ["key", "value"]
    ExprPtr iterable;
    StmtPtr body;
};

struct FuncStmt : Stmt {
    std::string name;
    std::vector<std::string> params;
    std::vector<ExprPtr> defaults; // parallel to params, nullptr if no default
    std::string restParam;         // empty = no rest param
    StmtPtr body;
    bool isGenerator = false;
};

struct ReturnStmt : Stmt {
    ExprPtr value; // nullptr for bare return
};

struct ClassMethod {
    std::string name;
    std::vector<std::string> params;
    std::vector<ExprPtr> defaults; // parallel to params, nullptr if no default
    std::string restParam;         // empty = no rest param
    std::vector<ExprPtr> decorators;
    std::vector<StmtPtr> body;
    int line = 0;
    bool isGenerator = false;
    bool isStatic = false;
};

struct ClassStmt : Stmt {
    std::string name;
    std::string superclass; // empty if no superclass
    std::vector<ClassMethod> methods;
};

// enum Name { A, B = 5, C }
struct EnumStmt : Stmt {
    std::string name;
    std::vector<std::string> members;
    std::vector<ExprPtr> values; // nullptr = auto-increment
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
