#include <gtk/gtk.h>
#include "motor_control.h"

#if USE_HARDWARE
#include <lgpio.h>
#endif

/* Sensor HIGH (about 3.3V) means a can is detected.
   GPIO 24 CANNOT be used: it controls the camera motor direction.
   GPIO 16 is also reserved by the motor driver (Motor 1 M0).
   Connect the sensor signal to GPIO 25 (physical pin 22) instead. */
#define CAN_SENSOR_PIN 25
static int sensor_handle = -1;
static int last_can_state = -1; /* -1 = not read yet */

typedef struct {
    GtkWidget *window, *speed, *speed_label, *conveyor_steps, *conveyor_steps_label;
    GtkWidget *mount_steps, *mount_steps_label, *conveyor_status, *mount_status;
    GtkWidget *up, *down, *left, *right, *center, *auto_mode;
    GtkWidget *preset_status, *sensor_label, *yellow_auto_button;
    GtkWidget *output_buttons[4];
    gboolean output_on[4], output_known[4];
    gboolean yellow_auto;
} UI;

/* Output modules on this Pi are active HIGH: HIGH = ON, LOW = OFF. */
/* Output order: red, yellow, green, valve (BCM pins 26, 6, 5, 23). */
static const int output_pins[4] = {26, 6, 5, 23};
static const char *output_names[4] = {"RED", "YELLOW", "GREEN", "VALVE"};
static const char *output_styles[4] = {"red-output", "yellow-output", "green-output", "valve-output"};

static gboolean output_write(int pin, gboolean on) {
    char number[12];
    g_snprintf(number, sizeof number, "%d", pin);
    /* HIGH turns output ON; LOW turns it OFF. */
    gchar *argv[] = {"pinctrl", "set", number, "op", on ? "dh" : "dl", NULL};
    GError *error = NULL;
    gint status = -1;
    gboolean launched = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                                      NULL, NULL, NULL, NULL, &status, &error);
    if (!launched || !g_spawn_check_wait_status(status, &error)) {
        g_warning("GPIO %d operation failed: %s", pin, error ? error->message : "unknown error");
        g_clear_error(&error);
        return FALSE;
    }
    return TRUE;
}

static void output_update(UI *u) {
    for (int i = 0; i < 4; i++) {
        if (u->output_buttons[i]) {
            char title[64];
            g_snprintf(title, sizeof title, "%s\n%s", output_names[i],
                       !u->output_known[i] ? "--" : u->output_on[i] ? "ON" : "OFF");
            gtk_button_set_label(GTK_BUTTON(u->output_buttons[i]), title);
            gtk_widget_add_css_class(u->output_buttons[i], output_styles[i]);
            if (u->output_known[i] && u->output_on[i])
                gtk_widget_add_css_class(u->output_buttons[i], "output-on");
            else
                gtk_widget_remove_css_class(u->output_buttons[i], "output-on");
        }
    }
}

static void output_clicked(GtkButton *button, gpointer data) {
    UI *u = data;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "gpio-index"));
    /* Yellow is manual when automatic sensor control is OFF. */
    if (index == 1 && u->yellow_auto) return;
    gboolean next = !u->output_on[index];
    if (output_write(output_pins[index], next)) { u->output_on[index] = next; u->output_known[index] = TRUE; }
    output_update(u);
}

/* Toggle automatic yellow-light control; manual control works when OFF. */
static void yellow_auto_clicked(GtkButton *button, gpointer data) {
    UI *u = data;
    u->yellow_auto = !u->yellow_auto;
    gtk_button_set_label(button, u->yellow_auto ? "AUTO: ON" : "AUTO: OFF");
    if (u->yellow_auto) gtk_widget_add_css_class(GTK_WIDGET(button), "auto-on");
    else gtk_widget_remove_css_class(GTK_WIDGET(button), "auto-on");
    gtk_widget_set_sensitive(u->output_buttons[1], !u->yellow_auto);
    /* Synchronize immediately on enabling; leave yellow as-is when disabling. */
    if (u->yellow_auto) last_can_state = -1;
}

