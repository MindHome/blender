/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eduv
 *
 * Utilities for manipulating UV islands.
 *
 * \note This is similar to `GEO_uv_parametrizer.hh`,
 * however the data structures there don't support arbitrary topology
 * such as an edge with 3 or more faces using it.
 * This API uses #BMesh data structures and doesn't have limitations for manifold meshes.
 */

#include "MEM_guardedalloc.h"

#include "DNA_meshdata_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BLI_boxpack_2d.h"
#include "BLI_convexhull_2d.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_rect.h"

#include "BKE_customdata.h"
#include "BKE_editmesh.h"
#include "BKE_image.h"

#include "DEG_depsgraph.h"

#include "ED_uvedit.h" /* Own include. */

#include "GEO_uv_pack.hh"

#include "WM_api.h"
#include "WM_types.h"

#include "bmesh.h"

static void mul_v2_m2_add_v2v2(float r[2],
                               const float mat[2][2],
                               const float a[2],
                               const float b[2])
{
  /* Compute `r = mat * (a + b)` with high precision. */
  const double x = double(a[0]) + double(b[0]);
  const double y = double(a[1]) + double(b[1]);

  r[0] = float(mat[0][0] * x + mat[1][0] * y);
  r[1] = float(mat[0][1] * x + mat[1][1] * y);
}

static void island_uv_transform(FaceIsland *island,
                                const float matrix[2][2],    /* Scale and rotation. */
                                const float pre_translate[2] /* (pre) Translation. */
)
{
  /* Use a pre-transform to compute `A * (x+b)`
   *
   * \note Ordinarily, we'd use a post_transform like `A * x + b`
   * In general, post-transforms are easier to work with when using homogenous co-ordinates.
   *
   * When UV mapping into the unit square, post-transforms can lose precision on small islands.
   * Instead we're using a pre-transform to maintain precision.
   *
   * To convert post-transform to pre-transform, use `A * x + b == A * (x + c), c = A^-1 * b`
   */

  const int cd_loop_uv_offset = island->offsets.uv;
  const int faces_len = island->faces_len;
  for (int i = 0; i < faces_len; i++) {
    BMFace *f = island->faces[i];
    BMLoop *l;
    BMIter iter;
    BM_ITER_ELEM (l, &iter, f, BM_LOOPS_OF_FACE) {
      float *luv = BM_ELEM_CD_GET_FLOAT_P(l, cd_loop_uv_offset);
      mul_v2_m2_add_v2v2(luv, matrix, luv, pre_translate);
    }
  }
}

/* -------------------------------------------------------------------- */
/** \name UV Face Array Utilities
 * \{ */

static void bm_face_array_calc_bounds(BMFace **faces,
                                      const int faces_len,
                                      const int cd_loop_uv_offset,
                                      rctf *r_bounds_rect)
{
  BLI_assert(cd_loop_uv_offset >= 0);
  float bounds_min[2], bounds_max[2];
  INIT_MINMAX2(bounds_min, bounds_max);
  for (int i = 0; i < faces_len; i++) {
    BMFace *f = faces[i];
    BM_face_uv_minmax(f, bounds_min, bounds_max, cd_loop_uv_offset);
  }
  r_bounds_rect->xmin = bounds_min[0];
  r_bounds_rect->ymin = bounds_min[1];
  r_bounds_rect->xmax = bounds_max[0];
  r_bounds_rect->ymax = bounds_max[1];
}

/**
 * Return an array of un-ordered UV coordinates,
 * without duplicating coordinates for loops that share a vertex.
 */
