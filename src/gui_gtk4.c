#define _GNU_SOURCE
#include "core.h"
#include "kovaaks_offsets.h"
#include "faf_telemetry.h"

#include <cairo.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *DARK_CSS =
    "window, .main-bg { background-color: #111318; color: #e2e8f0; font-family: 'Inter', 'Segoe UI', sans-serif; font-size: 13px; }\n"
    "frame { background-color: #171922; border: 1px solid #262a38; border-radius: 10px; padding: 10px; }\n"
    "frame > label { color: #00f0ff; font-weight: bold; font-size: 13px; padding: 0 6px; }\n"
    "button { background-image: none; background-color: #212636; border: 1px solid #31384e; border-radius: 6px; padding: 5px 10px; color: #f1f5f9; font-weight: 600; }\n"
    "button:hover { background-color: #2c3348; border-color: #00f0ff; color: #00f0ff; }\n"
    "button.primary-btn { background-color: #00d4ff; color: #0f172a; border: none; font-weight: bold; }\n"
    "button.primary-btn:hover { background-color: #38bdf8; color: #0f172a; }\n"
    "button.slot-saved { border-color: #4ade80; }\n"
    "button.slot-active, button.slot-active:checked { border-color: #00f0ff; border-width: 2px; }\n"
    "button:checked { background-color: #2c3348; color: #00f0ff; }\n"
    "label.dg-warn { color: #fbbf24; font-size: 12px; }\n"
    "label.dg-info { color: #e2e8f0; font-size: 12px; }\n"
    "label.dg-model { color: #00f0ff; font-weight: bold; font-size: 13px; }\n"
    "label.dg-mice { font-family: monospace; font-size: 11px; color: #cbd5e1; }\n"
    ".badge { background-color: #1e2536; border: 1px solid #334155; border-radius: 6px; padding: 3px 8px; font-size: 11px; font-weight: bold; }\n"
    ".badge-cyan { color: #38bdf8; }\n"
    ".badge-green { color: #4ade80; }\n"
    ".badge-pink { color: #f472b6; }\n"
    ".badge-amber { color: #fbbf24; }\n"
    "spinbutton, dropdown, entry { background-color: #1d212c; border: 1px solid #2e3547; border-radius: 6px; color: #00f0ff; font-weight: bold; }\n"
    "scale trough { background-color: #242938; border-radius: 3px; min-height: 6px; }\n"
    "scale highlight { background-color: #00f0ff; border-radius: 3px; }\n"
    "scale slider { background-color: #ffffff; border: 2px solid #00f0ff; border-radius: 8px; min-width: 14px; min-height: 14px; }\n";

typedef struct {
    GtkWidget *window;
    GtkWidget *drawing_area;

    /* Hardware status widgets */
    GtkWidget *lbl_mouse_ident;
    GtkWidget *lbl_rt_status;
    GtkWidget *badge_rate;
    GtkWidget *badge_bat;
    GtkWidget *badge_sync;
    GtkWidget *badge_rssi;
    GtkWidget *badge_dg;          /* the dongle's own polling rate toward the PC (shown while a dongle is linked) */
    GtkWidget *cpi_dropdown;
    GtkWidget *btn_cpi_3200;
    GtkWidget *btn_cpi_6400;
    GtkWidget *rate_dropdown;
    GtkStringList *rate_model;
    int        rate_values[8];
    int        rate_count;
    int        current_rate_hz;   /* Last rate the mouse reported (0 = unknown) */

    /* Device selection widgets */
    GtkWidget *device_dropdown;
    GtkStringList *device_model;
    GtkWidget *device_entry;
    GtkWidget *btn_identify;
    mouse_device_t devices[DEVICES_MAX];
    int        device_count;
    int        last_active_vid;
    int        last_active_pid;
    bool       last_grabbed;
    uint32_t   last_dongle_rev;
    uint32_t   last_mice_rev;
    int        tick_count;

    /* Dongle: stored profiles, and the mice behind it */
    GtkWidget *dg_frame;
    GtkWidget *dg_slot_btn[AMV_NUM_SLOTS];
    GtkWidget *btn_dg_load;
    GtkWidget *btn_dg_save;
    GtkWidget *chk_dg_bypass;
    GtkWidget *lbl_dg_status;
    GtkWidget *lbl_dg_warn;       /* why a mouse behind the dongle shows blanks */
    GtkWidget *lbl_dg_mice;       /* every device plugged into the dongle */
    GtkWidget *lbl_dg_model;      /* "AnyMouse Pro Dongle" / "AnyMouse Dongle", firmware */
    GtkWidget *lbl_dg_info;       /* the dongle's polling: toward the PC, and from the mice */
    int        dg_sel;            /* slot picked in the panel (0-based); -1 = follow the active slot */
    bool       dg_busy;           /* a slot / bypass request is running on its worker thread */
    uint32_t   last_slots_rev;
    uint32_t   dg_feedback_ticks;

    /* FpsAimForge installer widgets */
    GtkWidget *lbl_faf_install;
    GtkWidget *btn_faf_install;
    GtkWidget *btn_faf_remove;
    int        faf_feedback_ticks;

    /* Master & Symmetry controls */
    GtkWidget *master_scale;
    GtkWidget *master_spin;
    GtkWidget *chk_mirror_lr;
    GtkWidget *chk_mirror_ud;

    /* Per-octant widgets */
    GtkWidget *oct_scales[8];
    GtkWidget *oct_spins[8];
    GtkWidget *oct_dpi_lbls[8];
    GtkWidget *oct_ratio_lbls[8];

    /* KovaaK's & Ballistic Calibration widgets */
    GtkWidget *lbl_kovaaks_status;
    GtkWidget *lbl_csv_summary;
    GtkWidget *lbl_last_flick;
    GtkWidget *chk_auto_calib;
    GtkWidget *lbl_profile_status;

    /* Interactive radar state */
    float      max_scale;
    int        dragging_idx;
    int        hover_idx;
    double     drag_start_x;
    double     drag_start_y;
    double     grab_dx;           /* handle centre minus the click, so grabbing a dot never makes it jump */
    double     grab_dy;
    GtkWidget *root_box;          /* two columns side by side, or stacked when the window is narrow */
    bool       stacked;
    bool       mirror_lr;
    bool       mirror_ud;
    bool       updating_ui;
} studio_ui_t;

static studio_ui_t g_ui = {
    .max_scale = 0.60f,
    .dragging_idx = -1,
    .hover_idx = -1,
    .updating_ui = false
};

static const int CPI_OPTIONS[] = { 800, 1600, 3200, 6400, 8000, 12000, 26000 };
static const char *CPI_STRINGS[] = { "800", "1600", "3200", "6400", "8000", "12000", "26000", NULL };

static void sync_ui_from_state(void);
static void update_responsive_layout(void);

static void set_octant_weight_with_symmetry(int idx, float val) {
    if (val < 0.01f) val = 0.01f;
    if (val > 3.00f) val = 3.00f;

    atomic_store_explicit(&g_state.octant_weights[idx], val, memory_order_relaxed);

    if (g_ui.mirror_lr) {
        const int lr_map[8] = { 4, 3, -1, 1, 0, 7, -1, 5 };
        int partner = lr_map[idx];
        if (partner >= 0) {
            atomic_store_explicit(&g_state.octant_weights[partner], val, memory_order_relaxed);
        }
    }
    if (g_ui.mirror_ud) {
        if (idx == 2) atomic_store_explicit(&g_state.octant_weights[6], val, memory_order_relaxed);
        if (idx == 6) atomic_store_explicit(&g_state.octant_weights[2], val, memory_order_relaxed);
    }

    sync_ui_from_state();
}

/* Radar Coordinate Helpers */
static void get_radar_geometry(int width, int height, double *cx, double *cy, double *radius) {
    *cx = width * 0.5;
    *cy = height * 0.5;
    double r = ((*cx < *cy) ? *cx : *cy) - 48.0;
    *radius = (r > 50.0) ? r : 50.0;
}

static void octant_screen_pos(int idx, float weight, double cx, double cy, double radius,
                              double *out_x, double *out_y) {
    double rad = OCTANT_INFO[idx].angle_deg * (PI / 180.0);
    double norm_r = (weight / g_ui.max_scale) * radius;
    *out_x = cx + norm_r * cos(rad);
    *out_y = cy - norm_r * sin(rad);
}

/* A handle is grabbed only by clicking on the dot itself (its drawn radius plus a few pixels of slack),
 * never by clicking somewhere in the same direction: the chart is easy to brush past and an accidental
 * click must not change a weight. Overlapping dots: the nearest centre wins. */
#define HANDLE_HIT_PX 10.0

static int find_nearest_octant_handle(double mx, double my, int width, int height) {
    double cx, cy, radius;
    get_radar_geometry(width, height, &cx, &cy, &radius);
    int best = -1;
    double best_d = HANDLE_HIT_PX;
    for (int i = 0; i < 8; i++) {
        float w = atomic_load_explicit(&g_state.octant_weights[i], memory_order_relaxed);
        double hx, hy;
        octant_screen_pos(i, w, cx, cy, radius, &hx, &hy);
        double d = hypot(mx - hx, my - hy);
        if (d <= best_d) { best_d = d; best = i; }
    }
    return best;
}