static void output_all_off(UI *u) {
    for (int i = 0; i < 4; i++) {
        if (output_write(output_pins[i], FALSE)) { u->output_on[i] = FALSE; u->output_known[i] = TRUE; }
    }
    output_update(u);
}

static int speed_value(UI *u) {
    return 53 + (int)(gtk_range_get_value(GTK_RANGE(u->speed)) * 30.0 / 100.0 + 0.5);
}
static int conveyor_distance(UI *u) { return (int)gtk_range_get_value(GTK_RANGE(u->conveyor_steps)); }
static int mount_distance(UI *u) { return (int)gtk_range_get_value(GTK_RANGE(u->mount_steps)); }

/* Pressing the currently active command again stops only its motor. */
static void forward_clicked(GtkButton *b, gpointer d) {
    (void)b; UI *u=d; MotorStatus s=motor_status();
    if (s.conveyor_running && !s.conveyor_oscillating && !s.conveyor_continuous && s.conveyor_direction>0)
        motor_conveyor_stop();
    else motor_conveyor_start(1,speed_value(u),conveyor_distance(u));
}
static void reverse_clicked(GtkButton *b, gpointer d) {
    (void)b; UI *u=d; MotorStatus s=motor_status();
    if (s.conveyor_running && !s.conveyor_oscillating && !s.conveyor_continuous && s.conveyor_direction<0)
        motor_conveyor_stop();
    else motor_conveyor_start(-1,speed_value(u),conveyor_distance(u));
}
static void back_forth_clicked(GtkButton *b, gpointer d) {
    (void)b; UI *u=d; MotorStatus s=motor_status();
    if (s.conveyor_running && (s.conveyor_oscillating || s.conveyor_continuous)) {
        motor_conveyor_stop();
        return;
    }
    switch (gtk_drop_down_get_selected(GTK_DROP_DOWN(u->auto_mode))) {
        case 0: motor_conveyor_run_forever(1, speed_value(u)); break;
        case 1: motor_conveyor_run_forever(-1, speed_value(u)); break;
        default: motor_conveyor_back_forth(conveyor_distance(u), speed_value(u)); break;
    }
}
static void mount_up_clicked(GtkButton *b, gpointer d) {
    (void)b; UI *u=d; MotorStatus s=motor_status();
    if (s.mount_running && s.mount_direction>0) motor_mount_stop();
    else motor_mount_move(1,mount_distance(u));
}
static void mount_down_clicked(GtkButton *b, gpointer d) {
    (void)b; UI *u=d; MotorStatus s=motor_status();
    if (s.mount_running && s.mount_direction<0) motor_mount_stop();
    else motor_mount_move(-1,mount_distance(u));
}
static void set_mount_reference_clicked(GtkButton *b, gpointer d) {
    (void)b; UI *u=d;
    MotorStatus s=motor_status();
    if (!s.mount_running) {
        motor_mount_set_reference();
        gtk_label_set_text(GTK_LABEL(u->preset_status),"12oz reference saved · Camera at 0 steps");
    } else {
        gtk_label_set_text(GTK_LABEL(u->preset_status),"Stop camera before calibrating");
    }
}
static void preset_clicked(GtkButton *b, gpointer d) {
    UI *u=d;
    int target=GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b),"target"));
    if (!motor_mount_goto_position(target)) {
        gtk_label_set_text(GTK_LABEL(u->preset_status),"Set 12oz Position first");
    }
}
static void stop_all_clicked(GtkButton *b, gpointer d) {
    (void)b; (void)d; motor_stop_all();
}
static void speed_changed(GtkRange *r, gpointer d) {
    UI *u=d; char label[24]; MotorStatus s=motor_status();
    g_snprintf(label,sizeof label,"%.0f%%",gtk_range_get_value(r));
    gtk_label_set_text(GTK_LABEL(u->speed_label),label);
    motor_conveyor_settings(s.conveyor_direction,speed_value(u));
}
static void conveyor_steps_changed(GtkRange *r, gpointer d) {
    UI *u=d; char label[32];
    g_snprintf(label,sizeof label,"%.0f steps",gtk_range_get_value(r));
    gtk_label_set_text(GTK_LABEL(u->conveyor_steps_label),label);
}
static void mount_steps_changed(GtkRange *r, gpointer d) {
    UI *u=d; char label[32];
    g_snprintf(label,sizeof label,"%.0f steps",gtk_range_get_value(r));
    gtk_label_set_text(GTK_LABEL(u->mount_steps_label),label);
}
static void set_active(GtkWidget *button, gboolean active) {
    if (active) gtk_widget_add_css_class(button,"active-motion");
    else gtk_widget_remove_css_class(button,"active-motion");
}
static void sensor_update(UI *u) {
#if USE_HARDWARE
    int level = sensor_handle >= 0 ? lgGpioRead(sensor_handle, CAN_SENSOR_PIN) : -1;
    if (level < 0) {
        gtk_label_set_text(GTK_LABEL(u->sensor_label), "Sensor unavailable");
        gtk_widget_remove_css_class(u->sensor_label, "can-detected");
        return;
    }
    gboolean detected = (level == 0); /* Existing sensor wiring is active-low. */
    gtk_label_set_text(GTK_LABEL(u->sensor_label), detected ? "● Can detected" : "○ No can");
    if (detected) gtk_widget_add_css_class(u->sensor_label, "can-detected");
    else gtk_widget_remove_css_class(u->sensor_label, "can-detected");

    /* Sensor controls yellow only while AUTO is enabled. */
    if (u->yellow_auto && last_can_state != (int)detected) {
        last_can_state = detected;
        if (output_write(output_pins[1], detected)) {
            u->output_on[1] = detected;
            u->output_known[1] = TRUE;
            output_update(u);
        }
    }

#else
    gtk_label_set_text(GTK_LABEL(u->sensor_label), "Sensor: simulation");
#endif
}

