# Praia Language Documentation

Praia is a dynamically typed, interpreted programming language built in C++.

## Table of Contents

- [Getting Started](#getting-started)
- [Variables](#variables)
- [Destructuring](#destructuring)
- [Spread Operator](#spread-operator)
- [Data Types](#data-types)
- [Operators](#operators)
- [Strings](#strings)
- [Arrays](#arrays)
- [Maps](#maps)
- [Enums](#enums)
- [Control Flow](#control-flow)
- [Error Handling](#error-handling)
- [Loops](#loops)
- [Functions](#functions)
- [Lambdas](#lambdas)
- [Classes](#classes)
- [Built-in Functions](#built-in-functions)
- [String Methods](#string-methods)
- [Regex](#regex)
- [Array Methods](#array-methods)
- [The sys Namespace](#the-sys-namespace)
- [Pipe Operator](#pipe-operator)
- [JSON](#json)
- [YAML](#yaml)
- [Base64](#base64)
- [Path](#path)
- [URL](#url)
- [Concurrency](#concurrency)
- [Async / Await](#async--await)
- [HTTP Networking](#http-networking)
- [Router](#router)
- [Middleware](#middleware)
- [Logger](#logger)
- [Cookies](#cookies)
- [Sessions](#sessions)
- [SQLite](#sqlite)
- [Math](#math)
- [Random](#random)
- [Time](#time)
- [Bitwise Operators](#bitwise-operators)
- [Bytes](#bytes)
- [Crypto](#crypto)
- [TCP Sockets](#tcp-sockets)
- [Grains (Modules)](#grains-modules)
- [Comments](#comments)
- [Operator Precedence](#operator-precedence)
- [REPL](#repl)
- [Command-Line Usage](#command-line-usage)

---

## Getting Started

Build and run:

```sh
make
./praia script.praia          # run a file
./praia                       # start the REPL
```

Hello world:

```
print("Hello, World!")
```

---

## Variables

Declare variables with `let`. Uninitialized variables are `nil`.

```
let name = "Ada"
let age = 36
let score              // nil

age = 37               // reassignment
```

---

## Destructuring

Unpack arrays and maps into variables in a single `let` statement.

### Array destructuring

```
let [a, b, c] = [1, 2, 3]
print(a, b, c)              // 1 2 3

let [first, ...rest] = [1, 2, 3, 4, 5]
print(first)                 // 1
print(rest)                  // [2, 3, 4, 5]
```

Missing elements become `nil`:
```
let [x, y, z] = [1, 2]
print(z)                     // nil
```

### Map destructuring

```
let {name, age} = {name: "Ada", age: 36}
print(name, age)             // Ada 36
```

Rename with `key: varName`:
```
let {name: userName, age: userAge} = {name: "Ada", age: 36}
print(userName)              // Ada
```

Rest collects remaining keys:
```
let {name, ...other} = {name: "Ada", age: 36, lang: "Praia"}
print(other)                 // {age: 36, lang: "Praia"}
```

---

## Spread Operator

The `...` operator spreads arrays and maps into literals.

### Array spread

```
let a = [1, 2, 3]
let b = [4, 5, 6]
let combined = [...a, ...b]       // [1, 2, 3, 4, 5, 6]
let withExtra = [0, ...a, 99]     // [0, 1, 2, 3, 99]
```

### Map spread

```
let defaults = {host: "localhost", port: 8080}
let overrides = {port: 3000, debug: true}
let config = {...defaults, ...overrides}
// {host: "localhost", port: 3000, debug: true}
```

Later spreads override earlier keys (like `Object.assign` in JavaScript).

---

## Data Types

Praia has 7 types:

| Type | Examples | Notes |
|------|---------|-------|
| `nil` | `nil` | The absence of a value |
| `bool` | `true`, `false` | |
| `int` | `42`, `0`, `-7` | 64-bit integer (exact up to 2^63) |
| `number` | `3.14`, `0.5` | Double-precision float |
| `string` | `"hello"` | Supports interpolation and escape sequences |
| `array` | `[1, 2, 3]` | Ordered, mixed-type, reference semantics |
| `map` | `{name: "Ada"}` | String keys, reference semantics |
| `function` | `func add(a, b) { ... }` | First-class, supports closures |

### Truthiness

`nil`, `false`, and `0` are falsy. Everything else is truthy, including `""` and `[]`.

```
if (1)   { print("truthy") }      // prints
if (0)   { print("truthy") }      // does not print
if (nil) { print("truthy") }      // does not print
```

---

## Operators

### Arithmetic

```
2 + 3       // 5
10 - 4      // 6
3 * 7       // 21
15 / 4      // 3.75
17 % 5      // 2
[1,2] + [3,4]  // [1, 2, 3, 4]  (array concat)
```

### Comparison

```
3 < 5       // true
3 > 5       // false
3 <= 3      // true
3 >= 5      // false
```

### Equality

Works on any types. Arrays and maps compare by value.

```
1 == 1              // true
"hi" == "hi"        // true
[1, 2] == [1, 2]    // true
nil == nil           // true
1 == "1"             // false
```

### Logical

`&&` and `||` short-circuit and return the deciding value, not just `true`/`false`.

```
true && "yes"       // "yes"
false && "yes"      // false
nil || "default"    // "default"
true || "other"     // true
!true               // false
!nil                // true
```

### Ternary

```
let label = x > 5 ? "big" : "small"
let grade = score >= 90 ? "A" : score >= 80 ? "B" : "C"   // nests right-to-left
```

### Increment / Decrement

```
let i = 0
i++                 // i is now 1
i--                 // i is now 0
```

### String Concatenation

`+` concatenates when either side is a string:

```
"hello " + "world"  // "hello world"
"count: " + 42      // "count: 42"
```

---

## Strings

Strings are enclosed in double quotes.

### Escape Sequences

| Escape | Meaning |
|--------|---------|
| `\n` | Newline |
| `\t` | Tab |
| `\\` | Backslash |
| `\"` | Double quote |
| `\%` | Literal `%` (prevents interpolation) |

### String Interpolation

Use `%{expression}` inside strings:

```
let name = "Ada"
let age = 36
print("%{name} is %{age} years old")
// Ada is 36 years old

print("2 + 2 = %{2 + 2}")
// 2 + 2 = 4
```

### Multiline Strings

Triple-quoted strings (`"""` or `'''`) span multiple lines. The first newline after the opening quotes is stripped.

```
let html = """
<html>
  <body>
    <h1>%{title}</h1>
  </body>
</html>
"""
```

Interpolation and escape sequences work inside triple-quoted strings.

### String Indexing

```
let s = "hello"
print(s[0])         // h
print(s[-1])        // o (negative = from end)
```

---

## Arrays

Arrays are ordered, mixed-type collections with reference semantics.

```
let nums = [1, 2, 3]
let mixed = [1, "two", true, nil]
let empty = []
```

### Index Access and Assignment

```
let arr = [10, 20, 30]
print(arr[0])       // 10
print(arr[-1])      // 30

arr[1] = 99
print(arr)          // [10, 99, 30]
```

### Nested Arrays

```
let matrix = [[1, 2], [3, 4]]
print(matrix[1][0]) // 3
```

### Reference Semantics

```
let a = [1, 2, 3]
let b = a           // b points to the same array
b.push(4)
print(a)            // [1, 2, 3, 4]
```

---

## Maps

Maps hold key-value pairs with string keys. Keys can be identifiers or quoted strings.

```
let person = {name: "Ada", age: 36}
let config = {"api-key": "abc123"}
let empty = {}
```

### Access and Assignment

```
// Dot notation
print(person.name)          // Ada
person.email = "ada@ex.com"

// Bracket notation
print(person["name"])       // Ada
person["city"] = "London"
```

### Reference Semantics

Maps, like arrays, use reference semantics:

```
let a = {x: 1}
let b = a
b.y = 2
print(a)            // {x: 1, y: 2}
```

---

## Integers and Numbers

Praia has two numeric types: 64-bit integers (`int`) and double-precision floats (`number`). Integer literals (no decimal point) create ints; decimal literals create doubles.

```
type(42)        // "int"
type(3.14)      // "number"
```

### Arithmetic rules

- `int + int`, `int - int`, `int * int`, `int % int` → int
- Anything involving a double → double
- `/` always returns double: `7 / 2` → `3.5` (like Python 3)

```
42 + 8          // 50 (int)
42 + 0.5        // 42.5 (number)
7 / 2           // 3.5 (always double)
7 % 2           // 1 (int)
```

### Large integers

Integers are 64-bit, so they're exact up to 2^63. No precision loss like doubles above 2^53:

```
let big = 9007199254740993
print(big + 1)     // 9007199254740994 (exact)
```

### Comparison

Ints and doubles compare by value: `42 == 42.0` is `true`.

---

## Enums

Enums create named constants with auto-incrementing integer values.

```
enum Color { Red, Green, Blue }
print(Color.Red)      // 0
print(Color.Green)    // 1
print(Color.Blue)     // 2
```

### Custom values

```
enum Status { Active = 1, Inactive = 0, Pending = 2 }

if (status == Status.Active) {
    print("active")
}
```

### Auto-increment continues from the last value

```
enum Level { Low = 10, Medium, High }
print(Level.Medium)   // 11
print(Level.High)     // 12
```

Enums are maps — you can pass them around, iterate their keys, etc.

---

## Control Flow

### if / elif / else

Conditions are always wrapped in parentheses. Bodies use braces.

```
let score = 85

if (score >= 90) {
    print("A")
} elif (score >= 80) {
    print("B")
} elif (score >= 70) {
    print("C")
} else {
    print("F")
}
```

Truthiness check:

```
let name = nil
if (name) {
    print(name)
} else {
    print("no name set")
}
```

---

## Error Handling

### try / catch

Wrap code that might fail in a `try` block. If an error occurs — either from `throw` or a runtime error — execution jumps to the `catch` block with the error value.

```
try {
    let data = sys.read("config.txt")
    print(data)
} catch (err) {
    print("failed to read config:", err)
}
```

### throw

Throw any value as an error. If not caught by a `try/catch`, the program terminates.

```
func divide(a, b) {
    if (b == 0) {
        throw "division by zero"
    }
    return a / b
}

try {
    print(divide(10, 0))
} catch (err) {
    print("error:", err)     // error: division by zero
}
```

You can throw any value — strings, numbers, maps:

```
throw {code: 404, message: "not found"}
```

Runtime errors (type errors, index out of bounds, etc.) are also caught:

```
try {
    let arr = [1, 2, 3]
    print(arr[99])
} catch (err) {
    print(err)              // Array index out of bounds
}
```

### ensure

`ensure` is an early-exit guard (like Swift's `guard`). If the condition is falsy, the `else` block runs — which should exit the scope (typically `return` or `throw`).

```
func greet(name) {
    ensure (name) else {
        print("no name provided")
        return
    }
    print("hello %{name}!")
}

greet("Ada")    // hello Ada!
greet(nil)      // no name provided
```

`ensure` is useful for input validation at the top of functions:

```
func processAge(age) {
    ensure (type(age) == "number") else {
        throw "age must be a number"
    }
    ensure (age >= 0 && age <= 150) else {
        throw "age out of range"
    }
    print("valid age: %{age}")
}
```

---

## Loops

### while

```
let i = 0
while (i < 5) {
    print(i)
    i++
}
```

### for (range)

`for (var in start..end)` — end is exclusive.

```
for (i in 0..5) {
    print(i)            // 0, 1, 2, 3, 4
}

// Expressions work as bounds
let n = 10
for (i in 1..n + 1) {
    print(i)            // 1 through 10
}
```

### for-in (arrays)

```
let names = ["alice", "bob", "charlie"]
for (name in names) {
    print("hello %{name}")
}
```

### for-in (maps)

Iterating a map yields `{key, value}` entries:

```
let config = {host: "localhost", port: 8080}
for (entry in config) {
    print("%{entry.key}: %{entry.value}")
}
// host: localhost
// port: 8080
```

### break and continue

`break` exits the innermost loop. `continue` skips to the next iteration. Both work in `while`, `for`, and `for-in` loops.

```
// Skip odd numbers
for (i in 0..10) {
    if (i % 2 != 0) { continue }
    print(i)                        // 0, 2, 4, 6, 8
}

// Stop at first match
let names = ["alice", "bob", "charlie"]
for (name in names) {
    if (name == "bob") { break }
    print(name)                     // alice
}

// break in while
let n = 0
while (true) {
    if (n >= 3) { break }
    print(n)                        // 0, 1, 2
    n++
}
```

In nested loops, `break` and `continue` only affect the innermost loop.

---

## Functions

Define functions with `func`. Functions are first-class values.

```
func add(a, b) {
    return a + b
}

print(add(2, 3))    // 5
```

### Default parameters

Parameters can have default values. Non-default parameters must come before default ones.

```
func greet(name, greeting = "Hello") {
    print("%{greeting}, %{name}!")
}

greet("Ada")              // Hello, Ada!
greet("Ada", "Welcome")   // Welcome, Ada!
```

Defaults also work in lambdas and class methods:

```
let inc = lam{ x, step = 1 in x + step }
print(inc(10))       // 11
print(inc(10, 5))    // 15

class Server {
    init(port = 8080) {
        this.port = port
    }
}
```

### Implicit nil Return

Functions without an explicit `return` return `nil`.

```
func greet(name) {
    print("hello %{name}")
}
```

### Closures

Functions capture their enclosing scope:

```
func makeCounter() {
    let count = 0
    func increment() {
        count = count + 1
        return count
    }
    return increment
}

let counter = makeCounter()
print(counter())    // 1
print(counter())    // 2
print(counter())    // 3
```

### Recursion

```
func fib(n) {
    if (n <= 1) { return n }
    return fib(n - 1) + fib(n - 2)
}
print(fib(10))      // 55
```

### Functions as Values

```
func apply(f, x) {
    return f(x)
}

func double(n) { return n * 2 }

print(apply(double, 21))   // 42
```

---

## Lambdas

Lambdas are anonymous functions defined inline with `lam{ params in body }`.

### Single expression (auto-returned)

```
let double = lam{ x in x * 2 }
let add = lam{ a, b in a + b }

print(double(5))        // 10
print(add(3, 4))        // 7
```

A single-expression lambda automatically returns its result — no `return` needed.

### Multi-line (explicit return)

```
let process = lam{ x, y in
    let sum = x + y
    let product = x * y
    return {sum: sum, product: product}
}
```

### No parameters

```
let sayHi = lam{ in print("hello!") }
sayHi()
```

### Passing lambdas to functions

Lambdas are ideal for callbacks, filters, and transforms:

```
func filter(arr, predicate) {
    let result = []
    for (item in arr) {
        if (predicate(item)) { result.push(item) }
    }
    return result
}

func map(arr, transform) {
    let result = []
    for (item in arr) { result.push(transform(item)) }
    return result
}

let nums = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
let evens = filter(nums, lam{ n in n % 2 == 0 })
let squares = map(nums, lam{ n in n * n })

print(evens)            // [2, 4, 6, 8, 10]
print(squares)          // [1, 4, 9, 16, 25, 36, 49, 64, 81, 100]
```

### Closures

Lambdas capture their enclosing scope, just like named functions:

```
func makeMultiplier(factor) {
    return lam{ x in x * factor }
}

let triple = makeMultiplier(3)
print(triple(5))        // 15
```

### Lambdas in maps

```
let actions = {
    double: lam{ x in x * 2 },
    negate: lam{ x in -x }
}
print(actions.double(21))   // 42
```

---

## Classes

### Defining a class

```
class Animal {
    init(name, sound) {
        this.name = name
        this.sound = sound
    }

    speak() {
        print("%{this.name} says %{this.sound}")
    }
}
```

- `class` keyword defines a class
- `init` is the constructor (called automatically when creating instances)
- `this` refers to the current instance
- Methods are defined without `func`

### Creating instances

Call the class like a function — no `new` keyword:

```
let cat = Animal("Whiskers", "meow")
cat.speak()         // Whiskers says meow
```

### Properties

Properties are set on `this` inside methods and accessed with dot notation:

```
print(cat.name)     // Whiskers
cat.name = "Luna"   // reassignment works
cat.speak()         // Luna says meow
```

### Inheritance

Use `extends` for single inheritance:

```
class Dog extends Animal {
    init(name) {
        super.init(name, "woof")
        this.tricks = []
    }

    learn(trick) {
        this.tricks.push(trick)
    }
}

let buddy = Dog("Buddy")
buddy.speak()           // Buddy says woof (inherited)
buddy.learn("sit")
```

### super

Use `super.method()` to call the parent class's version of a method:

```
class Cat extends Animal {
    init(name) {
        super.init(name, "meow")
    }

    describe() {
        return "%{this.name} the cat"
    }
}
```

`super` works correctly with multi-level inheritance (e.g., Kitten -> Cat -> Animal).

### Method overriding

Child classes can override parent methods. The child's version is used:

```
class Animal {
    describe() { return "an animal" }
}

class Cat extends Animal {
    describe() { return "a cat" }
}

let c = Cat()
print(c.describe())    // a cat
c.speak()              // still works (inherited from Animal)
```

### Classes are values

Classes are first-class — they can be stored in variables and passed around:

```
let MyClass = Animal
let a = MyClass("Rex", "woof")
a.speak()
```

### Instance equality

Instances use reference equality:

```
let a = Animal("Rex", "woof")
let b = a
print(a == b)       // true (same reference)

let c = Animal("Rex", "woof")
print(a == c)       // false (different instances)
```

---

## Built-in Functions

| Function | Description |
|----------|-------------|
| `print(args...)` | Print values separated by spaces, followed by a newline |
| `len(value)` | Length of an array, string, or map |
| `push(array, value)` | Append a value to an array |
| `pop(array)` | Remove and return the last element of an array |
| `type(value)` | Return the type as a string: `"nil"`, `"bool"`, `"number"`, `"string"`, `"array"`, `"map"`, `"function"` |
| `str(value)` | Convert any value to a string |
| `num(value)` | Convert a string or number to a number |
| `filter(arr, fn)` | Keep elements where fn returns truthy |
| `map(arr, fn)` | Transform each element |
| `each(arr, fn)` | Call fn on each element, returns the array |
| `sort(arr)` | Return sorted copy (ascending) |
| `keys(map)` | Return array of map keys |
| `values(map)` | Return array of map values |

```
print(len([1, 2, 3]))      // 3
print(len("hello"))         // 5
print(len({a: 1, b: 2}))   // 2

print(type(42))             // number
print(type("hi"))           // string

print(str(42) + "!")        // 42!
print(num("3.14") * 2)     // 6.28
```

---

## Universal Methods

These work on any value type via dot notation.

| Method | Description |
|--------|-------------|
| `.toString()` | Convert any value to its string representation |
| `.toNum()` | Convert to number — works on numbers (identity), bools (`true`=1, `false`=0), and numeric strings. Also handles `"true"`/`"false"` (case-insensitive). Throws on invalid strings. |

```
42.toString()           // "42"
true.toString()         // "true"
[1, 2].toString()       // "[1, 2]"

true.toNum()            // 1
false.toNum()           // 0
"3.14".toNum()          // 3.14
"TRUE".toNum()          // 1
"hello".toNum()         // Error: Cannot convert 'hello' to number
```

---

## String Methods

Methods are called with dot notation on string values.

| Method | Description |
|--------|-------------|
| `.upper()` | Uppercase copy |
| `.lower()` | Lowercase copy |
| `.strip()` | Remove leading/trailing whitespace |
| `.split(sep)` | Split into array by separator |
| `.contains(sub)` | Check if substring exists |
| `.replace(old, new)` | Replace all occurrences |
| `.startsWith(prefix)` | Check prefix |
| `.endsWith(suffix)` | Check suffix |
| `.title()` | Capitalize first letter of each word, lowercase the rest |
| `.capitalize()` | Capitalize first letter, lowercase the rest |
| `.capitalizeFirst()` | Capitalize first letter, leave the rest intact |
| `.slice(start, end?)` | Extract substring (negative indices supported) |
| `.indexOf(substr, start?)` | Find first position (-1 if not found) |
| `.lastIndexOf(substr)` | Find last position (-1 if not found) |
| `.repeat(count)` | Repeat string N times |
| `.padStart(len, char?)` | Left-pad to width (default: space) |
| `.padEnd(len, char?)` | Right-pad to width (default: space) |
| `.trimStart()` | Remove leading whitespace |
| `.trimEnd()` | Remove trailing whitespace |

```
"hello".upper()                  // "HELLO"
"  hello  ".strip()              // "hello"
"a,b,c".split(",")              // ["a", "b", "c"]
"hello world".contains("world") // true
"hello".replace("l", "r")       // "herro"
"hello".startsWith("hel")      // true

// Casing variants
"how old is Ada?".title()           // "How Old Is Ada?"
"how old is Ada?".capitalize()      // "How old is ada?"
"how old is Ada?".capitalizeFirst() // "How old is Ada?"

// Chaining works
"  Hello World  ".strip().lower()   // "hello world"
```

---

## Regex

Regular expressions are available as string methods. Patterns use ECMAScript regex syntax.

| Method | Description |
|--------|-------------|
| `.test(pattern)` | Returns `true` if the pattern matches anywhere in the string |
| `.match(pattern)` | Returns a map with `match`, `groups`, and `index` for the first match, or `nil` |
| `.matchAll(pattern)` | Returns an array of match maps for all matches |
| `.replacePattern(pattern, replacement)` | Replaces all matches with the replacement string |

### test

```
"hello123".test("[0-9]+")       // true
"hello".test("[0-9]+")          // false
```

### match

Returns a map with the full match, capture groups, and position — or `nil` if no match:

```
let m = "age: 25".match("(\\w+): (\\d+)")
print(m.match)      // age: 25
print(m.groups)     // ["age", "25"]
print(m.index)      // 0

"hello".match("\\d+")   // nil
```

### matchAll

Returns an array of match maps:

```
let nums = "abc123def456".matchAll("\\d+")
for (m in nums) {
    print(m.match, "at", m.index)
}
// 123 at 3
// 456 at 9
```

### replacePattern

Replaces all regex matches. Supports back-references (`$1`, `$2`):

```
"hello   world".replacePattern("\\s+", " ")         // "hello world"
"John Smith".replacePattern("(\\w+) (\\w+)", "$2, $1")  // "Smith, John"
```

Use `.replace()` for literal string replacement, `.replacePattern()` for regex.

### Error handling

Invalid regex patterns throw a catchable error:

```
try {
    "test".test("[invalid")
} catch (err) {
    print(err)      // Invalid regex: ...
}
```

### Practical examples

```
// Email validation
let email = "ada@example.com"
if (email.test("^[\\w.+-]+@[\\w-]+\\.[\\w.]+$")) {
    print("valid email")
}

// Extract all words
let words = "Hello, World! 123".matchAll("[a-zA-Z]+")
for (w in words) { print(w.match) }

// Clean up whitespace
let clean = "  too   many   spaces  ".strip().replacePattern("\\s+", " ")
print(clean)    // "too many spaces"
```

---

## Array Methods

Methods are called with dot notation on array values.

| Method | Description |
|--------|-------------|
| `.push(value)` | Append an element |
| `.pop()` | Remove and return the last element |
| `.contains(value)` | Check if value is in the array |
| `.join(separator)` | Join elements into a string |
| `.reverse()` | Reverse the array in place |
| `.shift()` | Remove and return the first element |
| `.unshift(val)` | Add element to the beginning |
| `.slice(start, end?)` | Extract subarray (negative indices supported) |
| `.indexOf(val)` | Find index of element (-1 if not found) |
| `.find(fn)` | First element where fn returns truthy (nil if not found) |

```
let arr = [1, 2, 3]
arr.push(4)                     // [1, 2, 3, 4]
arr.pop()                       // returns 4
arr.contains(2)                 // true
["a", "b", "c"].join(", ")     // "a, b, c"
arr.reverse()                   // [3, 2, 1]
```

---

## The sys Namespace

`sys` is a built-in map providing OS-level operations.

### File I/O

```
// Write a file
sys.write("output.txt", "hello from praia")

// Read a file
let content = sys.read("output.txt")
print(content)          // hello from praia

// Append to a file
sys.append("output.txt", "\nmore text")
```

### File System

```
sys.mkdir("my/nested/dir")          // creates all parent dirs
print(sys.exists("output.txt"))     // true
sys.remove("output.txt")            // delete file
```

### Running Commands

```
let result = sys.exec("ls -la")
print(result)

let date = sys.exec("date")
print("Today is %{date}")
```

### Command-Line Arguments

Arguments passed after the script name are available in `sys.args`:

```sh
./praia script.praia hello world
```

```
// Inside script.praia:
print(sys.args)         // ["hello", "world"]
for (arg in sys.args) {
    print(arg)
}
```

### Exiting

```
sys.exit(0)             // exit with code 0
sys.exit(1)             // exit with code 1
```

### Reading from stdin

`sys.input(prompt?)` reads one line from standard input and returns it as a
string. The optional prompt is printed without a trailing newline before
reading. Returns `nil` on EOF (Ctrl-D or a closed pipe).

```
let name = sys.input("What's your name? ")
if (name == nil) { sys.exit(0) }        // user pressed Ctrl-D

let ans = sys.input("Continue? [y/N] ")
if (ans != nil && ans.lower() == "y") {
    print("Onwards.")
}
```

The line is returned without its trailing newline. Use `.strip()` if you
want to also drop leading/trailing whitespace.

---

## Pipe Operator

The pipe operator `|>` passes the left side as the first argument to the right side. It turns nested calls into readable top-to-bottom chains.

### Basic usage

```
// Without pipe: nested, reads inside-out
print(sort(filter(nums, lam{ n in n > 5 })))

// With pipe: linear, reads top-to-bottom
nums
    |> filter(lam{ n in n > 5 })
    |> sort
    |> print
```

`a |> f` becomes `f(a)`. `a |> f(x)` becomes `f(a, x)` — the left side is prepended as the first argument.

### Chaining

```
let result = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
    |> filter(lam{ n in n % 2 == 0 })
    |> map(lam{ n in n * n })
    |> sort
print(result)   // [4, 16, 36, 64, 100]
```

### With any function

```
func double(x) { return x * 2 }
func add10(x) { return x + 10 }

let val = 5 |> double |> add10 |> double
print(val)      // 40
```

### Data processing pipeline

```
let adults = sys.read("users.json")
    |> json.parse
    |> filter(lam{ u in u.age >= 18 })
    |> map(lam{ u in u.name })
    |> sort
```

### Functional built-ins

These functions are designed to work with `|>`:

| Function | Description |
|----------|-------------|
| `filter(arr, predicate)` | Keep elements where predicate returns truthy |
| `map(arr, transform)` | Transform each element |
| `each(arr, fn)` | Call fn on each element (side effects), returns the array |
| `sort(arr)` | Return sorted copy (numbers ascending, strings alphabetical) |
| `keys(map)` | Return array of map keys |
| `values(map)` | Return array of map values |

```
let config = {host: "localhost", port: 8080}
config |> keys |> print          // ["host", "port"]
config |> values |> print        // ["localhost", 8080]
```

---

## JSON

The `json` namespace provides fast built-in JSON parsing and serialization.

### json.parse(string)

Converts a JSON string into Praia values:

| JSON | Praia |
|------|-------|
| `{}` | map |
| `[]` | array |
| `"string"` | string |
| `123` | number |
| `true/false` | bool |
| `null` | nil |

```
let data = json.parse("{\"name\": \"Ada\", \"age\": 36}")
print(data.name)        // Ada
print(data.age)         // 36

let list = json.parse("[1, 2, 3]")
print(list)             // [1, 2, 3]
```

### json.stringify(value, indent?)

Converts a Praia value to a JSON string. Optional `indent` for pretty-printing.

```
let obj = {name: "Ada", scores: [100, 95]}

json.stringify(obj)         // {"name":"Ada","scores":[100,95]}
json.stringify(obj, 2)      // pretty-printed with 2-space indent
```

### Round-tripping

```
let original = {users: [{name: "Alice"}, {name: "Bob"}]}
let str = json.stringify(original)
let restored = json.parse(str)
print(restored.users[0].name)   // Alice
```

---

## YAML

The `yaml` namespace provides built-in YAML parsing and serialization.

### yaml.parse(string)

Parses a YAML string into Praia values. Supports mappings, sequences, nested structures, comments, flow sequences, and quoted strings.

```
let config = yaml.parse("host: localhost\nport: 8080\ndebug: true")
print(config.host)      // localhost
print(config.port)      // 8080
print(config.debug)     // true
```

Nested:

```
let yaml_str = "database:\n  host: localhost\n  port: 5432"
let conf = yaml.parse(yaml_str)
print(conf.database.host)   // localhost
```

Sequences:

```
let list = yaml.parse("- apple\n- banana\n- cherry")
print(list)                 // ["apple", "banana", "cherry"]
```

Flow sequences:

```
let data = yaml.parse("tags: [web, api, fast]")
print(data.tags)            // ["web", "api", "fast"]
```

Comments are stripped:

```
let data = yaml.parse("name: Ada  # the inventor")
print(data.name)            // Ada
```

### yaml.stringify(value)

Converts a Praia value to a YAML string.

```
let obj = {name: "Praia", features: ["fast", "simple"]}
print(yaml.stringify(obj))
// name: Praia
// features:
//   - fast
//   - simple
```

### Practical: reading config files

```
let config = yaml.parse(sys.read("config.yaml"))
print("Listening on port %{config.server.port}")
```

---

## Base64

| Function | Description |
|----------|-------------|
| `base64.encode(str)` | Encode string to base64 |
| `base64.decode(str)` | Decode base64 to string |

```
base64.encode("hello")         // "aGVsbG8="
base64.decode("aGVsbG8=")     // "hello"
```

---

## Path

Path manipulation using `<filesystem>`. All functions work with strings.

| Function | Description |
|----------|-------------|
| `path.join(parts...)` | Join path segments |
| `path.dirname(p)` | Parent directory |
| `path.basename(p)` | Filename component |
| `path.ext(p)` | File extension (including dot) |
| `path.resolve(p)` | Absolute path |

```
path.join("src", "main.cpp")       // "src/main.cpp"
path.dirname("/usr/local/bin/p")   // "/usr/local/bin"
path.basename("/usr/local/bin/p")  // "p"
path.ext("script.praia")          // ".praia"
path.resolve("src")               // "/full/path/to/src"
```

---

## URL

| Function | Description |
|----------|-------------|
| `url.parse(str)` | Parse URL into components |

```
let u = url.parse("https://example.com:8080/api?key=val")
print(u.scheme)    // "https"
print(u.host)      // "example.com"
print(u.port)      // 8080
print(u.path)      // "/api"
print(u.query)     // "key=val"
```

---

## Concurrency

### How the HTTP server works

The HTTP server is **single-threaded** — handlers run one at a time, serially. The next request is not accepted until the current handler returns. This means there are no race conditions by default.

However, if you use `async` inside a handler (e.g., parallel database calls), multiple async tasks may access shared state concurrently. Use `Lock()` to protect shared data.

### Lock

`Lock()` creates a mutex for thread-safe access to shared state.

```
let lock = Lock()
let counter = 0

// Manual acquire/release
lock.acquire()
counter = counter + 1
lock.release()

// withLock — auto-releases when the function returns (or throws)
lock.withLock(lam{ in
    counter = counter + 1
})
```

| Method | Description |
|--------|-------------|
| `lock.acquire()` | Acquire the lock (blocks if held by another thread) |
| `lock.release()` | Release the lock |
| `lock.withLock(fn)` | Acquire, call fn, release — even if fn throws. Returns fn's return value. |

**Always prefer `withLock`** — it's impossible to forget to release, and it handles errors correctly.

### Example: safe shared state

```
let lock = Lock()
let db = sqlite.open("app.db")

server.post("/increment", lam{ req, params in
    let result = lock.withLock(lam{ in
        let row = db.query("SELECT count FROM counters WHERE id = 1")
        let newCount = row[0].count + 1
        db.run("UPDATE counters SET count = ? WHERE id = 1", [newCount])
        return newCount
    })
    return http.json({count: result})
})
```

The lock is re-entrant (recursive) — the same thread can acquire it multiple times without deadlocking.

---

## Async / Await

`async` runs a function call in a background thread and returns a **future**. `await` blocks until the future has a result.

### Basic usage

```
// Start tasks in parallel
let f1 = async sys.exec("sleep 1 && echo done1")
let f2 = async sys.exec("sleep 1 && echo done2")
let f3 = async sys.exec("sleep 1 && echo done3")

// Wait for results
let r1 = await f1
let r2 = await f2
let r3 = await f3
// Total time: ~1 second (not 3)
```

### Parallel HTTP requests

```
let f1 = async http.get("http://api1.com/data")
let f2 = async http.get("http://api2.com/data")
let f3 = async http.get("http://api3.com/data")

let r1 = await f1
let r2 = await f2
let r3 = await f3
```

### Error handling

If an async task throws an error, `await` re-throws it:

```
let f = async http.get("http://invalid-host")
try {
    let r = await f
} catch (err) {
    print("request failed:", err)
}
```

### How it works

- `async funcCall(args)` evaluates the function and arguments on the current thread, then spawns the actual call in a new OS thread
- Returns a **future** value immediately
- `await future` blocks until the background thread finishes
- **Native functions** (http.get, sys.exec, sys.read, etc.) run in **true parallel** — they're pure C++ and don't need the interpreter
- **Praia functions** use a mutex (like Python's GIL) — only one runs at a time, but I/O operations still overlap

### Building on futures in grains

Futures are regular values, so you can build patterns on top:

```
// grains/parallel.praia
func all(futures) {
    let results = []
    for (f in futures) {
        results.push(await f)
    }
    return results
}

export { all }
```

```
use "parallel"
let results = parallel.all([
    async http.get("http://api1.com"),
    async http.get("http://api2.com")
])
```

---

## HTTP Networking

The `http` namespace provides an HTTP/HTTPS client and server. HTTPS requires OpenSSL (auto-detected at build time).

### HTTP Client

#### GET request

```
let res = http.get("http://example.com/api")
print(res.status)       // 200
print(res.body)         // response body string
print(res.headers)      // map of lowercase header names
```

#### POST request

```
// Simple string body
let res = http.post("http://example.com/api", "hello")

// With headers
let res = http.post("http://example.com/api", {
    body: "{\"name\": \"Ada\"}",
    headers: {"Content-Type": "application/json"}
})
```

#### General request

```
let res = http.request({
    method: "PUT",
    url: "http://example.com/api/1",
    body: "updated data",
    headers: {"Content-Type": "text/plain"}
})
```

#### Response format

All client methods return a map:

```
{
    status: 200,
    body: "...",
    headers: {"content-type": "text/html", ...}
}
```

Header names are lowercased for consistent access.

### HTTP Server

#### Creating a server

Pass a handler function to `http.createServer`. The handler receives a request map and returns a response map:

```
let server = http.createServer(lam{ req in
    if (req.path == "/") {
        return {
            status: 200,
            body: "<h1>Hello!</h1>",
            headers: {"Content-Type": "text/html"}
        }
    }
    return {status: 404, body: "Not Found"}
})

server.listen(8080)     // blocks, prints "Server listening on port 8080"
```

#### Request object

The handler receives a map with:

| Field | Description |
|-------|-------------|
| `method` | `"GET"`, `"POST"`, etc. |
| `path` | URL path (e.g. `"/hello"`) |
| `query` | Parsed query parameters as a map (e.g. `{name: "Ada", age: "36"}`) |
| `headers` | Map of lowercase header names |
| `body` | Request body string |

#### Response format

Return a map with:

| Field | Default | Description |
|-------|---------|-------------|
| `status` | `200` | HTTP status code |
| `body` | `""` | Response body |
| `headers` | `{"Content-Type": "text/plain"}` | Response headers |

You can also return a plain string — it becomes a 200 text/plain response.

#### Example: JSON API

```
let server = http.createServer(lam{ req in
    if (req.method == "GET" && req.path == "/api/time") {
        return {
            status: 200,
            body: "{\"time\": \"%{sys.exec("date")}\"}",
            headers: {"Content-Type": "application/json"}
        }
    }

    if (req.method == "POST" && req.path == "/api/echo") {
        return {
            status: 200,
            body: req.body,
            headers: {"Content-Type": req.headers["content-type"]}
        }
    }

    return {status: 404, body: "Not Found"}
})

server.listen(3000)
```

#### Error handling

If the handler throws an error, the server returns a 500 response and continues running.

#### Graceful shutdown

The server handles `SIGINT` (Ctrl-C) and `SIGTERM` (container stop) gracefully — it finishes the current request, closes the socket, and returns from `listen()`. Code after `listen()` runs normally:

```
server.listen(8080)
// This runs after Ctrl-C or SIGTERM:
print("Shutting down...")
db.close()
```

#### Server-Sent Events (SSE)

`http.sse(req, callback)` keeps the connection open for real-time streaming. The callback receives a `send` function.

```
server.get("/events", lam{ req, params in
    return http.sse(req, lam{ send in
        for (i in 0..10) {
            send(json.stringify({count: i}), "update")   // send(data, event?)
            time.sleep(1000)
        }
        send("done", "close")
    })
})
```

The client receives standard SSE format:

```
event: update
data: {"count":0}

event: update
data: {"count":1}
```

`send(data)` sends a plain `data:` message. `send(data, eventName)` adds an `event:` field. The connection closes when the callback returns.

**Browser client:**

```javascript
const events = new EventSource('/events');
events.addEventListener('update', e => {
    console.log(JSON.parse(e.data));
});
```

#### Query parameters

Query strings are automatically parsed into a map. Values are URL-decoded.

```
// Request: GET /search?q=hello+world&page=2

server.get("/search", lam{ req, params in
    print(req.query.q)      // "hello world"
    print(req.query.page)   // "2"
})
```

### Response Helpers

Instead of manually building response maps, use these helpers:

| Helper | Description |
|--------|-------------|
| `http.json(obj, status?)` | JSON response with `application/json` |
| `http.text(str, status?)` | Plain text response |
| `http.html(str, status?)` | HTML response with `charset=utf-8` |
| `http.redirect(url, status?)` | Redirect (302 by default) |
| `http.file(path, status?)` | Serve a file with auto-detected MIME type |

```
// Before
return {status: 200, body: json.stringify(data), headers: {"Content-Type": "application/json"}}

// After
return http.json(data)
return http.json({error: "not found"}, 404)
return http.text("hello")
return http.html("<h1>Hi</h1>")
return http.redirect("/login")
return http.redirect("/new-url", 301)    // permanent redirect
return http.file("public/style.css")     // auto-detects text/css
```

`http.file()` detects MIME types for: html, css, js, json, xml, txt, csv, svg, png, jpg, gif, ico, webp, woff, woff2, pdf, zip, mp3, mp4, wasm.

#### Static file serving

```
server.get("/static/:filename", lam{ req, params in
    return http.file("public/" + params.filename)
})
```

### URL Encoding

| Function | Description |
|----------|-------------|
| `http.encodeURI(str)` | Percent-encode a string (RFC 3986) |
| `http.decodeURI(str)` | Decode percent-encoded sequences |

```
http.encodeURI("hello world")       // "hello%20world"
http.encodeURI("a=1&b=2")          // "a%3D1%26b%3D2"
http.decodeURI("hello%20world")    // "hello world"
```

`encodeURI` leaves unreserved characters (`A-Z a-z 0-9 - _ . ~`) as-is and percent-encodes everything else. `decodeURI` reverses `%XX` sequences. Use these when building URLs with user input to prevent injection.

---

## Router

The `router` grain provides Express-style HTTP routing with path parameters.

### Setup

```
use "router"

let server = router.create()

server.get("/", lam{ req, params in
    return {status: 200, body: "Home"}
})

server.listen(8080)
```

### Path parameters

Use `:name` segments to capture parts of the URL. Captured values are passed as the second argument to the handler.

```
server.get("/users/:id", lam{ req, params in
    return {status: 200, body: "User %{params.id}"}
})

server.post("/api/game/:id/guess", lam{ req, params in
    let gameId = params.id
    let guess = json.parse(req.body)
    // ...
})

// Multiple params
server.get("/users/:userId/posts/:postId", lam{ req, params in
    print(params.userId, params.postId)
})
```

### HTTP methods

| Method | Description |
|--------|-------------|
| `server.get(path, handler)` | GET |
| `server.post(path, handler)` | POST |
| `server.put(path, handler)` | PUT |
| `server.delete(path, handler)` | DELETE |
| `server.patch(path, handler)` | PATCH |
| `server.options(path, handler)` | OPTIONS |
| `server.all(path, handler)` | Match any method |

### Custom 404

```
server.notFound(lam{ req, params in
    return {
        status: 404,
        body: json.stringify({error: "Not found", path: req.path}),
        headers: {"Content-Type": "application/json"}
    }
})
```

### Handler signature

All handlers receive two arguments: `(req, params)`.

- `req` — the request map with `method`, `path`, `query`, `headers`, `body`
- `params` — map of captured path parameters (empty `{}` for the 404 handler)

### Using without a server

Use `.handle(req)` to test routing without starting a server:

```
let result = server.handle({method: "GET", path: "/users/42", query: {}, body: ""})
print(result.body)      // "User 42"
```

---

## Middleware

The `middleware` grain provides common middleware functions for the router. Middleware runs on every request, in registration order.

### Using middleware

```
use "router"
use "middleware"

let server = router.create()
server.use(middleware.requestId())
server.use(middleware.cors())
server.use(middleware.jsonBody())
```

### How middleware works

Each middleware is a function that receives `(req, next)`. Call `next(req)` to pass the request to the next middleware (or the route handler). Return a response map to short-circuit.

```
// Custom middleware
server.use(lam{ req, next in
    let start = time.now()
    let res = next(req)      // call next middleware / handler
    let ms = time.now() - start
    print("%{req.method} %{req.path} took %{ms}ms")
    return res
})
```

### Built-in middleware

| Middleware | Description |
|-----------|-------------|
| `middleware.cors()` | Adds CORS headers, handles OPTIONS preflight |
| `middleware.cors({origin: "...", methods: "...", headers: "..."})` | CORS with custom options |
| `middleware.jsonBody()` | Parses JSON request bodies into `req.json` |
| `middleware.requestId()` | Adds unique `req.id` and `X-Request-Id` response header |
| `middleware.auth(verifier)` | Bearer token auth — calls `verifier(token)`, sets `req.user` |
| `middleware.headers(map)` | Adds fixed headers to every response |

### CORS

```
server.use(middleware.cors())                              // allow all origins
server.use(middleware.cors({origin: "https://myapp.com"})) // specific origin
```

### JSON body parsing

```
server.use(middleware.jsonBody())

server.post("/api/data", lam{ req, params in
    print(req.json.name)    // parsed from {"name": "Ada"}
})
```

Returns 400 automatically if the JSON is malformed.

### Authentication

```
server.use(middleware.auth(lam{ token in
    if (token == "secret123") {
        return {id: 1, name: "Ada"}   // user object
    }
    return nil                         // reject
}))

server.get("/profile", lam{ req, params in
    return {status: 200, body: "Hello %{req.user.name}"}
})
```

Returns 401 if no `Authorization: Bearer ...` header, 403 if the verifier returns nil.

---

## Logger

The `logger` grain provides structured logging with levels.

### Creating a logger

```
use "logger"

let log = logger.create("MyApp")
log.info("server started")
log.warn("disk space low")
log.error("connection failed")
log.debug("verbose output")
```

Output: `[2026-04-19 13:00:00] INFO [MyApp] server started`

### Log levels

| Level | Priority |
|-------|----------|
| `debug` | 0 (lowest) |
| `info` | 1 (default) |
| `warn` | 2 |
| `error` | 3 |
| `none` | 4 (disables all) |

```
log.setLevel("debug")    // show everything
log.setLevel("warn")     // only warn and error
log.setLevel("none")     // silence all
```

### As router middleware

```
use "logger"
use "router"

let log = logger.create("API")
let server = router.create()

server.use(logger.middleware(log))
// Logs: [timestamp] INFO [API] GET /users/42 200 12ms
```

---

## Cookies

The `cookie` grain parses and builds HTTP cookie headers.

### Parsing cookies

```
use "cookie"

// Parse a Cookie header into a map
let cookies = cookie.parse(req.headers["cookie"])
print(cookies.sessionId)
print(cookies.theme)
```

### Building Set-Cookie headers

```
// Simple cookie with secure defaults (HttpOnly, SameSite=Lax, Path=/)
let header = cookie.build("theme", "dark")

// With options
let header = cookie.build("session", token, {
    httpOnly: true,
    secure: true,
    sameSite: "Strict",
    maxAge: 3600,        // 1 hour in seconds
    path: "/",
    domain: "example.com"
})
```

### Clearing cookies

```
let header = cookie.clear("session")   // sets Max-Age=0
```

### Default options

If no options are passed, `cookie.build` uses safe defaults:
- `HttpOnly` — not accessible from JavaScript
- `SameSite=Lax` — prevents CSRF
- `Path=/` — available on all paths

---

## Sessions

The `session` grain provides server-side session management as router middleware.

### Setup

```
use "router"
use "session"

let server = router.create()
let sessions = session.create()
server.use(sessions.middleware())
```

### Using sessions in handlers

```
// Store data
server.post("/login", lam{ req, params in
    req.session.set("user", {name: "Ada", role: "admin"})
    return http.json({message: "logged in"})
})

// Read data
server.get("/profile", lam{ req, params in
    let user = req.session.get("user")
    if (!user) {
        return http.json({error: "not logged in"}, 401)
    }
    return http.json({user: user})
})

// Destroy session (logout)
server.post("/logout", lam{ req, params in
    req.session.destroy()
    return http.json({message: "logged out"})
})
```

### Session API

| Method | Description |
|--------|-------------|
| `req.session.get(key)` | Get a value (returns nil if not set) |
| `req.session.set(key, value)` | Store a value |
| `req.session.has(key)` | Check if key exists |
| `req.session.delete(key)` | Remove a key |
| `req.session.destroy()` | Delete the entire session |
| `req.session.id` | The session ID string |

### Options

```
let sessions = session.create({
    cookieName: "myapp.sid",    // default: "praia.sid"
    maxAge: 7200,               // 2 hours (default: 86400 = 1 day)
    secure: true,               // HTTPS only
    sameSite: "Strict"          // default: "Lax"
})
```

### How it works

1. On each request, the middleware reads the session cookie
2. If valid, loads session data from an in-memory store
3. If missing or invalid, creates a new session
4. Attaches `req.session` with get/set/destroy methods
5. After the handler runs, sets the `Set-Cookie` header on the response

Sessions are stored in memory — they're lost when the server restarts. For persistent sessions, store session data in SQLite.

---

## SQLite

Built-in SQLite database support. `sqlite.open()` returns a database object with `query`, `run`, and `close` methods. Available when built on a system with libsqlite3.

### Opening a database

```
let db = sqlite.open("myapp.db")       // file-based
let db = sqlite.open(":memory:")       // in-memory
```

### Queries

`db.query(sql, params?)` executes a SELECT and returns an array of maps (one map per row):

```
let users = db.query("SELECT * FROM users WHERE age > ?", [18])
for (user in users) {
    print(user.name, user.age)
}
```

### Executing statements

`db.run(sql, params?)` executes INSERT/UPDATE/DELETE and returns `{changes, lastId}`:

```
let result = db.run("INSERT INTO users (name, age) VALUES (?, ?)", ["Ada", 36])
print(result.lastId)      // auto-increment id
print(result.changes)     // rows affected
```

### Parameterized queries

Always use `?` placeholders — they prevent SQL injection:

```
// Safe
db.query("SELECT * FROM users WHERE name = ?", [name])

// Unsafe — never do this
db.query("SELECT * FROM users WHERE name = '" + name + "'")
```

Parameters are bound by type: strings, numbers, bools, and nil are all handled automatically.

### Closing

```
db.close()
```

### Example: REST API with SQLite

```
let db = sqlite.open(":memory:")
db.run("CREATE TABLE todos (id INTEGER PRIMARY KEY, title TEXT, done INT)")

let server = http.createServer(lam{ req in
    if (req.method == "GET" && req.path == "/todos") {
        let todos = db.query("SELECT * FROM todos")
        return {
            status: 200,
            body: json.stringify(todos),
            headers: {"Content-Type": "application/json"}
        }
    }
    if (req.method == "POST" && req.path == "/todos") {
        let todo = json.parse(req.body)
        db.run("INSERT INTO todos (title, done) VALUES (?, ?)", [todo.title, 0])
        return {status: 201, body: json.stringify({ok: true})}
    }
    return {status: 404, body: "Not Found"}
})

server.listen(8080)
```

---

## Math

The `math` namespace provides mathematical constants and functions.

### Constants

| Name | Value |
|------|-------|
| `math.PI` | 3.14159265358979 |
| `math.E` | 2.71828182845905 |
| `math.INF` | Infinity |

### Functions

| Function | Description |
|----------|-------------|
| `math.sqrt(x)` | Square root |
| `math.pow(x, y)` | x raised to power y |
| `math.abs(x)` | Absolute value |
| `math.floor(x)` | Round down |
| `math.ceil(x)` | Round up |
| `math.round(x)` | Round to nearest |
| `math.min(a, b)` | Minimum |
| `math.max(a, b)` | Maximum |
| `math.clamp(x, lo, hi)` | Clamp x between lo and hi |
| `math.sin(x)`, `cos`, `tan` | Trigonometry (radians) |
| `math.asin(x)`, `acos`, `atan` | Inverse trig |
| `math.log(x)` | Natural log |
| `math.log2(x)`, `log10(x)` | Base-2 and base-10 log |
| `math.exp(x)` | e^x |

```
print(math.sqrt(144))              // 12
print(math.pow(2, 10))             // 1024
print(math.sin(math.PI / 2))       // 1
print(math.clamp(150, 0, 100))     // 100
```

---

## Random

The `random` namespace provides random number generation using a Mersenne Twister engine.

| Function | Description |
|----------|-------------|
| `random.int(min, max)` | Random integer between min and max (inclusive) |
| `random.float()` | Random float between 0.0 and 1.0 |
| `random.choice(arr)` | Random element from an array |
| `random.shuffle(arr)` | Shuffle an array in place |
| `random.seed(n)` | Set the seed for reproducible results |

```
print(random.int(1, 100))          // e.g. 42
print(random.float())              // e.g. 0.7312
print(random.choice(["a", "b"]))   // "a" or "b"

let deck = [1, 2, 3, 4, 5]
random.shuffle(deck)
print(deck)

// Reproducible
random.seed(42)
print(random.int(0, 100))          // always 51
```

---

## Time

The `time` namespace provides timestamps, formatting, and sleep.

| Function | Description |
|----------|-------------|
| `time.now()` | Current time as Unix milliseconds |
| `time.epoch()` | Current time as Unix seconds |
| `time.sleep(ms)` | Pause execution for ms milliseconds |
| `time.format(fmt?, timestamp?)` | Format time as string (default: `"%Y-%m-%d %H:%M:%S"`) |

```
let start = time.now()
time.sleep(100)
print(time.now() - start)          // ~100

print(time.format())               // "2026-04-18 13:00:00"
print(time.format("%H:%M"))        // "13:00"
print(time.epoch())                // 1776510000
```

### Benchmarking

```
let start = time.now()
// ... code to measure ...
print("took", time.now() - start, "ms")
```

---

## OS extras (sys)

In addition to file/directory operations, `sys` provides:

| Field/Function | Description |
|----------------|-------------|
| `sys.env(name)` | Read environment variable (returns nil if not set) |
| `sys.cwd()` | Current working directory |
| `sys.platform` | `"darwin"`, `"linux"`, or `"windows"` |

```
print(sys.env("HOME"))             // "/Users/ada"
print(sys.cwd())                   // "/path/to/project"
print(sys.platform)                // "darwin"

let dbUrl = sys.env("DATABASE_URL")
ensure (dbUrl) else { throw "DATABASE_URL not set" }
```

---

## Bitwise Operators

| Operator | Description |
|----------|-------------|
| `&` | Bitwise AND |
| `\|` | Bitwise OR |
| `^` | Bitwise XOR |
| `~` | Bitwise NOT (unary) |
| `<<` | Left shift |
| `>>` | Right shift |

```
255 & 15        // 15
240 | 15        // 255
255 ^ 15        // 240
~0              // -1
1 << 8          // 256
256 >> 4        // 16
```

All values are converted to 64-bit integers for bitwise operations.

Note: `|` (single pipe) is bitwise OR. `|>` is the pipe operator. `||` is logical OR. The lexer distinguishes them by what follows the `|`.

---

## Bytes

The `bytes` namespace provides binary data packing and unpacking for working with binary protocols.

### bytes.pack(format, values)

Packs an array of numbers into a binary string.

```
let data = bytes.pack("u32be", [256])      // 4-byte big-endian
let msg = bytes.pack("u16be", [1234, 5678]) // two 16-bit values
```

### bytes.unpack(format, data)

Unpacks a binary string into an array of numbers.

```
let vals = bytes.unpack("u32be", data)     // [256]
```

### Formats

| Format | Size | Description |
|--------|------|-------------|
| `u8`, `i8` | 1 byte | Unsigned/signed 8-bit |
| `u16be`, `u16le` | 2 bytes | Unsigned 16-bit big/little endian |
| `i16be`, `i16le` | 2 bytes | Signed 16-bit big/little endian |
| `u32be`, `u32le` | 4 bytes | Unsigned 32-bit big/little endian |
| `i32be`, `i32le` | 4 bytes | Signed 32-bit big/little endian |

### Character codes

```
"A".charCode()          // 65
"hello".charCode(1)     // 101 (character 'e')
fromCharCode(65)        // "A"
```

---

## Crypto

The `crypto` namespace provides cryptographic hash functions.

| Function | Description |
|----------|-------------|
| `crypto.md5(string)` | MD5 hash (32-char hex string) |
| `crypto.sha256(string)` | SHA-256 hash (64-char hex string) |

```
crypto.md5("hello")     // "5d41402abc4b2a76b9719d911017c592"
crypto.sha256("hello")  // "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"
```

These are essential for implementing authentication protocols (PostgreSQL, MySQL, etc.) in grains.

---

## TCP Sockets

The `net` namespace provides raw TCP socket operations. Sockets are represented as numbers (file descriptors).

### Client

```
let sock = net.connect("localhost", 5432)
net.send(sock, "hello")
let response = net.recv(sock)
print(response)
net.close(sock)
```

### Server

```
let server = net.listen(9000)
print("listening on 9000")

while (true) {
    let client = net.accept(server)
    let data = net.recv(client)
    net.send(client, "echo: " + data)
    net.close(client)
}
```

### Functions

| Function | Description |
|----------|-------------|
| `net.connect(host, port)` | Connect to a TCP server, returns socket |
| `net.listen(port)` | Bind and listen on a port, returns server socket |
| `net.accept(server)` | Wait for and accept a connection, returns client socket |
| `net.send(sock, data)` | Send a string, returns bytes sent |
| `net.recv(sock, maxBytes?)` | Receive data (default 4096 bytes max), returns string |
| `net.recvAll(sock)` | Read until the connection closes, returns string |
| `net.close(sock)` | Close a socket |

### Use cases

With raw TCP sockets, these can be implemented:
- Database clients (PostgreSQL, MySQL, Redis)
- SMTP (email sending)
- WebSocket protocol
- Any custom TCP protocol

---

## Grains (Modules)

Praia's module system uses **grains** (like sand grains). Each grain is a `.praia` file that exports functions and values.

### Creating a grain

A grain is any `.praia` file that ends with an `export` statement:

```
// grains/math.praia
let PI = 3.14159

func square(x) { return x * x }
func cube(x) { return x * x * x }

export { PI, square, cube }
```

### Importing a grain

Use `use` to import a grain. The grain is bound to a variable named after the last path segment:

```
use "math"

print(math.PI)          // 3.14159
print(math.square(5))   // 25
```

### Custom alias

Use `as` to bind a grain to a different name:

```
use "logger" as log
use "collections" as col

let l = log.create("App")
let s = col.Stack()
```

### Relative imports

Paths starting with `./` or `../` are resolved relative to the importing file:

```
use "./helpers/greeter"

greeter.hello("world")
```

### Resolution order

When you write `use "math"`, Praia looks for the grain in this order:

1. **`grains/`** directory — walking up from the current file's directory to find a `grains/` folder
2. **`grains/`** relative to the current working directory
3. **`~/.praia/grains/`** — global grains (for a future package manager)

The `.praia` extension is added automatically.

### Grains importing other grains

Grains can import other grains:

```
// grains/geometry.praia
use "math"

func circleArea(r) {
    return math.PI * math.square(r)
}

export { circleArea }
```

### Rules

- **No duplicate imports** — importing the same grain twice in one file is an error (enforces clean code)
- **Grains run once** — if multiple files import the same grain, it is only executed the first time; subsequent imports get the cached exports
- **Isolated scope** — grains cannot access the importer's variables; they only see globals and their own definitions
- **Explicit exports** — only names listed in `export { ... }` are visible to the importer

```
use "math"
use "math"      // Error: Grain 'math' is already imported in this file
```

### Project structure

A typical Praia project might look like:

```
my-project/
├── grains/
│   ├── math.praia
│   ├── strings.praia
│   └── geometry.praia
├── examples/
│   └── demo.praia
└── main.praia
```

---

## Comments

```
// This is a single-line comment

/* This is a
   multi-line comment */

/* Block comments /* can nest */ like this */
```

---

## Operator Precedence

From highest to lowest:

| Precedence | Operators | Description |
|-----------|-----------|-------------|
| 1 | `()` `[]` `.` | Call, index, field access |
| 2 | `++` `--` | Postfix increment/decrement |
| 3 | `-` `!` | Unary negation, logical NOT |
| 4 | `*` `/` `%` | Multiplication, division, modulo |
| 5 | `+` `-` | Addition, subtraction |
| 6 | `<` `>` `<=` `>=` | Comparison |
| 7 | `==` `!=` | Equality |
| 8 | `&&` | Logical AND |
| 9 | `\|\|` | Logical OR |
| 10 | `=` | Assignment (right-associative) |

Parentheses can override precedence:

```
print(2 + 3 * 4)       // 14
print((2 + 3) * 4)     // 20
```

---

## Error Stack Traces

When an error occurs inside nested function calls, Praia prints the full call stack:

```
[line 3] Runtime error: Division by zero
  at divide() line 7
  at calculate() line 11
  at main() line 14
```

Stack traces work for all error types: runtime errors, `throw`, and uncaught exceptions. Caught errors (via `try/catch`) do not print a trace — only uncaught errors that terminate execution.

---

## REPL

Run `./praia` with no arguments to start the interactive REPL.

```
$ ./praia
Praia REPL (type 'exit' to quit)
>> 2 + 3
5
>> let x = 10
>> x * 2
20
>> "hello".upper()
HELLO
```

Features:
- **Arrow keys** for command history (up/down) and line editing (left/right)
- **Auto-print** expression results (nil results are hidden)
- **Multi-line input** detected automatically when braces are unbalanced
- **Persistent state** across inputs (variables, functions survive between lines)
- **Ctrl-D** or `exit` to quit

```
>> func greet(name) {
..   print("hello %{name}")
.. }
>> greet("world")
hello world
```

---

## Command-Line Usage

```
./praia                             # REPL
./praia script.praia                # run a script
./praia script.praia arg1 arg2      # run with arguments (sys.args)
./praia -c 'print("hello")'        # run a one-liner
./praia -c 'print(sys.args)' a b   # one-liner with arguments
./praia test                        # run test suite in tests/
./praia --tokens script.praia       # show lexer tokens
./praia --ast script.praia          # show parse tree
```
