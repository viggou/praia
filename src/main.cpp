#include "ast.h"
#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#ifdef HAVE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

// ── AST printer ──────────────────────────────────────────────

static void printIndent(int level) {
    for (int i = 0; i < level; i++) std::cout << "  ";
}

static void printExpr(const Expr* expr, int level);
static void printStmt(const Stmt* stmt, int level);

static void printExpr(const Expr* expr, int level) {
    if (!expr) { printIndent(level); std::cout << "<null>\n"; return; }

    if (auto* e = dynamic_cast<const NumberExpr*>(expr)) {
        printIndent(level); std::cout << "Number(" << e->value << ")\n";
    } else if (auto* e = dynamic_cast<const StringExpr*>(expr)) {
        printIndent(level); std::cout << "String(\"" << e->value << "\")\n";
    } else if (auto* e = dynamic_cast<const BoolExpr*>(expr)) {
        printIndent(level); std::cout << "Bool(" << (e->value ? "true" : "false") << ")\n";
    } else if (dynamic_cast<const NilExpr*>(expr)) {
        printIndent(level); std::cout << "Nil\n";
    } else if (auto* e = dynamic_cast<const IdentifierExpr*>(expr)) {
        printIndent(level); std::cout << "Ident(" << e->name << ")\n";
    } else if (auto* e = dynamic_cast<const UnaryExpr*>(expr)) {
        printIndent(level); std::cout << "Unary(" << tokenTypeToString(e->op) << ")\n";
        printExpr(e->operand.get(), level + 1);
    } else if (auto* e = dynamic_cast<const BinaryExpr*>(expr)) {
        printIndent(level); std::cout << "Binary(" << tokenTypeToString(e->op) << ")\n";
        printExpr(e->left.get(), level + 1);
        printExpr(e->right.get(), level + 1);
    } else if (auto* e = dynamic_cast<const PostfixExpr*>(expr)) {
        printIndent(level); std::cout << "Postfix(" << tokenTypeToString(e->op) << ")\n";
        printExpr(e->operand.get(), level + 1);
    } else if (auto* e = dynamic_cast<const AssignExpr*>(expr)) {
        printIndent(level); std::cout << "Assign(" << e->name << ")\n";
        printExpr(e->value.get(), level + 1);
    } else if (auto* e = dynamic_cast<const CallExpr*>(expr)) {
        printIndent(level); std::cout << "Call\n";
        printExpr(e->callee.get(), level + 1);
        for (const auto& arg : e->args)
            printExpr(arg.get(), level + 1);
    } else if (auto* e = dynamic_cast<const PipeExpr*>(expr)) {
        printIndent(level); std::cout << "Pipe\n";
        printExpr(e->left.get(), level + 1);
        printExpr(e->right.get(), level + 1);
    } else if (auto* e = dynamic_cast<const AsyncExpr*>(expr)) {
        printIndent(level); std::cout << "Async\n";
        printExpr(e->expr.get(), level + 1);
    } else if (auto* e = dynamic_cast<const AwaitExpr*>(expr)) {
        printIndent(level); std::cout << "Await\n";
        printExpr(e->expr.get(), level + 1);
    } else if (dynamic_cast<const ThisExpr*>(expr)) {
        printIndent(level); std::cout << "This\n";
    } else if (auto* e = dynamic_cast<const SuperExpr*>(expr)) {
        printIndent(level); std::cout << "Super." << e->method << "\n";
    } else if (auto* e = dynamic_cast<const InterpolatedStringExpr*>(expr)) {
        printIndent(level); std::cout << "InterpString\n";
        for (const auto& part : e->parts)
            printExpr(part.get(), level + 1);
    } else if (auto* e = dynamic_cast<const LambdaExpr*>(expr)) {
        printIndent(level); std::cout << "Lambda";
        if (!e->params.empty()) {
            std::cout << "(";
            for (size_t i = 0; i < e->params.size(); i++) {
                if (i > 0) std::cout << ", ";
                std::cout << e->params[i];
            }
            std::cout << ")";
        }
        std::cout << "\n";
        for (const auto& s : e->body)
            printStmt(s.get(), level + 1);
    } else if (auto* e = dynamic_cast<const ArrayLiteralExpr*>(expr)) {
        printIndent(level); std::cout << "ArrayLiteral\n";
        for (const auto& elem : e->elements)
            printExpr(elem.get(), level + 1);
    } else if (auto* e = dynamic_cast<const IndexExpr*>(expr)) {
        printIndent(level); std::cout << "Index\n";
        printExpr(e->object.get(), level + 1);
        printExpr(e->index.get(), level + 1);
    } else if (auto* e = dynamic_cast<const IndexAssignExpr*>(expr)) {
        printIndent(level); std::cout << "IndexAssign\n";
        printExpr(e->object.get(), level + 1);
        printExpr(e->index.get(), level + 1);
        printExpr(e->value.get(), level + 1);
    } else if (auto* e = dynamic_cast<const MapLiteralExpr*>(expr)) {
        printIndent(level); std::cout << "MapLiteral\n";
        for (size_t i = 0; i < e->keys.size(); i++) {
            printIndent(level + 1); std::cout << e->keys[i] << ":\n";
            printExpr(e->values[i].get(), level + 2);
        }
    } else if (auto* e = dynamic_cast<const DotExpr*>(expr)) {
        printIndent(level); std::cout << "Dot(." << e->field << ")\n";
        printExpr(e->object.get(), level + 1);
    } else if (auto* e = dynamic_cast<const DotAssignExpr*>(expr)) {
        printIndent(level); std::cout << "DotAssign(." << e->field << ")\n";
        printExpr(e->object.get(), level + 1);
        printExpr(e->value.get(), level + 1);
    }
}

