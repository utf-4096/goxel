/* Goxel 3D voxels editor
 *
 * copyright (c) 2024-present Guillaume Chereau <guillaume@noctua-software.com>
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
#include <time.h>
#include <math.h>

/*
 * Filter to place doodads on top of the image.
 * 
 * TODO:
 *   - Choose whether to base on image or on a specific layer?
 *   - Replace rotation checkboxes with a combo box
 *   - Coverage %
 *   - Min distance between placements?
 */

static file_format_t *g_current = NULL;

typedef struct doodad_model doodad_model_t;
struct doodad_model
{
    int ref;
    doodad_model_t  *next, *prev;
    const char      *file_name;
    const char      *path;
    volume_t        *volume;
    float           translation[4][4];
};

typedef struct
{
    filter_t filter;
    int num_doodads;
    int max_placement_attempts;
    bool place_on_0;
    bool place_on_empty;
    bool rotate90;
    bool rotate45;
    bool rotate22pt5;
    bool randomly_flip;

    doodad_model_t *models;
    doodad_model_t *active_model;
} filter_doodadplacement_t;

static bool next_doodad_pos(filter_doodadplacement_t *filter, int *heights, int image_dimensions[3], int doodad_dimensions[3], int attempt, int out[3])
{
    //LOG_D("Image dimensions: %i by %i by %i", image_dimensions[0], image_dimensions[1], image_dimensions[2]);
    int x = random_int(0, image_dimensions[0] - 1);
    int y = random_int(0, image_dimensions[1] - 1);
    int z = heights[y * image_dimensions[0] + x];

    int w = ceil(doodad_dimensions[0] / 2.0f);
    int d = ceil(doodad_dimensions[1] / 2.0f);
    //int h = ceil(doodad_dimensions[2] / 2.0f);
    //LOG_D("Attempt %i: %i/%i/%i, w: %i, d: %i, h: %i", attempt, x, y, z, w, d, h);
    if (attempt > filter->max_placement_attempts)
    {
        LOG_D("Attempted %i times to acquire suitable placement and failed, stopping", filter->max_placement_attempts);
        return false;
    }
    
    //LOG_D("x+w+1 (%i) >= %i?, x-w-1 (%i) <= 0?, y+d+1 (%i) >= %i?, y-d-1 (%i) <= 0", (x+w+1), image_dimensions[0], (x-w-1), (y+d+1), image_dimensions[1], (y-w-1));
    if (z + doodad_dimensions[2] >= image_dimensions[2] || (!filter->place_on_empty && z == -1) || (!filter->place_on_0 && z == 0)
        || x + w + 1 >= image_dimensions[0] || x - w - 1 <= 0
        || y + d + 1 >= image_dimensions[1] || y - d - 1 <= 0)
    {
        attempt += 1;
        return next_doodad_pos(filter, heights, image_dimensions, doodad_dimensions, attempt, out);
    }
    else
    {
        out[0] = x;
        out[1] = y;
        out[2] = z + 1;
    }
    return true;
}

static int count_doodads(filter_doodadplacement_t *filter) {
    doodad_model_t *iter;
    int count;
    DL_COUNT(filter->models, iter, count);
    return count;
}

static doodad_model_t *choose_random_doodad_model(filter_doodadplacement_t *filter)
{
    doodad_model_t *iter;
    int i, idx, count;
    count = count_doodads(filter);
    // LOG_D("Count: %i", count);

    if (count == 0)
        return NULL;
    idx = count == 1 ? 0 : random_int(0, count - 1);
    i = 0;

    DL_FOREACH(filter->models, iter)
    {
        // LOG_D("Log: %i == %i ? %s", i, idx, iter->file_name);
        if (i == idx)
            break;
        i++;
    }
    return iter;
}

