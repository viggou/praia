#include "../builtins.h"
#include <condition_variable>
#include <mutex>
#include <queue>

void registerConcurrencyBuiltins(Interpreter* self, std::shared_ptr<Environment> globals) {
    globals->define("Lock", Value(makeNative("Lock", 0,
        [self](const std::vector<Value>&) -> Value {
            auto mtx = std::make_shared<std::recursive_mutex>();
            auto lock = std::make_shared<PraiaMap>();

            lock->entries["acquire"] = Value(makeNative("acquire", 0,
                [mtx](const std::vector<Value>&) -> Value {
                    mtx->lock();
                    return Value();
                }));

            lock->entries["release"] = Value(makeNative("release", 0,
                [mtx](const std::vector<Value>&) -> Value {
                    mtx->unlock();
                    return Value();
                }));

            lock->entries["withLock"] = Value(makeNative("withLock", 1,
                [mtx, self](const std::vector<Value>& args) -> Value {
                    if (!args[0].isCallable())
                        throw RuntimeError("withLock() requires a function", 0);
                    std::lock_guard<std::recursive_mutex> guard(*mtx);
                    return callSafe(*self, args[0].asCallable(), {});
                }));

            return Value(lock);
        })));

    // ── Channel() — thread-safe queue for async communication ──

    struct ChannelState {
        std::mutex mtx;
        std::condition_variable cv;
        std::queue<Value> buffer;
        bool closed = false;
    };

    globals->define("Channel", Value(makeNative("Channel", -1,
        [](const std::vector<Value>& args) -> Value {
            auto state = std::make_shared<ChannelState>();
            int capacity = 0; // unbuffered by default
            if (!args.empty() && args[0].isNumber())
                capacity = static_cast<int>(args[0].asNumber());

            auto ch = std::make_shared<PraiaMap>();

            ch->entries["send"] = Value(makeNative("send", 1,
                [state, capacity](const std::vector<Value>& args) -> Value {
                    std::unique_lock<std::mutex> lock(state->mtx);
                    if (state->closed)
                        throw RuntimeError("Cannot send on a closed channel", 0);
                    if (capacity > 0) {
                        state->cv.wait(lock, [&] {
                            return static_cast<int>(state->buffer.size()) < capacity || state->closed;
                        });
                        if (state->closed)
                            throw RuntimeError("Cannot send on a closed channel", 0);
                    }
                    state->buffer.push(args[0]);
                    state->cv.notify_all();
                    return Value();
                }));

            ch->entries["recv"] = Value(makeNative("recv", 0,
                [state](const std::vector<Value>&) -> Value {
                    std::unique_lock<std::mutex> lock(state->mtx);
                    state->cv.wait(lock, [&] {
                        return !state->buffer.empty() || state->closed;
                    });
                    if (state->buffer.empty()) return Value(); // closed + empty = nil
                    Value val = state->buffer.front();
                    state->buffer.pop();
                    state->cv.notify_all();
                    return val;
                }));

            ch->entries["tryRecv"] = Value(makeNative("tryRecv", 0,
                [state](const std::vector<Value>&) -> Value {
                    std::lock_guard<std::mutex> lock(state->mtx);
                    if (state->buffer.empty()) return Value(); // nil
                    Value val = state->buffer.front();
                    state->buffer.pop();
                    state->cv.notify_all();
                    return val;
                }));

            ch->entries["close"] = Value(makeNative("close", 0,
                [state](const std::vector<Value>&) -> Value {
                    std::lock_guard<std::mutex> lock(state->mtx);
                    state->closed = true;
                    state->cv.notify_all();
                    return Value();
                }));

            ch->entries["closed"] = Value(makeNative("closed", 0,
                [state](const std::vector<Value>&) -> Value {
                    std::lock_guard<std::mutex> lock(state->mtx);
                    return Value(state->closed && state->buffer.empty());
                }));

            return Value(ch);
        })));

    // ── futures namespace ──

    auto asyncMap = std::make_shared<PraiaMap>();

    asyncMap->entries["all"] = Value(makeNative("futures.all", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("async.all() requires an array of futures", 0);
            auto& futures = args[0].asArray()->elements;
            auto results = std::make_shared<PraiaArray>();
            for (auto& f : futures) {
                if (!f.isFuture())
                    throw RuntimeError("async.all() array must contain only futures", 0);
                results->elements.push_back(f.asFuture()->future.get());
            }
            return Value(results);
        }));

    asyncMap->entries["race"] = Value(makeNative("futures.race", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("async.race() requires an array of futures", 0);
            auto& futures = args[0].asArray()->elements;
            if (futures.empty())
                throw RuntimeError("async.race() requires at least one future", 0);
            // Poll futures until one is ready
            // Note: shared_future doesn't have a ready() check, so we
            // use wait_for with zero timeout
            while (true) {
                for (auto& f : futures) {
                    if (!f.isFuture()) continue;
                    auto& sf = f.asFuture()->future;
                    if (sf.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                        return sf.get();
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }));

    globals->define("futures", Value(asyncMap));
}