static float (*bm_face_array_calc_unique_uv_coords(
    BMFace **faces, int faces_len, const int cd_loop_uv_offset, int *r_coords_len))[2]
{
  BLI_assert(cd_loop_uv_offset >= 0);
  int coords_len_alloc = 0;
  for (int i = 0; i < faces_len; i++) {
    BMFace *f = faces[i];
    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      BM_elem_flag_enable(l_iter, BM_ELEM_TAG);
    } while ((l_iter = l_iter->next) != l_first);
    coords_len_alloc += f->len;
  }

  float(*coords)[2] = static_cast<float(*)[2]>(
      MEM_mallocN(sizeof(*coords) * coords_len_alloc, __func__));
  int coords_len = 0;

  for (int i = 0; i < faces_len; i++) {
    BMFace *f = faces[i];
    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      if (!BM_elem_flag_test(l_iter, BM_ELEM_TAG)) {
        /* Already walked over, continue. */
        continue;
      }

      BM_elem_flag_disable(l_iter, BM_ELEM_TAG);
      const float *luv = BM_ELEM_CD_GET_FLOAT_P(l_iter, cd_loop_uv_offset);
      copy_v2_v2(coords[coords_len++], luv);

      /* Un tag all connected so we don't add them twice.
       * Note that we will tag other loops not part of `faces` but this is harmless,
       * since we're only turning off a tag. */
      BMVert *v_pivot = l_iter->v;
      BMEdge *e_first = v_pivot->e;
      const BMEdge *e = e_first;
      do {
        if (e->l != nullptr) {
          const BMLoop *l_radial = e->l;
          do {
            if (l_radial->v == l_iter->v) {
              if (BM_elem_flag_test(l_radial, BM_ELEM_TAG)) {
                const float *luv_radial = BM_ELEM_CD_GET_FLOAT_P(l_radial, cd_loop_uv_offset);
                if (equals_v2v2(luv, luv_radial)) {
                  /* Don't add this UV when met in another face in `faces`. */
                  BM_elem_flag_disable(l_iter, BM_ELEM_TAG);
                }
              }
            }
          } while ((l_radial = l_radial->radial_next) != e->l);
        }
      } while ((e = BM_DISK_EDGE_NEXT(e, v_pivot)) != e_first);
    } while ((l_iter = l_iter->next) != l_first);
  }
  *r_coords_len = coords_len;
  return coords;
}

