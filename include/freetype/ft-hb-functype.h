/****************************************************************************
 *
 * ft-hb-functype.h
 *
 *   FreeType-HarfBuzz bridge (specification).
 *
 * Copyright (C) 2025 by
 * 6ziv.
 *
 * This file is part of the FreeType project, and may only be used,
 * modified, and distributed under the terms of the FreeType project
 * license, LICENSE.TXT.  By continuing to use, modify, or distribute
 * this file you indicate that you have read the license and
 * understand and accept it fully.
 *
 *
 * Originally located in src/autofit/ft-hb.h
 * Move to a public header so that external projects can rely on this.
 *
 */

#ifndef FT_HB_FUNCTYPE_H
#define FT_HB_FUNCTYPE_H

#include <freetype/ft-hb-types.h>
#if defined( FT_CONFIG_OPTION_USE_HARFBUZZ_DYNAMIC )   || \
    defined( FT_CONFIG_OPTION_USE_HARFBUZZ_CALLBACKS )

#include <freetype/config/ftoption.h>

#if defined(FT_CONFIG_OPTION_USE_HARFBUZZ_CALLING_CONVENTION_CDECL)

#if defined( __GNUC__ )
#define FT_HB_CALLBACK
#elif defined( _MSC_VER)
#define FT_HB_CALLBACK __cdecl
#endif

#elif defined(FT_CONFIG_OPTION_USE_HARFBUZZ_CALLING_CONVENTION_STDCALL) /* FT_CONFIG_OPTION_USE_HARFBUZZ_CALLING_CONVENTION_CDECL */

#if defined( __GNUC__ )
#define FT_HB_CALLBACK __attribute__((stdcall))
#elif defined( _MSC_VER)
#define FT_HB_CALLBACK __stdcall
#endif

#endif /* FT_CONFIG_OPTION_USE_HARFBUZZ_CALLING_CONVENTION_CDECL */

#ifndef FT_HB_CALLBACK
#define FT_HB_CALLBACK
#endif

#define FT_HB_EXTERN( ret, name, args ) \
  typedef ret (FT_HB_CALLBACK *ft_ ## name ## _func_t) args;
#include <freetype/ft-hb-decls.h>
#undef FT_HB_EXTERN

typedef struct ft_hb_funcs_t
{

    #define FT_HB_EXTERN( ret, name, args ) \
  ft_ ## name ## _func_t  name;
#include <freetype/ft-hb-decls.h>
#undef FT_HB_EXTERN

} ft_hb_funcs_t;

#endif

#endif /* FT_HB_FUNCTYPE_H */