static void on_radar_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    (void)area;
    (void)user_data;

    double cx, cy, radius;
    get_radar_geometry(width, height, &cx, &cy, &radius);

    float weights[8];
    float max_w = 0.0f;
    for (int i = 0; i < 8; i++) {
        weights[i] = atomic_load_explicit(&g_state.octant_weights[i], memory_order_relaxed);
        if (weights[i] > max_w) max_w = weights[i];
    }
    if (max_w > g_ui.max_scale * 0.90f) {
        g_ui.max_scale = max_w * 1.25f;
    } else if (max_w < 0.35f && g_ui.max_scale > 0.60f) {
        g_ui.max_scale = 0.60f;
    }

    /* 1. Radial Background Gradient */
    cairo_pattern_t *bg_pat = cairo_pattern_create_radial(cx, cy, 2.0, cx, cy, radius * 1.1);
    cairo_pattern_add_color_stop_rgb(bg_pat, 0.0, 24.0 / 255.0, 28.0 / 255.0, 40.0 / 255.0);
    cairo_pattern_add_color_stop_rgb(bg_pat, 1.0, 14.0 / 255.0, 16.0 / 255.0, 22.0 / 255.0);
    cairo_set_source(cr, bg_pat);
    cairo_arc(cr, cx, cy, radius * 1.05, 0, 2.0 * PI);
    cairo_fill(cr);
    cairo_pattern_destroy(bg_pat);

    /* 2. Concentric Dashed Guideline Rings */
    const double dashes[] = { 4.0, 4.0 };
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.0);

    for (int ring = 1; ring <= 4; ring++) {
        double r = (radius / 4.0) * ring;
        float val = (g_ui.max_scale / 4.0f) * (float)ring;

        cairo_set_dash(cr, dashes, 2, 0.0);
        cairo_set_line_width(cr, 1.0);
        cairo_set_source_rgb(cr, 46.0 / 255.0, 56.0 / 255.0, 77.0 / 255.0);
        cairo_arc(cr, cx, cy, r, 0, 2.0 * PI);
        cairo_stroke(cr);

        cairo_set_dash(cr, NULL, 0, 0.0);
        cairo_set_source_rgb(cr, 100.0 / 255.0, 116.0 / 255.0, 139.0 / 255.0);
        char ring_lbl[32];
        snprintf(ring_lbl, sizeof(ring_lbl), "%.2fx", val);
        cairo_move_to(cr, cx + 4.0, cy - r + 12.0);
        cairo_show_text(cr, ring_lbl);
    }

    /* 3. Spoke Axes & Labels */
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12.0);

    for (int i = 0; i < 8; i++) {
        double rad = OCTANT_INFO[i].angle_deg * (PI / 180.0);
        double ex = cx + radius * cos(rad);
        double ey = cy - radius * sin(rad);

        cairo_set_line_width(cr, 1.5);
        cairo_set_source_rgb(cr, 42.0 / 255.0, 50.0 / 255.0, 70.0 / 255.0);
        cairo_move_to(cr, cx, cy);
        cairo_line_to(cr, ex, ey);
        cairo_stroke(cr);

        double lx = cx + (radius + 24.0) * cos(rad);
        double ly = cy - (radius + 24.0) * sin(rad);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, OCTANT_INFO[i].short_name, &ext);

        if (i == g_ui.hover_idx || i == g_ui.dragging_idx) {
            cairo_set_source_rgb(cr, 0.0, 240.0 / 255.0, 1.0);
        } else {
            cairo_set_source_rgb(cr, 148.0 / 255.0, 163.0 / 255.0, 184.0 / 255.0);
        }
        cairo_move_to(cr, lx - ext.width * 0.5, ly + ext.height * 0.5);
        cairo_show_text(cr, OCTANT_INFO[i].short_name);
    }

    /* 4. Draw 8-Octant Sensitivity Polygon */
    double pts_x[8], pts_y[8];
    for (int i = 0; i < 8; i++) {
        octant_screen_pos(i, weights[i], cx, cy, radius, &pts_x[i], &pts_y[i]);
    }

    cairo_new_path(cr);
    for (int i = 0; i < 8; i++) {
        if (i == 0) cairo_move_to(cr, pts_x[i], pts_y[i]);
        else        cairo_line_to(cr, pts_x[i], pts_y[i]);
    }
    cairo_close_path(cr);

    cairo_pattern_t *poly_pat = cairo_pattern_create_radial(cx, cy, 2.0, cx, cy, radius * 0.9);
    cairo_pattern_add_color_stop_rgba(poly_pat, 0.0, 0.0, 240.0 / 255.0, 1.0, 0.42);
    cairo_pattern_add_color_stop_rgba(poly_pat, 0.7, 14.0 / 255.0, 165.0 / 255.0, 233.0 / 255.0, 0.28);
    cairo_pattern_add_color_stop_rgba(poly_pat, 1.0, 99.0 / 255.0, 102.0 / 255.0, 241.0 / 255.0, 0.12);
    cairo_set_source(cr, poly_pat);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(poly_pat);

    cairo_set_line_width(cr, 2.5);
    cairo_set_source_rgb(cr, 0.0, 240.0 / 255.0, 1.0);
    cairo_stroke(cr);

    /* 5. Draw Last Ballistic Flick Telemetry Vector Overlay (P_1 vs T) */
    pthread_mutex_lock(&g_state.telemetry_mutex);
    float p1_x = g_state.calib.last_p1_x;
    float p1_y = g_state.calib.last_p1_y;
    float t_x  = g_state.calib.last_t_x;
    float t_y  = g_state.calib.last_t_y;
    int   total_flicks = g_state.calib.total_valid_flicks;
    pthread_mutex_unlock(&g_state.telemetry_mutex);

    if (total_flicks > 0 && hypotf(t_x, t_y) > 1.0f) {
        float t_mag = hypotf(t_x, t_y);
        double scale_px = (radius * 0.72) / t_mag;
        double sx_t  = cx + t_x * scale_px;
        double sy_t  = cy - t_y * scale_px;
        double sx_p1 = cx + p1_x * scale_px;
        double sy_p1 = cy - p1_y * scale_px;

        /* Target vector line (green dashed) */
        cairo_set_dash(cr, dashes, 2, 0.0);
        cairo_set_line_width(cr, 1.5);
        cairo_set_source_rgba(cr, 0.29, 0.87, 0.50, 0.75);
        cairo_move_to(cr, cx, cy);
        cairo_line_to(cr, sx_t, sy_t);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0.0);

        /* Target T marker (green circle) */
        cairo_arc(cr, sx_t, sy_t, 5.0, 0, 2.0 * PI);
        cairo_set_source_rgba(cr, 0.29, 0.87, 0.50, 0.90);
        cairo_fill(cr);

        /* Primary submovement P_1 landing marker (amber/pink dot) */
        cairo_arc(cr, sx_p1, sy_p1, 4.5, 0, 2.0 * PI);
        cairo_set_source_rgba(cr, 0.98, 0.75, 0.14, 0.95);
        cairo_fill(cr);
    }

    /* 6. Draw Interactive Control Handles */
    for (int i = 0; i < 8; i++) {
        bool active = (i == g_ui.dragging_idx || i == g_ui.hover_idx);
        double hr = active ? 6.5 : 4.8;

        cairo_arc(cr, pts_x[i], pts_y[i], hr, 0, 2.0 * PI);
        if (active) {
            cairo_set_source_rgb(cr, 1.0, 0.0, 0.5);
        } else {
            cairo_set_source_rgb(cr, 0.0, 240.0 / 255.0, 1.0);
        }
        cairo_fill_preserve(cr);

        cairo_set_line_width(cr, active ? 2.0 : 1.5);
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_stroke(cr);
    }
}

static void update_radar_from_pointer(double mx, double my) {
    if (g_ui.dragging_idx < 0 || g_ui.dragging_idx >= 8) return;
    int w = gtk_widget_get_width(g_ui.drawing_area);
    int h = gtk_widget_get_height(g_ui.drawing_area);
    double cx, cy, radius;
    get_radar_geometry(w, h, &cx, &cy, &radius);

    mx += g_ui.grab_dx;
    my += g_ui.grab_dy;
    double dist = hypot(mx - cx, my - cy);
    float new_w = (float)((dist / radius) * g_ui.max_scale);
    if (new_w < 0.02f) new_w = 0.02f;
    if (new_w > 2.50f) new_w = 2.50f;
    new_w = roundf(new_w * 1000.0f) / 1000.0f;

    set_octant_weight_with_symmetry(g_ui.dragging_idx, new_w);
}

static void on_radar_drag_begin(GtkGestureDrag *gesture, double start_x, double start_y, gpointer user_data) {
    (void)gesture;
    (void)user_data;
    int w = gtk_widget_get_width(g_ui.drawing_area);
    int h = gtk_widget_get_height(g_ui.drawing_area);
    g_ui.drag_start_x = start_x;
    g_ui.drag_start_y = start_y;
    g_ui.dragging_idx = find_nearest_octant_handle(start_x, start_y, w, h);
    g_ui.grab_dx = g_ui.grab_dy = 0.0;
    if (g_ui.dragging_idx >= 0) {
        /* Remember where on the dot it was grabbed; nothing changes until the pointer actually moves. */
        double cx, cy, radius, hx, hy;
        get_radar_geometry(w, h, &cx, &cy, &radius);
        float cur = atomic_load_explicit(&g_state.octant_weights[g_ui.dragging_idx], memory_order_relaxed);
        octant_screen_pos(g_ui.dragging_idx, cur, cx, cy, radius, &hx, &hy);
        g_ui.grab_dx = hx - start_x;
        g_ui.grab_dy = hy - start_y;
        gtk_widget_queue_draw(g_ui.drawing_area);
    }
}

static void on_radar_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data) {
    (void)gesture;
    (void)user_data;
    if (g_ui.dragging_idx >= 0) {
        update_radar_from_pointer(g_ui.drag_start_x + offset_x, g_ui.drag_start_y + offset_y);
        gtk_widget_queue_draw(g_ui.drawing_area);
    }
}

static void on_radar_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data) {
    (void)gesture;
    (void)offset_x;
    (void)offset_y;
    (void)user_data;
    g_ui.dragging_idx = -1;
    gtk_widget_queue_draw(g_ui.drawing_area);
}

static void on_radar_motion(GtkEventControllerMotion *ctrl, double x, double y, gpointer user_data) {
    (void)ctrl;
    (void)user_data;
    if (g_ui.dragging_idx >= 0) return;
    int w = gtk_widget_get_width(g_ui.drawing_area);
    int h = gtk_widget_get_height(g_ui.drawing_area);
    int hover = find_nearest_octant_handle(x, y, w, h);
    if (hover != g_ui.hover_idx) {
        g_ui.hover_idx = hover;
        gtk_widget_queue_draw(g_ui.drawing_area);
    }
}

static void sync_ui_from_state(void) {
    if (g_ui.updating_ui || !g_ui.window) return;
    g_ui.updating_ui = true;

    int cpi = atomic_load_explicit(&g_state.hardware_cpi, memory_order_relaxed);
    double sum = 0.0;
    for (int i = 0; i < 8; i++) {
        float w = atomic_load_explicit(&g_state.octant_weights[i], memory_order_relaxed);
        gtk_range_set_value(GTK_RANGE(g_ui.oct_scales[i]), w * 1000.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_ui.oct_spins[i]), w);

        int eff_dpi = (int)lroundf(w * (float)cpi);
        char dpi_buf[32];
        snprintf(dpi_buf, sizeof(dpi_buf), "%d DPI", eff_dpi);
        gtk_label_set_text(GTK_LABEL(g_ui.oct_dpi_lbls[i]), dpi_buf);
        sum += w;
    }

    /* The Master Vector Scaling widgets show the mean weight (equal to the uniform scalar
     * whenever the octants are uniform), so loaded or calibrated profiles are reflected. */
    double mean = sum / 8.0;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_ui.master_spin), mean);
    gtk_range_set_value(GTK_RANGE(g_ui.master_scale), mean * 100.0);

    gtk_widget_queue_draw(g_ui.drawing_area);
    g_ui.updating_ui = false;
}

static void on_octant_scale_changed(GtkRange *range, gpointer user_data) {
    if (g_ui.updating_ui) return;
    int idx = GPOINTER_TO_INT(user_data);
    float w = (float)(gtk_range_get_value(range) / 1000.0);
    set_octant_weight_with_symmetry(idx, w);
}

static void on_octant_spin_changed(GtkSpinButton *spin, gpointer user_data) {
    if (g_ui.updating_ui) return;
    int idx = GPOINTER_TO_INT(user_data);
    float w = (float)gtk_spin_button_get_value(spin);
    set_octant_weight_with_symmetry(idx, w);
}

static void on_master_scale_changed(GtkRange *range, gpointer user_data) {
    (void)user_data;
    if (g_ui.updating_ui) return;
    double scalar = gtk_range_get_value(range) / 100.0;
    g_ui.updating_ui = true;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_ui.master_spin), scalar);
    g_ui.updating_ui = false;

    for (int i = 0; i < 8; i++) {
        atomic_store_explicit(&g_state.octant_weights[i], (float)scalar, memory_order_relaxed);
    }
    sync_ui_from_state();
}

static void on_master_spin_changed(GtkSpinButton *spin, gpointer user_data) {
    (void)user_data;
    if (g_ui.updating_ui) return;
    double scalar = gtk_spin_button_get_value(spin);
    g_ui.updating_ui = true;
    gtk_range_set_value(GTK_RANGE(g_ui.master_scale), scalar * 100.0);
    g_ui.updating_ui = false;

    for (int i = 0; i < 8; i++) {
        atomic_store_explicit(&g_state.octant_weights[i], (float)scalar, memory_order_relaxed);
    }
    sync_ui_from_state();
}

static void on_mirror_lr_toggled(GtkCheckButton *btn, gpointer user_data) {
    (void)user_data;
    g_ui.mirror_lr = gtk_check_button_get_active(btn);
    if (g_ui.mirror_lr) {
        atomic_store_explicit(&g_state.octant_weights[4], atomic_load(&g_state.octant_weights[0]), memory_order_relaxed);
        atomic_store_explicit(&g_state.octant_weights[3], atomic_load(&g_state.octant_weights[1]), memory_order_relaxed);
        atomic_store_explicit(&g_state.octant_weights[5], atomic_load(&g_state.octant_weights[7]), memory_order_relaxed);
        sync_ui_from_state();
    }
}

static void on_mirror_ud_toggled(GtkCheckButton *btn, gpointer user_data) {
    (void)user_data;
    g_ui.mirror_ud = gtk_check_button_get_active(btn);
    if (g_ui.mirror_ud) {
        atomic_store_explicit(&g_state.octant_weights[6], atomic_load(&g_state.octant_weights[2]), memory_order_relaxed);
        sync_ui_from_state();
    }
}

static void on_preset_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    const char *preset = (const char *)user_data;
    core_apply_preset(preset);
    sync_ui_from_state();
}

static void rescan_devices(bool force);

static int g_hw_busy = 0; /* Outstanding hardware jobs (GTK thread only) */

static void update_rate_dropdown(const mouse_hw_status_t *st) {
    int rates[8];
    int count = 0;
    if (st->node[0] && st->accessible) {
        for (int i = 0; st->supported_rates[i] && i < 8; i++) rates[count++] = st->supported_rates[i];
    }

    g_ui.updating_ui = true;
    /* Rebuild the item list only when the supported rates change */
    bool same = (count == g_ui.rate_count) && (count == 0 || memcmp(rates, g_ui.rate_values, sizeof(int) * (size_t)count) == 0);
    if (!same || g_list_model_get_n_items(G_LIST_MODEL(g_ui.rate_model)) == 0) {
        guint n_items = g_list_model_get_n_items(G_LIST_MODEL(g_ui.rate_model));
        gtk_string_list_splice(g_ui.rate_model, 0, n_items, NULL);
        for (int i = 0; i < count; i++) {
            char b[32];
            snprintf(b, sizeof(b), "%d Hz", rates[i]);
            gtk_string_list_append(g_ui.rate_model, b);
            g_ui.rate_values[i] = rates[i];
        }
        g_ui.rate_count = count;
        if (count == 0) gtk_string_list_append(g_ui.rate_model, "Not supported");
    }
    /* An unreadable rate (0) keeps the current selection rather than jumping to the first item */
    guint sel = (same && st->polling_rate_hz == 0) ? gtk_drop_down_get_selected(GTK_DROP_DOWN(g_ui.rate_dropdown)) : 0;
    for (int i = 0; i < count; i++) {
        if (rates[i] == st->polling_rate_hz) sel = (guint)i;
    }
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_ui.rate_dropdown), sel);
    gtk_widget_set_sensitive(g_ui.rate_dropdown, count > 0 && g_hw_busy == 0);
    if (st->polling_rate_hz > 0 || count == 0) g_ui.current_rate_hz = st->polling_rate_hz;
    g_ui.updating_ui = false;
}

