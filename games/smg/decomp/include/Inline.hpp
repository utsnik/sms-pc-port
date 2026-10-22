#pragma once

// Macro which is put after the function definition (only in the header) to prevent the function from being inlined.
// Example: void someFunction(int someArg) const NO_INLINE;
#if defined(TARGET_WIIU)
// GCC rejects a post-declarator attribute on an inline function definition,
// which is the form used throughout the decompilation.  Stage 1 prioritizes
// a complete linkable target; the annotation remains active on desktop.
#define NO_INLINE
#else
#define NO_INLINE __attribute__((noinline))
#endif

// Macros which should be used when a function is both inlined and not inlined (so it's auto-inlined by the compiler).
// This is used to declare the inline version of the function. If not a constructor, the non-inline version
// should call the inline version.
// Example:
// int someFunction(int SomeArg)
// inline int INLINE_FUNC_DECL(someFunction, int someArg);
// CALL_INLINE_FUNC(someFunction, 0);
#define INLINE_FUNC_DECL(name, ...) name(void*****, __VA_ARGS__)
#define INLINE_FUNC_DECL_NO_ARG(name) name(void*****)

#define CALL_INLINE_FUNC(name, ...) name((void*****)0, __VA_ARGS__)
#define CALL_INLINE_FUNC_NO_ARG(name) name((void*****)0)