static void face_island_uv_rotate_fit_aabb(FaceIsland *island)
{
  BMFace **faces = island->faces;
  const int faces_len = island->faces_len;
  const float aspect_y = island->aspect_y;
  const int cd_loop_uv_offset = island->offsets.uv;

  /* Calculate unique coordinates since calculating a convex hull can be an expensive operation. */
  int coords_len;
  float(*coords)[2] = bm_face_array_calc_unique_uv_coords(
      faces, faces_len, cd_loop_uv_offset, &coords_len);

  /* Correct aspect ratio. */
  if (aspect_y != 1.0f) {
    for (int i = 0; i < coords_len; i++) {
      coords[i][1] /= aspect_y;
    }
  }

  float angle = BLI_convexhull_aabb_fit_points_2d(coords, coords_len);

  /* Rotate coords by `angle` before computing bounding box. */
  if (angle != 0.0f) {
    float matrix[2][2];
    angle_to_mat2(matrix, angle);
    matrix[0][1] *= aspect_y;
    matrix[1][1] *= aspect_y;
    for (int i = 0; i < coords_len; i++) {
      mul_m2_v2(matrix, coords[i]);
    }
  }

  /* Compute new AABB. */
  float bounds_min[2], bounds_max[2];
  INIT_MINMAX2(bounds_min, bounds_max);
  for (int i = 0; i < coords_len; i++) {
    minmax_v2v2_v2(bounds_min, bounds_max, coords[i]);
  }

  float size[2];
  sub_v2_v2v2(size, bounds_max, bounds_min);
  if (size[1] < size[0]) {
    angle += DEG2RADF(90.0f);
  }

  MEM_freeN(coords);

  /* Apply rotation back to BMesh. */
  if (angle != 0.0f) {
    float matrix[2][2];
    float pre_translate[2] = {0, 0};
    angle_to_mat2(matrix, angle);
    matrix[1][0] *= 1.0f / aspect_y;
    /* matrix[1][1] *= aspect_y / aspect_y; */
    matrix[0][1] *= aspect_y;
    island_uv_transform(island, matrix, pre_translate);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name UDIM packing helper functions
 * \{ */

bool uv_coords_isect_udim(const Image *image, const int udim_grid[2], const float coords[2])
{
  const float coords_floor[2] = {floorf(coords[0]), floorf(coords[1])};
  const bool is_tiled_image = image && (image->source == IMA_SRC_TILED);

  if (coords[0] < udim_grid[0] && coords[0] > 0 && coords[1] < udim_grid[1] && coords[1] > 0) {
    return true;
  }
  /* Check if selection lies on a valid UDIM image tile. */
  if (is_tiled_image) {
    LISTBASE_FOREACH (const ImageTile *, tile, &image->tiles) {
      const int tile_index = tile->tile_number - 1001;
      const int target_x = (tile_index % 10);
      const int target_y = (tile_index / 10);
      if (coords_floor[0] == target_x && coords_floor[1] == target_y) {
        return true;
      }
    }
  }
  /* Probably not required since UDIM grid checks for 1001. */
  else if (image && !is_tiled_image) {
    if (is_zero_v2(coords_floor)) {
      return true;
    }
  }

  return false;
}

/**
 * Calculates distance to nearest UDIM image tile in UV space and its UDIM tile number.
 */
static float uv_nearest_image_tile_distance(const Image *image,
                                            const float coords[2],
                                            float nearest_tile_co[2])
{
  BKE_image_find_nearest_tile_with_offset(image, coords, nearest_tile_co);

  /* Add 0.5 to get tile center coordinates. */
  float nearest_tile_center_co[2] = {nearest_tile_co[0], nearest_tile_co[1]};
  add_v2_fl(nearest_tile_center_co, 0.5f);

  return len_squared_v2v2(coords, nearest_tile_center_co);
}

/**
 * Calculates distance to nearest UDIM grid tile in UV space and its UDIM tile number.
 */
static float uv_nearest_grid_tile_distance(const int udim_grid[2],
                                           const float coords[2],
                                           float nearest_tile_co[2])
{
  const float coords_floor[2] = {floorf(coords[0]), floorf(coords[1])};

  if (coords[0] > udim_grid[0]) {
    nearest_tile_co[0] = udim_grid[0] - 1;
  }
  else if (coords[0] < 0) {
    nearest_tile_co[0] = 0;
  }
  else {
    nearest_tile_co[0] = coords_floor[0];
  }

  if (coords[1] > udim_grid[1]) {
    nearest_tile_co[1] = udim_grid[1] - 1;
  }
  else if (coords[1] < 0) {
    nearest_tile_co[1] = 0;
  }
  else {
    nearest_tile_co[1] = coords_floor[1];
  }

  /* Add 0.5 to get tile center coordinates. */
  float nearest_tile_center_co[2] = {nearest_tile_co[0], nearest_tile_co[1]};
  add_v2_fl(nearest_tile_center_co, 0.5f);

  return len_squared_v2v2(coords, nearest_tile_center_co);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Calculate UV Islands
 * \{ */

struct SharedUVLoopData {
  BMUVOffsets offsets;
  bool use_seams;
};

static bool bm_loop_uv_shared_edge_check(const BMLoop *l_a, const BMLoop *l_b, void *user_data)
{
  const struct SharedUVLoopData *data = static_cast<const struct SharedUVLoopData *>(user_data);

  if (data->use_seams) {
    if (BM_elem_flag_test(l_a->e, BM_ELEM_SEAM)) {
      return false;
    }
  }

  return BM_loop_uv_share_edge_check((BMLoop *)l_a, (BMLoop *)l_b, data->offsets.uv);
}

/**
 * Returns true if `efa` is able to be affected by a packing operation, given various parameters.
 *
 * Checks if it's (not) hidden, and optionally selected, and/or UV selected.
 *
 * Will eventually be superseded by `BM_uv_element_map_create()`.
 *
 * Loosely based on `uvedit_is_face_affected`, but "bug-compatible" with previous code.
 */
static bool uvedit_is_face_affected_for_calc_uv_islands(const Scene *scene,
                                                        BMFace *efa,
                                                        const bool only_selected_faces,
                                                        const bool only_selected_uvs,
                                                        const BMUVOffsets &uv_offsets)
{
  if (BM_elem_flag_test(efa, BM_ELEM_HIDDEN)) {
    return false;
  }
  if (only_selected_faces) {
    if (only_selected_uvs) {
      return BM_elem_flag_test(efa, BM_ELEM_SELECT) &&
             uvedit_face_select_test(scene, efa, uv_offsets);
    }
    return BM_elem_flag_test(efa, BM_ELEM_SELECT);
  }
  return true;
}

/**
 * Calculate islands and add them to \a island_list returning the number of items added.
 */
int bm_mesh_calc_uv_islands(const Scene *scene,
                            BMesh *bm,
                            ListBase *island_list,
                            const bool only_selected_faces,
                            const bool only_selected_uvs,
                            const bool use_seams,
                            const float aspect_y,
                            const BMUVOffsets uv_offsets)
{
  BLI_assert(uv_offsets.uv >= 0);
  int island_added = 0;
  BM_mesh_elem_table_ensure(bm, BM_FACE);

  int *groups_array = static_cast<int *>(
      MEM_mallocN(sizeof(*groups_array) * size_t(bm->totface), __func__));

  int(*group_index)[2];

  /* Set the tag for `BM_mesh_calc_face_groups`. */
  BMFace *f;
  BMIter iter;
  BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
    const bool face_affected = uvedit_is_face_affected_for_calc_uv_islands(
        scene, f, only_selected_faces, only_selected_uvs, uv_offsets);
    BM_elem_flag_set(f, BM_ELEM_TAG, face_affected);
  }

  struct SharedUVLoopData user_data = {{0}};
  user_data.offsets = uv_offsets;
  user_data.use_seams = use_seams;

  const int group_len = BM_mesh_calc_face_groups(bm,
                                                 groups_array,
                                                 &group_index,
                                                 nullptr,
                                                 bm_loop_uv_shared_edge_check,
                                                 &user_data,
                                                 BM_ELEM_TAG,
                                                 BM_EDGE);

  for (int i = 0; i < group_len; i++) {
    const int faces_start = group_index[i][0];
    const int faces_len = group_index[i][1];
    BMFace **faces = static_cast<BMFace **>(MEM_mallocN(sizeof(*faces) * faces_len, __func__));

    float bounds_min[2], bounds_max[2];
    INIT_MINMAX2(bounds_min, bounds_max);

    for (int j = 0; j < faces_len; j++) {
      faces[j] = BM_face_at_index(bm, groups_array[faces_start + j]);
    }

    struct FaceIsland *island = static_cast<struct FaceIsland *>(
        MEM_callocN(sizeof(*island), __func__));
    island->faces = faces;
    island->faces_len = faces_len;
    island->offsets = uv_offsets;
    island->aspect_y = aspect_y;
    BLI_addtail(island_list, island);
    island_added += 1;
  }

  MEM_freeN(groups_array);
  MEM_freeN(group_index);
  return island_added;
}

/** \} */

static bool island_has_pins(const Scene *scene,
                            FaceIsland *island,
                            const UVPackIsland_Params *params)
{
  const bool pin_unselected = params->pin_unselected;
  const bool only_selected_faces = params->only_selected_faces;
  BMLoop *l;
  BMIter iter;
  const int pin_offset = island->offsets.pin;
  for (int i = 0; i < island->faces_len; i++) {
    BMFace *efa = island->faces[i];
    if (pin_unselected && only_selected_faces && !BM_elem_flag_test(efa, BM_ELEM_SELECT)) {
      return true;
    }
    BM_ITER_ELEM (l, &iter, island->faces[i], BM_LOOPS_OF_FACE) {
      if (BM_ELEM_CD_GET_BOOL(l, pin_offset)) {
        return true;
      }
      if (pin_unselected && !uvedit_uv_select_test(scene, l, island->offsets)) {
        return true;
      }
    }
  }
  return false;
}

/* -------------------------------------------------------------------- */
/** \name Public UV Island Packing
 *
 * \note This behavior loosely follows #geometry::uv_parametrizer_pack.
 * \{ */

void ED_uvedit_pack_islands_multi(const Scene *scene,
                                  Object **objects,
                                  const uint objects_len,
                                  BMesh **bmesh_override,
                                  const UVMapUDIM_Params *closest_udim,
                                  const UVPackIsland_Params *params)
{
  blender::Vector<FaceIsland *> island_vector;

  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *obedit = objects[ob_index];
    BMesh *bm = nullptr;
    if (bmesh_override) {
      /* Note: obedit is still required for aspect ratio and ID_RECALC_GEOMETRY. */
      bm = bmesh_override[ob_index];
    }
    else {
      BMEditMesh *em = BKE_editmesh_from_object(obedit);
      bm = em->bm;
    }
    BLI_assert(bm);

    const BMUVOffsets offsets = BM_uv_map_get_offsets(bm);
    if (offsets.uv == -1) {
      continue;
    }

    const float aspect_y = params->correct_aspect ? ED_uvedit_get_aspect_y(obedit) : 1.0f;

    bool only_selected_faces = params->only_selected_faces;
    bool only_selected_uvs = params->only_selected_uvs;
    if (params->ignore_pinned && params->pin_unselected) {
      only_selected_faces = false;
      only_selected_uvs = false;
    }
    ListBase island_list = {nullptr};
    bm_mesh_calc_uv_islands(scene,
                            bm,
                            &island_list,
                            only_selected_faces,
                            only_selected_uvs,
                            params->use_seams,
                            aspect_y,
                            offsets);

    /* Remove from linked list and append to blender::Vector. */
    LISTBASE_FOREACH_MUTABLE (struct FaceIsland *, island, &island_list) {
      BLI_remlink(&island_list, island);
      if (params->ignore_pinned && island_has_pins(scene, island, params)) {
        MEM_freeN(island->faces);
        MEM_freeN(island);
        continue;
      }
      island_vector.append(island);
    }
  }

  if (island_vector.size() == 0) {
    return;
  }

  /* Coordinates of bounding box containing all selected UVs. */
  float selection_min_co[2], selection_max_co[2];
  INIT_MINMAX2(selection_min_co, selection_max_co);

  for (int index = 0; index < island_vector.size(); index++) {
    FaceIsland *island = island_vector[index];
    if (closest_udim) {
      /* Only calculate selection bounding box if using closest_udim. */
      for (int i = 0; i < island->faces_len; i++) {
        BMFace *f = island->faces[i];
        BM_face_uv_minmax(f, selection_min_co, selection_max_co, island->offsets.uv);
      }
    }

    if (params->rotate) {
      face_island_uv_rotate_fit_aabb(island);
    }

    bm_face_array_calc_bounds(
        island->faces, island->faces_len, island->offsets.uv, &island->bounds_rect);
  }

  /* Center of bounding box containing all selected UVs. */
  float selection_center[2];
  if (closest_udim) {
    selection_center[0] = (selection_min_co[0] + selection_max_co[0]) / 2.0f;
    selection_center[1] = (selection_min_co[1] + selection_max_co[1]) / 2.0f;
  }

  float scale[2] = {1.0f, 1.0f};
  blender::Vector<blender::geometry::PackIsland *> pack_island_vector;
  for (int i = 0; i < island_vector.size(); i++) {
    FaceIsland *face_island = island_vector[i];
    blender::geometry::PackIsland *pack_island = new blender::geometry::PackIsland();
    pack_island->bounds_rect = face_island->bounds_rect;
    pack_island_vector.append(pack_island);
  }
  BoxPack *box_array = pack_islands(pack_island_vector, *params, scale);

  float base_offset[2] = {0.0f, 0.0f};
  copy_v2_v2(base_offset, params->udim_base_offset);

  if (closest_udim) {
    const Image *image = closest_udim->image;
    const int *udim_grid = closest_udim->grid_shape;
    /* Check if selection lies on a valid UDIM grid tile. */
    bool is_valid_udim = uv_coords_isect_udim(image, udim_grid, selection_center);
    if (is_valid_udim) {
      base_offset[0] = floorf(selection_center[0]);
      base_offset[1] = floorf(selection_center[1]);
    }
    /* If selection doesn't lie on any UDIM then find the closest UDIM grid or image tile. */
    else {
      float nearest_image_tile_co[2] = {FLT_MAX, FLT_MAX};
      float nearest_image_tile_dist = FLT_MAX, nearest_grid_tile_dist = FLT_MAX;
      if (image) {
        nearest_image_tile_dist = uv_nearest_image_tile_distance(
            image, selection_center, nearest_image_tile_co);
      }

      float nearest_grid_tile_co[2] = {0.0f, 0.0f};
      nearest_grid_tile_dist = uv_nearest_grid_tile_distance(
          udim_grid, selection_center, nearest_grid_tile_co);

      base_offset[0] = (nearest_image_tile_dist < nearest_grid_tile_dist) ?
                           nearest_image_tile_co[0] :
                           nearest_grid_tile_co[0];
      base_offset[1] = (nearest_image_tile_dist < nearest_grid_tile_dist) ?
                           nearest_image_tile_co[1] :
                           nearest_grid_tile_co[1];
    }
  }

  float matrix[2][2];
  float matrix_inverse[2][2];
  float pre_translate[2];
  for (int i = 0; i < island_vector.size(); i++) {
    FaceIsland *island = island_vector[box_array[i].index];
    matrix[0][0] = scale[0];
    matrix[0][1] = 0.0f;
    matrix[1][0] = 0.0f;
    matrix[1][1] = scale[1];
    invert_m2_m2(matrix_inverse, matrix);

    /* Add base_offset, post transform. */
    mul_v2_m2v2(pre_translate, matrix_inverse, base_offset);

    /* Translate to box_array from bounds_rect. */
    pre_translate[0] += box_array[i].x - island->bounds_rect.xmin;
    pre_translate[1] += box_array[i].y - island->bounds_rect.ymin;
    island_uv_transform(island, matrix, pre_translate);
  }

  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *obedit = objects[ob_index];
    DEG_id_tag_update(static_cast<ID *>(obedit->data), ID_RECALC_GEOMETRY);
    WM_main_add_notifier(NC_GEOM | ND_DATA, obedit->data);
  }

  for (FaceIsland *island : island_vector) {
    MEM_freeN(island->faces);
    MEM_freeN(island);
  }

  for (int i = 0; i < pack_island_vector.size(); i++) {
    blender::geometry::PackIsland *pack_island = pack_island_vector[i];
    pack_island_vector[i] = nullptr;
    delete pack_island;
  }

  MEM_freeN(box_array);
}

/** \} */
