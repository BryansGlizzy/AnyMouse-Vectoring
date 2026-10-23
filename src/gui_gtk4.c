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

static const char *DARK_CSS =
    "window, .main-bg { background-color: #111318; color: #e2e8f0; font-family: 'Inter', 'Segoe UI', sans-serif; font-size: 13px; }\n"
    "frame { background-color: #171922; border: 1px solid #262a38; border-radius: 10px; padding: 10px; }\n"
    "frame > label { color: #00f0ff; font-weight: bold; font-size: 13px; padding: 0 6px; }\n"
    "button { background-image: none; background-color: #212636; border: 1px solid #31384e; border-radius: 6px; padding: 5px 10px; color: #f1f5f9; font-weight: 600; }\n"
    "button:hover { background-color: #2c3348; border-color: #00f0ff; color: #00f0ff; }\n"
    "button.primary-btn { background-color: #00d4ff; color: #0f172a; border: none; font-weight: bold; }\n"
    "button.primary-btn:hover { background-color: #38bdf8; color: #0f172a; }\n"
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
    GtkWidget *cpi_dropdown;

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

static int find_nearest_octant_handle(double mx, double my, int width, int height) {
    double cx, cy, radius;
    get_radar_geometry(width, height, &cx, &cy, &radius);

    double dx = mx - cx;
    double dy = -(my - cy);
    double angle = atan2(dy, dx) * (180.0 / PI);
    if (angle < 0.0) angle += 360.0;

    int best_idx = 0;
    double min_diff = 360.0;
    for (int i = 0; i < 8; i++) {
        double diff = fabs(angle - OCTANT_INFO[i].angle_deg);
        if (diff > 180.0) diff = 360.0 - diff;
        if (diff < min_diff) {
            min_diff = diff;
            best_idx = i;
        }
    }

    float w = atomic_load_explicit(&g_state.octant_weights[best_idx], memory_order_relaxed);
    double hx, hy;
    octant_screen_pos(best_idx, w, cx, cy, radius, &hx, &hy);
    double dist_to_handle = hypot(mx - hx, my - hy);

    if (dist_to_handle < 26.0 || min_diff < 16.0) {
        return best_idx;
    }
    return -1;
}

/* Cairo Draw Callback for the Polar Anisotropic Sensitivity Map */
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
    if (g_ui.dragging_idx >= 0) {
        update_radar_from_pointer(start_x, start_y);
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
    for (int i = 0; i < 8; i++) {
        float w = atomic_load_explicit(&g_state.octant_weights[i], memory_order_relaxed);
        gtk_range_set_value(GTK_RANGE(g_ui.oct_scales[i]), w * 1000.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_ui.oct_spins[i]), w);

        int eff_dpi = (int)lroundf(w * (float)cpi);
        char dpi_buf[32];
        snprintf(dpi_buf, sizeof(dpi_buf), "%d DPI", eff_dpi);
        gtk_label_set_text(GTK_LABEL(g_ui.oct_dpi_lbls[i]), dpi_buf);
    }

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

static void refresh_hardware_widgets(void) {
    mouse_hw_status_t st;
    bool ok = hid_query_active_mouse(&st);

    if (!ok && st.node[0] == '\0') {
        gtk_label_set_text(GTK_LABEL(g_ui.lbl_mouse_ident), "🖱️ No OpenMouse HID device detected");
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

static void on_cpi_dropdown_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    (void)user_data;
    if (g_ui.updating_ui) return;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    if (sel < sizeof(CPI_OPTIONS) / sizeof(CPI_OPTIONS[0])) {
        int cpi = CPI_OPTIONS[sel];
        atomic_store_explicit(&g_state.hardware_cpi, cpi, memory_order_relaxed);
        if (hid_set_mouse_cpi(cpi)) {
            refresh_hardware_widgets();
        } else {
            sync_ui_from_state();
        }
    }
}

static void on_set_device_cpi_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    int cpi = GPOINTER_TO_INT(user_data);
    if (hid_set_mouse_cpi(cpi)) {
        refresh_hardware_widgets();
    }
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
    if (core_save_profile_json("profile.json")) {
        gtk_label_set_text(GTK_LABEL(g_ui.lbl_profile_status), "💾 Saved to profile.json");
    } else {
        gtk_label_set_text(GTK_LABEL(g_ui.lbl_profile_status), "❌ Failed to save profile.json");
    }
}

static void on_load_profile_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    if (core_load_profile_json("profile.json")) {
        gtk_label_set_text(GTK_LABEL(g_ui.lbl_profile_status), "📂 Loaded profile.json");
        sync_ui_from_state();
    } else {
        gtk_label_set_text(GTK_LABEL(g_ui.lbl_profile_status), "⚠️ No profile.json found");
    }
}