static void randomly_flip_rotate(filter_doodadplacement_t *filter, doodad_model_t *doodad, float trans[4][4])
{
    if (filter->randomly_flip) {
        int i = random_int(0, 3);
        // 0 = no flipping
        if (i == 1 || i == 3) mat4_iscale(trans, -1,  1,  1); // flip x
        if (i == 2 || i == 3) mat4_iscale(trans, 1,  -1,  1); // flip y
    }
    if (!filter->rotate90 && !filter->rotate45 && !filter->rotate22pt5) return;

    int possible = 4;
    if (filter->rotate22pt5) possible = 16;
    if (filter->rotate45) possible = 8;
    
    mat4_irotate(trans, (M_PI / possible) * random_int(1, possible), 0, 0, 1);
}

static void set_initial_offset(doodad_model_t *doodad, float trans[4][4])
{
    float box[4][4];
    int start_pos[3];
    volume_get_box(doodad->volume, true, box);
    box_get_start_pos(box, start_pos);

    // Offset overall based on start position of volume (because some outputs are completely elsewhere to begin with)
    trans[3][0] -= start_pos[0];
    trans[3][1] -= start_pos[1];
    trans[3][2] -= start_pos[2];
}

static void dynamically_offset(doodad_model_t *doodad, float trans[4][4])
{
    float box[4][4];
    int x, y, z, num_found_blocks, dimensions[3], start_pos[3], pos[3];
    int positions[3] = { 0, 0, 0 };
    float offset[3] = { 0, 0, 0 };
    uint8_t color[4];
    volume_iterator_t iter;
    volume_get_box(doodad->volume, true, box);
    box_get_dimensions(box, dimensions);
    box_get_start_pos(box, start_pos);

    iter = volume_get_iterator(doodad->volume, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);

    //LOG_D("Dimensions: %i by %i by %i", dimensions[0], dimensions[1], dimensions[2]);
    for (z = 0; z < dimensions[2]; z++) {
        num_found_blocks = 0;
        pos[2] = z + start_pos[2];
        for (x = 0; x < dimensions[0]; x++) {
            for (y = 0; y < dimensions[1]; y++) {
                pos[0] = x + start_pos[0];
                pos[1] = y + start_pos[1];

                volume_get_at(doodad->volume, &iter, pos, color);
                if (color[3] != 0) {
                    positions[0] += x;
                    positions[1] += y;
                    positions[2] += z;
                    num_found_blocks++;
                }
            }
        }
        if (num_found_blocks > 0) {
            offset[0] += (int)floor(positions[0] / num_found_blocks);
            offset[1] += (int)floor(positions[1] / num_found_blocks);
            offset[2] += (int)floor(positions[2] / num_found_blocks);
            //LOG_I("Doodad offset acquired: %f/%f/%f", offset[0], offset[1], offset[2]);
            vec3_isub(trans[3], offset);
            return;
        }
    }
    LOG_W("Unable to find offset of doodad '%s'", doodad->file_name);
}