/* A mouse behind the dongle. Only what the dongle reports is shown as a value; anything the mouse
 * did not report says so, with the reason, instead of a number that merely looks plausible. */
static void apply_dongle_mouse(const mouse_hw_status_t *st) {
    char b[256];
    snprintf(b, sizeof(b), "🖱️ %s%s%s · %s", st->brand, st->brand[0] ? " " : "", st->model, st->node);
    gtk_label_set_text(GTK_LABEL(g_ui.lbl_mouse_ident), b);

    /* Polling: what the mouse is set to, versus what the PC can receive from the dongle */
    char tip[512];
    if (st->polling_rate_hz > 0) {
        if (st->pc_link_hz > 0 && st->polling_rate_hz > st->pc_link_hz)
            snprintf(b, sizeof(b), "⚡ %d Hz on the mouse → %d Hz to the PC", st->polling_rate_hz, st->pc_link_hz);
        else
            snprintf(b, sizeof(b), "⚡ %d Hz", st->polling_rate_hz);
    } else if (st->measured_hz > 0) {
        snprintf(b, sizeof(b), "⚡ ~%d Hz measured · mouse rate not reported", st->measured_hz);
    } else {
        snprintf(b, sizeof(b), "⚡ Rate not reported (move the mouse to measure)");
    }
    gtk_label_set_text(GTK_LABEL(g_ui.badge_rate), b);
    snprintf(tip, sizeof(tip),
             "Set on the mouse: %s\nUSB interval of the mouse's port: %d Hz (a ceiling set by the bus, not a measurement)\n"
             "Measured at the dongle (highest reports/s in the last 10 s, rises only while moving): %d Hz\n"
             "Dongle → PC link ceiling: %s",
             st->polling_rate_hz > 0 ? "reported" : "not reported by the mouse",
             st->nominal_hz, st->measured_hz,
             st->pc_link_hz > 0 ? (st->pc_link_hz >= 8000 ? "8000 Hz" : st->pc_link_hz >= 1000 ? "1000 Hz (Full-Speed USB)" : "125 Hz") : "unknown");
    gtk_widget_set_tooltip_text(g_ui.badge_rate, tip);

    /* Battery */
    if (st->battery_pct > 0) snprintf(b, sizeof(b), "🔋 %d%%%s", st->battery_pct, st->charging ? " ⚡ charging" : "");
    else snprintf(b, sizeof(b), "🔋 Battery: not reported");
    gtk_label_set_text(GTK_LABEL(g_ui.badge_bat), b);
    gtk_widget_set_tooltip_text(g_ui.badge_bat, st->battery_pct > 0 ? NULL
        : "The mouse did not report a battery level (a wired mouse has none, or its vendor protocol did not answer).");

    /* Motion Sync */
    if (st->motion_sync_known) snprintf(b, sizeof(b), "🎯 Motion Sync: %s", st->motion_sync ? "ON" : "OFF");
    else snprintf(b, sizeof(b), "🎯 Motion Sync: not reported");
    gtk_label_set_text(GTK_LABEL(g_ui.badge_sync), b);
    gtk_widget_set_tooltip_text(g_ui.badge_sync, st->motion_sync_known ? NULL
        : "The mouse did not report Motion Sync. Not reported is not the same as off.");

    /* Link. The dongle protocol has no signal strength (dBm); it reports whether the mouse answers. */
    static const char *LINK[] = { "wired", "connected", "asleep: move it" };
    if (st->link >= 0 && st->link < 3) snprintf(b, sizeof(b), "📶 Link: %s", LINK[st->link]);
    else snprintf(b, sizeof(b), "📶 Link: not reported");
    gtk_label_set_text(GTK_LABEL(g_ui.badge_rssi), b);
    gtk_widget_set_tooltip_text(g_ui.badge_rssi,
        "The dongle reports whether the mouse answers, not a signal strength in dBm.");

    if (st->driver_error) {
        snprintf(b, sizeof(b),
                 "⚠️ The dongle has a driver for %s %s, but the mouse did not answer it. Battery, Motion Sync and the real "
                 "polling rate are unknown. Move the mouse (wireless mice sleep when idle) and press Refresh; the dongle also retries "
                 "on its own.", st->brand[0] ? st->brand : "this", st->model);
        gtk_label_set_text(GTK_LABEL(g_ui.lbl_dg_warn), b);
        gtk_widget_set_visible(g_ui.lbl_dg_warn, TRUE);
    } else {
        gtk_widget_set_visible(g_ui.lbl_dg_warn, FALSE);
    }
}

/* Apply a hardware snapshot produced by the HID worker (GTK thread, outside any widget signal) */
static void apply_hw_status(const mouse_hw_status_t *in, bool ok) {
    mouse_hw_status_t st = *in;
    bool hw_ok = st.node[0] != '\0' && st.accessible;
    bool cpi_ok = st.via_dongle ? st.can_set_cpi : st.vid == 0x361D;
    gtk_widget_set_sensitive(g_ui.btn_cpi_3200, hw_ok && cpi_ok && g_hw_busy == 0);
    gtk_widget_set_sensitive(g_ui.btn_cpi_6400, hw_ok && cpi_ok && g_hw_busy == 0);
    update_rate_dropdown(&st);

    if (!ok && st.node[0] == '\0') {
        bool generic = atomic_load(&g_state.evdev_grabbed) &&
                       !devices_is_hw_supported((uint16_t)atomic_load(&g_state.active_vid),
                                                (uint16_t)atomic_load(&g_state.active_pid));
        gtk_label_set_text(GTK_LABEL(g_ui.lbl_mouse_ident), generic
            ? "🖱️ Generic mouse: vectoring active, no vendor protocol for CPI / polling control (set CPI below manually)"
            : "🖱️ No supported vendor HID interface detected");
        const char *dash[] = { "⚡ Polling: -- Hz", "🔋 Battery: --%", "🎯 Motion Sync: --", "📶 Signal: -- dBm" };
        GtkWidget *badges[] = { g_ui.badge_rate, g_ui.badge_bat, g_ui.badge_sync, g_ui.badge_rssi };
        for (int i = 0; i < 4; i++) gtk_label_set_text(GTK_LABEL(badges[i]), dash[i]);
        gtk_widget_set_visible(g_ui.lbl_dg_warn, FALSE);
        gtk_widget_set_tooltip_text(g_ui.badge_rate, NULL);
        gtk_widget_set_tooltip_text(g_ui.badge_bat, NULL);
        gtk_widget_set_tooltip_text(g_ui.badge_sync, NULL);
        gtk_widget_set_tooltip_text(g_ui.badge_rssi, NULL);
    } else {
        if (st.via_dongle) {
            apply_dongle_mouse(&st);
        } else {
        char ident[256];
        if (st.serial[0] != '\0') {
            snprintf(ident, sizeof(ident), "🖱️ %s %s (%s) · %s", st.brand, st.model, st.serial, st.node);
        } else {
            snprintf(ident, sizeof(ident), "🖱️ %s %s · %s", st.brand, st.model, st.node);
        }
        gtk_label_set_text(GTK_LABEL(g_ui.lbl_mouse_ident), ident);

        if (st.polling_rate_hz > 0) {
            char b[64];
            snprintf(b, sizeof(b), "⚡ %d Hz", st.polling_rate_hz);
            gtk_label_set_text(GTK_LABEL(g_ui.badge_rate), b);
        }
        if (st.battery_mv > 0) {
            char b[64];
            snprintf(b, sizeof(b), "🔋 %d%% (%.2fV)", st.battery_pct, st.battery_mv / 1000.0f);
            gtk_label_set_text(GTK_LABEL(g_ui.badge_bat), b);
        }
        char ms_buf[64];
        snprintf(ms_buf, sizeof(ms_buf), "🎯 Motion Sync: %s", st.motion_sync ? "ON" : "OFF");
        gtk_label_set_text(GTK_LABEL(g_ui.badge_sync), ms_buf);

        if (st.rssi_dbm != 0) {
            char b[64];
            snprintf(b, sizeof(b), "📶 %d dBm", st.rssi_dbm);
            gtk_label_set_text(GTK_LABEL(g_ui.badge_rssi), b);
        }
        gtk_widget_set_visible(g_ui.lbl_dg_warn, FALSE);
        }

        if (st.dpi > 0) {
            for (size_t i = 0; i < sizeof(CPI_OPTIONS) / sizeof(CPI_OPTIONS[0]); i++) {
                if (CPI_OPTIONS[i] == st.dpi) {
                    g_ui.updating_ui = true;
                    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_ui.cpi_dropdown), (guint)i);
                    g_ui.updating_ui = false;
                    break;
                }
            }
        }
    }
    sync_ui_from_state();
}

/* ---------- Hardware I/O worker ----------
 * Vendor HID exchanges can take seconds (the Attack Shark 2.4G relay polls the
 * receiver with 100 ms waits), so they never run on the GTK thread: running them
 * inside a dropdown's selection handler froze its popover. One worker executes
 * jobs in order and hands results back through an idle callback. */
typedef enum { HW_JOB_QUERY, HW_JOB_SET_RATE, HW_JOB_SET_CPI } hw_job_kind_t;

typedef struct {
    hw_job_kind_t     kind;
    int               value;
    bool              ok;
    mouse_hw_status_t st;
    bool              st_ok;
} hw_job_t;

static GAsyncQueue *g_hw_queue = NULL;
static _Atomic int  g_hw_queries_pending = 0;

static gboolean on_hw_job_done(gpointer data) {
    hw_job_t *job = data;
    g_hw_busy--;
    char msg[96] = "";
    if (job->kind == HW_JOB_SET_RATE) {
        snprintf(msg, sizeof(msg), job->ok ? "⚡ Polling rate set to %d Hz" : "❌ Mouse rejected polling rate %d Hz",
                 job->value);
    } else if (job->kind == HW_JOB_SET_CPI) {
        if (job->ok) snprintf(msg, sizeof(msg), "🎯 Sensor CPI set to %d", job->value);
        else if (job->st.vid == 0x361D) snprintf(msg, sizeof(msg), "❌ Mouse rejected CPI %d", job->value);
    }
    if (msg[0]) gtk_label_set_text(GTK_LABEL(g_ui.lbl_profile_status), msg);
    apply_hw_status(&job->st, job->st_ok); /* Shows what the mouse actually reports back */
    g_free(job);
    return G_SOURCE_REMOVE;
}

static gpointer hw_worker(gpointer data) {
    (void)data;
    for (;;) {
        hw_job_t *job = g_async_queue_pop(g_hw_queue);
        switch (job->kind) {
            case HW_JOB_QUERY:
                atomic_fetch_sub(&g_hw_queries_pending, 1);
                break;
            case HW_JOB_SET_RATE:
                job->ok = hid_set_mouse_polling_rate(job->value);
                break;
            case HW_JOB_SET_CPI:
                job->ok = hid_set_mouse_cpi(job->value);
                break;
        }
        job->st_ok = hid_query_active_mouse(&job->st);
        g_idle_add(on_hw_job_done, job);
    }
    return NULL;
}

static void hw_submit(hw_job_kind_t kind, int value) {
    /* Coalesce refreshes: one queued query already covers any later request */
    if (kind == HW_JOB_QUERY && atomic_fetch_add(&g_hw_queries_pending, 1) > 0) {
        atomic_fetch_sub(&g_hw_queries_pending, 1);
        return;
    }
    hw_job_t *job = g_new0(hw_job_t, 1);
    job->kind = kind;
    job->value = value;
    g_hw_busy++;
    gtk_widget_set_sensitive(g_ui.rate_dropdown, FALSE);
    gtk_widget_set_sensitive(g_ui.btn_cpi_3200, FALSE);
    gtk_widget_set_sensitive(g_ui.btn_cpi_6400, FALSE);
    g_async_queue_push(g_hw_queue, job);
}

static void refresh_hardware_widgets(void) {
    hw_submit(HW_JOB_QUERY, 0);
}

static void on_cpi_dropdown_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    (void)user_data;
    if (g_ui.updating_ui) return;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    if (sel < sizeof(CPI_OPTIONS) / sizeof(CPI_OPTIONS[0])) {
        int cpi = CPI_OPTIONS[sel];
        atomic_store_explicit(&g_state.hardware_cpi, cpi, memory_order_relaxed);
        sync_ui_from_state();
        hw_submit(HW_JOB_SET_CPI, cpi);
    }
}

