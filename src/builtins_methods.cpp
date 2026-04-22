#include "builtins.h"
#include "value.h"
#include "vm/vm.h"
#include <algorithm>
#include <memory>
#include <regex>
#include <string>

Value getStringMethod(const std::string& str,
                      const std::string& name, int line) {
    if (name == "upper") {
        return Value(makeNative("upper", 0, [str](const std::vector<Value>&) -> Value {
            std::string r = str;
            std::transform(r.begin(), r.end(), r.begin(), ::toupper);
            return Value(std::move(r));
        }));
    }
    if (name == "lower") {
        return Value(makeNative("lower", 0, [str](const std::vector<Value>&) -> Value {
            std::string r = str;
            std::transform(r.begin(), r.end(), r.begin(), ::tolower);
            return Value(std::move(r));
        }));
    }
    if (name == "strip") {
        return Value(makeNative("strip", 0, [str](const std::vector<Value>&) -> Value {
            size_t start = str.find_first_not_of(" \t\n\r");
            if (start == std::string::npos) return Value(std::string(""));
            size_t end = str.find_last_not_of(" \t\n\r");
            return Value(str.substr(start, end - start + 1));
        }));
    }
    if (name == "split") {
        return Value(makeNative("split", 1, [str](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("split() separator must be a string", 0);
            auto& sep = args[0].asString();
            auto arr = std::make_shared<PraiaArray>();
            if (sep.empty()) {
                for (char c : str)
                    arr->elements.push_back(Value(std::string(1, c)));
                return Value(arr);
            }
            size_t pos = 0, found;
            while ((found = str.find(sep, pos)) != std::string::npos) {
                arr->elements.push_back(Value(str.substr(pos, found - pos)));
                pos = found + sep.size();
            }
            arr->elements.push_back(Value(str.substr(pos)));
            return Value(arr);
        }));
    }
    if (name == "contains") {
        return Value(makeNative("contains", 1, [str](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("contains() argument must be a string", 0);
            return Value(str.find(args[0].asString()) != std::string::npos);
        }));
    }
    if (name == "replace") {
        return Value(makeNative("replace", 2, [str](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("replace() arguments must be strings", 0);
            auto& from = args[0].asString();
            auto& to = args[1].asString();
            std::string result = str;
            size_t pos = 0;
            while ((pos = result.find(from, pos)) != std::string::npos) {
                result.replace(pos, from.size(), to);
                pos += to.size();
            }
            return Value(std::move(result));
        }));
    }
    if (name == "startsWith") {
        return Value(makeNative("startsWith", 1, [str](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("startsWith() argument must be a string", 0);
            auto& prefix = args[0].asString();
            return Value(str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0);
        }));
    }
    if (name == "endsWith") {
        return Value(makeNative("endsWith", 1, [str](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("endsWith() argument must be a string", 0);
            auto& suffix = args[0].asString();
            return Value(str.size() >= suffix.size() &&
                         str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0);
        }));
    }
    if (name == "title") {
        return Value(makeNative("title", 0, [str](const std::vector<Value>&) -> Value {
            std::string r = str;
            bool capNext = true;
            for (auto& c : r) {
                if (std::isspace(c)) { capNext = true; }
                else if (capNext) { c = std::toupper(c); capNext = false; }
                else { c = std::tolower(c); }
            }
            return Value(std::move(r));
        }));
    }
    if (name == "capitalize") {
        return Value(makeNative("capitalize", 0, [str](const std::vector<Value>&) -> Value {
            std::string r = str;
            if (!r.empty()) {
                r[0] = std::toupper(r[0]);
                for (size_t i = 1; i < r.size(); i++)
                    r[i] = std::tolower(r[i]);
            }
            return Value(std::move(r));
        }));
    }
    if (name == "capitalizeFirst") {
        return Value(makeNative("capitalizeFirst", 0, [str](const std::vector<Value>&) -> Value {
            std::string r = str;
            if (!r.empty()) r[0] = std::toupper(r[0]);
            return Value(std::move(r));
        }));
    }
    if (name == "charCode") {
        return Value(makeNative("charCode", -1, [str](const std::vector<Value>& args) -> Value {
            int idx = 0;
            if (!args.empty() && args[0].isNumber())
                idx = static_cast<int>(args[0].asNumber());
            if (idx < 0) idx += static_cast<int>(str.size());
            if (idx < 0 || idx >= static_cast<int>(str.size()))
                throw RuntimeError("charCode index out of bounds", 0);
            return Value(static_cast<int64_t>(static_cast<unsigned char>(str[idx])));
        }));
    }
    if (name == "test") {
        return Value(makeNative("test", 1, [str](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("test() pattern must be a string", 0);
            try {
                std::regex re(args[0].asString());
                return Value(std::regex_search(str, re));
            } catch (const std::regex_error& e) {
                throw RuntimeError(std::string("Invalid regex: ") + e.what(), 0);
            }
        }));
    }
    if (name == "match") {
        return Value(makeNative("match", 1, [str](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("match() pattern must be a string", 0);
            try {
                std::regex re(args[0].asString());
                std::smatch m;
                if (!std::regex_search(str, m, re)) return Value();
                // Return map with match and groups
                auto result = std::make_shared<PraiaMap>();
                result->entries["match"] = Value(m[0].str());
                result->entries["index"] = Value(static_cast<int64_t>(m.position(0)));
                auto groups = std::make_shared<PraiaArray>();
                for (size_t i = 1; i < m.size(); i++)
                    groups->elements.push_back(Value(m[i].str()));
                result->entries["groups"] = Value(groups);
                return Value(result);
            } catch (const std::regex_error& e) {
                throw RuntimeError(std::string("Invalid regex: ") + e.what(), 0);
            }
        }));
    }
    if (name == "matchAll") {
        return Value(makeNative("matchAll", 1, [str](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("matchAll() pattern must be a string", 0);
            try {
                std::regex re(args[0].asString());
                auto results = std::make_shared<PraiaArray>();
                auto begin = std::sregex_iterator(str.begin(), str.end(), re);
                auto end = std::sregex_iterator();
                for (auto it = begin; it != end; ++it) {
                    auto entry = std::make_shared<PraiaMap>();
                    entry->entries["match"] = Value((*it)[0].str());
                    entry->entries["index"] = Value(static_cast<int64_t>(it->position(0)));
                    auto groups = std::make_shared<PraiaArray>();
                    for (size_t i = 1; i < it->size(); i++)
                        groups->elements.push_back(Value((*it)[i].str()));
                    entry->entries["groups"] = Value(groups);
                    results->elements.push_back(Value(entry));
                }
                return Value(results);
            } catch (const std::regex_error& e) {
                throw RuntimeError(std::string("Invalid regex: ") + e.what(), 0);
            }
        }));
    }
    if (name == "replacePattern") {
        return Value(makeNative("replacePattern", 2, [str](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("replacePattern() requires string arguments", 0);
            try {
                std::regex re(args[0].asString());
                return Value(std::regex_replace(str, re, args[1].asString()));
            } catch (const std::regex_error& e) {
                throw RuntimeError(std::string("Invalid regex: ") + e.what(), 0);
            }
        }));
    }
    if (name == "slice") {
        return Value(makeNative("slice", -1, [str](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isNumber())
                throw RuntimeError("slice() requires a start index", 0);
            int len = static_cast<int>(str.size());
            int start = static_cast<int>(args[0].asNumber());
            if (start < 0) start += len;
            if (start < 0) start = 0;
            if (start >= len) return Value(std::string(""));
            if (args.size() > 1 && args[1].isNumber()) {
                int end = static_cast<int>(args[1].asNumber());
                if (end < 0) end += len;
                if (end <= start) return Value(std::string(""));
                if (end > len) end = len;
                return Value(str.substr(start, end - start));
            }
            return Value(str.substr(start));
        }));
    }
    if (name == "indexOf") {
        return Value(makeNative("indexOf", -1, [str](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isString())
                throw RuntimeError("indexOf() requires a string argument", 0);
            size_t startPos = 0;
            if (args.size() > 1 && args[1].isNumber())
                startPos = static_cast<size_t>(args[1].asNumber());
            auto pos = str.find(args[0].asString(), startPos);
            return Value(pos == std::string::npos ? static_cast<int64_t>(-1) : static_cast<int64_t>(pos));
        }));
    }
    if (name == "lastIndexOf") {
        return Value(makeNative("lastIndexOf", 1, [str](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("lastIndexOf() requires a string argument", 0);
            auto pos = str.rfind(args[0].asString());
            return Value(pos == std::string::npos ? static_cast<int64_t>(-1) : static_cast<int64_t>(pos));
        }));
    }
    if (name == "repeat") {
        return Value(makeNative("repeat", 1, [str](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("repeat() requires a number", 0);
            int count = static_cast<int>(args[0].asNumber());
            if (count < 0) throw RuntimeError("repeat() count cannot be negative", 0);
            std::string result;
            result.reserve(str.size() * count);
            for (int i = 0; i < count; i++) result += str;
            return Value(std::move(result));
        }));
    }
    if (name == "padStart") {
        return Value(makeNative("padStart", -1, [str](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isNumber())
                throw RuntimeError("padStart() requires a length", 0);
            int target = static_cast<int>(args[0].asNumber());
            std::string pad = " ";
            if (args.size() > 1 && args[1].isString()) pad = args[1].asString();
            std::string result = str;
            while (static_cast<int>(result.size()) < target)
                result = pad + result;
            return Value(std::move(result));
        }));
    }
    if (name == "padEnd") {
        return Value(makeNative("padEnd", -1, [str](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isNumber())
                throw RuntimeError("padEnd() requires a length", 0);
            int target = static_cast<int>(args[0].asNumber());
            std::string pad = " ";
            if (args.size() > 1 && args[1].isString()) pad = args[1].asString();
            std::string result = str;
            while (static_cast<int>(result.size()) < target)
                result += pad;
            return Value(std::move(result));
        }));
    }
    if (name == "trimStart") {
        return Value(makeNative("trimStart", 0, [str](const std::vector<Value>&) -> Value {
            size_t start = str.find_first_not_of(" \t\n\r");
            if (start == std::string::npos) return Value(std::string(""));
            return Value(str.substr(start));
        }));
    }
    if (name == "trimEnd") {
        return Value(makeNative("trimEnd", 0, [str](const std::vector<Value>&) -> Value {
            size_t end = str.find_last_not_of(" \t\n\r");
            if (end == std::string::npos) return Value(std::string(""));
            return Value(str.substr(0, end + 1));
        }));
    }
    throw RuntimeError("String has no method '" + name + "'", line);
}