static void place_doodads(filter_doodadplacement_t *filter)
{
    float box[4][4];
    int dimensions[3], start_pos[3], *heights;

    mat4_copy(goxel.image->box, box);
    box_get_dimensions(box, dimensions);
    box_get_start_pos(box, start_pos);

    if (dimensions[0] == 0 || dimensions[1] == 0)
    {
        LOG_W("Image has a 0 dimension, not running the script");
        return;
    }

    //clock_t start = clock(); // Start timing
    allocate_heights(dimensions, &heights);
    volume_get_heights_in_box(goxel_get_layers_volume(goxel.image), dimensions, start_pos, heights);
    //clock_t end = clock(); // End timing
    //double elapsed_time = (((double)(end - start)) / CLOCKS_PER_SEC) * 1000;
    //printf("Function took %.3fms\n", elapsed_time);

    int idx, pos[3];
    for (idx = 0; idx < filter->num_doodads; idx++)
    {
        doodad_model_t *doodad = choose_random_doodad_model(filter);
        if (!doodad)
        {
            LOG_W("Unable to acquire a doodad from the list");
            return;
        }
        volume_t *doodad_clone = volume_copy(doodad->volume);

        //const uint8_t c1[4] = { 255, 255, 255, 255 };
        //const uint8_t c2[4] = { 180, 180, 180, 255 };
        //const uint8_t c3[4] = { 120, 120, 120, 255 };
        //unint8_t c4[4] = { 70, 70, 70, 255 };
        //volume_merge(goxel.image->active_layer->volume, doodad_clone, MODE_INTERSECT, NULL);

        // Move doodad to its own 0
        float trans[4][4] = MAT4_IDENTITY;
        mat4_copy(doodad->translation, trans);
        volume_move(doodad_clone, trans);
        //volume_merge(goxel.image->active_layer->volume, doodad_clone, MODE_MAX, c2);

        // Find center of lowest blocks and offset off that
        mat4_copy(mat4_identity, trans);
        dynamically_offset(doodad, trans);
        volume_move(doodad_clone, trans);
        //volume_merge(goxel.image->active_layer->volume, doodad_clone, MODE_MAX, c2);

        // Flip/rotate
        mat4_copy(mat4_identity, trans);
        randomly_flip_rotate(filter, doodad, trans);
        volume_move(doodad_clone, trans);
        //volume_merge(goxel.image->active_layer->volume, doodad_clone, MODE_MAX, c3);

        int doodad_dimensions[3];
        float doodad_box[4][4];
        volume_get_box(doodad_clone, true, doodad_box);
        box_get_dimensions(doodad_box, doodad_dimensions);
        // LOG_D("Random doodad: '%s', height: %i", doodad->file_name, model_height);

        if (!next_doodad_pos(filter, heights, dimensions, doodad_dimensions, 0, pos))
           break;
        

        //LOG_D("Start pos: %i/%i/%i", start_pos[0], start_pos[1], start_pos[2]);
        mat4_copy(mat4_identity, trans);
        trans[3][0] = start_pos[0] + pos[0];
        trans[3][1] = start_pos[1] + pos[1];
        trans[3][2] = start_pos[2] + pos[2];
        volume_move(doodad_clone, trans);
        volume_merge(goxel.image->active_layer->volume, doodad_clone, MODE_OVER, NULL);
        volume_delete(doodad_clone);
    }
}

static bool render_model_list_item(void *item, int idx, bool current)
{
    doodad_model_t *model = item;
    _model_item(idx, &current, model->file_name, sizeof(model->file_name));
    return current;
}