static void on_rate_dropdown_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    (void)user_data;
    if (g_ui.updating_ui) return;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    if (sel >= (guint)g_ui.rate_count) return;
    int hz = g_ui.rate_values[sel];
    /* Only an explicit change of value reaches the mouse: never re-write the current rate,
     * and never stack writes while one is in flight (each one rewrites the settings block) */
    if (g_hw_busy > 0 || hz == g_ui.current_rate_hz) return;
    char msg[96];
    snprintf(msg, sizeof(msg), "⏳ Setting polling rate to %d Hz...", hz);
    gtk_label_set_text(GTK_LABEL(g_ui.lbl_profile_status), msg);
    hw_submit(HW_JOB_SET_RATE, hz);
}

/* ---------- Mouse device selection ---------- */

static bool same_device_path(const char *a, const char *b) {
    char ra[PATH_MAX], rb[PATH_MAX];
    if (!a[0] || !b[0]) return false;
    if (realpath(a, ra) && realpath(b, rb)) return strcmp(ra, rb) == 0;
    return strcmp(a, b) == 0;
}

static void select_requested_in_dropdown(void) {
    char req[256];
    devices_get_request(req, sizeof(req));
    guint sel = 0;
    if (req[0]) {
        sel = GTK_INVALID_LIST_POSITION;
        for (int i = 0; i < g_ui.device_count; i++) {
            if (same_device_path(req, g_ui.devices[i].event_path)) sel = (guint)(i + 1);
        }
        if (sel == GTK_INVALID_LIST_POSITION) {
            /* Manual path that isn't present right now: show it as its own entry */
            char item[300];
            snprintf(item, sizeof(item), "Manual: %s (not connected)", req);
            gtk_string_list_append(g_ui.device_model, item);
            sel = (guint)(g_ui.device_count + 1);
        }
    }
    g_ui.updating_ui = true;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_ui.device_dropdown), sel);
    g_ui.updating_ui = false;
}

/* Rebuild the mouse list; when !force, only if the set of connected mice changed */
static void rescan_devices(bool force) {
    mouse_device_t list[DEVICES_MAX];
    int n = devices_enumerate(list, DEVICES_MAX);
    if (!force && n == g_ui.device_count) {
        bool same = true;
        for (int i = 0; i < n && same; i++) {
            same = strcmp(list[i].event_path, g_ui.devices[i].event_path) == 0;
        }
        if (same) return;
    }
    memcpy(g_ui.devices, list, sizeof(list[0]) * (size_t)n);
    g_ui.device_count = n;

    g_ui.updating_ui = true;
    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(g_ui.device_model));
    gtk_string_list_splice(g_ui.device_model, 0, n_items, NULL);
    char auto_lbl[200];
    snprintf(auto_lbl, sizeof(auto_lbl), "Auto (best match%s%s)", n ? ": " : "", n ? list[0].name : ", none found");
    gtk_string_list_append(g_ui.device_model, auto_lbl);
    for (int i = 0; i < n; i++) {
        char item[300];
        snprintf(item, sizeof(item), "%s  [%04x:%04x]%s  %s", list[i].name, list[i].vid, list[i].pid,
                 list[i].is_dongle ? " 🔗 dongle output" : list[i].hw_supported ? " ★" : "", list[i].event_path);
        gtk_string_list_append(g_ui.device_model, item);
    }
    g_ui.updating_ui = false;
    select_requested_in_dropdown();
}

static void switch_to_device(const char *path) {
    devices_request_switch(path);
    config_save();
    char msg[300];
    snprintf(msg, sizeof(msg), "🖱️ Switching to %s", (path && path[0]) ? path : "automatic selection");
    gtk_label_set_text(GTK_LABEL(g_ui.lbl_profile_status), msg);
}

static void on_device_dropdown_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    (void)user_data;
    if (g_ui.updating_ui) return;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    if (sel == 0) {
        switch_to_device("");
    } else if (sel <= (guint)g_ui.device_count) {
        switch_to_device(devices_preferred_path(&g_ui.devices[sel - 1]));
    }
}

static void on_device_rescan_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    rescan_devices(true);
}

static void on_device_entry_apply(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    (void)user_data;
    const char *path = gtk_editable_get_text(GTK_EDITABLE(g_ui.device_entry));
    if (!path || !path[0]) return;
    if (access(path, F_OK) != 0) {
        gtk_label_set_text(GTK_LABEL(g_ui.lbl_profile_status), "❌ That device path does not exist");
        return;
    }
    switch_to_device(path);
    rescan_devices(true);
}

typedef struct {
    mouse_device_t dev;
    bool found;
} identify_result_t;

static gboolean on_identify_done(gpointer data) {
    identify_result_t *res = data;
    gtk_widget_set_sensitive(g_ui.btn_identify, TRUE);
    gtk_button_set_label(GTK_BUTTON(g_ui.btn_identify), "🎯 Identify");
    if (res->found) {
        switch_to_device(devices_preferred_path(&res->dev));
        rescan_devices(true);
        char msg[200];
        snprintf(msg, sizeof(msg), "🎯 Identified: %s", res->dev.name);
        gtk_label_set_text(GTK_LABEL(g_ui.lbl_profile_status), msg);
    } else {
        gtk_label_set_text(GTK_LABEL(g_ui.lbl_profile_status), "⚠️ No mouse movement detected; try again");
    }
    g_free(res);
    return G_SOURCE_REMOVE;
}

static gpointer identify_worker(gpointer data) {
    (void)data;
    identify_result_t *res = g_new0(identify_result_t, 1);
    res->found = devices_identify_by_motion(&res->dev, 4000) == 0;
    g_idle_add(on_identify_done, res);
    return NULL;
}

static void on_identify_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);
    gtk_button_set_label(btn, "Move the mouse you want (4s)...");
    g_thread_unref(g_thread_new("identify", identify_worker, NULL));
}

/* ---------- FpsAimForge install / repair ---------- */

static void refresh_faf_install_widgets(void) {
    faf_install_info_t info;
    faf_install_detect(&info);
    static const char *icons[] = { "⚫", "🟡", "🟠", "🟠", "🟢" };
    static const char *actions[] = { "Install", "Install", "Repair", "Update", "Reinstall" };
    char buf[600];
    snprintf(buf, sizeof(buf), "%s FpsAimForge: %s%s%s", icons[info.state], info.detail,
             info.game_dir[0] ? " · " : "", info.game_dir);
    gtk_label_set_text(GTK_LABEL(g_ui.lbl_faf_install), buf);
    gtk_widget_set_tooltip_text(g_ui.lbl_faf_install, buf);
    gtk_button_set_label(GTK_BUTTON(g_ui.btn_faf_install), actions[info.state]);
    gtk_widget_set_sensitive(g_ui.btn_faf_install, info.state != FAF_INSTALL_GAME_NOT_FOUND);
    gtk_widget_set_sensitive(g_ui.btn_faf_remove,
                             info.state == FAF_INSTALL_OK || info.state == FAF_INSTALL_OUTDATED);
}

static void on_faf_install_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    bool remove = GPOINTER_TO_INT(user_data) != 0;
    char msg[512], status[600];
    bool ok = remove ? faf_install_remove(msg, sizeof(msg)) : faf_install_apply(msg, sizeof(msg));
    snprintf(status, sizeof(status), "%s %s", ok ? "✅" : "❌", msg);
    gtk_label_set_text(GTK_LABEL(g_ui.lbl_profile_status), status);
    gtk_widget_set_tooltip_text(g_ui.lbl_profile_status, status);

    refresh_faf_install_widgets();
    gtk_label_set_text(GTK_LABEL(g_ui.lbl_faf_install), status);
    gtk_widget_set_tooltip_text(g_ui.lbl_faf_install, status);
    if (!remove && ok) {
        gtk_button_set_label(GTK_BUTTON(g_ui.btn_faf_install), "✓ Reinstalled");
    }
    g_ui.faf_feedback_ticks = 30; /* Keep confirmation visible for 3s before restoring normal status */
}

static void on_set_device_cpi_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    hw_submit(HW_JOB_SET_CPI, GPOINTER_TO_INT(user_data));
}

static void on_refresh_hw_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    refresh_hardware_widgets();
}

static void on_auto_calib_toggled(GtkCheckButton *btn, gpointer user_data) {
    (void)user_data;
    bool active = gtk_check_button_get_active(btn);
    atomic_store_explicit(&g_state.auto_calibrate, active, memory_order_relaxed);
}

static void on_apply_calib_now_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    int n = calibration_apply_damped_update(false);
    char msg[128];
    snprintf(msg, sizeof(msg), "⚡ Applied damped update to %d octant(s)", n);
    gtk_label_set_text(GTK_LABEL(g_ui.lbl_profile_status), msg);
    sync_ui_from_state();
}

static void on_reset_calib_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    calibration_reset_stats();
    gtk_label_set_text(GTK_LABEL(g_ui.lbl_profile_status), "↺ Reset ballistic calibration stats");
    sync_ui_from_state();
}

static void on_save_profile_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    char path[PATH_MAX + 32], msg[PATH_MAX + 64];
    if (core_autosave_profile(path, sizeof(path))) {
        snprintf(msg, sizeof(msg), "💾 Saved to %s", path);
    } else {
        snprintf(msg, sizeof(msg), "❌ Failed to save profile");
    }
    gtk_label_set_text(GTK_LABEL(g_ui.lbl_profile_status), msg);
}

static void on_load_profile_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    char path[PATH_MAX + 32], msg[PATH_MAX + 64];
    if (core_autoload_profile(path, sizeof(path))) {
        snprintf(msg, sizeof(msg), "📂 Loaded %s", path);
        sync_ui_from_state();
    } else {
        snprintf(msg, sizeof(msg), "⚠️ No saved profile found");
    }
    gtk_label_set_text(GTK_LABEL(g_ui.lbl_profile_status), msg);
}

/* ---------- AnyMouse dongle: stored profiles and attached mice ----------
 * Requests wait for the dongle thread (up to a few seconds), so they run on a short-lived worker
 * and report back through an idle callback; the GTK thread never blocks on the dongle. */

static const char *IGNORE_REASON[] = { "", "not a mouse", "absolute pointer", "report descriptor unusable",
                                       "too many mice", "could not be claimed" };

/* The dongle itself: its model, and its polling rates. The dongle has two: the rate it presents to the PC
 * (a property of its USB link, what an application on the PC can receive at most) and the rate it reads its
 * mice at (their own, shown per mouse below). The mouse's set rate is in the Hardware panel. */
static void refresh_dongle_info(const dongle_mouse_t *m, int n) {
    dongle_status_t dg;
    if (!dongle_get_status(&dg)) {
        gtk_widget_set_visible(g_ui.badge_dg, FALSE);
        return;
    }
    char b[320];
    snprintf(b, sizeof(b), "%s · firmware %u.%u.%u", dg.model, dg.fw[0], dg.fw[1], dg.fw[2]);
    gtk_label_set_text(GTK_LABEL(g_ui.lbl_dg_model), b);

    int in_hz = 0, meas = 0;
    for (int i = 0; i < n; i++) {
        if (m[i].info.tier == AMV_TIER_IGNORED) continue;
        if (m[i].info.nominal_hz > in_hz) in_hz = m[i].info.nominal_hz;
        if (m[i].info.measured_hz > meas) meas = m[i].info.measured_hz;
    }
    char in_txt[96];
    if (n < 0) snprintf(in_txt, sizeof(in_txt), "not reported by this firmware");
    else if (in_hz > 0) snprintf(in_txt, sizeof(in_txt), "%s", dongle_link_text(in_hz));
    else snprintf(in_txt, sizeof(in_txt), "no mouse attached");
    char meas_txt[64] = "";
    if (meas > 0) snprintf(meas_txt, sizeof(meas_txt), " · measured %d Hz", meas);
    snprintf(b, sizeof(b), "Polling to the PC: up to %s\nPolling from the mouse: up to %s%s",
             dongle_link_text(dg.pc_link_hz), in_txt, meas_txt);
    gtk_label_set_text(GTK_LABEL(g_ui.lbl_dg_info), b);
    gtk_widget_set_tooltip_text(g_ui.lbl_dg_info,
        dg.pro ? "The Pro dongle uses High-Speed USB on both sides: the PC can receive up to 8000 reports per second.\n"
                 "How many actually arrive depends on the mouse: it only reports while its sensor moves."
               : "This dongle's PC side is Full-Speed USB, which polls at most once per millisecond (1000 Hz), "
                 "whatever rate the mouse is set to. The mouse is still read at its own rate, so no motion is lost.");

    snprintf(b, sizeof(b), "🔗 %s → PC: %d Hz", dg.model, dg.pc_link_hz);
    gtk_label_set_text(GTK_LABEL(g_ui.badge_dg), dg.pc_link_hz > 0 ? b : dg.model);
    gtk_widget_set_tooltip_text(g_ui.badge_dg, dongle_link_text(dg.pc_link_hz));
    gtk_widget_set_visible(g_ui.badge_dg, TRUE);
}

