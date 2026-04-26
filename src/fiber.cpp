#include "fiber.h"

thread_local Fiber* Fiber::current_ = nullptr;
