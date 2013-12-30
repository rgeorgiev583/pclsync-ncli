/* Copyright (c) 2013 Anton Titov.
 * Copyright (c) 2013 pCloud Ltd.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of pCloud Ltd nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL pCloud Ltd BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _PSYNC_COMPILER_H
#define _PSYNC_COMPILER_H

#ifndef __has_attribute
#if defined(__GNUC__)
#define __has_attribute(x) 1
#else
#define __has_attribute(x) 0
#endif
#endif

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#if defined(__GNUC__) || (defined(__clang__) && __has_builtin(__builtin_expect))
#define likely(expr) __builtin_expect(!!(expr), 1)
#define unlikely(expr) __builtin_expect(!!(expr), 0)
#else
#define likely(expr) (expr)
#define unlikely(expr) (expr)
#endif

#define PSYNC_THREAD __thread

#if __has_attribute(malloc)
#define PSYNC_MALLOC __attribute__((malloc))
#else
#define PSYNC_MALLOC
#endif

#if __has_attribute(sentinel)
#define PSYNC_SENTINEL __attribute__ ((sentinel))
#else
#define PSYNC_SENTINEL
#endif

#if __has_attribute(pure)
#define PSYNC_PURE __attribute__ ((pure))
#else
#define PSYNC_PURE
#endif

#if __has_attribute(cold)
#define PSYNC_COLD __attribute__ ((cold))
#else
#define PSYNC_COLD
#endif 

#if __has_attribute(format)
#define PSYNC_FORMAT(a, b, c) __attribute__ ((format (a, b, c)))
#else
#define PSYNC_FORMAT(a, b, c)
#endif

#if __has_attribute(nonnull)
#define PSYNC_NONNULL(...) __attribute__ ((nonnull (__VA_ARGS__)))
#else
#define PSYNC_NONNULL(...)
#endif

#if _MSC_VER >= 1500
#define inline __inline
#define restrict __restrict
#elif __GNUC__ >= 3
#define inline __inline
#define restrict __restrict
#elif __STDC_VERSION__!=199901L
#define inline
#define restrict
#endif

#endif