static void refresh_dongle_mice(void) {
    dongle_mouse_t m[8];
    int n = dongle_get_mice(m, 8);
    refresh_dongle_info(m, n);
    if (n < 0) {
        gtk_label_set_text(GTK_LABEL(g_ui.lbl_dg_mice), "Mice: not reported by this firmware (needs 0.5.0)");
        return;
    }
    if (n == 0) {
        gtk_label_set_text(GTK_LABEL(g_ui.lbl_dg_mice), "Mice: nothing plugged into the dongle");
        return;
    }
    static const char *TIER[] = { "?", "known protocol", "generic HID", "boot fallback", "not used" };
    GString *g = g_string_new("Mice plugged into the dongle:");
    for (int i = 0; i < n; i++) {
        const amv_mouse_info_t *x = &m[i].info;
        const char *name = (m[i].has_model && m[i].model.model[0]) ? m[i].model.model : x->name;
        g_string_append_printf(g, "\n • %s [%04x:%04x] %s", name, x->vid, x->pid, x->tier <= 4 ? TIER[x->tier] : "?");
        if (x->tier == AMV_TIER_IGNORED) {
            g_string_append_printf(g, " (%s)", x->reason <= 5 ? IGNORE_REASON[x->reason] : "?");
            continue;
        }
        if (x->cpi) g_string_append_printf(g, " · %u CPI%s", x->cpi, x->cpi_source == AMV_CPI_USER ? " (your label)" : "");
        if (x->device_hz) g_string_append_printf(g, " · set %u Hz", x->device_hz);
        else g_string_append(g, " · set rate unknown");
        g_string_append_printf(g, " · read at %u Hz (USB interval) · measured %u Hz", x->nominal_hz, x->measured_hz);
        if (x->battery != 0xFF) g_string_append_printf(g, " · battery %u%%", x->battery);
        if (m[i].has_caps && m[i].caps.firmware[0]) g_string_append_printf(g, " · fw %s", m[i].caps.firmware);
        if (m[i].caps_unsupported || m[i].settings_unsupported || m[i].model_unsupported)
            g_string_append(g, " · extended mouse details not supported by this dongle");
        if (x->tier != AMV_TIER_KNOWN && m[i].has_model && m[i].model.state == AMV_MODEL_DRIVER)
            g_string_append(g, " · driver present, mouse did not answer");
    }
    gtk_label_set_text(GTK_LABEL(g_ui.lbl_dg_mice), g->str);
    g_string_free(g, TRUE);
}

static void refresh_dongle_panel(void) {
    dongle_slots_t sl;
    bool linked = dongle_get_slots(&sl);
    gtk_widget_set_visible(g_ui.dg_frame, linked);
    if (!linked) { gtk_widget_set_visible(g_ui.badge_dg, FALSE); return; }

    int sel = g_ui.dg_sel >= 0 && g_ui.dg_sel < sl.count ? g_ui.dg_sel : sl.active;
    g_ui.updating_ui = true;
    for (int i = 0; i < sl.count && i < AMV_NUM_SLOTS; i++) {
        bool saved = sl.mask >> i & 1;
        char lab[48];
        snprintf(lab, sizeof(lab), "%s Slot %d", saved ? "●" : "○", i + 1);
        gtk_button_set_label(GTK_BUTTON(g_ui.dg_slot_btn[i]), lab);
        if (saved) gtk_widget_add_css_class(g_ui.dg_slot_btn[i], "slot-saved");
        else gtk_widget_remove_css_class(g_ui.dg_slot_btn[i], "slot-saved");
        if (i == sl.active) gtk_widget_add_css_class(g_ui.dg_slot_btn[i], "slot-active");
        else gtk_widget_remove_css_class(g_ui.dg_slot_btn[i], "slot-active");
        gtk_widget_set_tooltip_text(g_ui.dg_slot_btn[i],
            saved ? (i == sl.active ? "Holds a saved profile; the dongle is working from it"
                                    : "Holds a saved profile")
                  : "Empty: nothing has been saved here");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_ui.dg_slot_btn[i]), i == sel);
    }
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_ui.chk_dg_bypass), sl.bypass);
    g_ui.updating_ui = false;

    bool sel_saved = sl.mask >> sel & 1;
    gtk_widget_set_sensitive(g_ui.btn_dg_load, !g_ui.dg_busy && sel_saved);
    gtk_widget_set_sensitive(g_ui.btn_dg_save, !g_ui.dg_busy);
    gtk_widget_set_sensitive(g_ui.chk_dg_bypass, !g_ui.dg_busy);
    for (int i = 0; i < AMV_NUM_SLOTS; i++) gtk_widget_set_sensitive(g_ui.dg_slot_btn[i], !g_ui.dg_busy);

    if (g_ui.dg_feedback_ticks == 0 && !g_ui.dg_busy) {
        char msg[160];
        snprintf(msg, sizeof(msg), "The dongle is working from slot %d%s. ● = saved on the dongle, ○ = empty.",
                 sl.active + 1, sl.bypass ? " · BYPASS ON (all directions 1.0)" : "");
        gtk_label_set_text(GTK_LABEL(g_ui.lbl_dg_status), msg);
    }
    refresh_dongle_mice();
}

typedef enum { DG_LOAD, DG_SAVE, DG_BYPASS } dg_kind_t;
typedef struct { dg_kind_t kind; int arg; int status; } dg_job_t;

static gboolean on_dg_job_done(gpointer data) {
    dg_job_t *j = data;
    g_ui.dg_busy = false;
    char msg[160];
    if (j->status == AMV_ST_OK) {
        switch (j->kind) {
        case DG_LOAD:   snprintf(msg, sizeof(msg), "✅ Loaded slot %d; the radar shows its weights", j->arg + 1); g_ui.dg_sel = -1; break;
        case DG_SAVE:   snprintf(msg, sizeof(msg), "💾 Saved the weights in effect to slot %d", j->arg + 1); g_ui.dg_sel = -1; break;
        case DG_BYPASS: snprintf(msg, sizeof(msg), j->arg ? "⏸ Bypass on: all directions 1.0, profile kept" : "▶ Bypass off: your profile is active"); break;
        }
    } else if (j->status == AMV_ST_EMPTY) {
        snprintf(msg, sizeof(msg), "⚠️ Slot %d is empty: nothing was ever saved there", j->arg + 1);
    } else if (j->status < 0) {
        snprintf(msg, sizeof(msg), "❌ The dongle did not answer (unplugged, or busy)");
    } else {
        snprintf(msg, sizeof(msg), "❌ The dongle refused it (status %d)", j->status);
    }
    gtk_label_set_text(GTK_LABEL(g_ui.lbl_dg_status), msg);
    g_ui.dg_feedback_ticks = 30;      /* keep the message for about 3 s */
    g_free(j);
    refresh_dongle_panel();
    sync_ui_from_state();
    return G_SOURCE_REMOVE;
}

static gpointer dg_job_thread(gpointer data) {
    dg_job_t *j = data;
    switch (j->kind) {
    case DG_LOAD:   j->status = dongle_slot_load((uint8_t)j->arg); break;
    case DG_SAVE:   j->status = dongle_slot_save((uint8_t)j->arg); break;
    case DG_BYPASS: j->status = dongle_set_bypass(j->arg != 0); break;
    }
    g_idle_add(on_dg_job_done, j);
    return NULL;
}

static void dg_submit(dg_kind_t kind, int arg) {
    if (g_ui.dg_busy) return;
    dg_job_t *j = g_new0(dg_job_t, 1);
    j->kind = kind;
    j->arg = arg;
    g_ui.dg_busy = true;
    g_ui.dg_feedback_ticks = 0;
    gtk_label_set_text(GTK_LABEL(g_ui.lbl_dg_status), "⏳ Talking to the dongle...");
    refresh_dongle_panel();
    g_thread_unref(g_thread_new("dongle-io", dg_job_thread, j));
}

static int dg_selected(void) {
    dongle_slots_t sl;
    if (!dongle_get_slots(&sl)) return 0;
    return g_ui.dg_sel >= 0 && g_ui.dg_sel < sl.count ? g_ui.dg_sel : sl.active;
}

static void on_dg_slot_toggled(GtkToggleButton *btn, gpointer user_data) {
    if (g_ui.updating_ui || !gtk_toggle_button_get_active(btn)) return;
    g_ui.dg_sel = GPOINTER_TO_INT(user_data);
    refresh_dongle_panel();
}

static void on_dg_load_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    dg_submit(DG_LOAD, dg_selected());
}

static void on_dg_overwrite_answer(GObject *src, GAsyncResult *res, gpointer data) {
    GError *err = NULL;
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, &err);
    if (err) g_error_free(err);               /* dismissed */
    else if (choice == 1) dg_submit(DG_SAVE, GPOINTER_TO_INT(data));
}

static void on_dg_save_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    dongle_slots_t sl;
    if (!dongle_get_slots(&sl)) return;
    int sel = dg_selected();
    /* Saving over the slot the dongle already works from is routine (it auto-saves there); any
     * other filled slot is somebody's stored profile: ask first, as the Studio does. */
    if ((sl.mask >> sel & 1) && sel != sl.active) {
        char title[64];
        snprintf(title, sizeof(title), "Overwrite slot %d?", sel + 1);
        GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", title);
        gtk_alert_dialog_set_detail(dlg, "It already holds a saved profile. Saving replaces it with the weights now in effect.");
        const char *buttons[] = { "Cancel", "Overwrite", NULL };
        gtk_alert_dialog_set_buttons(dlg, buttons);
        gtk_alert_dialog_set_cancel_button(dlg, 0);
        gtk_alert_dialog_set_default_button(dlg, 0);
        gtk_alert_dialog_choose(dlg, GTK_WINDOW(g_ui.window), NULL, on_dg_overwrite_answer, GINT_TO_POINTER(sel));
        g_object_unref(dlg);
        return;
    }
    dg_submit(DG_SAVE, sel);
}

static void on_dg_bypass_toggled(GtkCheckButton *btn, gpointer user_data) {
    (void)user_data;
    if (g_ui.updating_ui) return;
    dg_submit(DG_BYPASS, gtk_check_button_get_active(btn) ? 1 : 0);
}