/* Periodic 100ms UI Refresh Timer (reads lock-free atomics & telemetry mutex) */
static gboolean on_ui_tick(gpointer user_data) {
    (void)user_data;
    if (!g_ui.window) return G_SOURCE_REMOVE;

    bool grabbed = atomic_load_explicit(&g_state.evdev_grabbed, memory_order_relaxed);
    uint64_t ticks = atomic_load_explicit(&g_state.total_ticks_processed, memory_order_relaxed);
    char rt_buf[160];
    if (grabbed) {
        snprintf(rt_buf, sizeof(rt_buf), "🟢 RT Engine: Grabbed (%llu ticks, In-Memory Atomic)",
                 (unsigned long long)ticks);
    } else {
        snprintf(rt_buf, sizeof(rt_buf), "🟡 RT Engine: Monitor Mode (%s)",
                 g_state.evdev_error[0] ? g_state.evdev_error : "run with sudo for EVIOCGRAB");
    }
    gtk_label_set_text(GTK_LABEL(g_ui.lbl_rt_status), rt_buf);

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
    gtk_window_set_title(GTK_WINDOW(win), "AnyMouse Vectoring — Unified C 8-Octant Polar DPI & FpsAimForge Telemetry Studio");
    gtk_window_set_default_size(GTK_WINDOW(win), 1160, 820);

    GtkWidget *root_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_margin_start(root_hbox, 14);
    gtk_widget_set_margin_end(root_hbox, 14);
    gtk_widget_set_margin_top(root_hbox, 14);
    gtk_widget_set_margin_bottom(root_hbox, 14);
    gtk_window_set_child(GTK_WINDOW(win), root_hbox);

    /* ================= LEFT COLUMN ================= */
    GtkWidget *left_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_hexpand(left_vbox, TRUE);
    gtk_box_append(GTK_BOX(root_hbox), left_vbox);

    /* 1. Hardware Mouse & Sensor Sync Frame */
    GtkWidget *hw_frame = gtk_frame_new("Hardware Mouse & Sensor Sync (OpenMouse HIDraw + RT Engine)");
    GtkWidget *hw_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_frame_set_child(GTK_FRAME(hw_frame), hw_vbox);
    gtk_box_append(GTK_BOX(left_vbox), hw_frame);

    g_ui.lbl_mouse_ident = gtk_label_new("🖱️ Detecting Mouse...");
    gtk_widget_set_halign(g_ui.lbl_mouse_ident, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(hw_vbox), g_ui.lbl_mouse_ident);

    g_ui.lbl_rt_status = gtk_label_new("🟢 RT Engine: Initializing...");
    gtk_widget_set_halign(g_ui.lbl_rt_status, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(hw_vbox), g_ui.lbl_rt_status);

    GtkWidget *badge_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_ui.badge_rate = gtk_label_new("⚡ Polling: -- Hz");
    gtk_widget_add_css_class(g_ui.badge_rate, "badge");
    gtk_widget_add_css_class(g_ui.badge_rate, "badge-cyan");
    gtk_box_append(GTK_BOX(badge_hbox), g_ui.badge_rate);

    g_ui.badge_bat = gtk_label_new("🔋 Battery: --%");
    gtk_widget_add_css_class(g_ui.badge_bat, "badge");
    gtk_widget_add_css_class(g_ui.badge_bat, "badge-green");
    gtk_box_append(GTK_BOX(badge_hbox), g_ui.badge_bat);

    g_ui.badge_sync = gtk_label_new("🎯 Motion Sync: --");
    gtk_widget_add_css_class(g_ui.badge_sync, "badge");
    gtk_widget_add_css_class(g_ui.badge_sync, "badge-pink");
    gtk_box_append(GTK_BOX(badge_hbox), g_ui.badge_sync);

    g_ui.badge_rssi = gtk_label_new("📶 Signal: -- dBm");
    gtk_widget_add_css_class(g_ui.badge_rssi, "badge");
    gtk_widget_add_css_class(g_ui.badge_rssi, "badge-amber");
    gtk_box_append(GTK_BOX(badge_hbox), g_ui.badge_rssi);
    gtk_box_append(GTK_BOX(hw_vbox), badge_hbox);

    GtkWidget *cpi_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(cpi_hbox), gtk_label_new("Locked Sensor CPI:"));
    g_ui.cpi_dropdown = gtk_drop_down_new_from_strings(CPI_STRINGS);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_ui.cpi_dropdown), 2); /* 3200 CPI */
    g_signal_connect(g_ui.cpi_dropdown, "notify::selected", G_CALLBACK(on_cpi_dropdown_changed), NULL);
    gtk_box_append(GTK_BOX(cpi_hbox), g_ui.cpi_dropdown);

    GtkWidget *btn_3200 = gtk_button_new_with_label("Set 3200 CPI");
    g_signal_connect(btn_3200, "clicked", G_CALLBACK(on_set_device_cpi_clicked), GINT_TO_POINTER(3200));
    gtk_box_append(GTK_BOX(cpi_hbox), btn_3200);

    GtkWidget *btn_6400 = gtk_button_new_with_label("Set 6400 CPI");
    g_signal_connect(btn_6400, "clicked", G_CALLBACK(on_set_device_cpi_clicked), GINT_TO_POINTER(6400));
    gtk_box_append(GTK_BOX(cpi_hbox), btn_6400);

    GtkWidget *btn_refresh = gtk_button_new_with_label("🔄 Refresh");
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh_hw_clicked), NULL);
    gtk_box_append(GTK_BOX(cpi_hbox), btn_refresh);
    gtk_box_append(GTK_BOX(hw_vbox), cpi_hbox);

    /* 2. Interactive Polar Radar Chart Frame */
    GtkWidget *radar_frame = gtk_frame_new("Polar Anisotropic Sensitivity Map (Drag Handles · Yellow=P₁, Green=Target T)");
    gtk_widget_set_vexpand(radar_frame, TRUE);
    g_ui.drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(g_ui.drawing_area, 400, 400);
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
    GtkWidget *preset_frame = gtk_frame_new("Quick Presets");
    GtkWidget *preset_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
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
        gtk_box_append(GTK_BOX(preset_hbox), b);
    }
    gtk_box_append(GTK_BOX(left_vbox), preset_frame);

    /* ================= RIGHT COLUMN ================= */
    GtkWidget *right_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_hexpand(right_vbox, TRUE);
    gtk_box_append(GTK_BOX(root_hbox), right_vbox);

    /* 4. FpsAimForge (FAF) Live Telemetry & Phase 2 Calibration Frame */
    GtkWidget *kov_frame = gtk_frame_new("Phase 2: FpsAimForge (FAF) Native Plugin Telemetry & Ballistic Auto-Calibration");
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

    GtkWidget *calib_ctrl_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_ui.chk_auto_calib = gtk_check_button_new_with_label("Inter-Round Auto-Adapt (α=0.35, ±5% clamp on Round End)");
    g_signal_connect(g_ui.chk_auto_calib, "toggled", G_CALLBACK(on_auto_calib_toggled), NULL);
    /* Reflect --auto-calibrate passed on the command line */
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_ui.chk_auto_calib),
                                atomic_load_explicit(&g_state.auto_calibrate, memory_order_relaxed));
    gtk_box_append(GTK_BOX(calib_ctrl_hbox), g_ui.chk_auto_calib);

    GtkWidget *btn_apply_calib = gtk_button_new_with_label("⚡ Apply Damped Update");
    gtk_widget_add_css_class(btn_apply_calib, "primary-btn");
    g_signal_connect(btn_apply_calib, "clicked", G_CALLBACK(on_apply_calib_now_clicked), NULL);
    gtk_box_append(GTK_BOX(calib_ctrl_hbox), btn_apply_calib);

    GtkWidget *btn_reset_calib = gtk_button_new_with_label("↺ Reset Stats");
    g_signal_connect(btn_reset_calib, "clicked", G_CALLBACK(on_reset_calib_clicked), NULL);
    gtk_box_append(GTK_BOX(calib_ctrl_hbox), btn_reset_calib);
    gtk_box_append(GTK_BOX(kov_vbox), calib_ctrl_hbox);

    /* 5. Master Vector Scaling Frame */
    GtkWidget *master_frame = gtk_frame_new("Master Vector Scaling & Symmetry");
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

    GtkWidget *sym_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    g_ui.chk_mirror_lr = gtk_check_button_new_with_label("Link Left/Right (E↔W, NE↔NW, SE↔SW)");
    g_signal_connect(g_ui.chk_mirror_lr, "toggled", G_CALLBACK(on_mirror_lr_toggled), NULL);
    gtk_box_append(GTK_BOX(sym_hbox), g_ui.chk_mirror_lr);

    g_ui.chk_mirror_ud = gtk_check_button_new_with_label("Link Up/Down (N↔S)");
    g_signal_connect(g_ui.chk_mirror_ud, "toggled", G_CALLBACK(on_mirror_ud_toggled), NULL);
    gtk_box_append(GTK_BOX(sym_hbox), g_ui.chk_mirror_ud);
    gtk_box_append(GTK_BOX(master_vbox), sym_hbox);
    gtk_box_append(GTK_BOX(right_vbox), master_frame);

    /* 6. Octant Fine Tuning Grid */
    GtkWidget *oct_frame = gtk_frame_new("Octant Fine Tuning & Live Ballistic Ratios");
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
    GtkWidget *bot_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *btn_save = gtk_button_new_with_label("💾 Save Profile (profile.json)");
    g_signal_connect(btn_save, "clicked", G_CALLBACK(on_save_profile_clicked), NULL);
    gtk_box_append(GTK_BOX(bot_hbox), btn_save);

    GtkWidget *btn_load = gtk_button_new_with_label("📂 Load Profile (profile.json)");
    g_signal_connect(btn_load, "clicked", G_CALLBACK(on_load_profile_clicked), NULL);
    gtk_box_append(GTK_BOX(bot_hbox), btn_load);

    g_ui.lbl_profile_status = gtk_label_new("⚡ Direct Shared-Memory Atomic Engine");
    gtk_widget_add_css_class(g_ui.lbl_profile_status, "badge-green");
    gtk_box_append(GTK_BOX(bot_hbox), g_ui.lbl_profile_status);
    gtk_box_append(GTK_BOX(right_vbox), bot_hbox);

    refresh_hardware_widgets();
    g_timeout_add(100, on_ui_tick, NULL);

    gtk_window_present(GTK_WINDOW(win));
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