static void printStmt(const Stmt* stmt, int level) {
    if (!stmt) { printIndent(level); std::cout << "<null>\n"; return; }

    if (auto* s = dynamic_cast<const ExprStmt*>(stmt)) {
        printIndent(level); std::cout << "ExprStmt\n";
        printExpr(s->expr.get(), level + 1);
    } else if (auto* s = dynamic_cast<const LetStmt*>(stmt)) {
        printIndent(level); std::cout << "Let(" << s->name << ")\n";
        if (s->initializer) printExpr(s->initializer.get(), level + 1);
    } else if (auto* s = dynamic_cast<const BlockStmt*>(stmt)) {
        printIndent(level); std::cout << "Block\n";
        for (const auto& child : s->statements)
            printStmt(child.get(), level + 1);
    } else if (auto* s = dynamic_cast<const IfStmt*>(stmt)) {
        printIndent(level); std::cout << "If\n";
        printIndent(level + 1); std::cout << "cond:\n";
        printExpr(s->condition.get(), level + 2);
        printIndent(level + 1); std::cout << "then:\n";
        printStmt(s->thenBranch.get(), level + 2);
        for (const auto& elif : s->elifBranches) {
            printIndent(level + 1); std::cout << "elif:\n";
            printExpr(elif.condition.get(), level + 2);
            printStmt(elif.body.get(), level + 2);
        }
        if (s->elseBranch) {
            printIndent(level + 1); std::cout << "else:\n";
            printStmt(s->elseBranch.get(), level + 2);
        }
    } else if (auto* s = dynamic_cast<const WhileStmt*>(stmt)) {
        printIndent(level); std::cout << "While\n";
        printIndent(level + 1); std::cout << "cond:\n";
        printExpr(s->condition.get(), level + 2);
        printIndent(level + 1); std::cout << "body:\n";
        printStmt(s->body.get(), level + 2);
    } else if (auto* s = dynamic_cast<const ForStmt*>(stmt)) {
        printIndent(level); std::cout << "For(" << s->varName << ")\n";
        printIndent(level + 1); std::cout << "from:\n";
        printExpr(s->start.get(), level + 2);
        printIndent(level + 1); std::cout << "to:\n";
        printExpr(s->end.get(), level + 2);
        printIndent(level + 1); std::cout << "body:\n";
        printStmt(s->body.get(), level + 2);
    } else if (auto* s = dynamic_cast<const ForInStmt*>(stmt)) {
        printIndent(level); std::cout << "ForIn(" << s->varName << ")\n";
        printIndent(level + 1); std::cout << "iterable:\n";
        printExpr(s->iterable.get(), level + 2);
        printIndent(level + 1); std::cout << "body:\n";
        printStmt(s->body.get(), level + 2);
    } else if (auto* s = dynamic_cast<const FuncStmt*>(stmt)) {
        printIndent(level); std::cout << "Func(" << s->name << ")";
        if (!s->params.empty()) {
            std::cout << " params:";
            for (const auto& p : s->params) std::cout << " " << p;
        }
        std::cout << "\n";
        printStmt(s->body.get(), level + 1);
    } else if (auto* s = dynamic_cast<const ClassStmt*>(stmt)) {
        printIndent(level); std::cout << "Class(" << s->name << ")";
        if (!s->superclass.empty()) std::cout << " extends " << s->superclass;
        std::cout << "\n";
        for (const auto& m : s->methods) {
            printIndent(level + 1); std::cout << "Method(" << m.name << ")";
            if (!m.params.empty()) {
                std::cout << " params:";
                for (const auto& p : m.params) std::cout << " " << p;
            }
            std::cout << "\n";
        }
    } else if (auto* s = dynamic_cast<const ReturnStmt*>(stmt)) {
        printIndent(level); std::cout << "Return\n";
        if (s->value) printExpr(s->value.get(), level + 1);
    } else if (dynamic_cast<const BreakStmt*>(stmt)) {
        printIndent(level); std::cout << "Break\n";
    } else if (dynamic_cast<const ContinueStmt*>(stmt)) {
        printIndent(level); std::cout << "Continue\n";
    } else if (auto* s = dynamic_cast<const ThrowStmt*>(stmt)) {
        printIndent(level); std::cout << "Throw\n";
        printExpr(s->value.get(), level + 1);
    } else if (auto* s = dynamic_cast<const TryCatchStmt*>(stmt)) {
        printIndent(level); std::cout << "TryCatch(" << s->errorVar << ")\n";
        printIndent(level + 1); std::cout << "try:\n";
        printStmt(s->tryBody.get(), level + 2);
        printIndent(level + 1); std::cout << "catch:\n";
        printStmt(s->catchBody.get(), level + 2);
    } else if (auto* s = dynamic_cast<const EnsureStmt*>(stmt)) {
        printIndent(level); std::cout << "Ensure\n";
        printIndent(level + 1); std::cout << "cond:\n";
        printExpr(s->condition.get(), level + 2);
        printIndent(level + 1); std::cout << "else:\n";
        printStmt(s->elseBody.get(), level + 2);
    } else if (auto* s = dynamic_cast<const UseStmt*>(stmt)) {
        printIndent(level); std::cout << "Use(\"" << s->path << "\") as " << s->alias << "\n";
    } else if (auto* s = dynamic_cast<const ExportStmt*>(stmt)) {
        printIndent(level); std::cout << "Export {";
        for (size_t i = 0; i < s->names.size(); i++) {
            if (i > 0) std::cout << ", ";
            std::cout << s->names[i];
        }
        std::cout << "}\n";
    }
}

