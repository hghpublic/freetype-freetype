/****************************************************************************
 *
 * ttvarc.c
 *
 *   TrueType and OpenType VARC (Variable Composites) support (body).
 *
 * Copyright (C) 2025 by
 * Behdad Esfahbod and David Turner, Robert Wilhelm, and Werner Lemberg.
 *
 * This file is part of the FreeType project, and may only be used,
 * modified, and distributed under the terms of the FreeType project
 * license, LICENSE.TXT.  By continuing to use, modify, or distribute
 * this file you indicate that you have read the license and
 * understand and accept it fully.
 *
 */


  /**************************************************************************
   *
   * VARC table specification:
   *
   *   https://github.com/harfbuzz/boring-expansion-spec/blob/main/VARC.md
   *
   */


#include <freetype/internal/ftdebug.h>
#include <freetype/internal/ftstream.h>
#include <freetype/internal/ftcalc.h>
#include <freetype/internal/ftobjs.h>
#include <freetype/tttags.h>
#include <freetype/ftcolor.h>
#include <freetype/ftmm.h>
#include <freetype/fttrigon.h>
#include <freetype/ftoutln.h>
#include <ft2build.h>

#ifdef TT_CONFIG_OPTION_VARC

#include "ttvarc.h"
#include "ttload.h"

#ifdef TT_CONFIG_OPTION_GX_VAR_SUPPORT
#include "../truetype/ttgxvar.h"
#endif


  /**************************************************************************
   *
   * The macro FT_COMPONENT is used in trace mode.  It is an implicit
   * parameter of the FT_TRACE() and FT_ERROR() macros, used to print/log
   * messages during execution.
   */
#undef  FT_COMPONENT
#define FT_COMPONENT  ttvarc


  /* Macro for bounds checking when reading from table */
#define CHECK_TABLE_BOUNDS( p, size )                                        \
          ( (FT_Byte*)(p) >= (FT_Byte*)varc->table &&                        \
            (FT_Byte*)(p) + (size) <= (FT_Byte*)varc->table + varc->table_size )

  /* Stack allocation limits for performance */
