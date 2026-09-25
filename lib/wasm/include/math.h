/* Freestanding math.h for a wasm32 kernel built -nostdlib -ffreestanding.
 *
 * The easy half are single wasm instructions (sqrt, abs, floor, ceil) and go
 * straight to clang's builtins. The rest - trig, exp, log, pow - are defined
 * in ../libm.c as ordinary range-reduced series: close to a platform libm,
 * not bit-identical to one, so a drawing made with them matches its native
 * build to within a hair rather than to the bit. fminf/fmaxf are functions,
 * not builtins: wasm's f32.min propagates a NaN where libm's returns the
 * number. */
#ifndef WASM_LIBC_MATH_H
#define WASM_LIBC_MATH_H

#define M_PI 3.14159265358979323846

#define sqrtf(x)    __builtin_sqrtf(x)
#define sqrt(x)     __builtin_sqrt(x)
#define fabsf(x)    __builtin_fabsf(x)
#define fabs(x)     __builtin_fabs(x)
#define floorf(x)   __builtin_floorf(x)
#define ceilf(x)    __builtin_ceilf(x)

double sin(double x);
double cos(double x);
double exp(double x);
double log(double x);
double log2(double x);
float  sinf(float x);
float  cosf(float x);
float  acosf(float x);
float  powf(float x, float y);
float  logf(float x);
long   lroundf(float x);
float  fminf(float a, float b);
float  fmaxf(float a, float b);

#endif