static gboolean refresh(gpointer d) {
    UI *u=d; MotorStatus s=motor_status(); char label[192];
    sensor_update(u);
    gboolean f=s.conveyor_running && !s.conveyor_oscillating && !s.conveyor_continuous && s.conveyor_direction>0;
    gboolean r=s.conveyor_running && !s.conveyor_oscillating && !s.conveyor_continuous && s.conveyor_direction<0;
    gboolean a=s.conveyor_running && (s.conveyor_oscillating || s.conveyor_continuous);
    gboolean up=s.mount_running && s.mount_direction>0;
    gboolean down=s.mount_running && s.mount_direction<0;
    set_active(u->left,r); set_active(u->right,f); set_active(u->center,a);
    set_active(u->up,up); set_active(u->down,down);
    g_snprintf(label,sizeof label,"◀ REVERSE\nSteps %d / %d",
        r?s.conveyor_steps_done:0,r?s.conveyor_steps_requested:conveyor_distance(u));
    gtk_button_set_label(GTK_BUTTON(u->left),label);
    g_snprintf(label,sizeof label,"FORWARD ▶\nSteps %d / %d",
        f?s.conveyor_steps_done:0,f?s.conveyor_steps_requested:conveyor_distance(u));
    gtk_button_set_label(GTK_BUTTON(u->right),label);
    if (a && s.conveyor_continuous)
        g_snprintf(label,sizeof label,"AUTO %s\nCONTINUOUS",s.conveyor_direction>0 ? "▶" : "◀");
    else
        g_snprintf(label,sizeof label,"⇄ AUTO\nSteps %d / %d",
            a?s.conveyor_leg_steps_done:0,a?s.conveyor_oscillation_steps:conveyor_distance(u));
    gtk_button_set_label(GTK_BUTTON(u->center),label);
    g_snprintf(label,sizeof label,"▲ CAMERA UP\nSteps %d / %d",
        up?s.mount_steps_requested-s.mount_steps_remaining:0,
        up?s.mount_steps_requested:mount_distance(u));
    gtk_button_set_label(GTK_BUTTON(u->up),label);
    g_snprintf(label,sizeof label,"▼ CAMERA DOWN\nSteps %d / %d",
        down?s.mount_steps_requested-s.mount_steps_remaining:0,
        down?s.mount_steps_requested:mount_distance(u));
    gtk_button_set_label(GTK_BUTTON(u->down),label);
    g_snprintf(label,sizeof label,"Conveyor: %s",
        !s.conveyor_running ? "Stopped" : s.conveyor_oscillating ?
        (s.conveyor_direction>0 ? "Auto forward →" : "Auto reverse ←") :
        s.conveyor_continuous ? (s.conveyor_direction>0 ? "Continuous forward →" : "Continuous reverse ←") :
        (s.conveyor_direction>0 ? "Forward →" : "Reverse ←"));
    gtk_label_set_text(GTK_LABEL(u->conveyor_status),label);
    gtk_label_set_text(GTK_LABEL(u->mount_status), !s.mount_running ? "Camera: Stopped" :
        (s.mount_direction>0 ? "Camera: Up ↑" : "Camera: Down ↓"));
    return G_SOURCE_CONTINUE;
}
static gboolean closing(GtkWindow *window,gpointer d) {
    UI *u=d;
    output_all_off(u);
    guint id=GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(window),"refresh-id"));
    if(id) g_source_remove(id);
    motor_stop_all();
    return FALSE;
}
static GtkWidget *control_button(const char *name,const char *style) {
    GtkWidget *b=gtk_button_new_with_label(name);
    gtk_widget_add_css_class(b,"direction");
    gtk_widget_add_css_class(b,style);
    gtk_widget_set_size_request(b,176,176);
    gtk_widget_set_hexpand(b,FALSE);
    return b;
}
static void slider_row(GtkWidget *root,const char *caption,GtkWidget **range,
                       GtkWidget **value,int min,int max,int step,int initial) {
    GtkWidget *row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,10);
    gtk_box_append(GTK_BOX(root),row);
    GtkWidget *title=gtk_label_new(caption);
    gtk_widget_set_size_request(title,132,-1);
    gtk_widget_set_halign(title,GTK_ALIGN_START);
    gtk_widget_set_hexpand(title,FALSE);
    gtk_box_append(GTK_BOX(row),title);
    *range=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,min,max,step);
    gtk_scale_set_draw_value(GTK_SCALE(*range),FALSE);
    gtk_widget_set_hexpand(*range,TRUE);
    gtk_range_set_value(GTK_RANGE(*range),initial);
    gtk_box_append(GTK_BOX(row),*range);
    *value=gtk_label_new("");
    gtk_widget_set_size_request(*value,82,-1);
    gtk_widget_set_halign(*value,GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(row),*value);
}
static void activate(GtkApplication *app,gpointer ignored) {
    (void)ignored;
    UI *u=g_new0(UI,1);
    u->window=gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(u->window),"Conveyor + Camera Controller");
    gtk_window_set_default_size(GTK_WINDOW(u->window),985,-1);
    GtkCssProvider *css=gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "window { font-size: 17px; }"
        ".heading { font-size: 20px; font-weight: 750; }"
        ".direction { font-weight: 750; border-radius: 0px; padding: 7px; }"
        ".conveyor { background: #8653b3; color: white; }"
        ".mount { background: #198754; color: white; }"
        ".center { background: #495e7c; color: white; }"
        ".direction.active-motion { background: #e88e8e; color: #251919; border: 2px solid #f0bbbb; }"
        ".direction.center.active-motion { background: #2867ad; color: white; border: 2px solid #6195cd; }"
        ".stop { background: #d64747; color: white; font-weight: 750; border-radius: 3px; }"
        ".status { font-weight: 600; }"
        ".note { color: #a4a4a4; }"
        ".gpio-button { font-weight: 750; border-radius: 0px; border: 1px solid #5a5a66; }"
        ".preset-button { background: #49454c; color: #ededed; border-radius: 0px; }"
        ".gpio-button.red-output { background: #594548; color: #ededed; }"
        ".gpio-button.green-output { background: #414a46; color: #ededed; }"
        ".gpio-button.yellow-output { background: #514f45; color: #ededed; }"
        ".gpio-button.valve-output { background: #49454c; color: #ededed; }"
        ".gpio-button.red-output.output-on { background: #d64747; color: white; }"
        ".gpio-button.green-output.output-on { background: #198754; color: white; }"
        ".gpio-button.yellow-output.output-on { background: #f3cc18; color: #261e00; }"
        ".gpio-button.valve-output.output-on { background: #8653b3; color: white; }"
        ".sensor-label { font-size: 20px; font-weight: 750; padding: 10px 4px; color: #c6cbd4; }"
        ".sensor-label.can-detected { color: #f3cc18; }"
        ".mini-toggle { border-radius: 3px; min-height: 30px; }"
        ".mini-toggle.auto-on { background: #e0a915; color: #1d1d1d; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    GtkWidget *columns=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,20);
    gtk_widget_set_margin_top(columns,20); gtk_widget_set_margin_bottom(columns,20);
    gtk_widget_set_margin_start(columns,20); gtk_widget_set_margin_end(columns,20);
    gtk_window_set_child(GTK_WINDOW(u->window),columns);
    GtkWidget *root=gtk_box_new(GTK_ORIENTATION_VERTICAL,3);
    gtk_widget_set_hexpand(root, TRUE);
    gtk_box_append(GTK_BOX(columns),root);
    GtkWidget *side=gtk_box_new(GTK_ORIENTATION_VERTICAL,3);
    gtk_widget_set_size_request(side,176,-1);
    gtk_box_append(GTK_BOX(columns),side);
    GtkWidget *title=gtk_label_new("MOTOR CONTROLS");
    gtk_widget_add_css_class(title,"heading");
    gtk_box_append(GTK_BOX(root),title);
    slider_row(root,"Conveyor speed",&u->speed,&u->speed_label,0,100,1,80);
    slider_row(root,"Conveyor move",&u->conveyor_steps,&u->conveyor_steps_label,500,9000,100,5000);
    slider_row(root,"Camera move",&u->mount_steps,&u->mount_steps_label,10,5000,10,1000);
    GtkWidget *auto_row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12);
    gtk_widget_set_margin_top(auto_row,8);
    gtk_box_append(GTK_BOX(root),auto_row);
    gtk_box_append(GTK_BOX(auto_row),gtk_label_new("AUTO mode"));
    const char *modes[]={"Forward forever", "Reverse forever", "Back & forth forever", NULL};
    u->auto_mode=gtk_drop_down_new_from_strings(modes);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(u->auto_mode),2);
    gtk_widget_set_hexpand(u->auto_mode,TRUE);
    gtk_box_append(GTK_BOX(auto_row),u->auto_mode);
    GtkWidget *grid=gtk_grid_new();
    gtk_widget_set_margin_top(grid,28);
    gtk_grid_set_row_spacing(GTK_GRID(grid),2);
    gtk_grid_set_column_spacing(GTK_GRID(grid),2);
    gtk_widget_set_halign(grid,GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root),grid);
    u->up=control_button("▲  CAMERA UP","mount");
    u->down=control_button("▼  CAMERA DOWN","mount");
    u->left=control_button("◀  REVERSE","conveyor");
    u->right=control_button("FORWARD  ▶","conveyor");
    u->center=control_button("⇄  AUTO","center");
    gtk_grid_attach(GTK_GRID(grid),u->up,1,0,1,1);
    gtk_grid_attach(GTK_GRID(grid),u->left,0,1,1,1);
    gtk_grid_attach(GTK_GRID(grid),u->center,1,1,1,1);
    gtk_grid_attach(GTK_GRID(grid),u->right,2,1,1,1);
    gtk_grid_attach(GTK_GRID(grid),u->down,1,2,1,1);
    g_signal_connect(u->up,"clicked",G_CALLBACK(mount_up_clicked),u);
    g_signal_connect(u->down,"clicked",G_CALLBACK(mount_down_clicked),u);
    g_signal_connect(u->left,"clicked",G_CALLBACK(reverse_clicked),u);
    g_signal_connect(u->right,"clicked",G_CALLBACK(forward_clicked),u);
    g_signal_connect(u->center,"clicked",G_CALLBACK(back_forth_clicked),u);
    GtkWidget *stop_all=gtk_button_new_with_label("■ STOP ALL");
    gtk_widget_add_css_class(stop_all,"stop");
    gtk_widget_set_size_request(stop_all,176,176);
    gtk_grid_attach(GTK_GRID(grid),stop_all,2,2,1,1);
    GtkWidget *preset_title=gtk_label_new("CAMERA PRESETS");
    gtk_widget_add_css_class(preset_title,"heading");
    gtk_widget_set_margin_top(preset_title,44);
    gtk_widget_set_margin_bottom(preset_title,14);
    gtk_box_append(GTK_BOX(root),preset_title);
    GtkWidget *presets=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
    gtk_box_set_homogeneous(GTK_BOX(presets),TRUE);
    gtk_widget_set_margin_bottom(presets,10);
    gtk_box_append(GTK_BOX(root),presets);
    const char *preset_names[]={"12oz", "16oz", "19.2oz"};
    for (int i=0;i<3;i++) {
        GtkWidget *pb=gtk_button_new_with_label(preset_names[i]);
        gtk_widget_set_size_request(pb,-1,45);
        gtk_widget_add_css_class(pb,"preset-button");
        g_object_set_data(G_OBJECT(pb),"target",GINT_TO_POINTER(i*3300));
        g_signal_connect(pb,"clicked",G_CALLBACK(preset_clicked),u);
        gtk_box_append(GTK_BOX(presets),pb);
    }
    GtkWidget *reference=gtk_button_new_with_label("Set 12oz reference");
    gtk_widget_set_size_request(reference,-1,45);
    gtk_widget_add_css_class(reference,"preset-button");
    gtk_widget_set_margin_bottom(reference,8);
    gtk_box_append(GTK_BOX(root),reference);
    g_signal_connect(reference,"clicked",G_CALLBACK(set_mount_reference_clicked),u);
    u->preset_status=gtk_label_new("Calibrate 12oz height before using presets");
    gtk_widget_add_css_class(u->preset_status,"note");
    gtk_box_append(GTK_BOX(root),u->preset_status);
    g_signal_connect(stop_all,"clicked",G_CALLBACK(stop_all_clicked),u);
    u->conveyor_status=gtk_label_new("Conveyor: Stopped");
    u->mount_status=gtk_label_new("Camera: Stopped");
    gtk_widget_set_margin_top(u->mount_status,8);
    GtkWidget *states[]={u->conveyor_status,u->mount_status};
    for (int i=0;i<2;i++) {
        gtk_widget_add_css_class(states[i],"status");
        gtk_widget_set_size_request(states[i],-1,26);
        gtk_widget_set_valign(states[i],GTK_ALIGN_CENTER);
        gtk_widget_set_halign(states[i],GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(root),states[i]);
    }
    /* Sensor first; the panel reserves space for the motor sliders. */
    GtkWidget *sensor_panel=gtk_box_new(GTK_ORIENTATION_VERTICAL,3);
    gtk_widget_set_size_request(sensor_panel,-1,108);
    gtk_widget_set_valign(sensor_panel,GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(side),sensor_panel);
    GtkWidget *sensor_title=gtk_label_new("SENSOR");
    gtk_widget_add_css_class(sensor_title,"heading");
    gtk_box_append(GTK_BOX(sensor_panel),sensor_title);
    GtkWidget *sensor_pin=gtk_label_new("GPIO 25");
    gtk_widget_add_css_class(sensor_pin,"note");
    gtk_box_append(GTK_BOX(sensor_panel),sensor_pin);
    u->sensor_label=gtk_label_new("Sensor unavailable");
    gtk_widget_add_css_class(u->sensor_label,"sensor-label");
    gtk_box_append(GTK_BOX(sensor_panel),u->sensor_label);

    GtkWidget *light_header=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,4);
    gtk_widget_set_margin_top(light_header,28);
    gtk_box_append(GTK_BOX(side),light_header);
    GtkWidget *output_title=gtk_label_new("LIGHTS");
    gtk_widget_add_css_class(output_title,"heading");
    gtk_widget_set_hexpand(output_title,TRUE);
    gtk_box_append(GTK_BOX(light_header),output_title);
    u->yellow_auto_button = gtk_button_new_with_label("AUTO: OFF");
    gtk_widget_add_css_class(u->yellow_auto_button, "mini-toggle");
    gtk_widget_set_size_request(u->yellow_auto_button, 86, 32);
    gtk_box_append(GTK_BOX(light_header), u->yellow_auto_button);
    g_signal_connect(u->yellow_auto_button, "clicked", G_CALLBACK(yellow_auto_clicked), u);
    GtkWidget *outputs=gtk_box_new(GTK_ORIENTATION_VERTICAL,2);
    gtk_box_append(GTK_BOX(side),outputs);
    for (int i=0;i<4;i++) {
        GtkWidget *pb=gtk_button_new();
        gtk_widget_add_css_class(pb,"gpio-button");
        gtk_widget_set_size_request(pb,176,176);
        gtk_widget_set_halign(pb,GTK_ALIGN_CENTER);
        g_object_set_data(G_OBJECT(pb),"gpio-index",GINT_TO_POINTER(i));
        g_signal_connect(pb,"clicked",G_CALLBACK(output_clicked),u);
        gtk_box_append(GTK_BOX(outputs),pb);
        u->output_buttons[i]=pb;
        /* GPIO outputs are commanded OFF before the window opens. */
        u->output_known[i]=TRUE;
        u->output_on[i]=FALSE;
    }
    output_update(u);
    g_signal_connect(u->speed,"value-changed",G_CALLBACK(speed_changed),u);
    g_signal_connect(u->conveyor_steps,"value-changed",G_CALLBACK(conveyor_steps_changed),u);
    g_signal_connect(u->mount_steps,"value-changed",G_CALLBACK(mount_steps_changed),u);
    speed_changed(GTK_RANGE(u->speed),u);
    conveyor_steps_changed(GTK_RANGE(u->conveyor_steps),u);
    mount_steps_changed(GTK_RANGE(u->mount_steps),u);
    guint id=g_timeout_add(100,refresh,u);
    g_object_set_data(G_OBJECT(u->window),"refresh-id",GUINT_TO_POINTER(id));
    g_object_set_data_full(G_OBJECT(u->window),"ui",u,g_free);
    g_signal_connect(u->window,"close-request",G_CALLBACK(closing),u);
    refresh(u);
    gtk_window_present(GTK_WINDOW(u->window));
}
int main(int argc,char **argv) {
    if (!motor_init()) {
        g_printerr("Motor initialization failed.\n");
        return 1;
    }
#if USE_HARDWARE
    /* Match the OFF labels to actual output levels at launch. */
    for (int i = 0; i < 4; i++)
        output_write(output_pins[i], FALSE);
#endif
#if USE_HARDWARE
    sensor_handle = lgGpiochipOpen(0);  /* Raspberry Pi 5: RP1 GPIO controller */
    if (sensor_handle >= 0) {
        int result = lgGpioClaimInput(sensor_handle, 0, CAN_SENSOR_PIN);
        if (result < 0) {
            g_printerr("Cannot claim sensor GPIO %d (error %d).\n", CAN_SENSOR_PIN, result);
            lgGpiochipClose(sensor_handle);
            sensor_handle = -1;
        }
    } else {
        g_printerr("Cannot open GPIO chip for can sensor.\n");
    }
#endif
    GtkApplication *app=gtk_application_new("org.example.motorcontrol",G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app,"activate",G_CALLBACK(activate),NULL);
    int result=g_application_run(G_APPLICATION(app),argc,argv);
    g_object_unref(app);
#if USE_HARDWARE
    if (sensor_handle >= 0) lgGpiochipClose(sensor_handle);
#endif
    motor_shutdown();
    return result;
}
