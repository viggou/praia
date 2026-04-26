#pragma once

#include "token.h"
#include <memory>
#include <string>
#include <variant>
#include <vector>

struct ExprNode;
struct StmtNode;
using ExprPtr = std::unique_ptr<ExprNode>;
using StmtPtr = std::unique_ptr<StmtNode>;

// --- Helper structs (not in any variant) ---

struct PatternEntry {
    std::string name;
    std::string key;
    bool isRest = false;
};

struct ElifBranch {
    ExprPtr condition;
    StmtPtr body;
};

struct CaseBranch {
    ExprPtr pattern; // nullptr = default (_)
    StmtPtr body;
};

struct ClassMethod {
    std::string name;
    std::vector<std::string> params;
    std::vector<ExprPtr> defaults;
    std::string restParam;
    std::vector<ExprPtr> decorators;
    std::vector<StmtPtr> body;
    int line = 0;
    int column = 0;
    bool isGenerator = false;
    bool isStatic = false;
};

// --- Expression types ---

struct NumberExpr {
    double floatValue = 0;
    int64_t intValue = 0;
    bool isInt = false;
};

struct StringExpr {
    std::string value;
};

struct BoolExpr {
    bool value;
};

struct NilExpr {};

struct IdentifierExpr {
    std::string name;
};

struct UnaryExpr {
    TokenType op;
    ExprPtr operand;
};

struct BinaryExpr {
    ExprPtr left;
    TokenType op;
    ExprPtr right;
};

struct PostfixExpr {
    ExprPtr operand;
    TokenType op;
};

struct AssignExpr {
    std::string name;
    ExprPtr value;
};

struct CallExpr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
    std::vector<std::string> argNames; // parallel to args; empty string = positional
};

// Parts alternate: string literal, expression, string literal, expression, ..., string literal
struct InterpolatedStringExpr {
    std::vector<ExprPtr> parts;
};

// lam{ params in body }
struct LambdaExpr {
    std::vector<std::string> params;
    std::vector<ExprPtr> defaults; // parallel to params, nullptr if no default
    std::string restParam;         // empty = no rest param
    std::vector<StmtPtr> body;
    bool isGenerator = false;
};

struct TernaryExpr {
    ExprPtr condition;
    ExprPtr thenExpr;
    ExprPtr elseExpr;
};

struct SpreadExpr {
    ExprPtr expr;
};

struct ArrayLiteralExpr {
    std::vector<ExprPtr> elements; // may contain SpreadExpr nodes
};

struct IndexExpr {
    ExprPtr object;
    ExprPtr index;
    bool isOptional = false;
};

struct IndexAssignExpr {
    ExprPtr object;
    ExprPtr index;
    ExprPtr value;
};

struct MapLiteralExpr {
    std::vector<std::string> keys;
    std::vector<ExprPtr> values;
};

struct DotExpr {
    ExprPtr object;
    std::string field;
    bool isOptional = false;
};

struct DotAssignExpr {
    ExprPtr object;
    std::string field;
    ExprPtr value;
};

// a |> f becomes f(a), a |> f(x) becomes f(a, x)
struct PipeExpr {
    ExprPtr left;
    ExprPtr right;  // function or call expression
};

struct AsyncExpr {
    ExprPtr expr;  // the call expression to run async
};

struct AwaitExpr {
    ExprPtr expr;  // the future to await
};

struct YieldExpr {
    ExprPtr value;  // nullptr = yield nil
};

struct ThisExpr {};

struct SuperExpr {
    std::string method; // super.method
};

// --- Statement types ---

struct ExprStmt {
    ExprPtr expr;
};

struct LetStmt {
    // Simple: name is set, pattern is empty
    // Array destructuring: pattern has entries, isArrayPattern = true
    // Map destructuring: pattern has entries, isArrayPattern = false
    std::string name;
    std::vector<PatternEntry> pattern;
    bool isArrayPattern = false;
    ExprPtr initializer;
};

