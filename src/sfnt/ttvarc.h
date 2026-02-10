/****************************************************************************
 *
 * ttvarc.h
 *
 *   TrueType and OpenType VARC (Variable Composites) support (specification).
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


#ifndef TTVARC_H_
#define TTVARC_H_


#include "ttload.h"
#include <freetype/ftcolor.h>

#ifdef TT_CONFIG_OPTION_GX_VAR_SUPPORT
#include <freetype/internal/ftmmtypes.h>
#endif


FT_BEGIN_HEADER


#ifdef TT_CONFIG_OPTION_VARC


  /**************************************************************************
   *
   * VARC component flags from the specification
   *
   */

  /* Variable Component Flags - from VARC spec */
#define VARC_RESET_UNSPECIFIED_AXES        0x00000001U  /* Bit 0 */
#define VARC_HAVE_AXES                     0x00000002U  /* Bit 1 */
#define VARC_AXES_HAVE_VARIATION           0x00000004U  /* Bit 2 */
#define VARC_TRANSFORM_HAS_VARIATION       0x00000008U  /* Bit 3 */
#define VARC_HAVE_TRANSLATE_X              0x00000010U  /* Bit 4 */
#define VARC_HAVE_TRANSLATE_Y              0x00000020U  /* Bit 5 */
#define VARC_HAVE_ROTATION                 0x00000040U  /* Bit 6 */
#define VARC_HAVE_CONDITION                0x00000080U  /* Bit 7 */
#define VARC_HAVE_SCALE_X                  0x00000100U  /* Bit 8 */
#define VARC_HAVE_SCALE_Y                  0x00000200U  /* Bit 9 */
#define VARC_HAVE_TCENTER_X                0x00000400U  /* Bit 10 */
#define VARC_HAVE_TCENTER_Y                0x00000800U  /* Bit 11 */
#define VARC_GID_IS_24BIT                  0x00001000U  /* Bit 12 */
#define VARC_HAVE_SKEW_X                   0x00002000U  /* Bit 13 */
#define VARC_HAVE_SKEW_Y                   0x00004000U  /* Bit 14 */

  /* Reserved flags - bits 15-31 must be zero */
#define VARC_RESERVED_MASK                 0xFFFF8000U


  /**************************************************************************
   *
   * @Struct:
   *   TT_VarcComponentRec
   *
   * @Description:
   *   Represents a single component within a VARC glyph.
   *
   * @Fields:
   *   flags ::
   *     Component flags indicating which optional fields are present.
   *
   *   gid ::
   *     The glyph ID of the component to be included.
   *
   *   condition_index ::
   *     Index into the condition list (if VARC_HAVE_CONDITION is set).
   *     Component is only rendered if condition evaluates to true.
   *
   *   axis_indices_index ::
   *     Index into the axis indices list (if VARC_HAVE_AXES is set).
   *
   *   num_axis_values ::
   *     Number of axis values in the axis_values array.
   *
   *   axis_values ::
   *     Array of normalized axis coordinates (-1.0 to +1.0 in F2DOT14).
   *     These override the current design space location for this component.
   *
   *   axis_values_var_index ::
   *     Variation store index for axis values (if VARC_AXES_HAVE_VARIATION).
   *
   *   transform_var_index ::
   *     Variation store index for transform values (if VARC_TRANSFORM_HAS_VARIATION).
   *
   *   translate_x ::
   *   translate_y ::
   *     Translation amounts in font units (26.6 fixed-point).
   *
   *   rotation ::
   *     Rotation angle in radians, multiplied by pi (16.16 fixed-point).
   *     So 0x10000 represents pi radians (180 degrees).
   *
   *   scale_x ::
   *   scale_y ::
   *     Scale factors (16.16 fixed-point, 1.0 = 0x10000).
   *
   *   skew_x ::
   *   skew_y ::
   *     Skew angles in radians, multiplied by pi (16.16 fixed-point).
   *
   *   tcenter_x ::
   *   tcenter_y ::
   *     Transformation center point in font units (26.6 fixed-point).
   *     Transformations are applied relative to this point.
   */
  typedef struct  TT_VarcComponentRec_
  {
    FT_UInt32  flags;
    FT_UInt    gid;

    /* Conditional rendering */
    FT_UInt32  condition_index;

    /* Axis value overrides */
    FT_UInt32  axis_indices_index;
    FT_UInt    num_axis_values;
    FT_Fixed*  axis_values;          /* Normalized coordinates (F2DOT14) */
    FT_Bool    axis_values_on_heap;  /* TRUE if axis_values needs FT_FREE */
    FT_UInt32  axis_values_var_index;

    /* Transform variation */
    FT_UInt32  transform_var_index;

    /* Transform components (all 26.6 or 16.16 fixed-point) */
    FT_Pos     translate_x;
    FT_Pos     translate_y;
    FT_Fixed   rotation;             /* Radians * pi, 16.16 */
    FT_Fixed   scale_x;
    FT_Fixed   scale_y;
    FT_Fixed   skew_x;
    FT_Fixed   skew_y;
    FT_Pos     tcenter_x;
    FT_Pos     tcenter_y;

  } TT_VarcComponentRec, *TT_VarcComponent;


  /**************************************************************************
   *
   * @Struct:
   *   TT_VarcRec
   *
   * @Description:
   *   The VARC table structure, loaded from the font file.
   *
   * @Fields:
   *   version_major ::
   *     Major version of the VARC table (currently 1).
   *
   *   version_minor ::
   *     Minor version of the VARC table (currently 0).
   *
   *   coverage_offset ::
   *     Offset to the coverage table from start of VARC table.
   *
   *   coverage ::
   *     Pointer to the coverage table data.
   *
   *   multi_var_store_offset ::
   *     Offset to the MultiItemVariationStore.
   *
   *   condition_list_offset ::
   *     Offset to the condition list.
   *
   *   axis_indices_list_offset ::
   *     Offset to the axis indices list.
   *
   *   var_composite_glyphs_offset ::
   *     Offset to the glyph records (CFF2-style index).
   *
   *   var_store ::
   *     The loaded MultiItemVariationStore for variation deltas.
   *
   *   table ::
   *     Pointer to the raw VARC table data.
   *
   *   table_size ::
   *     Size of the VARC table in bytes.
   *
   *   stream ::
   *     The stream from which the table was loaded.
   */
  typedef struct  TT_VarcRec_
  {
    FT_UShort  version_major;
    FT_UShort  version_minor;

    /* Table offsets */
    FT_ULong   coverage_offset;
    FT_ULong   multi_var_store_offset;
    FT_ULong   condition_list_offset;
    FT_ULong   axis_indices_list_offset;
    FT_ULong   var_composite_glyphs_offset;

    /* Loaded data pointers */
    FT_Byte*   coverage;
    FT_Byte*   condition_list;
    FT_Byte*   axis_indices_list;
    FT_Byte*   var_composite_glyphs;

#ifdef TT_CONFIG_OPTION_GX_VAR_SUPPORT
    /* MultiItemVariationStore - stores tuples of deltas, not single values */
    FT_Byte*   multi_var_store;      /* Pointer to MultiItemVariationStore data */
    FT_Bool    var_store_loaded;
#endif

    /* Raw table data */
    void*      table;
    FT_ULong   table_size;

  } TT_VarcRec, *TT_Varc;


  /**************************************************************************
   *
   * Maximum recursion depth for VARC glyph loading.
   * This prevents stack overflow and infinite loops from malformed fonts.
   */
