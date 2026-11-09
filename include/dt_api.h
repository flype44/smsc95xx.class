/* SPDX-License-Identifier: MPL-2.0 */
/*
 * dt_api.h - call stubs for devicetree.resource (Emu68).
 *
 * The resource has no Open/Close/Expunge vectors: the first function sits at
 * -6 (see components/devicetree.resource/sfd/devicetree_lib.sfd, "bias 6").
 * The base returned by OpenResource() is passed explicitly.
 */

#ifndef DT_API_H
#define DT_API_H

#include <exec/types.h>

#define DT_LVO_OPENKEY          6
#define DT_LVO_CLOSEKEY         12
#define DT_LVO_FINDPROPERTY     24
#define DT_LVO_GETPROPLEN       36
#define DT_LVO_GETPROPVALUE     48
#define DT_LVO_FINDPROPRECURSE  66

static inline APTR DT_OpenKey(APTR b, CONST_STRPTR name)
{
    register APTR a6 __asm("a6") = b;
    register CONST_STRPTR a0 __asm("a0") = name;
    register APTR d0 __asm("d0");
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "=r"(d0), "+r"(a0)
        : "r"(a6), [lvo] "i"(DT_LVO_OPENKEY)
        : "d1", "a1", "memory", "cc");
    return d0;
}

static inline void DT_CloseKey(APTR b, APTR key)
{
    register APTR a6 __asm("a6") = b;
    register APTR a0 __asm("a0") = key;
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "+r"(a0)
        : "r"(a6), [lvo] "i"(DT_LVO_CLOSEKEY)
        : "d0", "d1", "a1", "memory", "cc");
}

static inline APTR DT_FindProperty(APTR b, APTR key, CONST_STRPTR name)
{
    register APTR a6 __asm("a6") = b;
    register APTR a0 __asm("a0") = key;
    register CONST_STRPTR a1 __asm("a1") = name;
    register APTR d0 __asm("d0");
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "=r"(d0), "+r"(a0), "+r"(a1)
        : "r"(a6), [lvo] "i"(DT_LVO_FINDPROPERTY)
        : "d1", "memory", "cc");
    return d0;
}

static inline ULONG DT_GetPropLen(APTR b, APTR prop)
{
    register APTR a6 __asm("a6") = b;
    register APTR a0 __asm("a0") = prop;
    register ULONG d0 __asm("d0");
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "=r"(d0), "+r"(a0)
        : "r"(a6), [lvo] "i"(DT_LVO_GETPROPLEN)
        : "d1", "a1", "memory", "cc");
    return d0;
}

static inline CONST_APTR DT_GetPropValue(APTR b, APTR prop)
{
    register APTR a6 __asm("a6") = b;
    register APTR a0 __asm("a0") = prop;
    register CONST_APTR d0 __asm("d0");
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "=r"(d0), "+r"(a0)
        : "r"(a6), [lvo] "i"(DT_LVO_GETPROPVALUE)
        : "d1", "a1", "memory", "cc");
    return d0;
}

static inline APTR DT_FindPropertyRecursive(APTR b, APTR key, CONST_STRPTR name)
{
    register APTR a6 __asm("a6") = b;
    register APTR a0 __asm("a0") = key;
    register CONST_STRPTR a1 __asm("a1") = name;
    register APTR d0 __asm("d0");
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "=r"(d0), "+r"(a0), "+r"(a1)
        : "r"(a6), [lvo] "i"(DT_LVO_FINDPROPRECURSE)
        : "d1", "memory", "cc");
    return d0;
}

#endif /* DT_API_H */
