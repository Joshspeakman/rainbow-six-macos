/* See string.h in this folder: the math the recompiled game code uses, without Apple's SDK. */
#ifndef SETUP_MATH_H
#define SETUP_MATH_H
double fabs(double); float fabsf(float); double floor(double); float floorf(float); double ceil(double); float ceilf(float);
double trunc(double); float truncf(float); double round(double); float roundf(float); double rint(double); float rintf(float);
double nearbyint(double); float nearbyintf(float); long lrint(double); long long llrint(double); long lrintf(float);
double sqrt(double); float sqrtf(float); double fmod(double, double); float fmodf(float, float);
double ldexp(double, int); float ldexpf(float, int); double frexp(double, int *); double scalbn(double, int); double copysign(double, double);
double pow(double, double); double exp(double); double log(double); double log2(double); double log10(double);
double sin(double); double cos(double); double tan(double); double atan(double); double atan2(double, double); double asin(double); double acos(double);
#define HUGE_VAL __builtin_huge_val()
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
#endif
