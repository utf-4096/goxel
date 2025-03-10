/* Goxel 3D voxels editor
 *
 * copyright (c) 2016 Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.

 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.

 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "goxel.h"
#include "xxhash.h"

camera_t *camera_new(const char *name)
{
    camera_t *cam = calloc(1, sizeof(*cam));
    if (name)
        strncpy(cam->name, name, sizeof(cam->name) - 1);
    mat4_set_identity(cam->mat);
    cam->dist = 96;
    cam->aspect = 1;
    cam->speed = 2.5;
    cam->fovy = 40.;
    cam->fovy_fpv = 100.;
    mat4_itranslate(cam->mat, 0, 0, cam->dist);
    camera_turntable(cam, M_PI / 4, M_PI / 4);
    return cam;
}

void camera_delete(camera_t *cam)
{
    if (!cam) return;
    if (--cam->ref > 0) return;
    free(cam);
}

camera_t *camera_copy(const camera_t *other)
{
    camera_t *cam = malloc(sizeof(*cam));
    *cam = *other;
    cam->ref = 1;
    cam->next = cam->prev = NULL;
    return cam;
}

void camera_set(camera_t *cam, const camera_t *other)
{
    cam->ortho = other->ortho;
    cam->dist = other->dist;
    cam->fpv = other->fpv;
    cam->speed = other->speed;
    cam->fovy = other->fovy;
    cam->fovy_fpv = other->fovy_fpv;
    cam->prev_dist = other->prev_dist;
    cam->prev_ortho = other->prev_ortho;
    mat4_copy(other->mat, cam->mat);
}

static void compute_clip(const float view_mat[4][4], float *near_, float *far_)
{
    int bpos[3];
    float p[3];
    float n = FLT_MAX, f = 256;
    int i;
    const int margin = 8 * BLOCK_SIZE;
    float vertices[8][3];
    const volume_t *volume = goxel_get_layers_volume(goxel.image);
    volume_iterator_t iter;

    if (!box_is_null(goxel.image->box)) {
        box_get_vertices(goxel.image->box, vertices);
        for (i = 0; i < 8; i++) {
            mat4_mul_vec3(view_mat, vertices[i], p);
            if (p[2] < 0) {
                n = min(n, -p[2] - margin);
                f = max(f, -p[2] + margin);
            }
        }
    }

    iter = volume_get_iterator(volume, VOLUME_ITER_TILES);
    while (volume_iter(&iter, bpos)) {
        vec3_set(p, bpos[0], bpos[1], bpos[2]);
        mat4_mul_vec3(view_mat, p, p);
        if (p[2] < 0) {
            n = min(n, -p[2] - margin);
            f = max(f, -p[2] + margin);
        }
    }
    if (n >= f) n = 1;
    n = max(n, 0.1);
    *near_ = n;
    *far_ = f;
}

void camera_update(camera_t *camera)
{
    float size;
    float clip_near, clip_far;

    mat4_invert(camera->mat, camera->view_mat);
    compute_clip(camera->view_mat, &clip_near, &clip_far);
    if (camera->ortho) {
        size = camera->dist;
        mat4_ortho(camera->proj_mat,
                -size, +size,
                -size / camera->aspect, +size / camera->aspect,
                clip_near, clip_far);
    } else {
        mat4_perspective(camera->proj_mat,
                (camera->fpv) ? camera->fovy_fpv : camera->fovy,
                camera->aspect, clip_near, clip_far);
    }
}

// Get the raytracing ray of the camera at a given screen position.
void camera_get_ray(const camera_t *camera, const float win[2],
                    const float viewport[4], float o[3], float d[3])
{
    float o1[3], o2[3], p[3];
    vec3_set(p, win[0], win[1], 0);
    unproject(p, camera->view_mat, camera->proj_mat, viewport, o1);
    vec3_set(p, win[0], win[1], 1);
    unproject(p, camera->view_mat, camera->proj_mat, viewport, o2);
    vec3_copy(o1, o);
    vec3_sub(o2, o1, d);
    vec3_normalize(d, d);
}

// Adjust the camera settings so that the rotation works for a given
// position.
void camera_set_target(camera_t *cam, const float pos[3])
{
    float world_to_mat[4][4], p[3];
    mat4_invert(cam->mat, world_to_mat);
    mat4_mul_vec3(world_to_mat, pos, p);
    cam->dist = -p[2];
}

/*
 * Function: camera_fit_box
 * Move a camera so that a given box is entirely visible.
 */