Value getArrayMethod(std::shared_ptr<PraiaArray> arr,
                     const std::string& name, int line,
                     Interpreter* interp, VM* vm) {
    if (name == "push") {
        return Value(makeNative("push", 1, [arr](const std::vector<Value>& args) -> Value {
            arr->elements.push_back(args[0]);
            return Value();
        }));
    }
    if (name == "pop") {
        return Value(makeNative("pop", 0, [arr](const std::vector<Value>&) -> Value {
            if (arr->elements.empty())
                throw RuntimeError("pop() on empty array", 0);
            Value last = arr->elements.back();
            arr->elements.pop_back();
            return last;
        }));
    }
    if (name == "contains") {
        return Value(makeNative("contains", 1, [arr](const std::vector<Value>& args) -> Value {
            for (auto& e : arr->elements)
                if (e == args[0]) return Value(true);
            return Value(false);
        }));
    }
    if (name == "join") {
        return Value(makeNative("join", 1, [arr](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("join() separator must be a string", 0);
            auto& sep = args[0].asString();
            std::string result;
            for (size_t i = 0; i < arr->elements.size(); i++) {
                if (i > 0) result += sep;
                result += arr->elements[i].toString();
            }
            return Value(std::move(result));
        }));
    }
    if (name == "reverse") {
        return Value(makeNative("reverse", 0, [arr](const std::vector<Value>&) -> Value {
            std::reverse(arr->elements.begin(), arr->elements.end());
            return Value();
        }));
    }
    if (name == "shift") {
        return Value(makeNative("shift", 0, [arr](const std::vector<Value>&) -> Value {
            if (arr->elements.empty())
                throw RuntimeError("shift() on empty array", 0);
            Value first = arr->elements.front();
            arr->elements.erase(arr->elements.begin());
            return first;
        }));
    }
    if (name == "unshift") {
        return Value(makeNative("unshift", 1, [arr](const std::vector<Value>& args) -> Value {
            arr->elements.insert(arr->elements.begin(), args[0]);
            return Value();
        }));
    }
    if (name == "slice") {
        return Value(makeNative("slice", -1, [arr](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isNumber())
                throw RuntimeError("slice() requires a start index", 0);
            int len = static_cast<int>(arr->elements.size());
            int start = static_cast<int>(args[0].asNumber());
            if (start < 0) start += len;
            if (start < 0) start = 0;
            int end = len;
            if (args.size() > 1 && args[1].isNumber()) {
                end = static_cast<int>(args[1].asNumber());
                if (end < 0) end += len;
            }
            if (start >= len || end <= start)
                return Value(std::make_shared<PraiaArray>());
            if (end > len) end = len;
            auto result = std::make_shared<PraiaArray>();
            result->elements.assign(arr->elements.begin() + start, arr->elements.begin() + end);
            return Value(result);
        }));
    }
    if (name == "indexOf") {
        return Value(makeNative("indexOf", 1, [arr](const std::vector<Value>& args) -> Value {
            for (size_t i = 0; i < arr->elements.size(); i++)
                if (arr->elements[i] == args[0]) return Value(static_cast<int64_t>(i));
            return Value(static_cast<int64_t>(-1));
        }));
    }
    if (name == "find") {
        return Value(makeNative("find", 1, [arr, interp, vm](const std::vector<Value>& args) -> Value {
            if (!args[0].isCallable())
                throw RuntimeError("find() requires a function", 0);
            auto pred = args[0].asCallable();
            for (auto& elem : arr->elements) {
                Value result = vm ? callWithVM(*vm, pred, {elem})
                                  : callSafe(*interp, pred, {elem});
                if (result.isTruthy()) return elem;
            }
            return Value();
        }));
    }
    throw RuntimeError("Array has no method '" + name + "'", line);
}
