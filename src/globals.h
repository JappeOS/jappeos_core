#pragma once

// Null safety
#define NULL_SAFE_CALL_RET(ptr, call) ((ptr) ? (ptr)->call : nullptr)
#define NULL_SAFE_CALL(ptr, call) do { if ((ptr)) { (ptr)->call; } } while (0)