#define VARC_STACK_AXIS_COUNT      64
#define VARC_STACK_DELTA_COUNT     128
#define VARC_STACK_COORD_COUNT     64
#define VARC_STACK_INDICES_COUNT   64


  /**************************************************************************
   *
   * Utility Functions
   *
   */

  /**************************************************************************
   *
   * @Function:
   *   read_uint32var
   *
   * @Description:
   *   Reads a variable-length uint32 value (1-5 bytes).
   *   Similar to UTF-8 encoding but for integers.
   *
   * @Input:
   *   p ::
   *     Pointer to the byte stream (will be advanced).
   *
   *   limit ::
   *     Maximum valid address in the stream.
   *
   * @Output:
   *   value ::
   *     The decoded uint32 value.
   *
   * @Return:
   *   FreeType error code. 0 means success.
   */
  static FT_Error
  read_uint32var( FT_Byte**  p,
                  FT_Byte*   limit,
                  FT_UInt32* value )
  {
    FT_Byte   b0;
    FT_UInt32 result;


    if ( *p >= limit )
      return FT_THROW( Invalid_Table );

    b0 = *(*p)++;

    /* Single byte: 0xxxxxxx */
    if ( b0 < 0x80 )
    {
      *value = b0;
      return FT_Err_Ok;
    }

    /* Two bytes: 10xxxxxx xxxxxxxx */
    if ( b0 < 0xC0 )
    {
      if ( *p >= limit )
        return FT_THROW( Invalid_Table );
      result = ( ( b0 - 0x80 ) << 8 ) | *(*p)++;
      *value = result;
      return FT_Err_Ok;
    }

    /* Three bytes: 110xxxxx xxxxxxxx xxxxxxxx */
    if ( b0 < 0xE0 )
    {
      if ( *p + 1 >= limit )
        return FT_THROW( Invalid_Table );
      result = ( ( b0 - 0xC0 ) << 16 ) | ( (*p)[0] << 8 ) | (*p)[1];
      *p += 2;
      *value = result;
      return FT_Err_Ok;
    }

    /* Four bytes: 1110xxxx xxxxxxxx xxxxxxxx xxxxxxxx */
    if ( b0 < 0xF0 )
    {
      if ( *p + 2 >= limit )
        return FT_THROW( Invalid_Table );
      result = ( ( b0 - 0xE0 ) << 24 ) | ( (*p)[0] << 16 ) | ( (*p)[1] << 8 ) | (*p)[2];
      *p += 3;
      *value = result;
      return FT_Err_Ok;
    }

    /* Five bytes: 11110000 xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx */
    if ( b0 == 0xF0 )
    {
      if ( *p + 3 >= limit )
        return FT_THROW( Invalid_Table );
      result = ( (*p)[0] << 24 ) | ( (*p)[1] << 16 ) | ( (*p)[2] << 8 ) | (*p)[3];
      *p += 4;
      *value = result;
      return FT_Err_Ok;
    }

    /* Invalid encoding */
    return FT_THROW( Invalid_Table );
  }


  /**************************************************************************
   *
   * @Function:
   *   read_fword
   *
   * @Description:
   *   Reads a signed 16-bit FWORD value.
   *
   * @Input:
   *   p ::
   *     Pointer to the byte stream (will be advanced).
   *
   *   limit ::
   *     Maximum valid address in the stream.
   *
   * @Output:
   *   value ::
   *     The decoded FWORD value as 26.6 fixed-point.
   *
   * @Return:
   *   FreeType error code. 0 means success.
   */
  static FT_Error
  read_fword( FT_Byte**  p,
              FT_Byte*   limit,
              FT_Pos*    value )
  {
    FT_Short  val;


    if ( *p + 1 >= limit )
      return FT_THROW( Invalid_Table );

    val = (FT_Short)( ( (*p)[0] << 8 ) | (*p)[1] );
    *p += 2;

    /* FWORD is a signed 16-bit integer, shift left by 6 for FT_Pos (26.6 format) */
    *value = (FT_Pos)val << 6;

    return FT_Err_Ok;
  }


  /**************************************************************************
   *
   * @Function:
   *   read_f4dot12
   *
   * @Description:
   *   Reads a signed F4DOT12 value (4.12 fixed-point).
   *
   * @Input:
   *   p ::
   *     Pointer to the byte stream (will be advanced).
   *
   *   limit ::
   *     Maximum valid address in the stream.
   *
   * @Output:
   *   value ::
   *     The decoded value as 16.16 fixed-point.
   *
   * @Return:
   *   FreeType error code. 0 means success.
   */
  static FT_Error
  read_f4dot12( FT_Byte**  p,
                FT_Byte*   limit,
                FT_Fixed*  value )
  {
    FT_Short  val;


    if ( *p + 1 >= limit )
      return FT_THROW( Invalid_Table );

    val = (FT_Short)( ( (*p)[0] << 8 ) | (*p)[1] );
    *p += 2;

    /* Convert F4DOT12 (4.12) to F16DOT16 (16.16) by shifting left 4 */
    *value = (FT_Fixed)val << 4;

    return FT_Err_Ok;
  }


  /**************************************************************************
   *
   * @Function:
   *   read_f6dot10
   *
   * @Description:
   *   Reads a signed F6DOT10 value (6.10 fixed-point).
   *
   * @Input:
   *   p ::
   *     Pointer to the byte stream (will be advanced).
   *
   *   limit ::
   *     Maximum valid address in the stream.
   *
   * @Output:
   *   value ::
   *     The decoded value as 16.16 fixed-point.
   *
   * @Return:
   *   FreeType error code. 0 means success.
   */
  static FT_Error
  read_f6dot10( FT_Byte**  p,
                FT_Byte*   limit,
                FT_Fixed*  value )
  {
    FT_Short  val;


    if ( *p + 1 >= limit )
      return FT_THROW( Invalid_Table );

    val = (FT_Short)( ( (*p)[0] << 8 ) | (*p)[1] );
    *p += 2;

    /* Simply shift left by 6 */
    *value = (FT_Fixed)val << 6;

    return FT_Err_Ok;
  }
  /**************************************************************************
   *
   * Coverage Table Functions
   *
   */

  /**************************************************************************
   *
   * @Function:
   *   tt_varc_get_coverage
   *
   * @Description:
   *   Checks if a glyph is covered by the VARC table using binary search.
   *
   * @Input:
   *   varc ::
   *     The VARC table structure.
   *
   *   glyph_index ::
   *     The glyph ID to check.
   *
   * @Return:
   *   Coverage index if found, -1 otherwise.
   */
  static FT_Int
  tt_varc_get_coverage( TT_Varc  varc,
                        FT_UInt  glyph_index )
  {
    FT_Byte*  p;
    FT_UInt   format;
    FT_UInt   count;
    FT_UInt   i;


    if ( !varc || !varc->coverage )
      return -1;

    p = varc->coverage;

    /* Check bounds for format and count */
    if ( !CHECK_TABLE_BOUNDS( p, 4 ) )
      return -1;

    format = ( p[0] << 8 ) | p[1];
    p += 2;

    /* Format 1: List of glyph IDs */
    if ( format == 1 )
    {
      count = ( p[0] << 8 ) | p[1];
      p += 2;

      if ( !CHECK_TABLE_BOUNDS( p, count * 2 ) )
        return -1;

      /* Binary search */
      {
        FT_UInt  min = 0;
        FT_UInt  max = count;

        while ( min < max )
        {
          FT_UInt   mid = ( min + max ) >> 1;
          FT_Byte*  gid_p = p + mid * 2;
          FT_UInt   gid = ( gid_p[0] << 8 ) | gid_p[1];

          if ( gid == glyph_index )
            return (FT_Int)mid;
          else if ( gid < glyph_index )
            min = mid + 1;
          else
            max = mid;
        }
      }
    }
    /* Format 2: Range list */
    else if ( format == 2 )
    {
      FT_UInt  coverage_index = 0;

      count = ( p[0] << 8 ) | p[1];
      p += 2;

      if ( !CHECK_TABLE_BOUNDS( p, count * 6 ) )
        return -1;

      for ( i = 0; i < count; i++ )
      {
        FT_UInt  start_glyph = ( p[0] << 8 ) | p[1];
        FT_UInt  end_glyph   = ( p[2] << 8 ) | p[3];
        p += 6; /* skip startCoverageIndex */

        if ( glyph_index >= start_glyph && glyph_index <= end_glyph )
          return (FT_Int)( coverage_index + ( glyph_index - start_glyph ) );

        coverage_index += end_glyph - start_glyph + 1;
      }
    }

    return -1;
  }


  /**************************************************************************
   *
   * CFF2IndexOf Functions
   *
   */

  /**************************************************************************
   *
   * @Function:
   *   tt_varc_get_glyph_record
   *
   * @Description:
   *   Gets the glyph record from the CFF2IndexOf structure.
   *
   * @Input:
   *   varc ::
   *     The VARC table structure.
   *
   *   glyph_index ::
   *     The glyph ID.
   *
   * @Output:
   *   record_data ::
   *     Pointer to the glyph record data.
   *
   *   record_size ::
   *     Size of the glyph record in bytes.
   *
   * @Return:
   *   FreeType error code. 0 means success.
   */
  static FT_Error
  tt_varc_get_glyph_record( TT_Varc    varc,
                            FT_UInt    glyph_index,
                            FT_Byte**  record_data,
                            FT_UInt*   record_size )
  {
    FT_Byte*  p;
    FT_UInt   count;
    FT_Int    coverage_index;
    if ( !varc || !varc->var_composite_glyphs )
    {
      return FT_THROW( Invalid_Table );
    }

    /* Get coverage index */
    coverage_index = tt_varc_get_coverage( varc, glyph_index );
    if ( coverage_index < 0 )
      return FT_THROW( Invalid_Glyph_Index );

    /* CFF2Index structure: count (uint32) + offSize (uint8) + offsets + data */
    p = varc->var_composite_glyphs;

    /* Count field is uint32 (not uint32var!) */
    {
      FT_UInt32  count32;

      if ( !CHECK_TABLE_BOUNDS( p, 4 ) )
      {
        return FT_THROW( Invalid_Table );
      }

      /* Read big-endian uint32 */
      count32 = ( (FT_UInt32)p[0] << 24 ) | ( (FT_UInt32)p[1] << 16 ) |
                ( (FT_UInt32)p[2] << 8 )  | (FT_UInt32)p[3];
      p += 4;

      count = (FT_UInt)count32;

      if ( (FT_UInt)coverage_index >= count )
      {
        return FT_THROW( Invalid_Glyph_Index );
      }

      /* Read offSize */
      if ( !CHECK_TABLE_BOUNDS( p, 1 ) )
        return FT_THROW( Invalid_Table );

      FT_UInt  offSize = *p++;
      if ( offSize < 1 || offSize > 4 )
        return FT_THROW( Invalid_Table );

      /* Read offsets */
      if ( !CHECK_TABLE_BOUNDS( p, ( count + 1 ) * offSize ) )
      {
        return FT_THROW( Invalid_Table );
      }

      {
        FT_ULong  offset1 = 0, offset2 = 0;
        FT_Byte*  offset_base = p + ( count + 1 ) * offSize;
        FT_Byte*  p_offset;
        /* Debug: show offsets around coverage_index */
        if ( coverage_index > 0 && (FT_UInt)(coverage_index - 1) < count )
        {
          FT_ULong  prev_off1 = 0, prev_off2 = 0;
          FT_Byte*  prev_p = p + (coverage_index - 1) * offSize;
          for ( FT_UInt i = 0; i < offSize; i++ )
            prev_off1 = (prev_off1 << 8) | prev_p[i];
          prev_p += offSize;
          for ( FT_UInt i = 0; i < offSize; i++ )
            prev_off2 = (prev_off2 << 8) | prev_p[i];
        }

        /* Read offset for this glyph */
        p_offset = p + coverage_index * offSize;
        for ( FT_UInt i = 0; i < offSize; i++ )
          offset1 = (offset1 << 8) | p_offset[i];

        /* Read next offset */
        p_offset += offSize;
        for ( FT_UInt i = 0; i < offSize; i++ )
          offset2 = (offset2 << 8) | p_offset[i];

        if ( offset2 < offset1 || offset2 - offset1 > 0xFFFFU )
        {
          return FT_THROW( Invalid_Table );
        }

        /* CFF2Index offsets are 1-based, so subtract 1 to get 0-based data pointer */
        *record_size = (FT_UInt)( offset2 - offset1 );
        *record_data = offset_base + offset1 - 1;

        if ( !CHECK_TABLE_BOUNDS( *record_data, *record_size ) )
          return FT_THROW( Invalid_Table );
      }

      return FT_Err_Ok;
    }

    return FT_THROW( Invalid_Table );
  }


  /**************************************************************************
   *
   * Component Parsing Functions
   *
   */

  /**************************************************************************
   *
   * @Function:
   *   tt_varc_parse_component
   *
   * @Description:
   *   Parses a single VARC component record.
   *
   * @Input:
   *   face ::
   *     The font face.
   *
   *   varc ::
   *     The VARC table structure.
   *
   *   p ::
   *     Pointer to component data (will be advanced).
   *
   *   limit ::
   *     End of component data.
   *
   * @Output:
   *   component ::
   *     The parsed component structure.
   *
   * @Return:
   *   FreeType error code. 0 means success.
   */
  static FT_Error
  tt_varc_parse_component( TT_Face            face,
                           TT_Varc            varc,
                           FT_Byte**          p,
                           FT_Byte*           limit,
                           TT_VarcComponent   component,
                           FT_Fixed*          axis_values_buffer,
                           FT_UInt            axis_values_buffer_size )
  {
    FT_Error   error;
    FT_UInt32  flags32;
    FT_Memory  memory = face->root.memory;


    FT_ZERO( component );
    /* Read flags (uint32var) */
    error = read_uint32var( p, limit, &flags32 );
    if ( error )
    {
      return error;
    }

    component->flags = flags32;
    /* Read GID */
    if ( component->flags & VARC_GID_IS_24BIT )
    {
      if ( *p + 2 >= limit )
        return FT_THROW( Invalid_Table );
      component->gid = ( (*p)[0] << 16 ) | ( (*p)[1] << 8 ) | (*p)[2];
      *p += 3;
    }
    else
    {
      if ( *p + 1 >= limit )
        return FT_THROW( Invalid_Table );
      component->gid = ( (*p)[0] << 8 ) | (*p)[1];
      *p += 2;
    }


    /* Validate GID - for now just warn, don't fail */
    if ( component->gid >= (FT_UInt)face->root.num_glyphs )
    {
      /* For now, don't fail - the font might be using extended GID space */
      /* return FT_THROW( Invalid_Glyph_Index ); */
    }

    /* Read condition index */
    if ( component->flags & VARC_HAVE_CONDITION )
    {
      error = read_uint32var( p, limit, &component->condition_index );
      if ( error )
        return error;
    }

    /* Read axis indices index */
    if ( component->flags & VARC_HAVE_AXES )
    {
      error = read_uint32var( p, limit, &component->axis_indices_index );
      if ( error )
        return error;
    }

    /* Read axis values (TupleValues) */
    if ( component->flags & VARC_HAVE_AXES )
    {
      FT_UInt   axis_count = 0;
      FT_Byte*  axis_indices_list;
      FT_Byte*  ai_p;
      FT_UInt32 tuple_count;
      FT_UInt   i;


      /* Get axis indices from the axis indices list */
      if ( !varc->axis_indices_list )
        return FT_THROW( Invalid_Table );

      axis_indices_list = varc->axis_indices_list;
      /* Read count - use table limit, not component limit */
      /* axis_indices_list is a CFF2Index, so count is fixed uint32 (big-endian) */
      {
        FT_Byte*  table_limit = (FT_Byte*)varc->table + varc->table_size;


        ai_p = axis_indices_list;

        /* Read big-endian uint32 count */
        if ( ai_p + 4 > table_limit )
        {
          return FT_THROW( Invalid_Table );
        }

        tuple_count = ( (FT_UInt32)ai_p[0] << 24 ) | ( (FT_UInt32)ai_p[1] << 16 ) |
                      ( (FT_UInt32)ai_p[2] << 8 )  | (FT_UInt32)ai_p[3];
        ai_p += 4;
        if ( component->axis_indices_index >= tuple_count )
        {
          /* Likely reached end of valid component data */
          /* Return a special error code that the caller can interpret as "end of components" */
          return FT_THROW( Invalid_Table );
        }

        /* Read offSize */
        if ( ai_p + 1 > table_limit )
          return FT_THROW( Invalid_Table );

        FT_UInt  offSize = *ai_p++;
        if ( offSize < 1 || offSize > 4 )
          return FT_THROW( Invalid_Table );

        /* Check bounds for offsets array */
        if ( ai_p + ( tuple_count + 1 ) * offSize > table_limit )
          return FT_THROW( Invalid_Table );

        /* Read offset for our tuple */
        {
          FT_Byte*  p_offset = ai_p + component->axis_indices_index * offSize;
          FT_Byte*  offset_base = ai_p + ( tuple_count + 1 ) * offSize;
          FT_ULong  offset1 = 0, offset2 = 0;


          for ( FT_UInt i = 0; i < offSize; i++ )
            offset1 = ( offset1 << 8 ) | p_offset[i];

          p_offset += offSize;
          for ( FT_UInt i = 0; i < offSize; i++ )
            offset2 = ( offset2 << 8 ) | p_offset[i];
          if ( offset2 < offset1 )
          {
            return FT_THROW( Invalid_Table );
          }

          /* Decode axis indices from TupleValues (packed format) */
          /* The data between offset1 and offset2 is encoded as TupleValues */
          /* We need to decode it to count how many axis indices there are */
          {
            FT_Byte*  tuple_data = offset_base + offset1 - 1;  /* CFF2Index offsets are 1-based */
            FT_Byte*  tuple_limit = offset_base + offset2 - 1;
            FT_Byte*  tp = tuple_data;

            axis_count = 0;
            /* Decode TupleValues just to count the values */
            while ( tp < tuple_limit )
            {
              FT_Byte  control;
              FT_UInt  cnt;

              if ( tp >= tuple_limit )
                break;
              control = *tp++;

              cnt = ( control & 0x3F ) + 1;

              /* Skip the data bytes based on encoding type */
              if ( ( control & 0xC0 ) == 0xC0 )
              {
                /* 32-bit values */
                if ( tp + 4 * cnt > tuple_limit )
                  break;
                tp += 4 * cnt;
              }
              else if ( control & 0x80 )
              {
                /* Zeros - no data bytes */
              }
              else if ( control & 0x40 )
              {
                /* 2-byte values */
                if ( tp + 2 * cnt > tuple_limit )
                  break;
                tp += 2 * cnt;
              }
              else
              {
                /* 1-byte values */
                if ( tp + cnt > tuple_limit )
                  break;
                tp += cnt;
              }

              axis_count += cnt;
            }
          }
        }
      }

      component->num_axis_values = axis_count;

      if ( axis_count > 0 )
      {
        FT_UInt    j;  /* Declare here for use both in loop and after */

        /* Use caller's stack buffer for small arrays, heap for large */
        if ( axis_count <= axis_values_buffer_size )
        {
          component->axis_values = axis_values_buffer;
          component->axis_values_on_heap = FALSE;
        }
        else
        {
          if ( FT_NEW_ARRAY( component->axis_values, axis_count ) )
            return error;
          component->axis_values_on_heap = TRUE;
        }

        /* Read axis values from TupleValues (Packed Deltas format) */
        i = 0;
        while ( i < axis_count )
        {
          FT_Byte  control;
          FT_UInt  cnt;

          /* Read control byte */
          if ( *p >= limit )
            goto Fail;
          control = *(*p)++;

          /* Extract run count (lower 6 bits), add 1 */
          cnt = ( control & 0x3F ) + 1;
          if ( cnt > axis_count - i )
            cnt = axis_count - i;

          /* Check top 2 bits for encoding type */
          if ( ( control & 0xC0 ) == 0xC0 )
          {
            /* Both bits set = 32-bit signed integers */
            if ( *p + 4 * cnt > limit )
              goto Fail;
            for ( j = 0; j < cnt; j++ )
            {
              /* Read as signed 32-bit integer, reinterpret as F2DOT14 -> F16DOT16 */
              FT_Int32  val = (FT_Int32)( ( (*p)[0] << 24 ) | ( (*p)[1] << 16 ) |
                                          ( (*p)[2] << 8 ) | (*p)[3] );
              component->axis_values[i++] = (FT_Fixed)( val << 2 );
              *p += 4;
            }
          }
          else if ( control & 0x80 )
          {
            /* Bit 7 set = zeros */
            for ( j = 0; j < cnt; j++ )
              component->axis_values[i++] = 0;
          }
          else if ( control & 0x40 )
          {
            /* Bit 6 set = 2-byte signed integers */
            if ( *p + 2 * cnt > limit )
              goto Fail;
            for ( j = 0; j < cnt; j++ )
            {
              /* Read as signed 16-bit integer, reinterpret as F2DOT14 -> F16DOT16 */
              FT_Short  val = (FT_Short)( ( (*p)[0] << 8 ) | (*p)[1] );
              component->axis_values[i++] = (FT_Fixed)( val << 2 );
              *p += 2;
            }
          }
          else
          {
            /* Neither bit set = 1-byte signed integers */
            if ( *p + cnt > limit )
              goto Fail;
            for ( j = 0; j < cnt; j++ )
            {
              /* Read signed byte integer, reinterpret as F2DOT14 -> F16DOT16 */
              FT_Char  val = (FT_Char)(*(*p)++);
              component->axis_values[i++] = (FT_Fixed)( val << 2 );
            }
          }
        }
        /* Print base axis values before any variations */
        for ( j = 0; j < axis_count; j++ )
        {
        }
      }
    }

    /* Read axis values variation index */
    if ( component->flags & VARC_AXES_HAVE_VARIATION )
    {
      error = read_uint32var( p, limit, &component->axis_values_var_index );
      if ( error )
        return error;
    }

    /* Read transform variation index */
    if ( component->flags & VARC_TRANSFORM_HAS_VARIATION )
    {
      error = read_uint32var( p, limit, &component->transform_var_index );
      if ( error )
        return error;
    }

    /* Initialize transform to identity */
    component->translate_x = 0;
    component->translate_y = 0;
    component->rotation = 0;
    component->scale_x = 0x10000;  /* 1.0 in 16.16 */
    component->scale_y = 0x10000;  /* 1.0 in 16.16 */
    component->skew_x = 0;
    component->skew_y = 0;
    component->tcenter_x = 0;
    component->tcenter_y = 0;

    /* Read transform components - each field is optional based on flags */
    if ( component->flags & VARC_HAVE_TRANSLATE_X )
    {
      error = read_fword( p, limit, &component->translate_x );
      if ( error )
        goto Fail;
    }

    if ( component->flags & VARC_HAVE_TRANSLATE_Y )
    {
      error = read_fword( p, limit, &component->translate_y );
      if ( error )
        goto Fail;
    }

    if ( component->flags & VARC_HAVE_ROTATION )
    {
      error = read_f4dot12( p, limit, &component->rotation );
      if ( error )
        goto Fail;
    }

    if ( component->flags & VARC_HAVE_SCALE_X )
    {
      error = read_f6dot10( p, limit, &component->scale_x );
      if ( error )
        goto Fail;
    }

    if ( component->flags & VARC_HAVE_SCALE_Y )
    {
      error = read_f6dot10( p, limit, &component->scale_y );
      if ( error )
        goto Fail;
    }
    else if ( component->flags & VARC_HAVE_SCALE_X )
    {
      /* ScaleY defaults to ScaleX if ScaleX is present */
      component->scale_y = component->scale_x;
    }

    if ( component->flags & VARC_HAVE_SKEW_X )
    {
      error = read_f4dot12( p, limit, &component->skew_x );
      if ( error )
        goto Fail;
    }

    if ( component->flags & VARC_HAVE_SKEW_Y )
    {
      error = read_f4dot12( p, limit, &component->skew_y );
      if ( error )
        goto Fail;
    }

    if ( component->flags & VARC_HAVE_TCENTER_X )
    {
      error = read_fword( p, limit, &component->tcenter_x );
      if ( error )
        goto Fail;
    }

    if ( component->flags & VARC_HAVE_TCENTER_Y )
    {
      error = read_fword( p, limit, &component->tcenter_y );
      if ( error )
        goto Fail;
    }

    return FT_Err_Ok;

  Fail:
    if ( component->axis_values && component->axis_values_on_heap )
      FT_FREE( component->axis_values );
    return error;
  }


  /**************************************************************************
   *
   * @Function:
   *   tt_varc_free_component
   *
   * @Description:
   *   Frees resources allocated for a component.
   *
   * @Input:
   *   face ::
   *     The font face.
   *
   *   component ::
   *     The component to free.
   */
  static void
  tt_varc_free_component( TT_Face           face,
                          TT_VarcComponent  component )
  {
    FT_Memory  memory = face->root.memory;


    if ( component->axis_values && component->axis_values_on_heap )
      FT_FREE( component->axis_values );
  }


  /**************************************************************************
   *
   * MultiItemVariationStore Functions
   *
   */

  /**************************************************************************
   *
   * @Function:
   *   tt_varc_evaluate_region
   *
   * @Description:
   *   Evaluates a variation region against current coordinates.
   *   Returns a scalar value (0.0 to 1.0) indicating how much this
   *   region contributes at the current variation location.
   *
   * @Input:
   *   region_data ::
   *     Pointer to region data (axis ranges).
   *
   *   limit ::
   *     End of valid data.
   *
   *   coords ::
   *     Current normalized coordinates (F2DOT14).
   *
   *   coord_count ::
   *     Number of coordinates.
   *
   * @Return:
   *   Scalar value as FT_Fixed (16.16 format), 0 to 0x10000 (1.0).
   */
  static FT_Fixed
  tt_varc_evaluate_region( FT_Byte*   region_data,
                           FT_Byte*   limit,
                           FT_Fixed*  coords,
                           FT_UInt    coord_count )
  {
    FT_Byte*  p = region_data;
    FT_Fixed  scalar = 0x10000;  /* Start with 1.0 in 16.16 */
    FT_UInt   axis_count;
    FT_UInt   i;


    if ( !coords || coord_count == 0 )
      return scalar;  /* No coords available, return 1.0 */

    /* SparseVarRegionList region format:
     * regionAxisCount (u16) - number of non-default axes
     * For each sparse axis:
     *   axisIndex (u16)
     *   startCoord (F2DOT14)
     *   peakCoord (F2DOT14)
     *   endCoord (F2DOT14)
     */

    if ( p + 2 > limit )
      return 0;

    /* Read sparse axis count (u16) */
    axis_count = ( (FT_UInt)p[0] << 8 ) | p[1];
    p += 2;


    /* Process each axis range */
    for ( i = 0; i < axis_count; i++ )
    {
      FT_UInt16 axis_index;
      FT_Short  min_val, peak_val, max_val;
      FT_Fixed  coord;
      FT_Fixed  axis_scalar;

      /* Read axis index (u16) */
      if ( p + 8 > limit )
        return 0;

      axis_index = ( (FT_UInt16)p[0] << 8 ) | p[1];
      p += 2;

      /* Read min, peak, max (F2DOT14) - each is 2 bytes, advance after each read */
      min_val  = (FT_Short)( ( p[0] << 8 ) | p[1] );
      p += 2;
      peak_val = (FT_Short)( ( p[0] << 8 ) | p[1] );  /* Fixed: read from p[0] after advancing */
      p += 2;
      max_val  = (FT_Short)( ( p[0] << 8 ) | p[1] );  /* Fixed: read from p[0] after advancing */
      p += 2;

      /* Get coordinate for this axis */
      if ( axis_index >= coord_count )
        continue;  /* Skip unknown axes */

      coord = coords[axis_index];

      /* Convert F2DOT14 to F16DOT16 for comparison */
      FT_Fixed min_16  = (FT_Fixed)min_val  << 2;
      FT_Fixed peak_16 = (FT_Fixed)peak_val << 2;
      FT_Fixed max_16  = (FT_Fixed)max_val  << 2;


      /* Evaluate this axis */
      if ( coord < min_16 || coord > max_16 )
      {
        /* Outside range - region doesn't contribute */
        return 0;
      }
      else if ( coord == peak_16 )
      {
        /* At peak - full contribution for this axis */
        axis_scalar = 0x10000;  /* 1.0 */
      }
      else if ( coord < peak_16 )
      {
        /* Between min and peak - interpolate */
        if ( peak_16 == min_16 )
          axis_scalar = 0x10000;
        else
          axis_scalar = FT_DivFix( coord - min_16, peak_16 - min_16 );
      }
      else  /* coord > peak_16 */
      {
        /* Between peak and max - interpolate */
        if ( max_16 == peak_16 )
          axis_scalar = 0x10000;
        else
          axis_scalar = FT_DivFix( max_16 - coord, max_16 - peak_16 );
      }

      /* Multiply into overall scalar */
      scalar = FT_MulFix( scalar, axis_scalar );


      /* Early exit if scalar becomes 0 */
      if ( scalar == 0 )
        return 0;
    }

    return scalar;
  }


  /**************************************************************************
   *
   * @Function:
   *   tt_varc_read_index2_data
   *
   * @Description:
   *   Reads data from a CFF2-style INDEX at a given index.
   *   INDEX2 format: count(u32) + offSize(u8) + offsets[] + data
   *
   * @Input:
   *   index_data ::
   *     Pointer to the INDEX2 structure.
   *
   *   table_limit ::
   *     End of valid table data.
   *
   *   index ::
   *     Index of the item to read.
   *
   * @Output:
   *   data ::
   *     Pointer to the data at the given index.
   *
   *   size ::
   *     Size of the data in bytes.
   *
   * @Return:
   *   FreeType error code. 0 means success.
   */
  static FT_Error
  tt_varc_read_index2_data( FT_Byte*   index_data,
                            FT_Byte*   table_limit,
                            FT_UInt    index,
                            FT_Byte**  data,
                            FT_UInt*   size )
  {
    FT_Byte*  p = index_data;
    FT_UInt32 count;
    FT_Byte   offSize;
    FT_ULong  offset1, offset2;
    FT_Byte*  offset_base;
    FT_UInt   i;


    /* Read count (u32) */
    if ( p + 4 > table_limit )
      return FT_THROW( Invalid_Table );

    count = ( (FT_UInt32)p[0] << 24 ) | ( (FT_UInt32)p[1] << 16 ) |
            ( (FT_UInt32)p[2] << 8 )  | (FT_UInt32)p[3];
    p += 4;

    /* If count is 0, there's no offSize or data */
    if ( count == 0 )
      return FT_THROW( Invalid_Table );

    /* Read offSize (u8) */
    if ( p >= table_limit )
      return FT_THROW( Invalid_Table );

    offSize = *p++;

    /* Validate offSize and index */
    if ( offSize == 0 || offSize > 4 || index >= count )
      return FT_THROW( Invalid_Table );

    /* Calculate base for data (after offset array) */
    offset_base = p + (count + 1) * offSize;

    /* Read offset for index and index+1 */
    p += index * offSize;

    if ( p + 2 * offSize > table_limit )
      return FT_THROW( Invalid_Table );

    /* Read offset1 */
    offset1 = 0;
    for ( i = 0; i < offSize; i++ )
      offset1 = (offset1 << 8) | p[i];
    p += offSize;

    /* Read offset2 */
    offset2 = 0;
    for ( i = 0; i < offSize; i++ )
      offset2 = (offset2 << 8) | p[i];

    /* Calculate data pointer and size */
    /* Offsets are 1-based in CFF INDEX */
    *data = offset_base + offset1 - 1;
    *size = (FT_UInt)(offset2 - offset1);

    /* Validate data bounds */
    if ( *data + *size > table_limit )
      return FT_THROW( Invalid_Table );

    return FT_Err_Ok;
  }


  /**************************************************************************
   *
   * MultiItemVariationStore Functions
   *
   */

  /**************************************************************************
   *
   * @Function:
   *   tt_varc_read_tuple_deltas
   *
   * @Description:
   *   Reads a tuple of delta values from TupleValues (packed deltas).
   *   Returns plain integers that will be scaled later.
   *
   * @Input:
   *   data ::
   *     Pointer to TupleValues data.
   *
   *   limit ::
   *     End of valid data.
   *
   *   num_deltas ::
   *     Number of deltas to read.
   *
   * @Output:
   *   deltas ::
   *     Array to store deltas (as plain integers).
   *
   * @Return:
   *   FreeType error code. 0 means success.
   */
  static FT_Error
  tt_varc_read_tuple_deltas( FT_Byte*   data,
                             FT_Byte*   limit,
                             FT_UInt    num_deltas,
                             FT_Int32*  deltas,
                             FT_UInt*   bytes_consumed )
  {
    FT_Byte*  p = data;
    FT_Byte*  start = data;
    FT_UInt   i = 0;


    /* Read deltas using TupleValues (packed delta) format */
    while ( i < num_deltas && p < limit )
    {
      FT_Byte  control;
      FT_UInt  cnt, j;

      /* Read control byte */
      if ( p >= limit )
      {
        return FT_THROW( Invalid_Table );
      }

      control = *p++;

      /* Extract run count (lower 6 bits), add 1 */
      cnt = ( control & 0x3F ) + 1;
      if ( cnt > num_deltas - i )
        cnt = num_deltas - i;

      /* Check top 2 bits for encoding type */
      if ( ( control & 0xC0 ) == 0xC0 )
      {
        /* Both bits set = 32-bit plain integer values */
        if ( p + 4 * cnt > limit )
          return FT_THROW( Invalid_Table );

        for ( j = 0; j < cnt; j++ )
        {
          /* Read as signed 32-bit plain integer */
          FT_Int32  val = (FT_Int32)( ( p[0] << 24 ) | ( p[1] << 16 ) |
                                      ( p[2] << 8 )  | p[3] );
          deltas[i++] = val;  /* Store as plain integer */
          p += 4;
        }
      }
      else if ( control & 0x80 )
      {
        /* Bit 7 set = zeros */
        for ( j = 0; j < cnt; j++ )
          deltas[i++] = 0;
      }
      else if ( control & 0x40 )
      {
        /* Bit 6 set = 2-byte signed integer values */
        if ( p + 2 * cnt > limit )
          return FT_THROW( Invalid_Table );

        for ( j = 0; j < cnt; j++ )
        {
          /* Read as signed 16-bit plain integer */
          FT_Short  val = (FT_Short)( ( p[0] << 8 ) | p[1] );
          deltas[i++] = val;  /* Store as plain integer */
          p += 2;
        }
      }
      else
      {
        /* Neither bit set = 1-byte signed integer values */
        if ( p + cnt > limit )
          return FT_THROW( Invalid_Table );

        for ( j = 0; j < cnt; j++ )
        {
          /* Read signed byte as plain integer */
          FT_Char  val = (FT_Char)(*p++);
          deltas[i++] = val;  /* Store as plain integer */
        }
      }
    }

    /* If we hit the end before reading all deltas, fill remaining with zeros */
    while ( i < num_deltas )
      deltas[i++] = 0;

    /* Return the number of bytes consumed */
    *bytes_consumed = (FT_UInt)( p - start );

    return FT_Err_Ok;
  }


  /**************************************************************************
   *
   * @Function:
   *   tt_varc_get_item_deltas
   *
   * @Description:
   *   Gets a tuple of deltas from the MultiItemVariationStore.
   *   Unlike ItemVariationStore which returns a single delta,
   *   MultiItemVariationStore returns multiple deltas (a tuple).
   *
   * @Input:
   *   face ::
   *     The font face.
   *
   *   varc ::
   *     The VARC table.
   *
   *   var_index ::
   *     Variation index (outer << 16 | inner).
   *
   *   num_deltas ::
   *     Number of deltas expected in the tuple.
   *
   * @Output:
   *   deltas ::
   *     Array to store the deltas (must have space for num_deltas).
   *
   * @Return:
   *   FreeType error code. 0 means success.
   */
  static FT_Error
  tt_varc_get_item_deltas( TT_Face    face,
                           TT_Varc    varc,
                           FT_UInt32  var_index,
                           FT_UInt    num_deltas,
                           FT_Fixed*  deltas,
                           FT_Fixed*  current_coords,
                           FT_UInt    num_coords )
  {
#ifdef TT_CONFIG_OPTION_GX_VAR_SUPPORT
    FT_Error  error;
    FT_Memory memory = face->root.memory;
    FT_Byte*  mvs_data;      /* MultiItemVariationStore */
    FT_Byte*  table_limit;
    FT_UInt16 format;
    FT_UInt32 regions_offset;
    FT_UInt16 data_sets_count;
    FT_UInt   outer, inner;
    FT_Byte*  p;
    FT_Byte*  data_set_offsets;
    FT_UInt32 data_set_offset;
    FT_Byte*  mvd_data;      /* MultiVarData */
    FT_Byte   mvd_format;
    FT_UInt16 region_count;
    FT_Byte*  delta_sets;
    FT_Byte*  tuple_data;
    FT_UInt   tuple_size;
    FT_UInt   i;
    FT_Int64  stack_accumulators[VARC_STACK_DELTA_COUNT];
    FT_Int32  stack_all_deltas[VARC_STACK_DELTA_COUNT * 16];  /* deltas * regions */

    if ( !varc->var_store_loaded || !varc->multi_var_store )
    {
      /* No variation store - return zeros */
      for ( i = 0; i < num_deltas; i++ )
        deltas[i] = 0;
      return FT_Err_Ok;
    }

    mvs_data = varc->multi_var_store;
    table_limit = (FT_Byte*)varc->table + varc->table_size;

    /* Split var_index into outer and inner */
    outer = var_index >> 16;
    inner = var_index & 0xFFFF;


    /* Parse MultiItemVariationStore header */
    p = mvs_data;

    if ( p + 8 > table_limit )
      return FT_THROW( Invalid_Table );

    /* Read format (u16) - must be 1 */
    format = ( (FT_UInt16)p[0] << 8 ) | p[1];
    p += 2;

    if ( format != 1 )
    {
      return FT_THROW( Invalid_Table );
    }

    /* Read regions offset (u32) */
    regions_offset = ( (FT_UInt32)p[0] << 24 ) | ( (FT_UInt32)p[1] << 16 ) |
                     ( (FT_UInt32)p[2] << 8 )  | (FT_UInt32)p[3];
    p += 4;

    FT_UNUSED( regions_offset );  /* TODO: Use for region evaluation */

    /* Read dataSets count (u16) */
    data_sets_count = ( (FT_UInt16)p[0] << 8 ) | p[1];
    p += 2;


    /* Validate outer index */
    if ( outer >= data_sets_count )
    {
      return FT_THROW( Invalid_Table );
    }

    /* Read data set offset for outer index */
    data_set_offsets = p;
    p = data_set_offsets + outer * 4;

    if ( p + 4 > table_limit )
      return FT_THROW( Invalid_Table );

    data_set_offset = ( (FT_UInt32)p[0] << 24 ) | ( (FT_UInt32)p[1] << 16 ) |
                      ( (FT_UInt32)p[2] << 8 )  | (FT_UInt32)p[3];

    /* Get MultiVarData */
    mvd_data = mvs_data + data_set_offset;

    if ( mvd_data + 3 > table_limit )
      return FT_THROW( Invalid_Table );

    /* Parse MultiVarData header */
    p = mvd_data;

    /* Read format (u8) - must be 1 */
    mvd_format = *p++;

    if ( mvd_format != 1 )
    {
      return FT_THROW( Invalid_Table );
    }

    /* Read regionIndices count (u16) */
    region_count = ( (FT_UInt16)p[0] << 8 ) | p[1];
    p += 2;
    /* Skip regionIndices array */
    p += region_count * 2;

    if ( p > table_limit )
      return FT_THROW( Invalid_Table );

    /* Now p points to deltaSets (INDEX2) */
    delta_sets = p;

    /* Read tuple from INDEX2 at inner index */
    error = tt_varc_read_index2_data( delta_sets, table_limit, inner,
                                      &tuple_data, &tuple_size );
    if ( error )
    {
      return error;
    }
    /* Allocate 64-bit accumulators for each delta */
    FT_Int64*  accumulators = NULL;

    if ( num_deltas <= VARC_STACK_DELTA_COUNT )
    {
      accumulators = stack_accumulators;
    }
    else
    {
      if ( FT_NEW_ARRAY( accumulators, num_deltas ) )
      {
        error = FT_THROW( Out_Of_Memory );
        goto Cleanup;
      }
    }

    /* Initialize accumulators to zero */
    for ( i = 0; i < num_deltas; i++ )
      accumulators[i] = FT_INT64_ZERO;

    /* Parse SparseVarRegionList to get region definitions */
    FT_Byte*  regions_data = NULL;
    FT_Byte*  regions_limit = NULL;

    if ( regions_offset > 0 && regions_offset < varc->table_size )
    {
      regions_data = mvs_data + regions_offset;
      regions_limit = table_limit;

      /* Read and skip region count (u16) */
      if ( regions_data + 2 <= regions_limit )
      {
        regions_data += 2;
      }
      else
        regions_data = NULL;
    }

    /* Read ALL deltas from the flat tuple (region_count * num_deltas values) */
    FT_UInt   total_deltas = region_count * num_deltas;
    FT_Int32* all_deltas = NULL;  /* Plain integers, will be scaled later */
    FT_UInt   bytes_consumed = 0;


    /* Allocate space for all deltas */
    if ( total_deltas <= VARC_STACK_DELTA_COUNT * 16 )
    {
      all_deltas = stack_all_deltas;
    }
    else
    {
      if ( FT_NEW_ARRAY( all_deltas, total_deltas ) )
      {
        error = FT_THROW( Out_Of_Memory );
        goto Cleanup;
      }
    }

    /* Read all deltas from TupleValues */
    error = tt_varc_read_tuple_deltas( tuple_data, tuple_data + tuple_size,
                                       total_deltas, all_deltas,
                                       &bytes_consumed );
    if ( error )
    {
      if ( all_deltas != stack_all_deltas )
        FT_FREE( all_deltas );
      if ( accumulators != stack_accumulators )
        FT_FREE( accumulators );
      goto Cleanup;
    }
    /* Parse regionIndices and accumulate scaled deltas */
    FT_Byte*  region_indices_ptr = mvd_data + 3;  /* After format(u8) + region_count(u16) */
    FT_UInt   region_idx;

    for ( region_idx = 0; region_idx < region_count; region_idx++ )
    {
      FT_UInt16 region_index;
      FT_Fixed  region_scalar;
      FT_UInt   k;

      /* Read region index from regionIndices array (u16) */
      if ( region_indices_ptr + 2 > table_limit )
        break;

      region_index = ( (FT_UInt16)region_indices_ptr[0] << 8 ) | region_indices_ptr[1];
      region_indices_ptr += 2;

      /* Evaluate region to get scalar */
      if ( regions_data && current_coords )
      {
        /* SparseVarRegionList format:
         * u16: regionCount
         * u32[regionCount]: array of offsets (from start of list) to each region */

        FT_Byte* region_list_start = regions_data - 2;  /* Back to start (before count) */
        FT_Byte* region_offset_ptr = regions_data + region_index * 4;

        if ( region_offset_ptr + 4 <= regions_limit )
        {
          FT_UInt32 region_offset = ( (FT_UInt32)region_offset_ptr[0] << 24 ) |
                                   ( (FT_UInt32)region_offset_ptr[1] << 16 ) |
                                   ( (FT_UInt32)region_offset_ptr[2] << 8 )  |
                                   (FT_UInt32)region_offset_ptr[3];

          FT_Byte* region_p = region_list_start + region_offset;


          if ( region_p < regions_limit )
          {
            region_scalar = tt_varc_evaluate_region( region_p, regions_limit,
                                                     current_coords, num_coords );
          }
          else
            region_scalar = 0;
        }
        else
          region_scalar = 0;
      }
      else
      {
        /* No coords or regions - use 1.0 for first region, 0 for others */
        region_scalar = ( region_idx == 0 ) ? 0x10000 : 0;
      }

      /* Scale and accumulate deltas for this region */
      /* Tuple is organized as regions-major: [d0_r0, d1_r0, ..., dN_r0, d0_r1, ...] */
      if ( region_scalar != 0 )
      {
        FT_UInt base_offset = region_idx * num_deltas;


        for ( k = 0; k < num_deltas; k++ )
        {
          FT_Int32 region_delta = all_deltas[base_offset + k];  /* Plain integer */

          /* Accumulate using 64-bit math: accum += delta * scalar */
#ifdef FT_INT64
          FT_Int64 product = (FT_Int64)region_delta * region_scalar;
          accumulators[k] += product;
#else
          /* 32-bit fallback */
          if ( (FT_UInt32)( region_delta + 0x8000 ) <= 0x20000 )
          {
            /* Fast path - multiplication fits in 32 bits */
            FT_Int32  product = region_delta * region_scalar;
            accumulators[k].lo += (FT_UInt32)product;
            if ( accumulators[k].lo < (FT_UInt32)product )
              accumulators[k].hi += ( product < 0 ) ? 0 : 1;
            if ( product < 0 )
              accumulators[k].hi -= 1;
          }
          else
          {
            /* Slow path - full 64-bit signed multiplication */
            FT_UInt32 a = ( region_delta < 0 ) ? -(FT_UInt32)region_delta : region_delta;
            FT_UInt32 b = ( region_scalar < 0 ) ? -(FT_UInt32)region_scalar : region_scalar;
            FT_UInt32 a_lo = a & 0xFFFF, a_hi = a >> 16;
            FT_UInt32 b_lo = b & 0xFFFF, b_hi = b >> 16;
            FT_UInt32 mid = (a_lo * b_hi) + (a_hi * b_lo);
            FT_UInt32 lo = a_lo * b_lo, hi = a_hi * b_hi;
            hi += (mid >> 16) + ( ((lo >> 16) + (mid & 0xFFFF)) >> 16 );
            lo += (mid << 16);
            if ( ( region_delta < 0 ) != ( region_scalar < 0 ) )
            {
              lo = ~lo + 1;
              hi = ~hi + !lo;
            }
            accumulators[k].lo += lo;
            if ( accumulators[k].lo < lo )
              accumulators[k].hi += 1;
            accumulators[k].hi += hi;
          }
#endif
        }
      }
    }

    /* Free the flat delta array */
    if ( all_deltas != stack_all_deltas )
      FT_FREE( all_deltas );

    /* Convert 64-bit accumulators back to plain integers with rounding and >> 16 */
    for ( i = 0; i < num_deltas; i++ )
    {
#ifdef FT_INT64
      /* Round and shift: (accum + 0x8000) >> 16 - back to plain integer */
      deltas[i] = (FT_Fixed)( ( accumulators[i] + 0x8000L ) >> 16 );
#else
      /* 32-bit fallback */
      FT_UInt hi = accumulators[i].hi;
      FT_UInt lo = accumulators[i].lo;
      /* Add 0x8000 to round */
      lo += 0x8000;
      if ( lo < 0x8000 )  /* overflow */
        hi += 1;
      /* Shift right by 16 bits */
      deltas[i] = (FT_Fixed)( ( hi << 16 ) | ( lo >> 16 ) );
#endif
    }

    /* Free accumulators */
    if ( accumulators != stack_accumulators )
      FT_FREE( accumulators );

    error = FT_Err_Ok;

  Cleanup:
    return error;
#else
    FT_UNUSED( face );
    FT_UNUSED( varc );
    FT_UNUSED( var_index );
    FT_UNUSED( num_deltas );
    FT_UNUSED( deltas );
    FT_UNUSED( current_coords );
    FT_UNUSED( num_coords );

    return FT_THROW( Unimplemented_Feature );
#endif
  }


  /**************************************************************************
   *
   * Axis Index Reading
   *
   */

  /**************************************************************************
   *
   * @Function:
   *   tt_varc_get_axis_indices
   *
   * @Description:
   *   Reads axis indices from the axis indices list (TupleList of integers).
   *
   * @Input:
   *   varc ::
   *     The VARC table.
   *
   *   axis_indices_index ::
   *     Index into the TupleList.
   *
   *   num_indices ::
   *     Number of indices to read.
   *
   * @Output:
   *   indices ::
   *     Array to store axis indices (caller allocates).
   *
   * @Return:
   *   FreeType error code. 0 means success.
   */
  static FT_Error
  tt_varc_get_axis_indices( TT_Varc   varc,
                            FT_UInt   axis_indices_index,
                            FT_UInt   num_indices,
                            FT_UInt*  indices )
  {
    FT_Byte*  axis_list;
    FT_Byte*  table_limit;
    FT_UInt   tuple_count;
    FT_UInt   offSize;
    FT_Byte*  p;
    FT_ULong  offset1 = 0, offset2 = 0;
    FT_Byte*  offset_base;
    FT_Byte*  tuple_data;
    FT_UInt   i, j;


    if ( !varc || !varc->axis_indices_list || !indices )
      return FT_THROW( Invalid_Argument );

    axis_list = varc->axis_indices_list;
    table_limit = (FT_Byte*)varc->table + varc->table_size;

    /* Read TupleList header: count (uint32) + offSize (uint8) */
    if ( !CHECK_TABLE_BOUNDS( axis_list, 5 ) )
      return FT_THROW( Invalid_Table );

    tuple_count = ( (FT_UInt)axis_list[0] << 24 ) |
                  ( (FT_UInt)axis_list[1] << 16 ) |
                  ( (FT_UInt)axis_list[2] << 8 )  |
                  (FT_UInt)axis_list[3];
    offSize = axis_list[4];
    p = axis_list + 5;

    if ( axis_indices_index >= tuple_count || offSize == 0 || offSize > 4 )
      return FT_THROW( Invalid_Table );

    /* Read offsets for this tuple */
    offset_base = p + (tuple_count + 1) * offSize;
    p += axis_indices_index * offSize;

    for ( i = 0; i < offSize; i++ )
      offset1 = (offset1 << 8) | p[i];
    p += offSize;
    for ( i = 0; i < offSize; i++ )
      offset2 = (offset2 << 8) | p[i];

    /* Decode axis indices from TupleValues */
    tuple_data = offset_base + offset1 - 1;
    p = tuple_data;

    /* Read indices using TupleValues encoding */
    i = 0;
    while ( i < num_indices && p < offset_base + offset2 - 1 )
    {
      FT_Byte  control;
      FT_UInt  cnt;

      if ( p >= table_limit )
        return FT_THROW( Invalid_Table );

      control = *p++;
      cnt = ( control & 0x3F ) + 1;

      if ( cnt > num_indices - i )
        cnt = num_indices - i;

      if ( ( control & 0xC0 ) == 0xC0 )
      {
        /* 32-bit values */
        if ( p + 4 * cnt > table_limit )
          return FT_THROW( Invalid_Table );

        for ( j = 0; j < cnt && i < num_indices; j++, i++ )
        {
          indices[i] = ( (FT_UInt)p[0] << 24 ) | ( (FT_UInt)p[1] << 16 ) |
                       ( (FT_UInt)p[2] << 8 )  | (FT_UInt)p[3];
          p += 4;
        }
      }
      else if ( control & 0x80 )
      {
        /* Zeros - implicit sequential indices */
        for ( j = 0; j < cnt && i < num_indices; j++, i++ )
          indices[i] = i;
      }
      else if ( control & 0x40 )
      {
        /* 2-byte values */
        if ( p + 2 * cnt > table_limit )
          return FT_THROW( Invalid_Table );

        for ( j = 0; j < cnt && i < num_indices; j++, i++ )
        {
          indices[i] = ( (FT_UInt)p[0] << 8 ) | (FT_UInt)p[1];
          p += 2;
        }
      }
      else
      {
        /* 1-byte values */
        if ( p + cnt > table_limit )
          return FT_THROW( Invalid_Table );

        for ( j = 0; j < cnt && i < num_indices; j++, i++ )
          indices[i] = *p++;
      }
    }

    if ( i < num_indices )
      return FT_THROW( Invalid_Table );

    return FT_Err_Ok;
  }


  /**************************************************************************
   *
   * Variation Delta Application Functions
   *
   */

  /**************************************************************************
   *
   * @Function:
   *   tt_varc_apply_axis_deltas
   *
   * @Description:
   *   Applies variation deltas to axis values using the MultiItemVariationStore.
   *
   * @Input:
   *   face ::
   *     The font face.
   *
   *   varc ::
   *     The VARC table.
   *
   *   component ::
   *     The component whose axis values to vary.
   */
  static void
  tt_varc_apply_axis_deltas( TT_Face           face,
                             TT_Varc           varc,
                             TT_VarcComponent  component,
                             FT_Fixed*         current_coords,
                             FT_UInt           num_coords )
  {
#ifdef TT_CONFIG_OPTION_GX_VAR_SUPPORT
    FT_Error  error;
    FT_Fixed  stack_deltas[VARC_STACK_DELTA_COUNT];
    FT_Fixed* deltas = NULL;
    FT_UInt   i;
    FT_Memory memory = face->root.memory;


    if ( !varc->var_store_loaded || component->num_axis_values == 0 )
      return;


    /* Use stack allocation for small arrays, heap for large */
    if ( component->num_axis_values <= VARC_STACK_DELTA_COUNT )
    {
      deltas = stack_deltas;
    }
    else
    {
      if ( FT_NEW_ARRAY( deltas, component->num_axis_values ) )
        return;
    }

    /* Get tuple of deltas from MultiItemVariationStore */
    error = tt_varc_get_item_deltas( face, varc,
                                     component->axis_values_var_index,
                                     component->num_axis_values,
                                     deltas,
                                     current_coords,
                                     num_coords );
    if ( !error )
    {
      /* Apply deltas to axis values:
       * - Deltas are plain integers from tt_varc_get_item_deltas
       * - They are reinterpreted as F2DOT14 values
       * - Convert to F16DOT16 by << 2, then add to axis values
       * - Base values are also in F16DOT16 (F2DOT14 << 2)
       */
      for ( i = 0; i < component->num_axis_values; i++ )
      {
        FT_Fixed delta_f16dot16 = deltas[i] << 2;  /* Reinterpret as F2DOT14, convert to F16DOT16 */


        component->axis_values[i] += delta_f16dot16;

      }
    }

    if ( deltas != stack_deltas )
      FT_FREE( deltas );
#else
    FT_UNUSED( face );
    FT_UNUSED( varc );
    FT_UNUSED( component );
    FT_UNUSED( current_coords );
    FT_UNUSED( num_coords );
#endif
  }


  /**************************************************************************
   *
   * @Function:
   *   tt_varc_apply_transform_deltas
   *
   * @Description:
   *   Applies variation deltas to transform values using the MultiItemVariationStore.
   *
   * @Input:
   *   face ::
   *     The font face.
   *
   *   varc ::
   *     The VARC table.
   *
   *   component ::
   *     The component whose transform values to vary.
   */
  static void
  tt_varc_apply_transform_deltas( TT_Face           face,
                                  TT_Varc           varc,
                                  TT_VarcComponent  component,
                                  FT_Fixed*         current_coords,
                                  FT_UInt           num_coords )
  {
#ifdef TT_CONFIG_OPTION_GX_VAR_SUPPORT
    FT_Error  error;
    FT_Fixed  stack_deltas[VARC_STACK_DELTA_COUNT];
    FT_Fixed* deltas = NULL;
    FT_UInt   num_deltas = 0;
    FT_UInt   delta_index = 0;
    FT_Memory memory = face->root.memory;


    if ( !varc->var_store_loaded )
      return;

    /* Count how many transform components are present */
    if ( component->flags & VARC_HAVE_TRANSLATE_X ) num_deltas++;
    if ( component->flags & VARC_HAVE_TRANSLATE_Y ) num_deltas++;
    if ( component->flags & VARC_HAVE_ROTATION )    num_deltas++;
    if ( component->flags & VARC_HAVE_SCALE_X )     num_deltas++;
    if ( component->flags & VARC_HAVE_SCALE_Y )     num_deltas++;
    if ( component->flags & VARC_HAVE_SKEW_X )      num_deltas++;
    if ( component->flags & VARC_HAVE_SKEW_Y )      num_deltas++;
    if ( component->flags & VARC_HAVE_TCENTER_X )   num_deltas++;
    if ( component->flags & VARC_HAVE_TCENTER_Y )   num_deltas++;

    if ( num_deltas == 0 )
      return;


    /* Use stack allocation for small arrays, heap for large */
    if ( num_deltas <= VARC_STACK_DELTA_COUNT )
    {
      deltas = stack_deltas;
    }
    else
    {
      if ( FT_NEW_ARRAY( deltas, num_deltas ) )
        return;
    }

    /* Get tuple of deltas from MultiItemVariationStore */
    error = tt_varc_get_item_deltas( face, varc,
                                     component->transform_var_index,
                                     num_deltas,
                                     deltas,
                                     current_coords,
                                     num_coords );
    if ( !error )
    {
      /* Apply deltas to transform components in order */
      delta_index = 0;

      if ( component->flags & VARC_HAVE_TRANSLATE_X )
      {
        /* Deltas are in font units (FWORD), convert to 26.6 format */
        FT_Pos delta_26dot6 = deltas[delta_index] << 6;
        component->translate_x += delta_26dot6;
        delta_index++;
      }

      if ( component->flags & VARC_HAVE_TRANSLATE_Y )
      {
        /* Deltas are in font units (FWORD), convert to 26.6 format */
        FT_Pos delta_26dot6 = deltas[delta_index] << 6;
        component->translate_y += delta_26dot6;
        delta_index++;
      }

      if ( component->flags & VARC_HAVE_ROTATION )
      {
        /* Deltas are in F4DOT12 format, convert to F16DOT16 (shift left 4) */
        FT_Fixed delta_16dot16 = deltas[delta_index] << 4;
        component->rotation += delta_16dot16;
        delta_index++;
      }

      if ( component->flags & VARC_HAVE_SCALE_X )
      {
        /* Deltas are in F6DOT10 format, convert to F16DOT16 (shift left 6) */
        FT_Fixed delta_16dot16 = deltas[delta_index] << 6;
        component->scale_x += delta_16dot16;
        delta_index++;
      }

      if ( component->flags & VARC_HAVE_SCALE_Y )
      {
        /* Deltas are in F6DOT10 format, convert to F16DOT16 (shift left 6) */
        FT_Fixed delta_16dot16 = deltas[delta_index] << 6;
        component->scale_y += delta_16dot16;
        delta_index++;
      }

      if ( component->flags & VARC_HAVE_SKEW_X )
      {
        /* Deltas are in F4DOT12 format, convert to F16DOT16 (shift left 4) */
        FT_Fixed delta_16dot16 = deltas[delta_index] << 4;
        component->skew_x += delta_16dot16;
        delta_index++;
      }

      if ( component->flags & VARC_HAVE_SKEW_Y )
      {
        /* Deltas are in F4DOT12 format, convert to F16DOT16 (shift left 4) */
        FT_Fixed delta_16dot16 = deltas[delta_index] << 4;
        component->skew_y += delta_16dot16;
        delta_index++;
      }

      if ( component->flags & VARC_HAVE_TCENTER_X )
      {
        /* Deltas already in F16DOT16 from API */
        component->tcenter_x += deltas[delta_index];
        delta_index++;
      }

      if ( component->flags & VARC_HAVE_TCENTER_Y )
      {
        /* Deltas already in F16DOT16 from API */
        component->tcenter_y += deltas[delta_index];
        delta_index++;
      }
    }

    if ( deltas != stack_deltas )
      FT_FREE( deltas );
#else
    FT_UNUSED( face );
    FT_UNUSED( varc );
    FT_UNUSED( component );
    FT_UNUSED( current_coords );
    FT_UNUSED( num_coords );
#endif
  }


  /**************************************************************************
   *
   * Transform Matrix Functions
   *
   */

  /**************************************************************************
   *
   * @Function:
   *   tt_varc_build_transform
   *
   * @Description:
   *   Builds a transformation matrix from component transform values.
   *
   *   Transformation order (per VARC spec):
   *   1. Translate to transformation center (-tCenterX, -tCenterY)
   *   2. Apply scale (scaleX, scaleY)
   *   3. Apply skew (skewX, skewY)
   *   4. Apply rotation (rotation * π)
   *   5. Translate back from center (tCenterX, tCenterY)
   *   6. Apply final translation (translateX, translateY)
   *
   * @Input:
   *   component ::
   *     The component with transform values.
   *
   * @Output:
   *   matrix ::
   *     The resulting 2x2 transformation matrix.
   *
   *   offset ::
   *     The translation vector.
   */
  static void
  tt_varc_build_transform( TT_VarcComponent  component,
                           FT_Matrix*        matrix,
                           FT_Vector*        offset )
  {
    FT_Fixed  scale_x = component->scale_x;
    FT_Fixed  scale_y = component->scale_y;
    FT_Fixed  skew_x  = component->skew_x;
    FT_Fixed  skew_y  = component->skew_y;
    FT_Fixed  rotation = component->rotation;


    /* Build rotation matrix */
    if ( rotation != 0 )
    {
      FT_Angle  angle;
      FT_Vector v;

      /* rotation is F4DOT12 in multiples of π, converted to F16DOT16 by << 4 */
      /* FT_ANGLE_PI = 180 * 65536 (degrees), so this converts π multiples to degrees */
      angle = FT_MulFix( rotation, FT_ANGLE_PI );

      FT_Vector_Unit( &v, angle );

      /* Rotation matrix: [cos -sin]
                          [sin  cos] */
      matrix->xx = v.x;
      matrix->xy = -v.y;
      matrix->yx = v.y;
      matrix->yy = v.x;
    }
    else
    {
      matrix->xx = 0x10000;
      matrix->xy = 0;
      matrix->yx = 0;
      matrix->yy = 0x10000;
    }

    /* Apply scale - scale each column, not row */
    /* First column (xx, yx) represents X-axis, scaled by scale_x */
    /* Second column (xy, yy) represents Y-axis, scaled by scale_y */
    if ( scale_x != 0x10000 || scale_y != 0x10000 )
    {
      matrix->xx = FT_MulFix( matrix->xx, scale_x );
      matrix->yx = FT_MulFix( matrix->yx, scale_x );
      matrix->xy = FT_MulFix( matrix->xy, scale_y );
      matrix->yy = FT_MulFix( matrix->yy, scale_y );
    }

    /* Apply skew */
    if ( skew_x != 0 || skew_y != 0 )
    {
      FT_Matrix  skew_matrix;
      FT_Fixed   tan_x, tan_y;

      /* Convert skew angles to tangent values */
      /* skew is in radians * pi, so we need to compute tan(skew * pi) */
      tan_x = skew_x ? FT_Tan( FT_MulFix( skew_x, FT_ANGLE_PI ) ) : 0;
      tan_y = skew_y ? FT_Tan( FT_MulFix( skew_y, FT_ANGLE_PI ) ) : 0;

      skew_matrix.xx = 0x10000;
      skew_matrix.xy = tan_x;
      skew_matrix.yx = tan_y;
      skew_matrix.yy = 0x10000;

      FT_Matrix_Multiply( &skew_matrix, matrix );
    }

    /* Calculate translation with center point transformation */
    offset->x = component->translate_x;
    offset->y = component->translate_y;


    if ( component->tcenter_x != 0 || component->tcenter_y != 0 )
    {
      FT_Vector  center;

      center.x = component->tcenter_x;
      center.y = component->tcenter_y;


      /* offset += center - matrix * center */
      offset->x += center.x - FT_MulFix( matrix->xx, center.x ) -
                              FT_MulFix( matrix->xy, center.y );
      offset->y += center.y - FT_MulFix( matrix->yx, center.x ) -
                              FT_MulFix( matrix->yy, center.y );

    }
  }


  /**************************************************************************
   *
   * Recursion Context Functions
   *
   */

  /**************************************************************************
   *
   * @Function:
   *   tt_varc_context_push
   *
   * @Description:
   *   Pushes a glyph ID onto the recursion stack.
   *
   * @Input:
   *   context ::
   *     The recursion context.
   *
   *   glyph_index ::
   *     The glyph ID to push.
   *
   * @Return:
   *   FreeType error code. Returns Invalid_Table if cycle detected or
   *   max depth exceeded.
   */
  static FT_Error
  tt_varc_context_push( TT_VarcContext  context,
                        FT_UInt         glyph_index )
  {
    FT_UInt  i;


    /* Check max recursion depth */
    if ( context->recursion_depth >= TT_VARC_MAX_NESTING_LEVEL )
      return FT_THROW( Invalid_Table );

    /* Check for cycles */
    for ( i = 0; i < context->recursion_depth; i++ )
    {
      if ( context->visited_glyphs[i] == glyph_index )
        return FT_THROW( Invalid_Table ); /* Cycle detected */
    }

    /* Push onto stack */
    context->visited_glyphs[context->recursion_depth] = glyph_index;
    context->recursion_depth++;

    return FT_Err_Ok;
  }


  /**************************************************************************
   *
   * @Function:
   *   tt_varc_context_pop
   *
   * @Description:
   *   Pops a glyph ID from the recursion stack.
   *
   * @Input:
   *   context ::
   *     The recursion context.
   */
  static void
  tt_varc_context_pop( TT_VarcContext  context )
  {
    if ( context->recursion_depth > 0 )
      context->recursion_depth--;
  }


  /**************************************************************************
   *
   * VARC Table Loading Functions
   *
   */

  /**************************************************************************
   *
   * @Function:
   *   tt_face_load_varc
   *
   * @Description:
   *   Loads the VARC table from the font file.
   *
   * @Input:
   *   face ::
   *     The font face.
   *
   *   stream ::
   *     The font stream.
   *
   * @Return:
   *   FreeType error code. 0 means success.
   */
  FT_EXPORT_DEF( FT_Error )
  tt_face_load_varc( TT_Face    face,
                     FT_Stream  stream )
  {
    FT_Error   error;
    FT_Memory  memory = face->root.memory;
    FT_Byte*   table = NULL;
    FT_ULong   table_size;
    TT_Varc    varc = NULL;
    FT_Byte*   p;
    /* Initialize VARC loading flag */
    face->varc_loading_components = FALSE;
    face->varc_context = NULL;

    error = face->goto_table( face, TTAG_VARC, stream, &table_size );
    if ( error )
    {
      goto Exit;
    }
    if ( table_size < 20 ) /* Minimum header size */
      goto Exit;

    if ( FT_FRAME_EXTRACT( table_size, table ) )
      goto Exit;

    if ( FT_NEW( varc ) )
      goto Fail;

    varc->table = table;
    varc->table_size = table_size;

    p = table;

    /* Read header */
    varc->version_major = ( p[0] << 8 ) | p[1];
    varc->version_minor = ( p[2] << 8 ) | p[3];
    p += 4;

    /* Check version */
    if ( varc->version_major != 1 || varc->version_minor != 0 )
    {
      error = FT_THROW( Invalid_Table );
      goto Fail;
    }

    /* Read offsets */
    varc->coverage_offset = ( (FT_ULong)p[0] << 24 ) | ( (FT_ULong)p[1] << 16 ) |
                            ( (FT_ULong)p[2] << 8 )  | (FT_ULong)p[3];
    p += 4;

    varc->multi_var_store_offset = ( (FT_ULong)p[0] << 24 ) | ( (FT_ULong)p[1] << 16 ) |
                                   ( (FT_ULong)p[2] << 8 )  | (FT_ULong)p[3];
    p += 4;

    varc->condition_list_offset = ( (FT_ULong)p[0] << 24 ) | ( (FT_ULong)p[1] << 16 ) |
                                  ( (FT_ULong)p[2] << 8 )  | (FT_ULong)p[3];
    p += 4;

    varc->axis_indices_list_offset = ( (FT_ULong)p[0] << 24 ) | ( (FT_ULong)p[1] << 16 ) |
                                     ( (FT_ULong)p[2] << 8 )  | (FT_ULong)p[3];
    p += 4;

    varc->var_composite_glyphs_offset = ( (FT_ULong)p[0] << 24 ) | ( (FT_ULong)p[1] << 16 ) |
                                        ( (FT_ULong)p[2] << 8 )  | (FT_ULong)p[3];

    /* Validate offsets */
    if ( varc->coverage_offset >= table_size ||
         varc->var_composite_glyphs_offset >= table_size )
    {
      error = FT_THROW( Invalid_Table );
      goto Fail;
    }

    /* Set up pointers */
    varc->coverage = table + varc->coverage_offset;
    varc->var_composite_glyphs = table + varc->var_composite_glyphs_offset;
    if ( varc->condition_list_offset > 0 && varc->condition_list_offset < table_size )
      varc->condition_list = table + varc->condition_list_offset;
    else
      varc->condition_list = NULL;

    if ( varc->axis_indices_list_offset > 0 && varc->axis_indices_list_offset < table_size )
      varc->axis_indices_list = table + varc->axis_indices_list_offset;
    else
      varc->axis_indices_list = NULL;

#ifdef TT_CONFIG_OPTION_GX_VAR_SUPPORT
    /* MultiItemVariationStore is NOT the same as ItemVariationStore! */
    /* MultiItemVariationStore stores TUPLES of deltas, not single deltas. */
    varc->var_store_loaded = FALSE;
    varc->multi_var_store = NULL;

    if ( varc->multi_var_store_offset > 0 && varc->multi_var_store_offset < table_size )
    {
      /* Point to the MultiItemVariationStore data in the VARC table */
      varc->multi_var_store = table + varc->multi_var_store_offset;
      varc->var_store_loaded = TRUE;

      /* TODO: Implement proper MultiItemVariationStore parser:
       * - Format: u16 (must be 1)
       * - Regions: Offset32 to SparseVarRegionList
       * - DataSets: Array16 of Offset32 to MultiVarData
       * MultiVarData format:
       * - Format: u8 (must be 1)
       * - RegionIndices: Array16 of u16
       * - DeltaSets: INDEX2 of TupleValues (CFF2-style index)
       */
    }
#endif

    face->varc = varc;
    goto Exit;

  Fail:
    FT_FREE( varc );
    FT_FRAME_RELEASE( table );

  Exit:
    return error;
  }


  /**************************************************************************
   *
   * @Function:
   *   tt_face_free_varc
   *
   * @Description:
   *   Frees the VARC table resources.
   *
   * @Input:
   *   face ::
   *     The font face.
   */
  FT_EXPORT_DEF( void )
  tt_face_free_varc( TT_Face  face )
  {
    FT_Memory  memory = face->root.memory;
    FT_Stream  stream = face->root.stream;
    TT_Varc    varc   = (TT_Varc)face->varc;


    if ( varc )
    {
#ifdef TT_CONFIG_OPTION_GX_VAR_SUPPORT
      /* MultiItemVariationStore cleanup */
      /* No special cleanup needed - we just point to data in the VARC table */
      varc->multi_var_store = NULL;
      varc->var_store_loaded = FALSE;
#endif

      FT_FRAME_RELEASE( varc->table );
      FT_FREE( varc );
      face->varc = NULL;
    }
  }


  /**************************************************************************
   *
   * @Function:
   *   tt_face_has_varc_glyph
   *
   * @Description:
   *   Checks if a glyph has VARC data.
   *
   * @Input:
   *   face ::
   *     The font face.
   *
   *   glyph_index ::
   *     The glyph ID to check.
   *
   * @Return:
   *   TRUE if the glyph has VARC data, FALSE otherwise.
   */
  FT_EXPORT_DEF( FT_Bool )
  tt_face_has_varc_glyph( TT_Face  face,
                          FT_UInt  glyph_index )
  {
    TT_Varc  varc = (TT_Varc)face->varc;
    FT_Int   coverage_idx;
    if ( !varc )
    {
      return FALSE;
    }

    coverage_idx = tt_varc_get_coverage( varc, glyph_index );
    return coverage_idx >= 0;
  }


  /**************************************************************************
   *
   * VARC Glyph Loading Functions
   *
   */

  /**************************************************************************
   *
   * @Function:
   *   tt_face_load_varc_glyph
   *
   * @Description:
   *   Loads a VARC glyph into the glyph slot.
   *
   * @Input:
   *   face ::
   *     The font face.
   *
   *   glyph_slot ::
   *     The glyph slot to load into.
   *
   *   glyph_index ::
   *     The glyph ID to load.
   *
   *   load_flags ::
   *     Loading flags.
   *
   * @Return:
   *   FreeType error code. 0 means success.
   */
  FT_EXPORT_DEF( FT_Error )
  tt_face_load_varc_glyph( TT_Face       face,
                           FT_GlyphSlot  glyph_slot,
                           FT_UInt       glyph_index,
                           FT_Int32      load_flags )
  {
    FT_Error            error;
    TT_Varc             varc = (TT_Varc)face->varc;
    FT_Byte*            record_data;
    FT_UInt             record_size;
    FT_Byte*            p = NULL;
    FT_Byte*            limit;
    TT_VarcContext      context;
    FT_Bool             context_owner = FALSE;
    FT_Memory           memory = face->root.memory;
    TT_GlyphSlot        slot = (TT_GlyphSlot)glyph_slot;
    FT_Fixed*           parent_coords = NULL;
    FT_UInt             num_coords = 0;
    FT_Fixed            stack_new_coords[VARC_STACK_COORD_COUNT];
    FT_UInt             stack_axis_indices[VARC_STACK_INDICES_COUNT];
    FT_Fixed            stack_axis_values[VARC_STACK_AXIS_COUNT];



    if ( !varc )
    {
      return FT_THROW( Invalid_Table );
    }

    /* Get or create recursion context */
    context = (TT_VarcContext)face->varc_context;
    if ( !context )
    {
      /* First call - create new context on heap */
      if ( FT_NEW( context ) )
        return error;

      FT_ZERO( context );
      face->varc_context = context;
      context_owner = TRUE;
      /* Save font's current variation coordinates for inheritance at depth 1 */
#ifdef TT_CONFIG_OPTION_GX_VAR_SUPPORT
      {
        FT_MM_Var*  master;

        /* FT_Get_MM_Var ensures face->blend is initialized (it's lazy) */
        error = FT_Get_MM_Var( (FT_Face)face, &master );
        if ( !error && master )
        {
          if ( master->num_axis > 0 )
            context->num_font_coords = master->num_axis;
          FT_Done_MM_Var( face->root.driver->root.library,
                          master );
        }
        error = FT_Err_Ok;  /* Non-fatal */
      }
      if ( context->num_font_coords > 0 )
      {
        if ( !FT_NEW_ARRAY( context->font_coords, context->num_font_coords ) )
        {
          error = FT_Get_Var_Blend_Coordinates( (FT_Face)face,
                                                context->num_font_coords,
                                                context->font_coords );
          if ( error )
          {
            FT_FREE( context->font_coords );
            context->num_font_coords = 0;
          }
          else
          {
            /* Set initial current_coords to font_coords */
            context->current_coords = context->font_coords;
            context->num_current_coords = context->num_font_coords;
          }
        }
      }
#endif
    }

    /* Get parent coords from context for delta evaluation */
    parent_coords = context->current_coords;
    num_coords = context->num_current_coords;

    /* Push this glyph onto recursion stack */
    error = tt_varc_context_push( context, glyph_index );
    if ( error )
    {
      if ( context_owner )
        face->varc_context = NULL;
      return error;
    }

    /* Get glyph record */
    error = tt_varc_get_glyph_record( varc, glyph_index, &record_data, &record_size );
    if ( error )
    {
      goto Cleanup;
    }

    p = record_data;
    limit = record_data + record_size;

    /* Parse components until we run out of data */
    while ( p < limit )
    {
      TT_VarcComponentRec  component;
      FT_GlyphSlot         component_slot;
      FT_Matrix            matrix;
      FT_Vector            offset;
      FT_UInt              i;
      /* Parse component */
      error = tt_varc_parse_component( face, varc, &p, limit, &component,
                                       stack_axis_values, VARC_STACK_AXIS_COUNT );
      if ( error )
      {
        /* Skip malformed component and continue with others */
        break;  /* Exit loop - can't safely continue parsing if we don't know component size */
      }

      /* Apply variation deltas to axis values if present */
      if ( component.flags & VARC_AXES_HAVE_VARIATION )
      {
        tt_varc_apply_axis_deltas( face, varc, &component,
                                   parent_coords, num_coords );
      }

      /* Apply variation deltas to transform values if present */
      if ( component.flags & VARC_TRANSFORM_HAS_VARIATION )
      {
        tt_varc_apply_transform_deltas( face, varc, &component,
                                        parent_coords, num_coords );
      }

      /* TODO: Evaluate condition - for now, always render */


      /* Build transformation matrix */
      tt_varc_build_transform( &component, &matrix, &offset );

      /* Load component glyph into a temporary slot */
      /* FT_New_GlyphSlot prepends to face->glyph list;            */
      /* FT_Done_GlyphSlot removes it, restoring the previous head */
      FT_GlyphSlot  temp_glyph;

      error = FT_New_GlyphSlot( (FT_Face)face, &temp_glyph );
      if ( error )
      {
        tt_varc_free_component( face, &component );
        goto Cleanup;
      }

      /* Apply axis value overrides if present */
      FT_Fixed*  new_coords = NULL;
      FT_Bool    new_coords_on_heap = FALSE;
      FT_Bool    has_axis_override = FALSE;

#ifdef TT_CONFIG_OPTION_GX_VAR_SUPPORT
      if ( component.num_axis_values > 0 && component.axis_values &&
           num_coords > 0 && parent_coords )
      {
        /* Allocate new_coords only (parent_coords replaces saved_coords) */
        if ( num_coords <= VARC_STACK_COORD_COUNT )
        {
          new_coords = stack_new_coords;
        }
        else
        {
          if ( FT_NEW_ARRAY( new_coords, num_coords ) )
            goto Skip_Axis_Override;
          new_coords_on_heap = TRUE;
        }

        /* Start with current coordinates or reset to default */
        if ( component.flags & VARC_RESET_UNSPECIFIED_AXES )
        {
          /* Reset to default normalized coordinates (0.0 for all axes) */
          FT_UInt  j;

          for ( j = 0; j < num_coords; j++ )
            new_coords[j] = 0;  /* 0.0 in normalized space = default */
        }
        else
        {
          /* Inherit from parent coords (handles both depth 1 and > 1) */
          FT_MEM_COPY( new_coords, parent_coords,
                       num_coords * sizeof( FT_Fixed ) );
        }

        /* Read axis indices and apply values */
        if ( component.flags & VARC_HAVE_AXES && varc->axis_indices_list )
        {
          FT_UInt*  axis_indices = NULL;
          FT_UInt   j;

          /* Allocate array for axis indices */
          if ( component.num_axis_values <= VARC_STACK_INDICES_COUNT )
          {
            axis_indices = stack_axis_indices;
          }
          else
          {
            if ( FT_NEW_ARRAY( axis_indices, component.num_axis_values ) )
            {
              if ( new_coords_on_heap )
                FT_FREE( new_coords );
              new_coords = NULL;
              goto Skip_Axis_Override;
            }
          }

          /* Read axis indices from TupleList */
          error = tt_varc_get_axis_indices( varc,
                                            component.axis_indices_index,
                                            component.num_axis_values,
                                            axis_indices );
          if ( error )
          {
            if ( axis_indices != stack_axis_indices )
              FT_FREE( axis_indices );
            if ( new_coords_on_heap )
              FT_FREE( new_coords );
            new_coords = NULL;
            goto Skip_Axis_Override;
          }

          /* Apply axis values using the indices */
          for ( j = 0; j < component.num_axis_values; j++ )
          {
            FT_UInt  axis_idx = axis_indices[j];

            if ( axis_idx < num_coords )
              new_coords[axis_idx] = component.axis_values[j];
          }

          if ( axis_indices != stack_axis_indices )
            FT_FREE( axis_indices );
        }

        /* Apply the new normalized coordinates */
        error = FT_Set_Var_Blend_Coordinates( (FT_Face)face,
                                               num_coords, new_coords );
        has_axis_override = TRUE;
      }
Skip_Axis_Override:
#endif

      /* Load component - allow recursive VARC loading */
      /* Compose transforms: we want (T1*T2)(leaf) not T1(T2(leaf)) */
      FT_Matrix  composed_matrix;
      FT_Vector  composed_delta;
      FT_Matrix  saved_parent_matrix;
      FT_Vector  saved_parent_delta;
      FT_Bool    saved_has_parent;

      /* Save current parent transform */
      saved_parent_matrix = context->parent_matrix;
      saved_parent_delta = context->parent_delta;
      saved_has_parent = context->has_parent_transform;

      /* Compose with parent transform if present */
      if ( context->has_parent_transform )
      {
        /* Compose: final = parent * child */
        composed_matrix = context->parent_matrix;
        FT_Matrix_Multiply( &matrix, &composed_matrix );

        /* Transform offset by parent matrix and add parent delta */
        composed_delta.x = FT_MulFix( offset.x, context->parent_matrix.xx ) +
                          FT_MulFix( offset.y, context->parent_matrix.xy ) +
                          context->parent_delta.x;
        composed_delta.y = FT_MulFix( offset.x, context->parent_matrix.yx ) +
                          FT_MulFix( offset.y, context->parent_matrix.yy ) +
                          context->parent_delta.y;
      }
      else
      {
        /* No parent transform, use component transform directly */
        composed_matrix = matrix;
        composed_delta = offset;
      }

      /* Store composed transform in context for child components */
      context->parent_matrix = composed_matrix;
      context->parent_delta = composed_delta;
      context->has_parent_transform = TRUE;


      /* Set child's coords in context for recursive VARC processing */
#ifdef TT_CONFIG_OPTION_GX_VAR_SUPPORT
      if ( has_axis_override )
      {
        context->current_coords = new_coords;
        context->num_current_coords = num_coords;
      }
#endif

      /* Load component with NO_SCALE to get raw outlines */
      FT_Int32  component_load_flags = load_flags | FT_LOAD_NO_SCALE;
      error = FT_Load_Glyph( (FT_Face)face, component.gid, component_load_flags );

#ifdef TT_CONFIG_OPTION_GX_VAR_SUPPORT
      /* Restore parent coordinates and context */
      if ( has_axis_override )
      {
        FT_Set_Var_Blend_Coordinates( (FT_Face)face,
                                       num_coords, parent_coords );
        context->current_coords = parent_coords;
        context->num_current_coords = num_coords;

        if ( new_coords_on_heap )
          FT_FREE( new_coords );
      }
#endif

      /* Restore parent transform in context */
      context->parent_matrix = saved_parent_matrix;
      context->parent_delta = saved_parent_delta;
      context->has_parent_transform = saved_has_parent;


      component_slot = temp_glyph;


      if ( error )
      {
        FT_Done_GlyphSlot( component_slot );
        tt_varc_free_component( face, &component );
        continue;  /* Skip failed component, continue with others */
      }


      /* Transform component outline manually (only for base glyphs) */
      /* VARC glyphs are already transformed recursively with composed transform */
      FT_Bool  component_is_varc = tt_face_has_varc_glyph( face, component.gid );

      if ( component_slot->outline.n_points > 0 )
      {
        /* Only apply transform to base glyphs, not VARC glyphs */
        if ( !component_is_varc )
        {

          /* Apply matrix transform */

          FT_Outline_Transform( &component_slot->outline, &composed_matrix );

          /* Apply translation - convert from 26.6 to font units */
          FT_Outline_Translate( &component_slot->outline, composed_delta.x >> 6, composed_delta.y >> 6 );

        }
        else
        {
        }

        /* Accumulate into main glyph slot (always, for both VARC and base) */
        if ( slot->outline.n_points == 0 )
        {
          /* First component - allocate and copy */

          /* Allocate memory for outline */
          error = FT_Outline_New( (FT_Library)face->root.driver->root.library,
                                  component_slot->outline.n_points,
                                  component_slot->outline.n_contours,
                                  &slot->outline );
          if ( error )
          {
            FT_Done_GlyphSlot( component_slot );
            tt_varc_free_component( face, &component );
            goto Cleanup;
          }

          /* Copy outline data */
          error = FT_Outline_Copy( &component_slot->outline, &slot->outline );
          if ( error )
          {
            FT_Outline_Done( (FT_Library)face->root.driver->root.library, &slot->outline );
            FT_Done_GlyphSlot( component_slot );
            tt_varc_free_component( face, &component );
            goto Cleanup;
          }
        }
        else
        {
          /* Append component to existing outline */
          FT_UInt  old_n_points = slot->outline.n_points;
          FT_UInt  old_n_contours = slot->outline.n_contours;
          FT_UInt  new_n_points = old_n_points + component_slot->outline.n_points;
          FT_UInt  new_n_contours = old_n_contours + component_slot->outline.n_contours;

          /* Reallocate outline arrays */
          if ( FT_RENEW_ARRAY( slot->outline.points, old_n_points, new_n_points ) ||
               FT_RENEW_ARRAY( slot->outline.tags, old_n_points, new_n_points ) ||
               FT_RENEW_ARRAY( slot->outline.contours, old_n_contours, new_n_contours ) )
          {
            FT_Done_GlyphSlot( component_slot );
            tt_varc_free_component( face, &component );
            goto Cleanup;
          }

          /* Copy component points */
          for ( i = 0; i < component_slot->outline.n_points; i++ )
          {
            slot->outline.points[old_n_points + i] = component_slot->outline.points[i];
            slot->outline.tags[old_n_points + i] = component_slot->outline.tags[i];
          }

          /* Copy component contours (adjust indices) */
          for ( i = 0; i < component_slot->outline.n_contours; i++ )
          {
            slot->outline.contours[old_n_contours + i] =
              component_slot->outline.contours[i] + old_n_points;
          }

          slot->outline.n_points = new_n_points;
          slot->outline.n_contours = new_n_contours;
        }
      }

      FT_Done_GlyphSlot( component_slot );
      tt_varc_free_component( face, &component );
    }


    /* If original load request wasn't NO_SCALE, scale the final outline */
    if ( !( load_flags & FT_LOAD_NO_SCALE ) && slot->outline.n_points > 0 )
    {
      /* Scale outline to current font size */
      FT_Matrix  scale_matrix;
      FT_Size    size = face->root.size;

      if ( size )
      {
        /* Build scaling matrix from face->size */
        scale_matrix.xx = size->metrics.x_scale;
        scale_matrix.xy = 0;
        scale_matrix.yx = 0;
        scale_matrix.yy = size->metrics.y_scale;


        FT_Outline_Transform( &slot->outline, &scale_matrix );
      }
    }

    error = FT_Err_Ok;

  Cleanup:
    /* Pop from recursion stack */
    tt_varc_context_pop( context );

    /* Cleanup context if we own it */
    if ( context_owner )
    {
      if ( context->font_coords )
        FT_FREE( context->font_coords );
      FT_FREE( context );
      face->varc_context = NULL;
    }
    else
    {
    }

    /* Convert back to font units if NO_SCALE was requested */
    /* Only do this at the top level (depth 0) to avoid double conversion */
    if ( !error && ( load_flags & FT_LOAD_NO_SCALE ) && slot->outline.n_points > 0 &&
         context->recursion_depth == 0 )
    {
      FT_UInt  i;

      for ( i = 0; i < slot->outline.n_points; i++ )
      {
        slot->outline.points[i].x >>= 6;
        slot->outline.points[i].y >>= 6;
      }

    }

    /* Set glyph format if successful */
    if ( !error )
    {
      glyph_slot->format = FT_GLYPH_FORMAT_OUTLINE;
    }
    else
    {
    }

    return error;
  }


#endif /* TT_CONFIG_OPTION_VARC */


/* END */
