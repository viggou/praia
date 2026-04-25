# Native C++ Plugins

Praia supports loading native C++ modules at runtime via `loadNative()`. This lets you write performance-critical code or wrap C/C++ libraries without modifying Praia itself.

## Quick start

**1. Write a plugin** (`mymodule.cpp`):

```cpp
#include "praia_plugin.h"

extern "C" void praia_register(PraiaMap* module) {
    module->entries["double"] = Value(makeNative("mymodule.double", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("expected a number", 0);
            return Value(args[0].asInt() * 2);
        }));
}
```

**2. Build it:**

```sh
make plugin SRC=mymodule.cpp OUT=mymodule.dylib   # macOS
make plugin SRC=mymodule.cpp OUT=mymodule.so       # Linux
```

**3. Use it in Praia:**

```
let mod = loadNative("./mymodule")
print(mod.double(21))  // 42
```

## Plugin API

### Entry point

Every plugin must export a single C function:

```cpp
extern "C" void praia_register(PraiaMap* module);
```

This function receives a pointer to an empty `PraiaMap`. Populate its `entries` with native functions. The map is returned to the Praia caller.

### Creating functions

Use `makeNative(name, arity, fn)` to create native functions:

```cpp
module->entries["add"] = Value(makeNative("mymod.add", 2,
    [](const std::vector<Value>& args) -> Value {
        return Value(args[0].asNumber() + args[1].asNumber());
    }));
```

- `name` — display name for error messages
- `arity` — number of parameters, or `-1` for variadic
- `fn` — `std::function<Value(const std::vector<Value>&)>`

### The Value type

`Value` is a variant that holds any Praia value:

| Constructor | Praia type |
|------------|------------|
| `Value()` | nil |
| `Value(true)` | bool |
| `Value(int64_t(42))` | int |
| `Value(3.14)` | float |
| `Value(std::string("hi"))` | string |
| `Value(shared_ptr<PraiaArray>)` | array |
| `Value(shared_ptr<PraiaMap>)` | map |

Type checking and accessors:

```cpp
args[0].isString()    // type check
args[0].asString()    // returns const std::string&
args[0].isNumber()    // true for int or float
args[0].asNumber()    // returns double (converts int)
args[0].isInt()       // true only for int
args[0].asInt()       // returns int64_t
args[0].isArray()     // true for array
args[0].asArray()     // returns shared_ptr<PraiaArray>
args[0].isMap()       // true for map
args[0].asMap()       // returns shared_ptr<PraiaMap>
```

### Creating arrays and maps

Use `gcNew<T>()` to create GC-tracked containers:

```cpp
auto arr = gcNew<PraiaArray>();
arr->elements.push_back(Value(1));
arr->elements.push_back(Value(2));
return Value(arr);

auto map = gcNew<PraiaMap>();
map->entries["key"] = Value("value");
return Value(map);
```

Always use `gcNew` instead of `std::make_shared` - it registers the object with Praia's garbage collector.

### Error handling

Throw `RuntimeError` to report errors back to Praia:

```cpp
if (!args[0].isString())
    throw RuntimeError("myFunc() requires a string", 0);
```

The second argument is a line number hint (use `0` from plugins).

## Header

Include a single header:

```cpp
#include "praia_plugin.h"
```

This re-exports:
- `value.h` — `Value`, `PraiaArray`, `PraiaMap`, `RuntimeError`
- `gc_heap.h` — `gcNew<T>()`
- `builtins.h` — `makeNative()`

## Building

The Makefile provides a convenience target:

```sh
make plugin SRC=path/to/plugin.cpp OUT=path/to/plugin.dylib
```

Or build manually using `praia --include-path` to find the headers:

```sh
# macOS
g++ -std=c++17 -shared -fPIC -I$(praia --include-path) -undefined dynamic_lookup -o myplugin.dylib myplugin.cpp

# Linux
g++ -std=c++17 -shared -fPIC -I$(praia --include-path) -o myplugin.so myplugin.cpp
```

Plugins must be compiled with a C++ compiler (the plugin API uses C++ types). They can freely call C functions and wrap C libraries.

## Wrapping C libraries

Plugins are `.cpp` files, but can wrap any C library. For example, wrapping C's `strtoll` for base conversion:

```cpp
#include "praia_plugin.h"
#include <cstdlib>

extern "C" void praia_register(PraiaMap* module) {
    module->entries["parseBase"] = Value(makeNative("mymod.parseBase", 2,
        [](const std::vector<Value>& args) -> Value {
            auto& str = args[0].asString();
            int base = static_cast<int>(args[1].asInt());
            return Value(static_cast<int64_t>(strtoll(str.c_str(), nullptr, base)));
        }));
}
```

See [`examples/plugins/strutil.cpp`](examples/plugins/strutil.cpp) for a full example wrapping C standard library functions.

## Behavior

- **Extension auto-detection** — `loadNative("./mymod")` tries `.dylib` on macOS, `.so` on Linux
- **Caching** — loading the same path twice returns the cached module
- **Lifetime** — plugins are never unloaded; function pointers remain valid for the process lifetime
- **GC integration** — containers created with `gcNew` participate in Praia's garbage collector
- **Thread safety** — plugin code runs on the interpreter's thread; `GcHeap::current()` works correctly

## Example

- [`examples/plugins/mathext.cpp`](examples/plugins/mathext.cpp) — math functions (gcd, lcm, fibonacci, hypot, sum)
- [`examples/plugins/strutil.cpp`](examples/plugins/strutil.cpp) — wrapping C stdlib functions (isAlpha, isDigit, base conversion)