void camera_fit_box(camera_t *cam, const float box[4][4])
{
    float size[3], dist;
    if (box_is_null(box)) {
        cam->dist = 128;
        cam->aspect = 1;
        return;
    }
    box_get_size(box, size);
    // XXX: not the proper way to compute the distance.
    dist = max3(size[0], size[1], size[2]) * 8;
    mat4_mul_vec3(box, VEC(0, 0, 0), cam->mat[3]);
    mat4_itranslate(cam->mat, 0, 0, dist);
    cam->dist = dist;
}

/*
 * Function: camera_get_key
 * Return a value that is guarantied to change when the camera change.
 */
uint32_t camera_get_key(const camera_t *cam)
{
    uint32_t key = 0;
    key = XXH32(&cam->name, sizeof(cam->name), key);
    key = XXH32(&cam->ortho, sizeof(cam->ortho), key);
    key = XXH32(&cam->dist, sizeof(cam->dist), key);
    key = XXH32(&cam->mat, sizeof(cam->mat), key);    
    key = XXH32(&cam->fpv, sizeof(cam->fpv), key);
    key = XXH32(&cam->fovy, sizeof(cam->fovy), key);
    key = XXH32(&cam->fovy_fpv, sizeof(cam->fovy_fpv), key);
    return key;
}

void camera_turntable(camera_t *camera, float rz, float rx)
{
    float center[3], mat[4][4] = MAT4_IDENTITY;

    mat4_mul_vec3(camera->mat, VEC(0, 0, -camera->dist), center);   // center (target) = 'dist' units away from camera in current direction
    mat4_itranslate(mat, center[0], center[1], center[2]);          // move camera to the target
    mat4_irotate(mat, rz, 0, 0, 1);                                 // rotate the camera vertically around the center point
    mat4_itranslate(mat, -center[0], -center[1], -center[2]);
    mat4_imul(mat, camera->mat);
    mat4_copy(mat, camera->mat);

    mat4_itranslate(camera->mat, 0, 0, -camera->dist);
    mat4_irotate(camera->mat, rx, 1, 0, 0);
    mat4_itranslate(camera->mat, 0, 0, camera->dist);
}

/* First person move
 * rz: up is +ve, down is -ve.
 * ry: forward is +ve, backwards is -ve.
 * rx - right is +ve, left is -ve.
 */
void camera_move(camera_t *cam, float rx, float ry, float rz)
{
    float mat[4][4];
    mat4_copy(cam->mat, mat);

    float multiplier = cam->speed / 20;

    mat4_itranslate(mat, 0, 0, ry*multiplier);
    mat4_itranslate(mat, rx*multiplier, 0, 0);

    // in mat[4][4], camera x/y/z position is [3][0]/[3][1]/[3][2]
    // z is just up/down in world space
    mat[3][2] += rz*multiplier;

    mat4_copy(mat, cam->mat);
}

/* Perform some property caching and edits post switching fpv on/off. */
void post_toggle_fpv(camera_t *cam)
{
    if (cam->fpv) {
        // If switching to first person
        // Stash current dist and replace with 0 for duration
        cam->prev_dist = cam->dist;
        cam->dist = 0;
        cam->prev_ortho = cam->ortho;
        cam->ortho = false;
    } else {
        // Switching off fpv, restore previous dist if applicable
        cam->dist = cam->prev_dist;
        cam->ortho = cam->prev_ortho;
    }
}
