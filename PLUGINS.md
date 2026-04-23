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

Always use `gcNew` instead of `std::make_shared` — it registers the object with Praia's garbage collector.

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

Or build manually:

```sh
# macOS
g++ -std=c++17 -shared -fPIC -Isrc -o myplugin.dylib myplugin.cpp

# Linux
g++ -std=c++17 -shared -fPIC -Isrc -o myplugin.so myplugin.cpp
```

Plugins must be compiled with the same C++ compiler and standard library as Praia.

## Behavior

- **Extension auto-detection** — `loadNative("./mymod")` tries `.dylib` on macOS, `.so` on Linux
- **Caching** — loading the same path twice returns the cached module
- **Lifetime** — plugins are never unloaded; function pointers remain valid for the process lifetime
- **GC integration** — containers created with `gcNew` participate in Praia's garbage collector
- **Thread safety** — plugin code runs on the interpreter's thread; `GcHeap::current()` works correctly

## Example

See [`examples/plugins/mathext.cpp`](examples/plugins/mathext.cpp) for a complete example with `gcd`, `lcm`, `fibonacci`, `hypot`, and `sum`.