/* Periodic 100ms UI Refresh Timer (reads lock-free atomics & telemetry mutex) */
static gboolean on_ui_tick(gpointer user_data) {
    (void)user_data;
    if (!g_ui.window) return G_SOURCE_REMOVE;
    /* SIGINT/SIGTERM only clear keep_running; close the window from the GTK thread */
    if (!atomic_load_explicit(&g_state.keep_running, memory_order_relaxed)) {
        g_application_quit(g_application_get_default());
        return G_SOURCE_REMOVE;
    }

    bool grabbed = atomic_load_explicit(&g_state.evdev_grabbed, memory_order_relaxed);
    uint64_t ticks = atomic_load_explicit(&g_state.total_ticks_processed, memory_order_relaxed);
    char rt_buf[512];
    char active_name[128], err[160];
    pthread_mutex_lock(&g_state.device_mutex);
    snprintf(active_name, sizeof(active_name), "%s", g_state.active_name);
    snprintf(err, sizeof(err), "%s", g_state.evdev_error);
    pthread_mutex_unlock(&g_state.device_mutex);
    if (grabbed) {
        snprintf(rt_buf, sizeof(rt_buf), "🟢 RT Engine: Vectoring \"%s\" (%llu ticks)",
                 active_name, (unsigned long long)ticks);
    } else {
        snprintf(rt_buf, sizeof(rt_buf), "🟡 RT Engine: %s",
                 err[0] ? err : "Selecting mouse...");
    }
    dongle_status_t dg;
    dongle_get_status(&dg);
    size_t used = strlen(rt_buf);
    switch (dg.link) {
    case DONGLE_LINKED:
        snprintf(rt_buf + used, sizeof(rt_buf) - used, "\n🔗 Vectoring: %s (%s; gen %u%s)",
                 dg.model, dg.detail, dg.gen, dg.bypass ? ", BYPASS" : "");
        break;
    case DONGLE_NO_ACCESS: case DONGLE_NOT_RESPONDING: case DONGLE_INCOMPATIBLE:
        snprintf(rt_buf + used, sizeof(rt_buf) - used, "\n⚠️ Dongle: %s", dg.detail);
        break;
    case DONGLE_NONE:
        break;
    }
    gtk_label_set_text(GTK_LABEL(g_ui.lbl_rt_status), rt_buf);

    /* The dongle (or another client, e.g. the web page) changed the weights */
    uint32_t drev = atomic_load_explicit(&g_state.dongle_weights_rev, memory_order_relaxed);
    if (drev != g_ui.last_dongle_rev) {
        g_ui.last_dongle_rev = drev;
        sync_ui_from_state();
    }

    /* Follow hotplug / device switches: re-query hardware when the grabbed mouse changes */
    int avid = atomic_load(&g_state.active_vid), apid = atomic_load(&g_state.active_pid);
    uint32_t mrev = atomic_load_explicit(&g_state.dongle_mice_rev, memory_order_relaxed);
    bool mrev_changed_for_panel = false;
    if (avid != g_ui.last_active_vid || apid != g_ui.last_active_pid || grabbed != g_ui.last_grabbed ||
        mrev != g_ui.last_mice_rev) {
        mrev_changed_for_panel = mrev != g_ui.last_mice_rev;
        g_ui.last_mice_rev = mrev;
        g_ui.last_active_vid = avid;
        g_ui.last_active_pid = apid;
        g_ui.last_grabbed = grabbed;
        refresh_hardware_widgets();
    }
    uint32_t srev = atomic_load_explicit(&g_state.dongle_slots_rev, memory_order_relaxed);
    if (srev != g_ui.last_slots_rev) {
        g_ui.last_slots_rev = srev;
        refresh_dongle_panel();
    } else if (mrev_changed_for_panel) {
        refresh_dongle_mice();
    }
    if (g_ui.dg_feedback_ticks > 0 && --g_ui.dg_feedback_ticks == 0) refresh_dongle_panel();
    update_responsive_layout();
    if (g_ui.faf_feedback_ticks > 0) {
        if (--g_ui.faf_feedback_ticks == 0) {
            refresh_faf_install_widgets();
        }
    }
    if (++g_ui.tick_count % 20 == 0) {
        rescan_devices(false);
        faf_install_info_t faf_info;
        if (faf_install_detect(&faf_info) &&
            (faf_info.state == FAF_INSTALL_NEEDS_REPAIR || faf_info.state == FAF_INSTALL_OUTDATED)) {
            char msg[512], status[600];
            if (faf_install_apply(msg, sizeof(msg))) {
                snprintf(status, sizeof(status), "🔄 Auto-repaired: %s", msg);
                gtk_label_set_text(GTK_LABEL(g_ui.lbl_profile_status), status);
                gtk_widget_set_tooltip_text(g_ui.lbl_profile_status, status);
            }
            refresh_faf_install_widgets();
        } else if (g_ui.faf_feedback_ticks == 0) {
            refresh_faf_install_widgets();
        }
    }

    pthread_mutex_lock(&g_state.telemetry_mutex);
    calib_telemetry_t cal = g_state.calib;
    pthread_mutex_unlock(&g_state.telemetry_mutex);

    /* Update per-octant landing ratio badges */
    int req = atomic_load_explicit(&g_state.trials_per_round, memory_order_relaxed);
    for (int i = 0; i < 8; i++) {
        char rbuf[64];
        if (cal.octant[i].total_flicks > 0) {
            snprintf(rbuf, sizeof(rbuf), "r̃=%.2f (%d/%d)",
                     cal.octant[i].median_ratio, cal.octant[i].trial_count, req);
        } else {
            snprintf(rbuf, sizeof(rbuf), "r̃=-- (0/%d)", req);
        }
        gtk_label_set_text(GTK_LABEL(g_ui.oct_ratio_lbls[i]), rbuf);
    }

    /* Update FpsAimForge (FAF) Native Plugin & Session State */
    char kov_buf[256];
    if (cal.faf_connected) {
        const char *scen = cal.faf_scenario[0] ? cal.faf_scenario : "Select Scenario";
        switch (cal.faf_run_state) {
            case FAF_STATE_WAITING_FOR_CLICK_TO_START:
                snprintf(kov_buf, sizeof(kov_buf),
                         "⏳ FpsAimForge (PID %d): \"%s\" — Click to Start Round (Telemetry Gated)",
                         cal.faf_pid, scen);
                break;
            case FAF_STATE_START_COUNTDOWN:
                snprintf(kov_buf, sizeof(kov_buf),
                         "⏱️ FpsAimForge (PID %d): \"%s\" — Countdown Starting... (Telemetry Gated)",
                         cal.faf_pid, scen);
                break;
            case FAF_STATE_RUNNING:
                snprintf(kov_buf, sizeof(kov_buf),
                         "🟢 FAF ACTIVE ROUND #%u: \"%s\" (%.1fs / %.0fs) · Hits: %d/%d · Targets: %d · Cam(%.1f°, %.1f°)",
                         cal.faf_round_id, scen, cal.faf_elapsed_sec, cal.faf_duration_sec,
                         cal.faf_hits, cal.faf_shots, cal.active_bot_count,
                         cal.cam_yaw, cal.cam_pitch);
                break;
            case FAF_STATE_DONE:
            case FAF_STATE_MENU:
            default:
                snprintf(kov_buf, sizeof(kov_buf),
                         "🟡 FpsAimForge (PID %d): In Menu / Round Complete — Completed Rounds: %u (Telemetry Gated)",
                         cal.faf_pid, cal.faf_rounds_completed);
                break;
        }
    } else {
        snprintf(kov_buf, sizeof(kov_buf),
                 "⚫ FpsAimForge: Offline — Launch FpsAimForge to begin Phase 2 Calibration (Desktop movements ignored)");
    }
    gtk_label_set_text(GTK_LABEL(g_ui.lbl_kovaaks_status), kov_buf);

    char csv_buf[256];
    if (cal.faf_connected || cal.faf_rounds_completed > 0 || cal.faf_scenario[0] != '\0') {
        snprintf(csv_buf, sizeof(csv_buf),
                 "📊 FAF Scenario: %s · Hits: %d/%d (%.1f%%) · Kills: %d · Sens: %.1f cm/360 · Rounds Done: %u (Aborted: %u)",
                 cal.faf_scenario[0] ? cal.faf_scenario : "None",
                 cal.faf_hits, cal.faf_shots, cal.last_accuracy, cal.faf_kills,
                 cal.faf_cm_per_360, cal.faf_rounds_completed, cal.faf_rounds_aborted);
    } else {
        snprintf(csv_buf, sizeof(csv_buf),
                 "📊 Ready for FpsAimForge (/dev/shm/anymouse_faf_telemetry) · Custom Bundle: \"AnyMouse 8-Octant Center-Out\"");
    }
    gtk_label_set_text(GTK_LABEL(g_ui.lbl_csv_summary), csv_buf);

    char flick_buf[256];
    if (cal.total_valid_flicks > 0) {
        snprintf(flick_buf, sizeof(flick_buf),
                 "🎯 Flicks: %d (Rounds Applied: %d) · Last [%s]: r_i = %.3f (%s) · Skew: %+.1f° · ω_peak: %.0f°/s",
                 cal.total_valid_flicks, cal.rounds_applied,
                 OCTANT_INFO[cal.last_flick_octant & 7].short_name,
                 cal.last_landing_ratio,
                 (cal.last_landing_ratio < 0.96f) ? "UNDERSHOOT" :
                 (cal.last_landing_ratio > 1.04f) ? "OVERSHOOT" : "ON-TARGET",
                 cal.last_skew_deg, cal.last_v_peak);
    } else {
        snprintf(flick_buf, sizeof(flick_buf),
                 "🎯 3D Ballistic Submovement Engine Ready (Direct FpsAimForge Camera & TargetManager Telemetry)");
    }
    gtk_label_set_text(GTK_LABEL(g_ui.lbl_last_flick), flick_buf);

    /* Keep sliders synced if auto-calibration updated weights */
    if (g_ui.dragging_idx < 0) {
        sync_ui_from_state();
    }
    return G_SOURCE_CONTINUE;
}

/* ---- layout helpers: everything must be able to shrink, wrap or scroll; nothing may force a window size ---- */

/* A dropdown whose entries are cut short with an ellipsis. By default a GtkDropDown is as wide as its longest
 * entry (a /dev/input/by-id path, or a long product name), which sets the window's minimum width. */
static void ellipsized_item_setup(GtkSignalListItemFactory *f, GtkListItem *item, gpointer d) {
    (void)f; (void)d;
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 60);
    gtk_list_item_set_child(item, l);
}

static void ellipsized_item_bind(GtkSignalListItemFactory *f, GtkListItem *item, gpointer d) {
    (void)f; (void)d;
    GtkStringObject *o = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
    GtkWidget *l = gtk_list_item_get_child(item);
    const char *t = o ? gtk_string_object_get_string(o) : "";
    gtk_label_set_text(GTK_LABEL(l), t);
    gtk_widget_set_tooltip_text(l, t);
}

static void dropdown_ellipsize(GtkWidget *dd) {
    GtkListItemFactory *f = gtk_signal_list_item_factory_new();
    g_signal_connect(f, "setup", G_CALLBACK(ellipsized_item_setup), NULL);
    g_signal_connect(f, "bind", G_CALLBACK(ellipsized_item_bind), NULL);
    gtk_drop_down_set_factory(GTK_DROP_DOWN(dd), f);
    gtk_drop_down_set_list_factory(GTK_DROP_DOWN(dd), f);
    g_object_unref(f);
}

/* Developer hook: ANYMOUSE_SHOT=file.png renders just this window to a PNG after the page has settled and then
 * exits WITHOUT saving the profile; ANYMOUSE_WINDOW=WxH opens it at that size. Used to check the layout at different window sizes. */
static gboolean dev_shot_cb(gpointer path) {
    GtkWidget *w = g_ui.window;
    int ww = gtk_widget_get_width(w), hh = gtk_widget_get_height(w);
    GdkPaintable *pt = gtk_widget_paintable_new(w);
    GtkSnapshot *sn = gtk_snapshot_new();
    gdk_paintable_snapshot(pt, sn, ww, hh);
    GskRenderNode *node = gtk_snapshot_free_to_node(sn);
    GskRenderer *r = gtk_native_get_renderer(GTK_NATIVE(w));
    graphene_rect_t b = GRAPHENE_RECT_INIT(0, 0, (float)ww, (float)hh);
    GdkTexture *tex = r && node ? gsk_renderer_render_texture(r, node, &b) : NULL;
    if (tex) {
        gdk_texture_save_to_png(tex, (const char *)path);
        printf("[GUI] wrote %dx%d window image to %s (%s layout)\n", ww, hh, (const char *)path, g_ui.stacked ? "stacked" : "two-column");
        g_object_unref(tex);
    }
    if (node) gsk_render_node_unref(node);
    g_object_unref(pt);
    fflush(stdout);
    _exit(0);      /* a normal quit would auto-save the weights this test run adopted from the dongle */
    return G_SOURCE_REMOVE;
}

/* A frame whose title can be cut short instead of widening the window. */
static GtkWidget *frame_new(const char *title) {
    GtkWidget *f = gtk_frame_new(NULL);
    GtkWidget *l = gtk_label_new(title);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_widget_set_tooltip_text(l, title);
    gtk_frame_set_label_widget(GTK_FRAME(f), l);
    return f;
}

/* A row of controls that wraps onto more lines when the window is narrow. */
static GtkWidget *flow_new(void) {
    GtkWidget *f = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(f), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(f), FALSE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(f), 32);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(f), 8);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(f), 6);
    gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(f), FALSE);
    return f;
}

static void flow_add(GtkWidget *f, GtkWidget *w) {
    gtk_flow_box_append(GTK_FLOW_BOX(f), w);
    GtkWidget *child = gtk_widget_get_parent(w);          /* the GtkFlowBoxChild wrapper: not a tab stop */
    if (child) gtk_widget_set_focusable(child, FALSE);
}