#define TT_VARC_MAX_NESTING_LEVEL  16


  /**************************************************************************
   *
   * @Struct:
   *   TT_VarcContextRec
   *
   * @Description:
   *   Context structure for recursive VARC glyph loading.
   *   Tracks recursion depth and visited glyphs for cycle detection.
   *
   * @Fields:
   *   recursion_depth ::
   *     Current depth in the recursion stack.
   *
   *   visited_glyphs ::
   *     Array of glyph IDs currently being processed.
   *     Used for cycle detection to prevent infinite loops.
   */
  typedef struct  TT_VarcContextRec_
  {
    FT_UInt    recursion_depth;
    FT_UInt    visited_glyphs[TT_VARC_MAX_NESTING_LEVEL];

    /* Parent transform to compose with child transforms */
    FT_Matrix  parent_matrix;
    FT_Vector  parent_delta;
    FT_Bool    has_parent_transform;

    /* Font's original variation coordinates (before VARC processing) */
    /* Used when RESET_UNSPECIFIED_AXES is not set at depth 1 */
    FT_Fixed*  font_coords;
    FT_UInt    num_font_coords;

    /* Reusable buffers to avoid per-component allocations */
    FT_Fixed*  axis_values_buffer;
    FT_UInt    axis_values_buffer_size;
    FT_Fixed*  deltas_buffer;
    FT_UInt    deltas_buffer_size;
    FT_Fixed*  coords_buffer;
    FT_UInt    coords_buffer_size;
    FT_UInt*   indices_buffer;
    FT_UInt    indices_buffer_size;

  } TT_VarcContextRec, *TT_VarcContext;


  /**************************************************************************
   *
   * VARC API functions
   *
   */

  /* Load the VARC table from the font stream */
  FT_EXPORT( FT_Error )
  tt_face_load_varc( TT_Face    face,
                     FT_Stream  stream );

  /* Free the VARC table resources */
  FT_EXPORT( void )
  tt_face_free_varc( TT_Face  face );

  /* Check if a glyph has VARC data */
  FT_EXPORT( FT_Bool )
  tt_face_has_varc_glyph( TT_Face  face,
                          FT_UInt  glyph_index );

  /* Load a VARC glyph into the glyph slot */
  FT_EXPORT( FT_Error )
  tt_face_load_varc_glyph( TT_Face       face,
                           FT_GlyphSlot  glyph_slot,
                           FT_UInt       glyph_index,
                           FT_Int32      load_flags );


#endif /* TT_CONFIG_OPTION_VARC */


FT_END_HEADER


#endif /* TTVARC_H_ */


/* END */
