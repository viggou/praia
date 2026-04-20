# Praia

Praia is a dynamically typed, interpreted programming language written in C++ from scratch (lexer, parser, interpreter — no external dependencies). It has a pipe operator for chaining data transformations and comes with HTTP, JSON, YAML, regex, and async built in.

## Example

A web server:

```
let server = http.createServer(lam{ req in
    let name = "world"
    if (req.query.contains("name=")) {
        name = req.query.replace("name=", "")
    }
    return {status: 200, body: "Hello, %{name}!"}
})

server.listen(8080)
```

A data pipeline:

```
let adults = sys.read("users.json")
    |> json.parse
    |> filter(lam{ u in u.age >= 18 })
    |> map(lam{ u in u.name })
    |> sort
```

More examples in the [examples directory](examples/).

## Building

Requires a C++17 compiler. No external dependencies.

```sh
make
./praia                   # REPL
./praia script.praia      # run a file
```

## Testing

```sh
make test                 # run the test suite in tests/
./praia test              # same thing
./praia test path/to/dir  # point at a different directory
```

`praia test` discovers `test_*.praia` files in the given directory, runs each
with a fresh interpreter, and reports pass/fail counts. Test files use the
`testing` grain and end with `testing.done()`:

```
use "testing"

testing.test("addition", lam{ in
    testing.assertEqual(1 + 1, 2, nil)
})

testing.done()
```

See [tests/](tests/) for examples covering arithmetic, strings, collections,
classes, control flow, error handling, pipes/closures, and JSON.

## Current features

* Pipe operator (`|>`) with filter, map, each, sort, keys, values
* Lambdas (`lam{ x in x * 2 }`)
* Classes with inheritance, super, this
* Enums and integer types (64-bit)
* Error handling with try/catch/throw and `ensure` (early-exit guard)
* String interpolation (`"%{name} is %{age}"`), regex, 14 string methods
* Arrays, maps, destructuring, spread operator
* HTTP client and server
* JSON and YAML parse/stringify
* async/await with OS threads
* Module system ("grains") with import/export
* Package manager ([sand](https://github.com/praia-lang/sand))
* File I/O, directories, copy/move (`sys` namespace)
* Bytecode VM (`--vm` flag) for faster execution
* REPL with readline history and multi-line input

## Project structure

```
Praia/
├── src/                         # interpreter source
│   ├── token.h                  # token types
│   ├── lexer.h/cpp              # tokenizer
│   ├── ast.h                    # AST node types
│   ├── parser.h/cpp             # recursive descent parser
│   ├── value.h                  # runtime value types
│   ├── environment.h            # variable scoping
│   ├── grain_resolve.h          # grain/module resolution logic
│   ├── interpreter.h            # Interpreter class + Callable subtypes
│   ├── interpreter.cpp          # grain loading, execute(), evaluate()
│   ├── interpreter_setup.cpp    # constructor wiring up all builtins
│   ├── interpreter_callables.cpp # PraiaFunction/Lambda/Method/Class::call
│   ├── builtins.h               # shared helpers (makeNative, etc.)
│   ├── builtins_http.cpp        # http client + server
│   ├── builtins_json.cpp        # JSON parser + stringifier
│   ├── builtins_yaml.cpp        # YAML parser + stringifier
│   ├── builtins_methods.cpp     # string/array dot-methods
│   ├── main.cpp                 # entry point + REPL
│   └── vm/                      # bytecode VM (--vm flag)
│       ├── opcode.h             # ~60 opcodes
│       ├── chunk.h/cpp          # bytecode container + constant pool
│       ├── compiler.h/cpp       # AST -> bytecode compiler
│       ├── vm.h/cpp             # stack-based virtual machine
│       └── debug.h/cpp          # bytecode disassembler
├── grains/                      # standard library modules
├── examples/                    # example programs
├── tests/                       # test suite (run via `make test`)
├── Makefile
└── DOCUMENTATION.md             # full language reference
```

## Documentation

See [DOCUMENTATION.md](DOCUMENTATION.md) for the complete reference.

## License

MIT