struct BlockStmt {
    std::vector<StmtPtr> statements;
};

struct IfStmt {
    ExprPtr condition;
    StmtPtr thenBranch;
    std::vector<ElifBranch> elifBranches;
    StmtPtr elseBranch; // nullptr if no else
};

struct MatchStmt {
    ExprPtr subject;
    std::vector<CaseBranch> cases;
};

struct WhileStmt {
    ExprPtr condition;
    StmtPtr body;
};

// for (varName in start..end) { body }
struct ForStmt {
    std::string varName;
    ExprPtr start;
    ExprPtr end;
    StmtPtr body;
};

// for (varName in iterable) { body }
// for ({key, value} in iterable) { body }  — map destructuring
struct ForInStmt {
    std::string varName;                      // simple: "entry"
    std::vector<std::string> destructureKeys; // destructuring: ["key", "value"]
    ExprPtr iterable;
    StmtPtr body;
};

struct FuncStmt {
    std::string name;
    std::vector<std::string> params;
    std::vector<ExprPtr> defaults; // parallel to params, nullptr if no default
    std::string restParam;         // empty = no rest param
    StmtPtr body;
    bool isGenerator = false;
};

struct ReturnStmt {
    ExprPtr value; // nullptr for bare return
};

struct ClassStmt {
    std::string name;
    std::string superclass; // empty if no superclass
    std::vector<ClassMethod> methods;
};

// enum Name { A, B = 5, C }
struct EnumStmt {
    std::string name;
    std::vector<std::string> members;
    std::vector<ExprPtr> values; // nullptr = auto-increment
};

struct BreakStmt {};
struct ContinueStmt {};

struct ThrowStmt {
    ExprPtr value;
};

// try { body } catch (varName) { handler } finally { cleanup }
struct TryCatchStmt {
    StmtPtr tryBody;
    std::string errorVar;
    StmtPtr catchBody;
    StmtPtr finallyBody; // nullptr if no finally
};

// ensure (condition) else { body }
struct EnsureStmt {
    ExprPtr condition;
    StmtPtr elseBody;
};

// use "path"  — imports a grain, binds to variable named after the file
struct UseStmt {
    std::string path;    // the string literal from use "path"
    std::string alias;   // derived variable name (e.g. "math" from "utils/math")
};

// export { name1, name2, ... }
struct ExportStmt {
    std::vector<std::string> names;
};

// --- Wrapper nodes ---

struct ExprNode {
    int line = 0;
    int column = 0;

    using Data = std::variant<
        NumberExpr,
        StringExpr,
        BoolExpr,
        NilExpr,
        IdentifierExpr,
        UnaryExpr,
        BinaryExpr,
        PostfixExpr,
        AssignExpr,
        CallExpr,
        InterpolatedStringExpr,
        LambdaExpr,
        TernaryExpr,
        SpreadExpr,
        ArrayLiteralExpr,
        IndexExpr,
        IndexAssignExpr,
        MapLiteralExpr,
        DotExpr,
        DotAssignExpr,
        PipeExpr,
        AsyncExpr,
        AwaitExpr,
        YieldExpr,
        ThisExpr,
        SuperExpr
    >;

    Data data;
};

struct StmtNode {
    int line = 0;
    int column = 0;

    using Data = std::variant<
        ExprStmt,
        LetStmt,
        BlockStmt,
        IfStmt,
        MatchStmt,
        WhileStmt,
        ForStmt,
        ForInStmt,
        FuncStmt,
        ReturnStmt,
        ClassStmt,
        EnumStmt,
        BreakStmt,
        ContinueStmt,
        ThrowStmt,
        TryCatchStmt,
        EnsureStmt,
        UseStmt,
        ExportStmt
    >;

    Data data;
};

// --- Overloaded helper for std::visit ---

template<class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };

template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;
