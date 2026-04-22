# Praia

Praia is a dynamically typed, interpreted programming language written in C++ from scratch (lexer, parser, interpreter). It has a pipe operator for chaining data transformations and comes with HTTP, JSON, YAML, regex, and async built in.

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

## Installing

```sh
git clone --recursive https://github.com/praia-lang/praia.git
cd praia
```

Install dependencies (optional - Praia builds without them, you just won't have HTTPS, SQLite, or REPL history):

```sh
# macOS
brew install openssl@3 readline sqlite

# Ubuntu / Debian
sudo apt install g++ make libssl-dev libreadline-dev libsqlite3-dev

# Fedora / RHEL
sudo dnf install gcc-c++ make openssl-devel readline-devel sqlite-devel
```

Build and install:

```sh
make
sudo make install
```

This installs `praia` and `sand` (the package manager) to `/usr/local/bin/`, with stdlib grains in `/usr/local/lib/praia/`. To customize:

```sh
sudo make install PREFIX=/usr              # /usr/bin/praia, /usr/lib/praia/
sudo make install LIBDIR=/opt/praia/lib    # custom lib path
```

Uninstall:

```sh
sudo make uninstall
```

## Building from source

If you just want to build and run locally without installing system-wide:

```sh
make
./praia                   # REPL
./praia script.praia      # run a file
./praia -v                # print version
```

### Optional dependencies

| Dependency | Enables | Required? |
|------------|---------|-----------|
| OpenSSL | HTTPS client (`http.get("https://...")`) | Optional |
| SQLite | `sqlite.open()` built-in | Optional |
| readline/libedit | REPL history and line editing | Optional |

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
* async/await with true parallelism, channels, futures.all/race
* Module system ("grains") with import/export
* Package manager ([sand](https://github.com/praia-lang/sand))
* File I/O, directories, copy/move (`sys` namespace)
* Bytecode VM (default, `--tree` for tree-walker fallback)
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
│   └── vm/                      # bytecode VM (default engine)
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

## Known limitations

Praia is still in active development. The language is generally functional, but some rough edges remain:

* **Async tasks are isolated (VM only)** - the bytecode VM deep-copies globals, arguments, upvalues, and instances for each `async` call. Tasks run in true parallel with no shared mutable state. The tree-walker (`--tree`) runs async tasks in parallel but shares environments, so concurrent mutation of globals/closures can race. Use the VM (default) for correct async isolation.
* **No native Windows support** - Praia uses POSIX APIs for sockets, terminal I/O, and environment variables. Works on macOS, Linux, and Windows via WSL. Should work on BSD systems but is untested.
* **No garbage collector** - memory is managed with reference counting (shared_ptr). Circular references will leak.

## Documentation

See [DOCUMENTATION.md](DOCUMENTATION.md) for the complete reference.

## License

MIT
