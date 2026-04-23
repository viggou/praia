// Praia Native Plugin API
//
// Include this single header in your plugin source file.
// Your plugin must export:
//
//   extern "C" void praia_register(PraiaMap* module);
//
// Example:
//
//   #include "praia_plugin.h"
//
//   extern "C" void praia_register(PraiaMap* module) {
//       module->entries["double"] = Value(makeNative("mymod.double", 1,
//           [](const std::vector<Value>& args) -> Value {
//               if (!args[0].isNumber())
//                   throw RuntimeError("expected a number", 0);
//               return Value(args[0].asInt() * 2);
//           }));
//   }
//
// Build:
//   make plugin SRC=myplugin.cpp OUT=myplugin.dylib   # macOS
//   make plugin SRC=myplugin.cpp OUT=myplugin.so       # Linux
//
// Use in Praia:
//   let mymod = loadNative("./myplugin.dylib")
//   print(mymod.double(21))  // 42

#pragma once

#include "value.h"      // Value, PraiaArray, PraiaMap, RuntimeError
#include "gc_heap.h"    // gcNew<T>()
#include "builtins.h"   // makeNative()