static void printAst(const std::vector<StmtPtr>& program) {
    for (const auto& stmt : program)
        printStmt(stmt.get(), 0);
}

// ── Main ─────────────────────────────────────────────────────

static std::string readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file '" << path << "'" << std::endl;
        exit(1);
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// Lex + parse source into an AST. Returns empty vector on error.
static std::vector<StmtPtr> compile(const std::string& source,
                                     bool showTokens, bool showAst) {
    Lexer lexer(source);
    auto tokens = lexer.tokenize();

    if (showTokens) {
        for (const auto& token : tokens)
            std::cout << token << std::endl;
        return {};
    }

    if (lexer.hasError()) return {};

    Parser parser(tokens);
    auto program = parser.parse();

    if (parser.hasError()) return {};

    if (showAst) {
        printAst(program);
        return {};
    }

    return program;
}

// readline wrapper — returns empty optional on EOF
static std::optional<std::string> readLine(const char* prompt) {
#ifdef HAVE_READLINE
    char* input = readline(prompt);
    if (!input) return std::nullopt;
    std::string result(input);
    free(input);
    return result;
#else
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    return line;
#endif
}

static void addHistory([[maybe_unused]] const std::string& line) {
#ifdef HAVE_READLINE
    add_history(line.c_str());
#endif
}

static void repl(bool showTokens, bool showAst) {
    std::cout << "Praia REPL (type 'exit' to quit)" << std::endl;
    Interpreter interpreter;

    // Keep all ASTs alive so function bodies (raw pointers) remain valid
    std::vector<std::vector<StmtPtr>> astStore;

    for (;;) {
        auto input = readLine(">> ");
        if (!input) break;              // Ctrl-D
        std::string line = *input;
        if (line == "exit") break;
        if (line.empty()) continue;

        // Multi-line: keep reading while braces are unbalanced
        int braces = 0;
        for (char c : line) {
            if (c == '{') braces++;
            if (c == '}') braces--;
        }
        while (braces > 0) {
            auto cont = readLine(".. ");
            if (!cont) break;
            line += "\n" + *cont;
            for (char c : *cont) {
                if (c == '{') braces++;
                if (c == '}') braces--;
            }
        }

        addHistory(line);

        auto program = compile(line, showTokens, showAst);
        if (!program.empty()) {
            interpreter.interpretRepl(program);
            astStore.push_back(std::move(program));
        }
    }
}

int main(int argc, char* argv[]) {
    bool showTokens = false;
    bool showAst = false;
    std::string filename;
    int fileArgIndex = -1;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--tokens")        showTokens = true;
        else if (arg == "--ast")      showAst = true;
        else if (filename.empty()) {  filename = arg; fileArgIndex = i; break; }
    }

    if (filename.empty()) {
        try { repl(showTokens, showAst); }
        catch (const ExitSignal& e) { return e.code; }
        return 0;
    }

    // Collect script arguments (everything after the filename)
    std::vector<std::string> scriptArgs;
    for (int i = fileArgIndex + 1; i < argc; i++)
        scriptArgs.push_back(argv[i]);

    std::string source = readFile(filename);
    auto program = compile(source, showTokens, showAst);
    if (!program.empty()) {
        Interpreter interpreter;
        interpreter.setArgs(scriptArgs);
        interpreter.setCurrentFile(filename);
        try { interpreter.interpret(program); }
        catch (const ExitSignal& e) { return e.code; }
    }
    return 0;
}
