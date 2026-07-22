//
//  easing.h
//
//  Copyright (c) 2011, Auerhaus Development, LLC
//
//  This program is free software. It comes without any warranty, to
//  the extent permitted by applicable law. You can redistribute it
//  and/or modify it under the terms of the Do What The Fuck You Want
//  To Public License, Version 2, as published by Sam Hocevar. See
//  http://sam.zoy.org/wtfpl/COPYING for more details.
//
// DECLARATIONS only - the function bodies live in easing.c (one translation
// unit), so this header is safe to include from any number of .c files.
// The include guard below prevents double-declaration in one file.

#ifndef EASING_H
#define EASING_H

#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

// An easing function: maps linear progress 0..1 to eased progress 0..1.
// Lets animation tables pick "how it moves" as data (e.g. sineEaseOutf).
typedef float (*EaseFn)(float);

float backEaseInOutf(float p);
float backEaseInf(float p);
float backEaseOutf(float p);
float bounceEaseInOutf(float p);
float bounceEaseInf(float p);
float bounceEaseOutf(float p);
float circularEaseInOutf(float p);
float circularEaseInf(float p);
float circularEaseOutf(float p);
float cubicEaseInOutf(float p);
float cubicEaseInf(float p);
float cubicEaseOutf(float p);
float elasticEaseInOutf(float p);
float elasticEaseInf(float p);
float elasticEaseOutf(float p);
float exponentialEaseInOutf(float p);
float exponentialEaseInf(float p);
float exponentialEaseOutf(float p);
float linearInterpolationf(float p);
float quadraticEaseInOutf(float p);
float quadraticEaseInf(float p);
float quadraticEaseOutf(float p);
float quarticEaseInOutf(float p);
float quarticEaseInf(float p);
float quarticEaseOutf(float p);
float quinticEaseInOutf(float p);
float quinticEaseInf(float p);
float quinticEaseOutf(float p);
float sineEaseInOutf(float p);
float sineEaseInf(float p);
float sineEaseOutf(float p);
double backEaseInOutd(double p);
double backEaseInd(double p);
double backEaseOutd(double p);
double bounceEaseInOutd(double p);
double bounceEaseInd(double p);
double bounceEaseOutd(double p);
double circularEaseInOutd(double p);
double circularEaseInd(double p);
double circularEaseOutd(double p);
double cubicEaseInOutd(double p);
double cubicEaseInd(double p);
double cubicEaseOutd(double p);
double elasticEaseInOutd(double p);
double elasticEaseInd(double p);
double elasticEaseOutd(double p);
double exponentialEaseInOutd(double p);
double exponentialEaseInd(double p);
double exponentialEaseOutd(double p);
double linearInterpolationd(double p);
double quadraticEaseInOutd(double p);
double quadraticEaseInd(double p);
double quadraticEaseOutd(double p);
double quarticEaseInOutd(double p);
double quarticEaseInd(double p);
double quarticEaseOutd(double p);
double quinticEaseInOutd(double p);
double quinticEaseInd(double p);
double quinticEaseOutd(double p);
double sineEaseInOutd(double p);
double sineEaseInd(double p);
double sineEaseOutd(double p);

#endif // EASING_H
