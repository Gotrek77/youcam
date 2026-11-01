/* YouCam entry point (main.swift).
 *
 * The windows run through XWayland: GNOME Wayland does not let an app keep a
 * window above others or place it, both essential for a floating camera.
 * Screen capture still goes through the portal, so Wayland apps are recorded.
 * A second launch reaches the running instance and brings the camera back. */

#include <stdlib.h>
#include <string.h>
#include "youcam.h"

static int SmokeTest;
static const char *SnapshotDir;

static void startup(GApplication *gapp, gpointer data) {
    (void)data;
    app_startup(GTK_APPLICATION(gapp), SmokeTest);
    if (SnapshotDir) app_snapshot(SnapshotDir);
}

static void activate(GApplication *gapp, gpointer data) {
    (void)gapp; (void)data;
    app_reopen();
}

int main(int argc, char **argv) {
    setenv("GDK_BACKEND", "x11", 0);
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--smoke-test") == 0) SmokeTest = 1;
        else if (strcmp(argv[i], "--snapshot") == 0 && i + 1 < argc) SnapshotDir = argv[++i];
    gst_init(NULL, NULL);
    GApplicationFlags flags = SmokeTest || SnapshotDir ? G_APPLICATION_NON_UNIQUE : G_APPLICATION_DEFAULT_FLAGS;
    GtkApplication *app = gtk_application_new(APP_ID, flags);
    g_signal_connect(app, "startup", G_CALLBACK(startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    char *args[] = {argv[0], NULL};
    int status = g_application_run(G_APPLICATION(app), 1, args);
    g_object_unref(app);
    return status;
}