/* Narrow window: put the two columns above each other (the page then scrolls) instead of cutting things off. */
static void update_responsive_layout(void) {
    if (!g_ui.root_box || !g_ui.window) return;
    int w = gtk_widget_get_width(g_ui.window);
    if (w <= 0) return;
    bool want = g_ui.stacked ? w < 1060 : w < 1000;       /* hysteresis, so dragging the edge does not flicker */
    if (want == g_ui.stacked) return;
    g_ui.stacked = want;
    gtk_orientable_set_orientation(GTK_ORIENTABLE(g_ui.root_box),
                                   want ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
}

static void on_app_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, DARK_CSS);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(provider);

    GtkWidget *win = gtk_application_window_new(app);
    g_ui.window = win;
    gtk_window_set_title(GTK_WINDOW(win), "AnyMouse Vectoring Studio");
    int win_w = 1120, win_h = 740;
    const char *env_size = g_getenv("ANYMOUSE_WINDOW");
    if (env_size) sscanf(env_size, "%dx%d", &win_w, &win_h);
    gtk_window_set_default_size(GTK_WINDOW(win), win_w, win_h);

    GtkWidget *root_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_margin_start(root_hbox, 14);
    gtk_widget_set_margin_end(root_hbox, 14);
    gtk_widget_set_margin_top(root_hbox, 14);
    gtk_widget_set_margin_bottom(root_hbox, 14);
    g_ui.root_box = root_hbox;

    /* The whole page scrolls, so the window can be any size and nothing is ever clipped out of reach. */
    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroller), FALSE);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), FALSE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), root_hbox);
    gtk_window_set_child(GTK_WINDOW(win), scroller);

    /* ================= LEFT COLUMN ================= */
    GtkWidget *left_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_hexpand(left_vbox, TRUE);
    gtk_box_append(GTK_BOX(root_hbox), left_vbox);

    /* 1. Hardware Mouse & Sensor Sync Frame */
    GtkWidget *hw_frame = frame_new("Hardware Mouse & Sensor Sync (OpenMouse HIDraw + RT Engine)");
    GtkWidget *hw_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_frame_set_child(GTK_FRAME(hw_frame), hw_vbox);
    gtk_box_append(GTK_BOX(left_vbox), hw_frame);

    GtkWidget *dev_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(dev_hbox), gtk_label_new("Mouse:"));
    g_ui.device_model = gtk_string_list_new(NULL);
    g_ui.device_dropdown = gtk_drop_down_new(G_LIST_MODEL(g_ui.device_model), NULL);
    gtk_widget_set_hexpand(g_ui.device_dropdown, TRUE);
    dropdown_ellipsize(g_ui.device_dropdown);
    g_signal_connect(g_ui.device_dropdown, "notify::selected", G_CALLBACK(on_device_dropdown_changed), NULL);
    gtk_box_append(GTK_BOX(dev_hbox), g_ui.device_dropdown);

    GtkWidget *btn_rescan = gtk_button_new_with_label("🔄 Rescan");
    g_signal_connect(btn_rescan, "clicked", G_CALLBACK(on_device_rescan_clicked), NULL);
    gtk_box_append(GTK_BOX(dev_hbox), btn_rescan);

    g_ui.btn_identify = gtk_button_new_with_label("🎯 Identify");
    gtk_widget_set_tooltip_text(g_ui.btn_identify, "Pick the mouse by moving it for a few seconds");
    g_signal_connect(g_ui.btn_identify, "clicked", G_CALLBACK(on_identify_clicked), NULL);
    gtk_box_append(GTK_BOX(dev_hbox), g_ui.btn_identify);
    gtk_box_append(GTK_BOX(hw_vbox), dev_hbox);

    GtkWidget *manual_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_ui.device_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_ui.device_entry),
                                   "Manual fallback: /dev/input/by-id/...-event-mouse or /dev/input/eventN");
    gtk_widget_set_hexpand(g_ui.device_entry, TRUE);
    g_signal_connect(g_ui.device_entry, "activate", G_CALLBACK(on_device_entry_apply), NULL);
    gtk_box_append(GTK_BOX(manual_hbox), g_ui.device_entry);
    GtkWidget *btn_use = gtk_button_new_with_label("Use Path");
    g_signal_connect(btn_use, "clicked", G_CALLBACK(on_device_entry_apply), NULL);
    gtk_box_append(GTK_BOX(manual_hbox), btn_use);
    gtk_box_append(GTK_BOX(hw_vbox), manual_hbox);

    g_ui.lbl_mouse_ident = gtk_label_new("🖱️ Detecting Mouse...");
    gtk_widget_set_halign(g_ui.lbl_mouse_ident, GTK_ALIGN_START);
    gtk_widget_set_hexpand(g_ui.lbl_mouse_ident, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(g_ui.lbl_mouse_ident), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(hw_vbox), g_ui.lbl_mouse_ident);

    g_ui.lbl_rt_status = gtk_label_new("🟢 RT Engine: Initializing...");
    gtk_widget_set_halign(g_ui.lbl_rt_status, GTK_ALIGN_START);
    gtk_widget_set_hexpand(g_ui.lbl_rt_status, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(g_ui.lbl_rt_status), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(hw_vbox), g_ui.lbl_rt_status);

    GtkWidget *badge_hbox = flow_new();
    g_ui.badge_rate = gtk_label_new("⚡ Polling: -- Hz");
    gtk_widget_add_css_class(g_ui.badge_rate, "badge");
    gtk_widget_add_css_class(g_ui.badge_rate, "badge-cyan");
    flow_add(badge_hbox, g_ui.badge_rate);

    g_ui.badge_bat = gtk_label_new("🔋 Battery: --%");
    gtk_widget_add_css_class(g_ui.badge_bat, "badge");
    gtk_widget_add_css_class(g_ui.badge_bat, "badge-green");
    flow_add(badge_hbox, g_ui.badge_bat);

    g_ui.badge_sync = gtk_label_new("🎯 Motion Sync: --");
    gtk_widget_add_css_class(g_ui.badge_sync, "badge");
    gtk_widget_add_css_class(g_ui.badge_sync, "badge-pink");
    flow_add(badge_hbox, g_ui.badge_sync);

    g_ui.badge_rssi = gtk_label_new("📶 Signal: -- dBm");
    gtk_widget_add_css_class(g_ui.badge_rssi, "badge");
    gtk_widget_add_css_class(g_ui.badge_rssi, "badge-amber");
    flow_add(badge_hbox, g_ui.badge_rssi);

    g_ui.badge_dg = gtk_label_new("🔗 AnyMouse Dongle → PC");
    gtk_widget_add_css_class(g_ui.badge_dg, "badge");
    gtk_widget_add_css_class(g_ui.badge_dg, "badge-cyan");
    gtk_widget_set_visible(g_ui.badge_dg, FALSE);        /* until a dongle links */
    flow_add(badge_hbox, g_ui.badge_dg);
    gtk_box_append(GTK_BOX(hw_vbox), badge_hbox);

    /* Shown only when a mouse behind the dongle has a driver but did not answer it */
    g_ui.lbl_dg_warn = gtk_label_new("");
    gtk_widget_add_css_class(g_ui.lbl_dg_warn, "dg-warn");
    gtk_widget_set_halign(g_ui.lbl_dg_warn, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(g_ui.lbl_dg_warn), TRUE);
    gtk_label_set_xalign(GTK_LABEL(g_ui.lbl_dg_warn), 0.0f);
    gtk_label_set_max_width_chars(GTK_LABEL(g_ui.lbl_dg_warn), 90);
    gtk_widget_set_visible(g_ui.lbl_dg_warn, FALSE);
    gtk_box_append(GTK_BOX(hw_vbox), g_ui.lbl_dg_warn);

    GtkWidget *cpi_hbox = flow_new();
    flow_add(cpi_hbox, gtk_label_new("Locked Sensor CPI:"));
    g_ui.cpi_dropdown = gtk_drop_down_new_from_strings(CPI_STRINGS);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_ui.cpi_dropdown), 2); /* 3200 CPI */
    g_signal_connect(g_ui.cpi_dropdown, "notify::selected", G_CALLBACK(on_cpi_dropdown_changed), NULL);
    flow_add(cpi_hbox, g_ui.cpi_dropdown);

    g_ui.btn_cpi_3200 = gtk_button_new_with_label("Set 3200 CPI");
    g_signal_connect(g_ui.btn_cpi_3200, "clicked", G_CALLBACK(on_set_device_cpi_clicked), GINT_TO_POINTER(3200));
    flow_add(cpi_hbox, g_ui.btn_cpi_3200);

    g_ui.btn_cpi_6400 = gtk_button_new_with_label("Set 6400 CPI");
    g_signal_connect(g_ui.btn_cpi_6400, "clicked", G_CALLBACK(on_set_device_cpi_clicked), GINT_TO_POINTER(6400));
    flow_add(cpi_hbox, g_ui.btn_cpi_6400);

    flow_add(cpi_hbox, gtk_label_new("Polling:"));
    g_ui.rate_model = gtk_string_list_new(NULL);
    g_ui.rate_dropdown = gtk_drop_down_new(G_LIST_MODEL(g_ui.rate_model), NULL);
    g_signal_connect(g_ui.rate_dropdown, "notify::selected", G_CALLBACK(on_rate_dropdown_changed), NULL);
    flow_add(cpi_hbox, g_ui.rate_dropdown);

    GtkWidget *btn_refresh = gtk_button_new_with_label("🔄 Refresh");
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh_hw_clicked), NULL);
    flow_add(cpi_hbox, btn_refresh);
    gtk_box_append(GTK_BOX(hw_vbox), cpi_hbox);

    /* 1b. AnyMouse dongle: profiles stored on the device (shown while a dongle is linked) */
    g_ui.dg_frame = frame_new("Dongle · Profiles Stored on the Device");
    GtkWidget *dg_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_frame_set_child(GTK_FRAME(g_ui.dg_frame), dg_vbox);
    g_ui.dg_sel = -1;

    g_ui.lbl_dg_model = gtk_label_new("AnyMouse Dongle");
    gtk_widget_add_css_class(g_ui.lbl_dg_model, "dg-model");
    gtk_widget_set_halign(g_ui.lbl_dg_model, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(g_ui.lbl_dg_model), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(dg_vbox), g_ui.lbl_dg_model);

    g_ui.lbl_dg_info = gtk_label_new("");
    gtk_widget_add_css_class(g_ui.lbl_dg_info, "dg-info");
    gtk_widget_set_halign(g_ui.lbl_dg_info, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(g_ui.lbl_dg_info), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(g_ui.lbl_dg_info), TRUE);
    gtk_box_append(GTK_BOX(dg_vbox), g_ui.lbl_dg_info);

    GtkWidget *dg_slots = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    for (int i = 0; i < AMV_NUM_SLOTS; i++) {
        g_ui.dg_slot_btn[i] = gtk_toggle_button_new_with_label("○ Slot");
        if (i > 0) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(g_ui.dg_slot_btn[i]), GTK_TOGGLE_BUTTON(g_ui.dg_slot_btn[0]));
        gtk_widget_set_hexpand(g_ui.dg_slot_btn[i], TRUE);
        g_signal_connect(g_ui.dg_slot_btn[i], "toggled", G_CALLBACK(on_dg_slot_toggled), GINT_TO_POINTER(i));
        gtk_box_append(GTK_BOX(dg_slots), g_ui.dg_slot_btn[i]);
    }
    gtk_box_append(GTK_BOX(dg_vbox), dg_slots);

    GtkWidget *dg_actions = flow_new();
    g_ui.btn_dg_load = gtk_button_new_with_label("📂 Load slot");
    gtk_widget_set_tooltip_text(g_ui.btn_dg_load, "Make the selected slot's saved weights the ones in effect (on the dongle and in the radar)");
    g_signal_connect(g_ui.btn_dg_load, "clicked", G_CALLBACK(on_dg_load_clicked), NULL);
    flow_add(dg_actions, g_ui.btn_dg_load);
    g_ui.btn_dg_save = gtk_button_new_with_label("💾 Save current to slot");
    gtk_widget_set_tooltip_text(g_ui.btn_dg_save, "Store the weights now in effect in the selected slot and make it the active one");
    g_signal_connect(g_ui.btn_dg_save, "clicked", G_CALLBACK(on_dg_save_clicked), NULL);
    flow_add(dg_actions, g_ui.btn_dg_save);
    g_ui.chk_dg_bypass = gtk_check_button_new_with_label("Bypass (all directions 1.0)");
    gtk_widget_set_tooltip_text(g_ui.chk_dg_bypass, "Disables the vectoring without touching the saved profile. Not saved: it resets when the dongle powers up.");
    g_signal_connect(g_ui.chk_dg_bypass, "toggled", G_CALLBACK(on_dg_bypass_toggled), NULL);
    flow_add(dg_actions, g_ui.chk_dg_bypass);
    gtk_box_append(GTK_BOX(dg_vbox), dg_actions);

    g_ui.lbl_dg_status = gtk_label_new("");
    gtk_widget_set_halign(g_ui.lbl_dg_status, GTK_ALIGN_START);
    gtk_widget_set_hexpand(g_ui.lbl_dg_status, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(g_ui.lbl_dg_status), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(dg_vbox), g_ui.lbl_dg_status);

    g_ui.lbl_dg_mice = gtk_label_new("");
    gtk_widget_add_css_class(g_ui.lbl_dg_mice, "dg-mice");
    gtk_widget_set_halign(g_ui.lbl_dg_mice, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(g_ui.lbl_dg_mice), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(g_ui.lbl_dg_mice), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(g_ui.lbl_dg_mice), 100);
    gtk_label_set_selectable(GTK_LABEL(g_ui.lbl_dg_mice), TRUE);
    gtk_box_append(GTK_BOX(dg_vbox), g_ui.lbl_dg_mice);

    gtk_widget_set_visible(g_ui.dg_frame, FALSE);        /* until a dongle links */
    gtk_box_append(GTK_BOX(left_vbox), g_ui.dg_frame);

    /* 2. Interactive Polar Radar Chart Frame */
    GtkWidget *radar_frame = frame_new("Polar Sensitivity Map (drag the dots · Yellow=P₁, Green=Target T)");
    gtk_widget_set_vexpand(radar_frame, TRUE);
    g_ui.drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(g_ui.drawing_area, 300, 300);
    gtk_widget_set_vexpand(g_ui.drawing_area, TRUE);
    gtk_widget_set_hexpand(g_ui.drawing_area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_ui.drawing_area), on_radar_draw, NULL, NULL);

    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_radar_drag_begin), NULL);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_radar_drag_update), NULL);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_radar_drag_end), NULL);
    gtk_widget_add_controller(g_ui.drawing_area, GTK_EVENT_CONTROLLER(drag));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_radar_motion), NULL);
    gtk_widget_add_controller(g_ui.drawing_area, motion);

    gtk_frame_set_child(GTK_FRAME(radar_frame), g_ui.drawing_area);
    gtk_box_append(GTK_BOX(left_vbox), radar_frame);

    /* 3. Quick Presets Bar */
    GtkWidget *preset_frame = frame_new("Quick Presets");
    GtkWidget *preset_hbox = flow_new();
    gtk_frame_set_child(GTK_FRAME(preset_frame), preset_hbox);

    const struct { const char *label; const char *key; } presets[] = {
        { "1:1 (1.0x)",       "passthrough_1x" },
        { "Half (0.50x)",     "half_05x" },
        { "Quarter (0.25x)",  "neutral" },
        { "Vertical Boost",   "vertical_boost" },
        { "Horizontal Boost", "horizontal_boost" },
        { "Diagonal ULX",     "diagonal_ulx" }
    };
    for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
        GtkWidget *b = gtk_button_new_with_label(presets[i].label);
        gtk_widget_set_hexpand(b, TRUE);
        g_signal_connect(b, "clicked", G_CALLBACK(on_preset_clicked), (gpointer)presets[i].key);
        flow_add(preset_hbox, b);
    }
    gtk_box_append(GTK_BOX(left_vbox), preset_frame);

    /* The radar is the main control: it and its presets come first, the hardware and dongle panels below it */
    gtk_box_reorder_child_after(GTK_BOX(left_vbox), radar_frame, NULL);
    gtk_box_reorder_child_after(GTK_BOX(left_vbox), preset_frame, radar_frame);

    /* ================= RIGHT COLUMN ================= */
    GtkWidget *right_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_hexpand(right_vbox, TRUE);
    gtk_box_append(GTK_BOX(root_hbox), right_vbox);

    /* 4. FpsAimForge (FAF) Live Telemetry & Phase 2 Calibration Frame */
    GtkWidget *kov_frame = frame_new("Phase 2: FpsAimForge (FAF) Native Plugin Telemetry & Ballistic Auto-Calibration");
    GtkWidget *kov_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_frame_set_child(GTK_FRAME(kov_frame), kov_vbox);
    gtk_box_append(GTK_BOX(right_vbox), kov_frame);

    g_ui.lbl_kovaaks_status = gtk_label_new("🎮 FpsAimForge: Waiting for connection...");
    gtk_widget_set_halign(g_ui.lbl_kovaaks_status, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(g_ui.lbl_kovaaks_status), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(kov_vbox), g_ui.lbl_kovaaks_status);

    g_ui.lbl_csv_summary = gtk_label_new("📊 FpsAimForge Native Shared-Memory Telemetry Ready");
    gtk_widget_set_halign(g_ui.lbl_csv_summary, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(g_ui.lbl_csv_summary), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(kov_vbox), g_ui.lbl_csv_summary);

    g_ui.lbl_last_flick = gtk_label_new("🎯 Ballistic Submovement Detector Ready");
    gtk_widget_set_halign(g_ui.lbl_last_flick, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(g_ui.lbl_last_flick), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(kov_vbox), g_ui.lbl_last_flick);

    GtkWidget *calib_ctrl_hbox = flow_new();
    g_ui.chk_auto_calib = gtk_check_button_new_with_label("Inter-Round Auto-Adapt (α=0.35, ±5% clamp on Round End)");
    g_signal_connect(g_ui.chk_auto_calib, "toggled", G_CALLBACK(on_auto_calib_toggled), NULL);
    /* Reflect --auto-calibrate passed on the command line */
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_ui.chk_auto_calib),
                                atomic_load_explicit(&g_state.auto_calibrate, memory_order_relaxed));
    flow_add(calib_ctrl_hbox, g_ui.chk_auto_calib);

    GtkWidget *btn_apply_calib = gtk_button_new_with_label("⚡ Apply Damped Update");
    gtk_widget_add_css_class(btn_apply_calib, "primary-btn");
    g_signal_connect(btn_apply_calib, "clicked", G_CALLBACK(on_apply_calib_now_clicked), NULL);
    flow_add(calib_ctrl_hbox, btn_apply_calib);

    GtkWidget *btn_reset_calib = gtk_button_new_with_label("↺ Reset Stats");
    g_signal_connect(btn_reset_calib, "clicked", G_CALLBACK(on_reset_calib_clicked), NULL);
    flow_add(calib_ctrl_hbox, btn_reset_calib);
    gtk_box_append(GTK_BOX(kov_vbox), calib_ctrl_hbox);

    GtkWidget *faf_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_ui.lbl_faf_install = gtk_label_new("FpsAimForge: checking Steam libraries...");
    gtk_widget_set_halign(g_ui.lbl_faf_install, GTK_ALIGN_START);
    gtk_widget_set_hexpand(g_ui.lbl_faf_install, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(g_ui.lbl_faf_install), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(faf_hbox), g_ui.lbl_faf_install);
    g_ui.btn_faf_install = gtk_button_new_with_label("Install");
    gtk_widget_set_tooltip_text(g_ui.btn_faf_install,
                                "Install the AnyMouse launcher, telemetry plugin and calibration scenarios into FpsAimForge");
    g_signal_connect(g_ui.btn_faf_install, "clicked", G_CALLBACK(on_faf_install_clicked), GINT_TO_POINTER(0));
    gtk_box_append(GTK_BOX(faf_hbox), g_ui.btn_faf_install);
    g_ui.btn_faf_remove = gtk_button_new_with_label("Uninstall");
    g_signal_connect(g_ui.btn_faf_remove, "clicked", G_CALLBACK(on_faf_install_clicked), GINT_TO_POINTER(1));
    gtk_box_append(GTK_BOX(faf_hbox), g_ui.btn_faf_remove);
    gtk_box_append(GTK_BOX(kov_vbox), faf_hbox);

    /* 5. Master Vector Scaling Frame */
    GtkWidget *master_frame = frame_new("Master Vector Scaling & Symmetry");
    GtkWidget *master_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_frame_set_child(GTK_FRAME(master_frame), master_vbox);

    GtkWidget *m_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(m_hbox), gtk_label_new("Uniform Octant Scalar:"));
    g_ui.master_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 5.0, 200.0, 1.0);
    gtk_range_set_value(GTK_RANGE(g_ui.master_scale), 25.0);
    gtk_scale_set_draw_value(GTK_SCALE(g_ui.master_scale), FALSE);
    gtk_widget_set_hexpand(g_ui.master_scale, TRUE);
    g_signal_connect(g_ui.master_scale, "value-changed", G_CALLBACK(on_master_scale_changed), NULL);
    gtk_box_append(GTK_BOX(m_hbox), g_ui.master_scale);

    g_ui.master_spin = gtk_spin_button_new_with_range(0.05, 2.00, 0.01);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_ui.master_spin), 0.25);
    g_signal_connect(g_ui.master_spin, "value-changed", G_CALLBACK(on_master_spin_changed), NULL);
    gtk_box_append(GTK_BOX(m_hbox), g_ui.master_spin);
    gtk_box_append(GTK_BOX(master_vbox), m_hbox);

    GtkWidget *sym_hbox = flow_new();
    g_ui.chk_mirror_lr = gtk_check_button_new_with_label("Link Left/Right (E↔W, NE↔NW, SE↔SW)");
    g_signal_connect(g_ui.chk_mirror_lr, "toggled", G_CALLBACK(on_mirror_lr_toggled), NULL);
    flow_add(sym_hbox, g_ui.chk_mirror_lr);

    g_ui.chk_mirror_ud = gtk_check_button_new_with_label("Link Up/Down (N↔S)");
    g_signal_connect(g_ui.chk_mirror_ud, "toggled", G_CALLBACK(on_mirror_ud_toggled), NULL);
    flow_add(sym_hbox, g_ui.chk_mirror_ud);
    gtk_box_append(GTK_BOX(master_vbox), sym_hbox);
    gtk_box_append(GTK_BOX(right_vbox), master_frame);

    /* 6. Octant Fine Tuning Grid */
    GtkWidget *oct_frame = frame_new("Octant Fine Tuning & Live Ballistic Ratios");
    gtk_widget_set_vexpand(oct_frame, TRUE);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_frame_set_child(GTK_FRAME(oct_frame), grid);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Octant"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Multiplier"), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Value"), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Eff. DPI"), 3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Median r̃"), 4, 0, 1, 1);

    for (int i = 0; i < 8; i++) {
        char lbl_txt[32];
        snprintf(lbl_txt, sizeof(lbl_txt), "%s (%.0f°)", OCTANT_INFO[i].short_name, OCTANT_INFO[i].angle_deg);
        GtkWidget *lbl = gtk_label_new(lbl_txt);
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, i + 1, 1, 1);

        GtkWidget *sc = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 20.0, 2000.0, 1.0);
        gtk_scale_set_draw_value(GTK_SCALE(sc), FALSE);
        gtk_widget_set_hexpand(sc, TRUE);
        g_signal_connect(sc, "value-changed", G_CALLBACK(on_octant_scale_changed), GINT_TO_POINTER(i));
        gtk_grid_attach(GTK_GRID(grid), sc, 1, i + 1, 1, 1);
        g_ui.oct_scales[i] = sc;

        GtkWidget *sp = gtk_spin_button_new_with_range(0.010, 3.000, 0.005);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(sp), 3);
        g_signal_connect(sp, "value-changed", G_CALLBACK(on_octant_spin_changed), GINT_TO_POINTER(i));
        gtk_grid_attach(GTK_GRID(grid), sp, 2, i + 1, 1, 1);
        g_ui.oct_spins[i] = sp;

        GtkWidget *dpi_l = gtk_label_new("800 DPI");
        gtk_widget_add_css_class(dpi_l, "badge-cyan");
        gtk_grid_attach(GTK_GRID(grid), dpi_l, 3, i + 1, 1, 1);
        g_ui.oct_dpi_lbls[i] = dpi_l;

        GtkWidget *rat_l = gtk_label_new("r̃=-- (0/5)");
        gtk_widget_add_css_class(rat_l, "badge");
        gtk_grid_attach(GTK_GRID(grid), rat_l, 4, i + 1, 1, 1);
        g_ui.oct_ratio_lbls[i] = rat_l;
    }
    gtk_box_append(GTK_BOX(right_vbox), oct_frame);

    /* 7. Bottom Profile Save/Load Bar */
    GtkWidget *bot_hbox = flow_new();
    GtkWidget *btn_save = gtk_button_new_with_label("💾 Save Profile (auto-saved on exit)");
    g_signal_connect(btn_save, "clicked", G_CALLBACK(on_save_profile_clicked), NULL);
    flow_add(bot_hbox, btn_save);

    GtkWidget *btn_load = gtk_button_new_with_label("📂 Reload Saved Profile");
    g_signal_connect(btn_load, "clicked", G_CALLBACK(on_load_profile_clicked), NULL);
    flow_add(bot_hbox, btn_load);

    g_ui.lbl_profile_status = gtk_label_new("⚡ Direct Shared-Memory Atomic Engine");
    gtk_widget_set_halign(g_ui.lbl_profile_status, GTK_ALIGN_START);
    gtk_widget_set_hexpand(g_ui.lbl_profile_status, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(g_ui.lbl_profile_status), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(g_ui.lbl_profile_status, "badge-green");
    flow_add(bot_hbox, g_ui.lbl_profile_status);
    gtk_box_append(GTK_BOX(right_vbox), bot_hbox);

    g_hw_queue = g_async_queue_new();
    g_thread_unref(g_thread_new("hw-io", hw_worker, NULL));

    rescan_devices(true);
    refresh_hardware_widgets();
    refresh_faf_install_widgets();
    g_timeout_add(100, on_ui_tick, NULL);

    gtk_window_present(GTK_WINDOW(win));
    const char *shot = g_getenv("ANYMOUSE_SHOT");
    if (shot && shot[0]) g_timeout_add(4000, dev_shot_cb, g_strdup(shot));
}

int gui_run(int argc, char **argv) {
    (void)argc;
    /* Only pass program name to GTK Application so custom CLI args don't trigger GApplication rejection */
    char *gtk_argv[2] = { argv[0], NULL };
    int gtk_argc = 1;

    GtkApplication *app = gtk_application_new("org.anymouse.vectoring", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), gtk_argc, gtk_argv);
    g_object_unref(app);
    return status;
}