// Copied from placer.c
static void on_file_import(const char *path, const char *file_name, const file_format_t *format)
{
    // Do nothing
}
static void add_model(filter_doodadplacement_t *filter, const char *file_name, const char *path, volume_t *vol)
{
    doodad_model_t *new_model;
    new_model = calloc(1, sizeof(*new_model));
    *new_model = (doodad_model_t){
        .path = path,
        .file_name = file_name,
        .volume = vol,
        .translation = MAT4_IDENTITY
    };
    DL_APPEND(filter->models, new_model);
    filter->active_model = new_model;
    set_initial_offset(new_model, new_model->translation);
}
static const char *make_label(const file_format_t *f, char *buf, int len)
{
    const char *ext = f->exts[0] + 1;
    snprintf(buf, len, "%s (%s)", f->name, ext);
    return buf;
}
static void on_format(void *user, file_format_t *f)
{
    char label[128];
    make_label(f, label, sizeof(label));
    if (gui_combo_item(label, f == g_current))
    {
        g_current = f;
    }
}
static int gui(filter_t *filter_)
{
    filter_doodadplacement_t *filter = (void *)filter_;

    const char *help_text = "This filter takes in a list of doodads and randomly places them in the image.";
    goxel_set_help_text(help_text);

    if (gui_collapsing_header("Hint", false))
    {
        const char *hint = "This filter takes in a list of doodads and randomly places them in the image.\n"
            "It will grab the entire image and use the max heights it finds as potential placement spots (ignoring z=0 or empty if checkboxes are checked).\n"
            "It will take into account the size of the doodad and prevent placement which would take blocks out of bounds.";
        gui_text_wrapped(hint);
    }

    gui_text("Doodad list:");

    int doodad_count = count_doodads(filter);
    if (doodad_count == 0) {
        gui_text("[Empty]");
    } else {
        gui_list(&(gui_list_t){
            .items = (void **)&filter->models,
            .current = (void **)&filter->active_model,
            .render = render_model_list_item,
        });

        if (gui_button("Remove selected", 0, 0) && filter->active_model)
        {
            volume_delete(filter->active_model->volume);
            DL_DELETE(filter->models, filter->active_model);
            filter->active_model = NULL;
        }
    }

    gui_separator();
    // File importer
    char label[128];
    gui_text("Import as");
    if (!g_current)
        g_current = file_formats_import_to_volume; // First one.

    make_label(g_current, label, sizeof(label));
    if (gui_combo_begin("Import as", label))
    {
        file_format_iter("v", NULL, on_format);
        gui_combo_end();
    }

    if (g_current->import_gui)
        g_current->import_gui(g_current);

    if (gui_button("Import", 1, 0))
    {
        const char *path;

        if (!g_current)
            return -1;
        path = strdup(sys_open_file_dialog("Import", NULL, g_current->exts, g_current->exts_desc));
        if (!path)
            return -1;
        const char *file_name = strdup(get_file_name_from_path(path));
        volume_t *vol = volume_new();
        goxel_import_file_to_volume(path, g_current->name, vol, on_file_import);
        add_model(filter, file_name, path, vol);
    }

    gui_separator();

    if(gui_section_begin("Settings", GUI_SECTION_COLLAPSABLE)) {
        gui_input_int("# of doodads", &filter->num_doodads, 0, 9999);
        gui_input_int("Attempt limit", &filter->max_placement_attempts, 0, 999);
        gui_checkbox(
            "Place on lowest",
            &filter->place_on_0,
            "If checked, the placement won't ignore the bottom layer of the map.\n"
            "If unchecked, the placement will ignore the bottom layer of the map as a potential placement spot.");
        gui_checkbox(
            "Place on empty",
            &filter->place_on_empty,
            "If checked, the placement will allow placing where there are no blocks.\n"
            "If unchecked, the placement will require there to be blocks.");
    } gui_section_end();

    if (gui_section_begin("Variation", GUI_SECTION_COLLAPSABLE)) {
        gui_checkbox(
            "Rotate 90deg",
            &filter->rotate90,
            "If checked, rotations can be 90 degrees.\n"
            "If unchecked, rotations might not be.");
        gui_checkbox(
            "Rotate 45deg",
            &filter->rotate45,
            "If checked, rotations can be 45 degrees (and 90).\n"
            "If unchecked, rotations might not be.");
        gui_checkbox(
            "Rotate 22.5deg",
            &filter->rotate22pt5,
            "If checked, rotations can be 22.5 degrees (and the others).\n"
            "If unchecked, rotations might not be.");
        gui_checkbox(
            "Randomly flip",
            &filter->randomly_flip,
            "If checked, sometimes it'll flip.\n"
            "If unchecked, it won't flip.");
    }; gui_section_end();

    gui_separator();

    if (gui_button("Apply", -1, 0))
    {
        image_history_push(goxel.image);
        place_doodads(filter);
    }
    return 0;
}

static void on_open(filter_t *filter_)
{   
    float box[4][4];
    int dimensions[3];

    mat4_copy(goxel.image->box, box);
    box_get_dimensions(box, dimensions);
    filter_doodadplacement_t *filter = (void *)filter_;
    // Botched guestimating of how many can fit inside the dimensions
    filter->num_doodads = 0.35 * sqrt(dimensions[0]*dimensions[1]);
    filter->max_placement_attempts = 20;
    filter->place_on_0 = false;
    filter->place_on_empty = false;

    filter->rotate90 = true;
    filter->rotate45 = true;
    filter->rotate22pt5 = true;
    filter->randomly_flip = true;
}

FILTER_REGISTER(doodadplacer, filter_doodadplacement_t,
                .name = "Generation - Doodad placement",
                .on_open = on_open,
                .gui_fn = gui, )