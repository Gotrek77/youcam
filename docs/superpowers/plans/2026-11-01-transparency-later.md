# Choose Camera Transparency After Recording Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the user record first and pick the camera transparency of the saved video afterwards, with a live preview: 0–80% with the YouCam Capture Exclusion extension, 0 to the preview transparency without it.

**Architecture:** A `later` recording writes a *project* MKV to `~/.local/share/youcam/pending/`: the screen track as today (H.264), the camera layer drawn by `layer_tick` at screen size on its own lossless track (FFV1, BGRA), and the microphone (AAC). The project settings (`T_A`, preview transparency, final container, audio present) are stored in the MKV header. When the recording stops, `later_dialog.c` shows two stills from the middle of the recording and blends them with cairo as the slider moves; Save runs `export.c`, which composites the layer over the decoded screen with opacity `α = 1 − t / T_A`, re-encodes with the same encoder chain as `writer.c`, copies the AAC, and writes `<name>.<ext>` on the Desktop.

**Tech Stack:** C99, GTK 3, cairo, GStreamer 1.28 (core, base, good, ugly, libav: `avenc_ffv1`, `avdec_ffv1`, `avdec_h264`, `avenc_aac` already used), GstPbutils (already linked).

**Spec:** `docs/superpowers/specs/2026-11-01-transparency-later-design.md`

## Global Constraints

- Branch `transparency-later`; all code in `linux/`; the Swift sources stay untouched.
- No new runtime dependency: gst-libav is already required (AAC). Availability of the feature = `avenc_ffv1 && avdec_ffv1 && avdec_h264` factories (`export_available()`).
- Lossless layer codec: **FFV1** (`avenc_ffv1` / `avdec_ffv1`), fed BGRA directly; it keeps alpha as `pix_fmt=bgra`. QTRLE is rejected: it cannot be muxed into `matroskamux`.
- Encoder choice (VA → NVENC → x264), its bitrate and the container muxers are shared by `writer.c` and `export.c` through `video_encoder_chain()` and `container_for()`; nothing is duplicated.
- Preference `savedTransparency` = `preview` | `opaque` | `later` replaces `recordCameraTransparency`: `true` → `preview`, `false` → `opaque`, missing → `opaque`, unknown → `opaque` (today's default is opaque). In C: `saved_transparency Prefs.saved_transparency`.
- CFLAGS stay `-std=c99 -D_DEFAULT_SOURCE -Wall -Wextra`; zero warnings. Declarations in `linux/youcam.h`, one section per file.
- Comments are antirez-style `/* */` and explain why. No `ponytail:` tags.
- Test counts today: `Passed 265 logic checks.`, `Passed 53 segmentation checks.`, `media ok: …`. Each task states the new counts.
- Privacy: recordings of the real screen show the user's screen and face. Implementers never open, decode to images, or view frames of a real recording; they only read metadata (`ffprobe` stream/format fields, sizes, times) and delete the files right after measuring. Synthetic test media (videotestsrc) may be viewed. The visual check of real recordings is done by the controller only (Task 8).
- `~/.config/youcam/prefs.ini` is backed up before and restored (verified with `cmp`) after any step that may change it.
- Commits: `<type>: <description>`, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Never push.

## Verified on this machine (spikes, 2026-11-01, GStreamer 1.28.8)

Throwaway programs in `$SPIKE` (below). Results the steps depend on:

1. **FFV1 in MKV with H.264 + AAC:** `matroskamux` takes `avenc_ffv1` fed BGRA; `ffprobe` reports `ffv1 pix_fmt=bgra`. A variable frame rate layer (`framerate=0/1`, as the writer's `appsrc`) muxes and exports fine. (FFV1 size/CPU at 2560x1600 measured earlier by the controller: ~6 Mbit/s worst case, mostly transparent, faster than real time.)
2. **Tags:** a custom registered tag and `GST_TAG_EXTENDED_COMMENT` are **not** written by `matroskamux`. `GST_TAG_COMMENT` and `GST_TAG_TITLE` are written, but only at EOS: a file whose writer is killed (`_exit` without EOS) has **no** tags. The `matroskamux` property `writing-app` goes into the Segment Info in the header and is on disk in both the clean and the killed file (found at byte 288 by a plain byte search). `matroskademux` does not expose WritingApp as a tag (`gst-discoverer -v` shows no application name), so it is read from the first bytes of the file. `taginject` stream titles do not change MKV track names.
3. **Demuxer pads:** `matroskademux` names the tracks `video_0` (H.264, stream id `…/001…`), `video_1` (FFV1, `…/002…`), `audio_0` (`…/003…`) in the order the writer links them to the muxer.
4. **Compositor alpha:** `compositor` with `sink_1::alpha=a` over a decoded H.264 screen, layer = opaque red square on transparent BGRA from FFV1: centre pixel = `0 0 252` (blue) at a = 0, `125 0 126` at a = 0.5, `252 0 0` at a = 1; outside the square the screen stays `0 0 252` for every a.
5. **Export into each container** with `aacparse` audio passthrough: `qtmux`, `mp4mux`, `matroskamux`, `avimux` all produce H.264 + AAC files readable by `ffprobe`, same video duration as the project. (A first run "failed" with not-linked only because zsh does not word-split `set -- $m`, so the muxer argument was `"qtmux mov"`: harness bug, not a pipeline problem.)
6. **Progress:** `gst_element_query_position(compositor, TIME)` advances to the end (0.73 s … 3.00 s); `gst_element_query_duration(matroskademux, TIME)` gives the project duration (3.00 s). On a killed file the duration is a guess (3333 s for 85 s of content), so progress there stays low until the end; export still completes (85.6 s out of 85.6 s, video and audio).
7. **Still at the middle:** with both video tracks linked in one pipeline, a `FLUSH|ACCURATE` seek to duration/2 gave a gray screen frame (the demuxer seeks by the FFV1 track's index, mid-GOP for H.264); `KEY_UNIT|SNAP_BEFORE` misaligned the two tracks. **One pipeline per track, the other tracks left unlinked**, gives the exact middle frame of both (screen and layer both at 1.50 s, correct colours, 141 ms in total). On a killed file the seek to the guessed middle yields no sample; seeking to 0 gives the first frame (54 ms). `avdec_h264 output-corrupt=false` is kept so a damaged GOP is skipped, never shown gray.
8. **Synthetic project from the shell** (for the dialog check in Task 5): `matroskamux writing-app="YouCam project;…"` in `gst-launch-1.0` writes the same header field.

## Deviations from the spec (deliberate, smaller)

- **Settings are not stored as named MKV tags.** Custom tag names are dropped by `matroskamux`, and the standard tags are written only when the file is finished, so a killed recording (which the spec requires to stay exportable) would lose `T_A`. The three values, named as in the spec, are written as one `key=value;` string `YouCam project;YOUCAM_T_A=1.000;YOUCAM_PREVIEW_TRANSPARENCY=0.300;YOUCAM_CONTAINER=mov;YOUCAM_AUDIO=1;` into the MKV header field WritingApp (`writing-app`), which is read back by a bounded byte search in the first 16 KiB; the same string also goes into `GST_TAG_COMMENT` (visible to ffprobe and players). A fourth key `YOUCAM_AUDIO` tells the export whether to link an audio branch (linking a missing track would make the muxer wait forever).
- **The dialog never builds still B.** Spec: B = layer over A, shown over A with opacity α. That equals painting the layer itself with opacity α over A, which is what `draw_preview` does; same pixels, one surface less.
- **Preview transparency is locked during a `later` recording without the extension.** The screen track then contains the translucent preview, and the export formula needs one `T_A` for the whole file, so the Transparency menu shows the value as a disabled line instead of the slider until the recording stops.
- **"The usual Saved … notification"** does not exist as a desktop notification in the Linux port: success shows as the status line "Saved to Desktop" (menu, panel, tray tooltip) plus "Show Last Recording in Files", which `recording_set_last_path()` drives. A `later` stop clears the last path, so the status says "Ready to record" until the export succeeds.
- **`.part` files:** the export writes `<name>.<ext>.part` on the Desktop (spec) and deletes it itself on failure or cancel; the start-up sweep touches only `pending/` (spec), where nothing normally writes a `.part`. A `.part` left on the Desktop by a crash during an export is left alone, as the spec forbids touching the Desktop.
- **Default for a missing or unknown key is `opaque`**, today's default (the check item starts unchecked; see `tests.c` "the saved-video option is off by default" and README). The spec was corrected to match (controller ruling, 2026-11-01).
- **Closing the dialog while saving cancels the export** (the `.part` is deleted, the project kept). The spec does not say; keeping a hidden export running would be surprising.
- **One dialog at a time.** A `later` recording that stops while the dialog is open replaces the dialog's content (the older project stays in `pending/`), unless the dialog is saving; then the new project waits for the menu item.
- **Alpha of antialiased edges:** the layer pixels are cairo's premultiplied ARGB, which the compositor reads as straight BGRA, exactly as today's composite path does; the outermost edge pixels are a shade darker in the export than in the dialog preview. Unchanged behaviour, not fixed here.

## Paths used below

- Repository: `/home/patane/Works/YouCam_A/youcam`; all commands run in `linux/` unless stated.
- `SCRATCH=/home/patane/.cache/claude-tmp/claude-1000/-home-patane-Works-YouCam-A-youcam/46c95e59-cadb-4b73-9cd8-9e427ef68d24/scratchpad` (never `/tmp`).
- `SPIKE=$SCRATCH/tl` (spike programs and synthetic files; may be deleted).
- `CHECK=$SCRATCH/tl-check` (per-task check files).

## File map

| File | Responsibility |
|---|---|
| `linux/export.c` (new) | α and slider limit, project settings format/parse/read, project paths and naming, stills, export pipeline |
| `linux/later_dialog.c` (new) | The GTK window after a `later` recording: preview, slider, Save / Later / Discard, progress |
| `linux/writer.c` | `writer_layer` modes; `WRITER_LAYER_TRACK` (FFV1 track, settings in the header); `video_encoder_chain()` public |
| `linux/recording.c` | `later` flow: project path, `T_A`, hidden camera title, opaque fallback, last-path semantics, transparency lock |
| `linux/app.c` | Project layer in `layer_tick`, Saved Video menu, Adjust item, notices, start-up sweep, snapshot of the dialog |
| `linux/prefs.c`, `linux/support.c`, `linux/overlay.c`, `linux/youcam.h` | `savedTransparency`, `recording_base_name()`, `overlay_set_capture_hidden()`, declarations |
| `linux/tests.c`, `linux/test_media.c` | Logic tests; synthetic project, stills and export tests |
| `linux/Makefile`, `linux/README.md`, `linux/CHANGELOG.md` | Build, docs |

---

### Task 1: `savedTransparency` preference with migration

**Files:**
- Modify: `linux/youcam.h` (prefs section), `linux/prefs.c`, `linux/tests.c`, `linux/app.c` (three call sites only)

**Interfaces:**
- Produces:
  ```c
  typedef enum { SAVED_PREVIEW, SAVED_OPAQUE, SAVED_LATER, SAVED_COUNT } saved_transparency;
  extern const char *const SavedTransparencyNames[SAVED_COUNT];   /* "preview", "opaque", "later" */
  saved_transparency saved_transparency_parse(const char *value, int has_legacy, int legacy);
  /* prefs: field `saved_transparency saved_transparency;` replaces `int record_transparency;` */
  ```
- Consumes: nothing new.

- [ ] **Step 1: Write the failing tests**

In `linux/tests.c`, `test_prefs()`: replace

```c
    Prefs.record_transparency = 1;
```
with
```c
    Prefs.saved_transparency = SAVED_LATER;
```
replace
```c
    EXPECT(Prefs.transparency == 0.55 && Prefs.record_transparency, "Both transparency choices are remembered");
```
with
```c
    EXPECT(Prefs.transparency == 0.55 && Prefs.saved_transparency == SAVED_LATER, "Both transparency choices are remembered");
```
replace
```c
    EXPECT(Prefs.transparency == TRANSPARENCY_MAX && !Prefs.record_transparency,
           "Saved transparency is normalized; the saved-video option is off by default");
```
with
```c
    EXPECT(Prefs.transparency == TRANSPARENCY_MAX && Prefs.saved_transparency == SAVED_OPAQUE,
           "Saved transparency is normalized; the saved video stays opaque by default");
    /* An old prefs.ini migrates, and the old key is gone after the next save. */
    g_file_set_contents(path, "[youcam]\nrecordCameraTransparency=false\n", -1, NULL);
    prefs_load();
    EXPECT(Prefs.saved_transparency == SAVED_OPAQUE, "An unchecked Apply Transparency to Saved Video becomes Opaque");
    prefs_save();
    char *saved = NULL;
    g_file_get_contents(path, &saved, NULL, NULL);
    EXPECT(saved && strstr(saved, "savedTransparency=opaque") && !strstr(saved, "recordCameraTransparency"),
           "The old key is replaced by savedTransparency on the next save");
    g_free(saved);
```

In `test_transparency()`, the message of the second check no longer holds ("by default"); replace

```c
    EXPECT(camera_layer_needed(0.55, 0, 1, 1), "Saved video stays opaque by default even with a translucent preview");
```
with
```c
    EXPECT(camera_layer_needed(0.55, 0, 1, 1), "An opaque saved video gets the camera copy over a translucent preview");
```

Add after `test_transparency()`:

```c
/* savedTransparency and its migration from recordCameraTransparency. */
static void test_saved_transparency(void) {
    EXPECT(saved_transparency_parse("later", 1, 0) == SAVED_LATER, "The new key wins over the old one");
    EXPECT(saved_transparency_parse("opaque", 0, 0) == SAVED_OPAQUE, "Opaque is read back");
    EXPECT(saved_transparency_parse(NULL, 1, 1) == SAVED_PREVIEW, "A checked old option keeps the preview transparency");
    EXPECT(saved_transparency_parse(NULL, 1, 0) == SAVED_OPAQUE, "An unchecked old option stays opaque");
    EXPECT(saved_transparency_parse(NULL, 0, 0) == SAVED_OPAQUE, "A fresh install keeps today's opaque default");
    EXPECT(saved_transparency_parse("sometimes", 0, 0) == SAVED_OPAQUE, "Unknown values fall back to opaque");
}
```

and in `main()` add `test_saved_transparency();` right after `test_transparency();`.

- [ ] **Step 2: Run the tests to see them fail**

```bash
cd /home/patane/Works/YouCam_A/youcam/linux && make tests 2>&1 | grep -m3 error
```
Expected: errors such as `error: 'prefs' has no member named 'saved_transparency'` and `error: implicit declaration of function 'saved_transparency_parse'`.

- [ ] **Step 3: Declare the type in `youcam.h`**

In the prefs section, after the `background_mode` typedef, add:

```c
/* What the saved video shows of the camera transparency. Linux adds
 * SAVED_LATER: the transparency is chosen after recording (export.c). */
typedef enum { SAVED_PREVIEW, SAVED_OPAQUE, SAVED_LATER, SAVED_COUNT } saved_transparency;
extern const char *const SavedTransparencyNames[SAVED_COUNT];   /* Prefs values: "preview", "opaque", "later". */
saved_transparency saved_transparency_parse(const char *value, int has_legacy, int legacy);
```

In `typedef struct { … } prefs;` replace

```c
    int record_transparency; /* Apply it to the saved video too. */
```
with
```c
    saved_transparency saved_transparency;   /* Same as the preview, opaque, or chosen after recording. */
```

- [ ] **Step 4: Parse, load and save in `prefs.c`**

After `static const char *AreaModes[] = …;` add:

```c
const char *const SavedTransparencyNames[SAVED_COUNT] = {"preview", "opaque", "later"};

/* savedTransparency replaced the check item "Apply Transparency to Saved
 * Video" (recordCameraTransparency): checked kept the preview transparency,
 * unchecked an opaque camera. A fresh install and unknown values keep
 * today's default, an opaque camera. */
saved_transparency saved_transparency_parse(const char *value, int has_legacy, int legacy) {
    if (value) {
        for (int i = 0; i < SAVED_COUNT; i++)
            if (strcmp(value, SavedTransparencyNames[i]) == 0) return (saved_transparency)i;
        return SAVED_OPAQUE;
    }
    if (has_legacy) return legacy ? SAVED_PREVIEW : SAVED_OPAQUE;
    return SAVED_OPAQUE;
}
```

In `prefs_load()` replace

```c
    Prefs.record_transparency = g_key_file_get_boolean(kf, "youcam", "recordCameraTransparency", NULL);
```
with
```c
    char *saved = g_key_file_get_string(kf, "youcam", "savedTransparency", NULL);
    GError *legacy_err = NULL;
    gboolean legacy = g_key_file_get_boolean(kf, "youcam", "recordCameraTransparency", &legacy_err);
    Prefs.saved_transparency = saved_transparency_parse(saved, legacy_err == NULL, legacy);
    g_clear_error(&legacy_err);
    g_free(saved);
```

In `prefs_save()` replace

```c
    g_key_file_set_boolean(kf, "youcam", "recordCameraTransparency", Prefs.record_transparency);
```
with
```c
    g_key_file_set_string(kf, "youcam", "savedTransparency", SavedTransparencyNames[Prefs.saved_transparency]);
```

(`prefs_save` builds a fresh key file, so the old key is not written back.)

- [ ] **Step 5: Keep `app.c` building (the menu is replaced in Task 7)**

In `layer_tick()` replace `Prefs.record_transparency,` with `Prefs.saved_transparency == SAVED_PREVIEW,`.
In `toggle_record_transparency()` replace

```c
    Prefs.record_transparency = gtk_check_menu_item_get_active(check);
```
with
```c
    Prefs.saved_transparency = gtk_check_menu_item_get_active(check) ? SAVED_PREVIEW : SAVED_OPAQUE;
```
In `build_transparency_menu()` replace `Prefs.record_transparency, 0, 1);` with `Prefs.saved_transparency == SAVED_PREVIEW, 0, 1);`.

- [ ] **Step 6: Run the tests**

```bash
make tests && ./tests | tail -1
```
Expected: `Passed 273 logic checks.`

- [ ] **Step 7: Full build without warnings, all tests**

```bash
make clean >/dev/null && make youcam tests test-media test-segment 2>&1 | grep -E "warning|error" || echo "no warnings"
make test 2>&1 | grep -E "^Passed|^media ok"
```
Expected: `no warnings`, then `Passed 273 logic checks.`, `Passed 53 segmentation checks.`, a line starting `media ok:`.

- [ ] **Step 8: Commit**

```bash
git add linux/youcam.h linux/prefs.c linux/tests.c linux/app.c
git commit -m "feat: savedTransparency preference replaces recordCameraTransparency

preview, opaque or later; the old boolean migrates on load.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Export helpers: α, slider limit, project settings, file names

**Files:**
- Create: `linux/export.c`
- Modify: `linux/support.c`, `linux/youcam.h`, `linux/Makefile`, `linux/tests.c`

**Interfaces:**
- Produces:
  ```c
  char *recording_base_name(time_t when);                       /* support.c: YouCam-Recording-<ddMMM-HH.mm.ss> */
  typedef struct { double t_a, preview; char container[8]; int has_audio; } project_info;   /* writer section */
  #define PROJECT_INFO_MAX 160
  #define PROJECT_SUFFIX ".youcam.mkv"
  double later_max(double t_a);
  double later_alpha(double t, double t_a);
  int project_info_format(const project_info *p, char *buf, size_t len);
  int project_info_parse(const guint8 *bytes, size_t len, project_info *out);
  int project_read_info(const char *path, project_info *out);
  char *project_dir(void);
  char *project_new_path(const char *dir, time_t when);
  char *project_latest(const char *dir);
  void project_clean_parts(const char *dir);
  char *export_output_path(const char *dir, const char *project, const char *ext);
  ```
- Consumes: `container_for()`, `transparency_normalize()` semantics (`TRANSPARENCY_MAX`).

- [ ] **Step 1: Write the failing tests**

In `linux/tests.c` add `#include <utime.h>` after `#include <string.h>`, then add before `int main(void)`:

```c
/* Choose After Recording (export.c): layer opacity, slider limit, the
 * project settings in the MKV header, and file names. */
static void test_later(void) {
    EXPECT(near(later_alpha(0, 1), 1) && near(later_alpha(0.4, 1), 0.6) && near(later_alpha(0.8, 1), 0.2),
           "With the extension the camera layer opacity is 1 - t");
    EXPECT(near(later_alpha(0, 0.3), 1) && near(later_alpha(0.15, 0.3), 0.5) && near(later_alpha(0.3, 0.3), 0),
           "Without it the layer fades out at the preview transparency");
    EXPECT(later_alpha(0, 0) == 1 && later_alpha(0.5, 0) == 1, "An opaque preview without the extension keeps the camera opaque");
    EXPECT(later_alpha(0.9, 0.3) == 0 && later_alpha(-1, 0.3) == 1 && later_alpha(NAN, 0.3) == 1,
           "Choices outside the range are clamped");
    double t_a = 0.6, t = 0.25, a = later_alpha(t, t_a);
    EXPECT(near(1 - (a + (1 - a) * (1 - t_a)), t), "The exported camera has exactly the chosen transparency");
    EXPECT(near(later_max(1), TRANSPARENCY_MAX) && near(later_max(0.3), 0.3) && later_max(0) == 0 && later_max(NAN) == 0,
           "The slider reaches 80% with the extension, the preview transparency without");

    project_info in = {1, 0.3, "mp4", 1}, out = {0};
    char text[PROJECT_INFO_MAX];
    EXPECT(project_info_format(&in, text, sizeof(text)), "Project settings fit their header field");
    guint8 header[512];
    for (size_t i = 0; i < sizeof(header); i++) header[i] = (guint8)(i * 37);   /* Binary MKV bytes around it. */
    memcpy(header + 288, text, strlen(text));
    EXPECT(project_info_parse(header, sizeof(header), &out) && out.t_a == 1 && near(out.preview, 0.3) &&
           strcmp(out.container, "mp4") == 0 && out.has_audio, "Project settings are found inside the MKV header");
    EXPECT(!project_info_parse(header, 288, &out), "A file without them is not a project");
    EXPECT(!project_info_parse(header, 288 + 20, &out), "Truncated settings are rejected");
    const char *bad = "YouCam project;YOUCAM_T_A=banana;YOUCAM_PREVIEW_TRANSPARENCY=0.3;YOUCAM_CONTAINER=mov;YOUCAM_AUDIO=1;";
    EXPECT(!project_info_parse((const guint8 *)bad, strlen(bad), &out), "Corrupt settings are rejected");
    const char *range = "YouCam project;YOUCAM_T_A=2;YOUCAM_PREVIEW_TRANSPARENCY=0.3;YOUCAM_CONTAINER=mov;YOUCAM_AUDIO=1;";
    EXPECT(!project_info_parse((const guint8 *)range, strlen(range), &out), "Out-of-range settings are rejected");
    const char *odd = "YouCam project;YOUCAM_T_A=1;YOUCAM_PREVIEW_TRANSPARENCY=0;YOUCAM_CONTAINER=webm;YOUCAM_AUDIO=0;";
    EXPECT(project_info_parse((const guint8 *)odd, strlen(odd), &out) && strcmp(out.container, "mov") == 0 &&
           !out.has_audio, "An unknown container falls back to MOV");

    char *dir = g_dir_make_tmp("youcam-later-XXXXXX", NULL);
    const char *project = "/x/pending/YouCam-Recording-01Nov-10.00.00" PROJECT_SUFFIX;
    char *first = export_output_path(dir, project, "mov");
    char *expected = g_build_filename(dir, "YouCam-Recording-01Nov-10.00.00.mov", NULL);
    EXPECT(strcmp(first, expected) == 0, "The movie takes the project's name and the chosen format");
    g_file_set_contents(first, "", 0, NULL);
    char *second = export_output_path(dir, project, "mov");
    EXPECT(g_str_has_suffix(second, "/YouCam-Recording-01Nov-10.00.00 (2).mov"), "An existing movie is never overwritten");
    g_file_set_contents(second, "", 0, NULL);
    char *third = export_output_path(dir, project, "mov");
    EXPECT(g_str_has_suffix(third, " (3).mov"), "The number keeps counting");
    char *fresh = project_new_path(dir, 0), *base = recording_base_name(0);
    EXPECT(g_str_has_suffix(fresh, PROJECT_SUFFIX) && strstr(fresh, base), "A project is named like the movie it becomes");
    char *old = g_build_filename(dir, "a" PROJECT_SUFFIX, NULL), *recent = g_build_filename(dir, "b" PROJECT_SUFFIX, NULL);
    char *part = g_build_filename(dir, "c.mov.part", NULL);
    g_file_set_contents(old, "", 0, NULL);
    g_file_set_contents(recent, "", 0, NULL);
    g_file_set_contents(part, "", 0, NULL);
    struct utimbuf t_old = {1000, 1000}, t_recent = {2000, 2000};
    g_utime(old, &t_old);
    g_utime(recent, &t_recent);
    char *latest = project_latest(dir);
    EXPECT(latest && strcmp(latest, recent) == 0, "The most recent project is the one opened");
    project_clean_parts(dir);
    EXPECT(!g_file_test(part, G_FILE_TEST_EXISTS) && g_file_test(old, G_FILE_TEST_EXISTS),
           "Stale .part files go, projects stay");
    EXPECT(!project_latest("/nonexistent-youcam-pending"), "No pending folder, no project");
    g_unlink(first); g_unlink(second); g_unlink(old); g_unlink(recent);
    g_rmdir(dir);
    g_free(first); g_free(expected); g_free(second); g_free(third); g_free(fresh); g_free(base);
    g_free(old); g_free(recent); g_free(part); g_free(latest); g_free(dir);
}
```

and in `main()` add `test_later();` after `test_saved_transparency();`.

- [ ] **Step 2: Run the tests to see them fail**

```bash
make tests 2>&1 | grep -m3 error
```
Expected: `error: unknown type name 'project_info'` (and implicit declarations of `later_alpha` etc.).

- [ ] **Step 3: Declarations in `youcam.h`**

Support section, after `char *recording_next_path(…);` add:

```c
char *recording_base_name(time_t when);   /* YouCam-Recording-<ddMMM-HH.mm.ss>. Caller frees. */
```

Writer section, right after `typedef struct writer writer;` add:

```c
/* Choose After Recording: what is needed to finish a project (export.c). */
typedef struct {
    double t_a;          /* Camera transparency in the screen track: 1 = absent (extension), else the preview's. */
    double preview;      /* Preview transparency when the recording started: where the slider starts. */
    char container[8];   /* Extension of the final movie (Containers[]). */
    int has_audio;
} project_info;
#define PROJECT_INFO_MAX 160    /* project_info_format() text, NUL included. */
```

After the whole writer section (after `void pipeline_stop_async(GstElement *pipeline);`) add a new section:

```c
/* ------------------------------------------------------------ export.c --- */

#define PROJECT_SUFFIX ".youcam.mkv"

double later_max(double t_a);                  /* Slider maximum: min(T_A, 0.8). */
double later_alpha(double t, double t_a);      /* Layer opacity for a final transparency t: 1 - t / T_A. */
int project_info_format(const project_info *p, char *buf, size_t len);
int project_info_parse(const guint8 *bytes, size_t len, project_info *out);   /* Untrusted bytes. */
int project_read_info(const char *path, project_info *out);
char *project_dir(void);                       /* ~/.local/share/youcam/pending, not created. */
char *project_new_path(const char *dir, time_t when);
char *project_latest(const char *dir);         /* Newest *.youcam.mkv, NULL if none. */
void project_clean_parts(const char *dir);
char *export_output_path(const char *dir, const char *project, const char *ext);
```

- [ ] **Step 4: `recording_base_name` in `support.c`**

Replace the whole `recording_next_path` function and its comment with:

```c
/* YouCam-Recording-<ddMMM-HH.mm.ss>. Month names are always English
 * (en_US_POSIX on macOS). Caller frees. */
char *recording_base_name(time_t when) {
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm tm;
    localtime_r(&when, &tm);
    return g_strdup_printf("YouCam-Recording-%02d%s-%02d.%02d.%02d",
                           tm.tm_mday, months[tm.tm_mon], tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* <dir>/<base name>.<ext>, never overwriting: -2, -3, ... Caller frees. */
char *recording_next_path(const char *dir, time_t when, const char *ext) {
    char *base = recording_base_name(when);
    ext = container_for(ext)->ext;
    char *path = g_strdup_printf("%s/%s.%s", dir, base, ext);
    for (int suffix = 2; g_file_test(path, G_FILE_TEST_EXISTS); suffix++) {
        g_free(path);
        path = g_strdup_printf("%s/%s-%d.%s", dir, base, suffix, ext);
    }
    g_free(base);
    return path;
}
```

- [ ] **Step 5: Create `linux/export.c`**

```c
/* Choose After Recording (Linux addition): projects and their export.
 *
 * A project is an MKV holding the screen (H.264, as any recording), the
 * camera layer at screen size (FFV1: lossless, keeps alpha) and the
 * microphone (AAC). Exporting lays the layer over the screen with opacity
 *
 *     alpha = 1 - t / T_A
 *
 * where T_A is the transparency the camera already has in the screen
 * track: 1 when the Capture Exclusion extension kept the camera window out
 * of it, the preview transparency otherwise. The camera then ends up
 * exactly t transparent: its opacity is alpha + (1 - alpha)(1 - T_A) = 1 - t.
 *
 * The settings travel in the MKV header (the WritingApp field), on disk
 * from the first second: GStreamer writes tags only when the file is
 * finished, so a killed recording would lose them. A copy goes into the
 * COMMENT tag for players and ffprobe. */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <glib/gstdio.h>
#include "youcam.h"

#define PROJECT_MAGIC "YouCam project;"
#define PROJECT_HEADER_BYTES 16384   /* WritingApp sits near the start (byte 288 in our files). */
#define KEY_T_A ";YOUCAM_T_A="
#define KEY_PREVIEW ";YOUCAM_PREVIEW_TRANSPARENCY="
#define KEY_CONTAINER ";YOUCAM_CONTAINER="
#define KEY_AUDIO ";YOUCAM_AUDIO="
#define EXT_CHARS "abcdefghijklmnopqrstuvwxyz0123456789"

double later_max(double t_a) {
    return isfinite(t_a) && t_a > 0 ? fmin(t_a, TRANSPARENCY_MAX) : 0;
}

/* T_A = 0 (opaque preview, no extension): the camera in the screen track
 * is already opaque and the slider only offers 0, so the layer is opaque. */
double later_alpha(double t, double t_a) {
    if (!isfinite(t_a) || t_a <= 0) return 1;
    if (!isfinite(t)) t = 0;
    return fmin(1, fmax(0, 1 - t / t_a));
}

int project_info_format(const project_info *p, char *buf, size_t len) {
    char t_a[G_ASCII_DTOSTR_BUF_SIZE], preview[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(t_a, sizeof(t_a), "%.3f", p->t_a);   /* Never a decimal comma, whatever the locale. */
    g_ascii_formatd(preview, sizeof(preview), "%.3f", p->preview);
    int n = snprintf(buf, len, PROJECT_MAGIC "YOUCAM_T_A=%s" KEY_PREVIEW "%s" KEY_CONTAINER "%s" KEY_AUDIO "%d;",
                     t_a, preview, container_for(p->container)->ext, p->has_audio ? 1 : 0);
    return n > 0 && (size_t)n < len;
}

/* The number after key, ended by ';', within [0, max]. */
static int parse_value(const char *text, const char *key, double max, double *out) {
    const char *at = strstr(text, key);
    if (!at) return 0;
    at += strlen(key);
    char *end;
    double v = g_ascii_strtod(at, &end);
    if (end == at || *end != ';' || !isfinite(v) || v < 0 || v > max) return 0;
    *out = v;
    return 1;
}

/* Any file in pending/ can be passed here, truncated or not ours: every
 * field is checked, and nothing is read past len. */
int project_info_parse(const guint8 *bytes, size_t len, project_info *out) {
    size_t magic = strlen(PROJECT_MAGIC);
    for (size_t i = 0; i + magic <= len; i++) {
        if (memcmp(bytes + i, PROJECT_MAGIC, magic) != 0) continue;
        char text[PROJECT_INFO_MAX];
        size_t n = MIN(len - i, sizeof(text) - 1);
        memcpy(text, bytes + i, n);
        text[n] = 0;   /* Binary MKV bytes after the field may hold a NUL earlier: strstr stops there. */
        project_info p = {0};
        const char *ext = strstr(text, KEY_CONTAINER), *audio = strstr(text, KEY_AUDIO);
        if (!parse_value(text, KEY_T_A, 1, &p.t_a) || !parse_value(text, KEY_PREVIEW, TRANSPARENCY_MAX, &p.preview) ||
            !ext || !audio)
            return 0;
        ext += strlen(KEY_CONTAINER);
        audio += strlen(KEY_AUDIO);
        size_t k = strspn(ext, EXT_CHARS);
        if (k == 0 || k >= sizeof(p.container) || ext[k] != ';' || (audio[0] != '0' && audio[0] != '1') ||
            audio[1] != ';')
            return 0;
        char name[sizeof(p.container)];
        memcpy(name, ext, k);
        name[k] = 0;
        g_strlcpy(p.container, container_for(name)->ext, sizeof(p.container));   /* Unknown: MOV. */
        p.has_audio = audio[0] == '1';
        *out = p;
        return 1;
    }
    return 0;
}

int project_read_info(const char *path, project_info *out) {
    FILE *f = g_fopen(path, "rb");
    if (!f) return 0;
    guint8 *head = g_malloc(PROJECT_HEADER_BYTES);
    size_t n = fread(head, 1, PROJECT_HEADER_BYTES, f);
    fclose(f);
    int ok = project_info_parse(head, n, out);
    g_free(head);
    return ok;
}

char *project_dir(void) {
    return g_build_filename(g_get_user_data_dir(), "youcam", "pending", NULL);
}

/* <dir>/<the final movie's name>.youcam.mkv, never overwriting. */
char *project_new_path(const char *dir, time_t when) {
    char *base = recording_base_name(when);
    char *path = g_strdup_printf("%s/%s" PROJECT_SUFFIX, dir, base);
    for (int suffix = 2; g_file_test(path, G_FILE_TEST_EXISTS); suffix++) {
        g_free(path);
        path = g_strdup_printf("%s/%s-%d" PROJECT_SUFFIX, dir, base, suffix);
    }
    g_free(base);
    return path;
}

/* The menu opens the newest project; older ones wait, without a list. */
char *project_latest(const char *dir) {
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return NULL;
    char *best = NULL;
    time_t best_time = 0;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (!g_str_has_suffix(name, PROJECT_SUFFIX)) continue;
        char *path = g_build_filename(dir, name, NULL);
        GStatBuf st;
        if (g_stat(path, &st) == 0 && (!best || st.st_mtime > best_time)) {
            g_free(best);
            best = path;
            best_time = st.st_mtime;
        } else {
            g_free(path);
        }
    }
    g_dir_close(d);
    return best;
}

/* A .part left in pending/ by a crash is never useful. The Desktop is the
 * user's own folder: nothing there is touched. */
void project_clean_parts(const char *dir) {
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (!g_str_has_suffix(name, ".part")) continue;
        char *path = g_build_filename(dir, name, NULL);
        if (g_unlink(path) != 0) g_printerr("youcam: could not remove %s\n", path);
        g_free(path);
    }
    g_dir_close(d);
}

/* <dir>/<project name without .youcam.mkv>.<ext>; an existing movie is
 * never overwritten: " (2)", " (3)"... before the extension. Caller frees. */
char *export_output_path(const char *dir, const char *project, const char *ext) {
    char *name = g_path_get_basename(project);
    if (g_str_has_suffix(name, PROJECT_SUFFIX)) name[strlen(name) - strlen(PROJECT_SUFFIX)] = 0;
    ext = container_for(ext)->ext;
    char *path = g_strdup_printf("%s/%s.%s", dir, name, ext);
    for (int n = 2; g_file_test(path, G_FILE_TEST_EXISTS); n++) {
        g_free(path);
        path = g_strdup_printf("%s/%s (%d).%s", dir, name, n, ext);
    }
    g_free(name);
    return path;
}
```

- [ ] **Step 6: Build it in (`Makefile`)**

Replace `MEDIA := writer.o audio.o` with `MEDIA := writer.o audio.o export.o`, and replace

```make
tests: tests.o $(CORE)
```
with
```make
tests: tests.o $(MEDIA) $(CORE)
```
(`export.o` is media code; the logic tests now link it for its pure helpers. `writer.o` and `audio.o` need only CORE.)

- [ ] **Step 7: Run the tests**

```bash
make tests && ./tests | tail -1
```
Expected: `Passed 293 logic checks.`

- [ ] **Step 8: Full build without warnings, all tests**

```bash
make clean >/dev/null && make youcam tests test-media test-segment 2>&1 | grep -E "warning|error" || echo "no warnings"
make test 2>&1 | grep -E "^Passed|^media ok"
```
Expected: `no warnings`; `Passed 293 logic checks.`, `Passed 53 segmentation checks.`, `media ok: …` (unchanged media line).

- [ ] **Step 9: Commit**

```bash
git add linux/export.c linux/support.c linux/youcam.h linux/Makefile linux/tests.c
git commit -m "feat: project settings, layer opacity and export naming helpers

Settings live in the MKV header so a killed recording keeps them.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Writer: camera layer on its own FFV1 track

**Files:**
- Modify: `linux/writer.c`, `linux/youcam.h` (writer section), `linux/recording.c` (one call), `linux/test_media.c`

**Interfaces:**
- Produces:
  ```c
  typedef enum { WRITER_LAYER_NONE, WRITER_LAYER_COMPOSITE, WRITER_LAYER_TRACK } writer_layer;
  writer *writer_start(const char *path, const char *video_source, GstElement *mic, const rect *crop_fraction,
                       writer_layer layer, const project_info *project, writer_callbacks cb, GError **err);
  void writer_push_layer(writer *w, const guint8 *bgra, int stride);   /* Movie-size frame; NULL = transparent. */
  int writer_layer_track(writer *w);
  const char *video_encoder_chain(void);                               /* Was static video_encoder(). */
  #define SRGB_LABEL "capssetter join=true caps=video/x-raw,colorimetry=sRGB"   /* Moved to youcam.h. */
  ```
  Project file layout: track 1 H.264 screen (`d.video_0`), track 2 FFV1 BGRA layer at movie size (`d.video_1`), track 3 AAC (`d.audio_0`) when there is a microphone; WritingApp and COMMENT = `project_info_format()`.
- Consumes: `project_info`, `project_info_format()`, `project_read_info()`, `PROJECT_SUFFIX` (Task 2).

- [ ] **Step 1: Write the failing media test**

In `linux/test_media.c`:

Replace the file comment with:

```c
/* Records synthetic screen + microphone through the real writer pipeline,
 * with a pause in the middle, then inspects the movie (RecordingTests.swift,
 * movie part). A second movie without audio must have no audio track.
 * Choose After Recording: a project keeps screen, camera layer and
 * microphone apart, with its settings in the MKV header. */
```

Add `#include <string.h>` after `#include <stdlib.h>`. After `#define PAUSE_S 2` add:

```c
#define PROJECT_S 4
#define PROJECT_COPY_S 2   /* When the "killed" copy is taken. */
#define SQUARE 64          /* The project's camera: an opaque red square... */
#define SQUARE_X 100       /* ...at this position in the 640x360 movie. */
#define SQUARE_Y 100
```

After `static writer *W;` add `static char *ProjectPath, *KilledPath;`.

After `push_layer()` add:

```c
/* Project layer: the movie-size frame, transparent but for the red square. */
static gboolean push_project_layer(gpointer d) {
    (void)d;
    if (!W) return G_SOURCE_REMOVE;
    int w, h;
    if (!writer_output_size(W, &w, &h)) return G_SOURCE_CONTINUE;
    guint8 *frame = g_malloc0((size_t)w * h * 4);
    for (int y = SQUARE_Y; y < SQUARE_Y + SQUARE && y < h; y++)
        for (int x = SQUARE_X; x < SQUARE_X + SQUARE && x < w; x++) {
            frame[((size_t)y * w + x) * 4 + 2] = 255;
            frame[((size_t)y * w + x) * 4 + 3] = 255;
        }
    writer_push_layer(W, frame, w * 4);
    g_free(frame);
    LayerFrames++;
    return G_SOURCE_CONTINUE;
}

/* What a killed YouCam leaves behind: the project as it is mid-recording. */
static gboolean copy_project(gpointer d) {
    (void)d;
    char *data = NULL;
    gsize len = 0;
    if (g_file_get_contents(ProjectPath, &data, &len, NULL)) g_file_set_contents(KilledPath, data, len, NULL);
    g_free(data);
    return G_SOURCE_REMOVE;
}

static GstDiscovererInfo *discover(const char *path) {
    GError *err = NULL;
    char *uri = gst_filename_to_uri(path, NULL);
    GstDiscoverer *d = gst_discoverer_new(5 * GST_SECOND, NULL);
    GstDiscovererInfo *info = gst_discoverer_discover_uri(d, uri, &err);
    EXPECT(info, "movie unreadable: %s", err ? err->message : "?");
    g_free(uri);
    g_object_unref(d);
    return info;
}
```

In `record()`, replace

```c
                     mic, &crop, with_layer, (writer_callbacks){finished, failed, NULL}, &err);
```
with
```c
                     mic, &crop, with_layer ? WRITER_LAYER_COMPOSITE : WRITER_LAYER_NONE, NULL,
                     (writer_callbacks){finished, failed, NULL}, &err);
```
and replace its tail, from `char *uri = gst_filename_to_uri(Result, NULL);` to `return info;`, with `return discover(Result);`.

After `record()` add:

```c
/* A blue screen, so the camera centre has a known colour under it. */
static GstDiscovererInfo *record_project(const project_info *project) {
    GstElement *mic = gst_element_factory_make("audiotestsrc", NULL);
    g_object_set(mic, "is-live", TRUE, NULL);
    rect crop = {0.25, 0.25, 0.5, 0.5};
    GError *err = NULL;
    Result = Error = NULL;
    LayerFrames = 0;
    W = writer_start(ProjectPath, "videotestsrc is-live=true pattern=solid-color foreground-color=0xff0000ff ! "
                     "video/x-raw,width=1280,height=720,framerate=30/1", mic, &crop, WRITER_LAYER_TRACK, project,
                     (writer_callbacks){finished, failed, NULL}, &err);
    EXPECT(W, "project writer did not start: %s", err ? err->message : "?");
    guint layer = g_timeout_add(33, push_project_layer, NULL);
    g_timeout_add_seconds(PROJECT_COPY_S, copy_project, NULL);
    g_timeout_add_seconds(PROJECT_S, do_finish, NULL);
    g_main_loop_run(Loop);
    W = NULL;
    g_source_remove(layer);
    EXPECT(!Error, "project recording failed: %s", Error);
    return discover(Result);
}

/* export.c links d.video_0 to the H.264 decoder and d.video_1 to FFV1:
 * the screen must be track 1 and the layer track 2. */
static void check_project_tracks(GstDiscovererInfo *info) {
    GList *videos = gst_discoverer_info_get_video_streams(info);
    GList *audios = gst_discoverer_info_get_audio_streams(info);
    EXPECT(g_list_length(videos) == 2 && g_list_length(audios) == 1,
           "a project holds the screen, the camera layer and the microphone (%u video, %u audio)",
           g_list_length(videos), g_list_length(audios));
    for (GList *l = videos; l; l = l->next) {
        GstCaps *caps = gst_discoverer_stream_info_get_caps(l->data);
        const char *id = gst_discoverer_stream_info_get_stream_id(l->data);
        int screen = gst_structure_has_name(gst_caps_get_structure(caps, 0), "video/x-h264");
        int layer = gst_structure_has_name(gst_caps_get_structure(caps, 0), "video/x-ffv");
        EXPECT(id && ((screen && strstr(id, "/001")) || (layer && strstr(id, "/002"))),
               "screen on track 1, FFV1 layer on track 2 (got %s on %s)", gst_caps_to_string(caps), id);
        gst_caps_unref(caps);
    }
    gst_discoverer_stream_info_list_free(videos);
    gst_discoverer_stream_info_list_free(audios);
}
```

In `main()`, replace everything from `int red = red_pixels_on_row(Result, 30, 132);` to the end of the function with:

```c
    int red = red_pixels_on_row(Result, 30, 132);
    EXPECT(red >= 60, "the camera layer is visible in the movie (%d red pixels)", red);
    gst_discoverer_stream_info_list_free(videos);
    gst_discoverer_info_unref(info);
    g_unlink(path);
    int composited = LayerFrames;

    /* Choose After Recording: three tracks; the settings survive a kill. */
    project_info project = {0.5, 0.5, "mov", 1}, back = {0};
    ProjectPath = g_build_filename(dir, "project" PROJECT_SUFFIX, NULL);
    KilledPath = g_build_filename(dir, "killed" PROJECT_SUFFIX, NULL);
    info = record_project(&project);
    double project_s = (double)gst_discoverer_info_get_duration(info) / GST_SECOND;
    check_project_tracks(info);
    char *comment = NULL;
    const GstTagList *tags = gst_discoverer_info_get_tags(info);
    EXPECT(tags && gst_tag_list_get_string(tags, GST_TAG_COMMENT, &comment) && strstr(comment, "YOUCAM_T_A=0.500;"),
           "a finished project also carries its settings in the MKV comment tag");
    g_free(comment);
    gst_discoverer_info_unref(info);
    EXPECT(fabs(project_s - PROJECT_S) < 1, "project duration %.2fs", project_s);
    EXPECT(LayerFrames > 60, "camera layer frames were pushed (%d)", LayerFrames);
    EXPECT(project_read_info(ProjectPath, &back) && back.t_a == 0.5 && back.preview == 0.5 &&
           strcmp(back.container, "mov") == 0 && back.has_audio, "a finished project is read back");
    EXPECT(project_read_info(KilledPath, &back) && back.t_a == 0.5,
           "a killed project keeps its settings (they are in the MKV header)");

    g_unlink(ProjectPath);
    g_unlink(KilledPath);
    g_rmdir(dir);
    printf("media ok: %.2fs 640x360 .mov with audio, pause removed; silent movie has no audio track; "
           "camera layer movie %.2fs with %d composited frames; mp4, mkv and avi ok; "
           "project %.2fs with screen, FFV1 layer and audio, settings read back after a finish and a kill\n",
           duration, layered, composited, project_s);
    return 0;
}
```

- [ ] **Step 2: Run it to see it fail**

```bash
make test-media 2>&1 | grep -m3 error
```
Expected: `error: 'WRITER_LAYER_COMPOSITE' undeclared` and an implicit declaration of `writer_push_layer`.

- [ ] **Step 3: Writer declarations in `youcam.h`**

In the writer section, replace

```c
writer *writer_start(const char *path, const char *video_source, GstElement *mic,
                     const rect *crop_fraction, int camera_layer, writer_callbacks cb, GError **err);
int writer_output_size(writer *w, int *width, int *height);
const char *video_encoder_name(void);
```
with
```c
/* NONE: screen only. COMPOSITE: the opaque camera copy laid over the screen
 * (writer_push_camera). TRACK: Choose After Recording, the layer on its own
 * lossless track of an MKV project (writer_push_layer). */
typedef enum { WRITER_LAYER_NONE, WRITER_LAYER_COMPOSITE, WRITER_LAYER_TRACK } writer_layer;

/* The screen arrives as BGRA labelled with varying spellings of sRGB
 * (0:0:4:1, 0:1:4:1). A differing label makes the compositor run a full
 * color conversion; relabelling, without touching pixels, keeps it a blend. */
#define SRGB_LABEL "capssetter join=true caps=video/x-raw,colorimetry=sRGB"

writer *writer_start(const char *path, const char *video_source, GstElement *mic, const rect *crop_fraction,
                     writer_layer layer, const project_info *project, writer_callbacks cb, GError **err);
int writer_output_size(writer *w, int *width, int *height);
const char *video_encoder_name(void);
const char *video_encoder_chain(void);   /* Encoder, conversion and bitrate, shared with export.c. */
void writer_push_layer(writer *w, const guint8 *bgra, int stride);   /* Movie-size frame; NULL = transparent. */
int writer_layer_track(writer *w);
```

- [ ] **Step 4: `writer.c`**

a) Delete the `SRGB_LABEL` comment and `#define` (now in `youcam.h`).

b) In `struct writer` add after `writer_callbacks cb;`:

```c
    writer_layer layer;
```

c) Replace the whole `writer_push_camera` function and its comment with:

```c
/* Copies a BGRA frame into a buffer for the camera appsrc; NULL pushes a
 * fully transparent one. The caps follow the frame size. */
static void push_frame(writer *w, const guint8 *bgra, int width, int height, int stride) {
    if (width != w->camera_w || height != w->camera_h) set_camera_caps(w, width, height);
    GstBuffer *buf = gst_buffer_new_allocate(NULL, (gsize)width * height * 4, NULL);
    GstMapInfo map;
    gst_buffer_map(buf, &map, GST_MAP_WRITE);
    if (!bgra) memset(map.data, 0, map.size);
    for (int row = 0; bgra && row < height; row++)
        memcpy(map.data + (size_t)row * width * 4, bgra + (size_t)row * stride, (size_t)width * 4);
    gst_buffer_unmap(buf, &map);
    GstFlowReturn ret;
    g_signal_emit_by_name(w->camera, "push-buffer", buf, &ret);
    gst_buffer_unref(buf);
}

/* Composite: camera layer frame, placed at (x, y) in movie pixels. When not
 * visible the layer is just made transparent; the compositor keeps its last frame. */
void writer_push_camera(writer *w, const guint8 *bgra, int width, int height, int stride,
                        int x, int y, int visible) {
    if (!w->camera_pad || w->done) return;
    if (!visible) {
        g_object_set(w->camera_pad, "alpha", 0.0, NULL);
        return;
    }
    if (w->paused_at) return;
    push_frame(w, bgra, width, height, stride);
    g_object_set(w->camera_pad, "xpos", x, "ypos", y, "width", width, "height", height, "alpha", 1.0, NULL);
}

/* Track: the whole movie-size layer. Its size is pinned by the first frame
 * (Matroska cannot change caps later), so frames come only once the movie
 * size is known, at exactly that size. */
void writer_push_layer(writer *w, const guint8 *bgra, int stride) {
    int width, height;
    if (w->layer != WRITER_LAYER_TRACK || w->done || w->paused_at || !writer_output_size(w, &width, &height)) return;
    push_frame(w, bgra, width, height, stride);
}

int writer_layer_track(writer *w) { return w->layer == WRITER_LAYER_TRACK; }
```

d) Rename the encoder chain lookup: replace

```c
static const char *video_encoder(void) {
```
with
```c
const char *video_encoder_chain(void) {
```

e) Before `writer *writer_start(`, add:

```c
/* A live pipeline starts on its first data, which may come from the camera
 * before PipeWire delivers the screen: hold the camera until then. */
static void hold_camera_until_screen(writer *w, GstPad *screen, GstPad *camera) {
    gst_pad_add_probe(screen, GST_PAD_PROBE_TYPE_BUFFER, screen_arrived, w, NULL);
    gst_pad_add_probe(camera, GST_PAD_PROBE_TYPE_BUFFER, drop_until_screen, w, NULL);
    gst_object_unref(screen);
    gst_object_unref(camera);
}

/* The settings go into the header (WritingApp), on disk from the start, and
 * into the COMMENT tag, which the muxer writes only at the end. */
static void tag_project(GstElement *pipeline, const project_info *project) {
    char text[PROJECT_INFO_MAX];
    if (!project || !project_info_format(project, text, sizeof(text))) return;
    GstElement *mux = gst_bin_get_by_name(GST_BIN(pipeline), "mux");
    g_object_set(mux, "writing-app", text, NULL);
    gst_tag_setter_add_tags(GST_TAG_SETTER(mux), GST_TAG_MERGE_REPLACE, GST_TAG_COMMENT, text, NULL);
    gst_object_unref(mux);
}
```

f) Replace the whole `writer_start` function with:

```c
writer *writer_start(const char *path, const char *video_source, GstElement *mic, const rect *crop_fraction,
                     writer_layer layer, const project_info *project, writer_callbacks cb, GError **err) {
    char *denoise = noise_reduction_element(Prefs.noise_reduction);
    char *audio = mic ? g_strdup_printf(
        "audioconvert name=aconvert ! audioresample ! %s"
        "audio/x-raw,format=F32LE,channels=1,rate=48000 ! identity name=gain ! "
        "audioconvert ! avenc_aac bitrate=%d ! queue ! mux. ", denoise, AUDIO_BITRATE) : g_strdup("");
    g_free(denoise);
    /* Composite: the camera layer as an appsrc laid over the screen. */
    const char *composite = layer == WRITER_LAYER_COMPOSITE
        ? "videoconvert ! video/x-raw,format=BGRA ! " SRGB_LABEL " ! comp.sink_0 "
          "appsrc name=camera is-live=true do-timestamp=true format=time "
          "caps=video/x-raw,format=BGRA,width=2,height=2,framerate=0/1 ! videoconvert ! "
          "video/x-raw,format=BGRA ! " SRGB_LABEL " ! comp.sink_1 "
          "compositor name=comp ignore-inactive-pads=true background=black ! "
          "capsfilter name=outsize caps=video/x-raw,framerate=30/1 ! "
        : "";
    /* Track: the layer at movie size on its own lossless track, linked after
     * the screen so it is track 2 (export.c relies on it). No caps here: they
     * come with the first frame. A frame is 16 MB at 2560x1600, so a busy
     * encoder drops new ones rather than queueing them; the export keeps
     * showing the last one. */
    const char *track = layer == WRITER_LAYER_TRACK
        ? "appsrc name=camera is-live=true do-timestamp=true format=time max-buffers=4 leaky-type=upstream ! "
          "avenc_ffv1 ! queue ! mux. "
        : "";
    const char *ext = strrchr(path, '.') ? strrchr(path, '.') + 1 : NULL;
    char *desc = g_strdup_printf(
        "%s ! videoconvert ! videocrop name=crop ! videorate ! video/x-raw,framerate=30/1 ! %s"
        "%s ! h264parse ! queue ! mux. %s%s%s name=mux ! filesink name=file",
        video_source, composite, video_encoder_chain(), track, audio,
        layer == WRITER_LAYER_TRACK ? "matroskamux" : container_for(ext)->muxer);
    GstElement *p = gst_parse_launch(desc, err);
    g_free(desc);
    g_free(audio);
    if (!p) return NULL;

    if (mic) {
        GstElement *convert = gst_bin_get_by_name(GST_BIN(p), "aconvert");
        int ok = gst_bin_add(GST_BIN(p), mic) && gst_element_link(mic, convert);
        gst_object_unref(convert);
        if (!ok) {
            g_set_error(err, GST_CORE_ERROR, GST_CORE_ERROR_NEGOTIATION,
                        "The microphone could not be connected.");
            gst_object_unref(p);
            return NULL;
        }
        GstElement *gain = gst_bin_get_by_name(GST_BIN(p), "gain");
        audio_install_gain_probe(gain);
        gst_object_unref(gain);
    }

    writer *w = g_new0(writer, 1);
    w->pipeline = p;
    w->path = g_strdup(path);
    w->cb = cb;
    w->layer = layer;
    GstElement *file = gst_bin_get_by_name(GST_BIN(p), "file");
    g_object_set(file, "location", path, NULL);
    gst_object_unref(file);
    GstElement *crop = gst_bin_get_by_name(GST_BIN(p), "crop");
    GstPad *pad = gst_element_get_static_pad(crop, "src");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, size_probe, w, NULL);
    if (layer != WRITER_LAYER_NONE) w->camera = gst_bin_get_by_name(GST_BIN(p), "camera");
    if (layer == WRITER_LAYER_TRACK) {
        hold_camera_until_screen(w, gst_object_ref(pad), gst_element_get_static_pad(w->camera, "src"));
        tag_project(p, project);
    }
    gst_object_unref(pad);
    if (crop_fraction) {
        w->crop = *crop_fraction;
        pad = gst_element_get_static_pad(crop, "sink");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, crop_probe, w, NULL);
        gst_object_unref(pad);
    }
    gst_object_unref(crop);
    if (layer == WRITER_LAYER_COMPOSITE) {
        GstElement *comp = gst_bin_get_by_name(GST_BIN(p), "comp");
        w->camera_pad = gst_element_get_static_pad(comp, "sink_1");
        w->outsize = gst_bin_get_by_name(GST_BIN(p), "outsize");
        hold_camera_until_screen(w, gst_element_get_static_pad(comp, "sink_0"),
                                 gst_element_get_static_pad(w->outsize, "src"));
        gst_object_unref(comp);
        g_object_set(w->camera_pad, "alpha", 0.0, "zorder", 1, NULL);
        w->camera_w = w->camera_h = 2;
        guint8 blank[2 * 2 * 4] = {0};
        writer_push_camera(w, blank, 2, 2, 8, 0, 0, 1);
        g_object_set(w->camera_pad, "alpha", 0.0, NULL);
    }
    GstBus *bus = gst_element_get_bus(p);
    gst_bus_add_signal_watch(bus);
    g_signal_connect(bus, "message::eos", G_CALLBACK(on_eos), w);
    g_signal_connect(bus, "message::error", G_CALLBACK(on_error), w);
    gst_object_unref(bus);

    if (gst_element_set_state(p, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        g_set_error(err, GST_CORE_ERROR, GST_CORE_ERROR_STATE_CHANGE, "The recording could not start.");
        bus = gst_element_get_bus(p);
        gst_bus_remove_signal_watch(bus);
        gst_object_unref(bus);
        gst_element_set_state(p, GST_STATE_NULL);
        if (w->camera) gst_object_unref(w->camera);
        if (w->camera_pad) gst_object_unref(w->camera_pad);
        if (w->outsize) gst_object_unref(w->outsize);
        gst_object_unref(p);
        g_unlink(path);
        g_free(w->path);
        g_free(w);
        return NULL;
    }
    w->started = g_get_monotonic_time();
    return w;
}
```

g) Update the file comment's first lines to mention the project mode: replace

```c
/* Movie writer (RecordingWriter.swift): screen + optional microphone into a
 * QuickTime .mov with H.264 video and AAC audio.
```
with
```c
/* Movie writer (RecordingWriter.swift): screen + optional microphone into a
 * QuickTime .mov with H.264 video and AAC audio. Linux adds the other
 * containers and, for Choose After Recording, an MKV project with the
 * camera layer on its own FFV1 track (see export.c).
```

- [ ] **Step 5: The one caller in `recording.c`**

Replace

```c
    R.movie = writer_start(path, video, mic, R.has_crop ? &R.crop : NULL, 1,
                           (writer_callbacks){movie_finished, movie_failed, NULL}, &err);
```
with
```c
    R.movie = writer_start(path, video, mic, R.has_crop ? &R.crop : NULL, WRITER_LAYER_COMPOSITE, NULL,
                           (writer_callbacks){movie_finished, movie_failed, NULL}, &err);
```

- [ ] **Step 6: Run the media test**

```bash
make test-media && ./test-media
```
Expected: one line starting `media ok:` and ending `project 4.0xs with screen, FFV1 layer and audio, settings read back after a finish and a kill` (durations vary by a few hundredths).

- [ ] **Step 7: Full build without warnings, all tests**

```bash
make clean >/dev/null && make youcam tests test-media test-segment 2>&1 | grep -E "warning|error" || echo "no warnings"
make test 2>&1 | grep -E "^Passed|^media ok"
```
Expected: `no warnings`; `Passed 293 logic checks.`, `Passed 53 segmentation checks.`, the `media ok:` line of Step 6.

- [ ] **Step 8: Commit**

```bash
git add linux/writer.c linux/youcam.h linux/recording.c linux/test_media.c
git commit -m "feat: writer records the camera layer on its own FFV1 track

Project MKV: screen, lossless BGRA layer, microphone; settings in the header.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Export: stills for the preview and the final movie

**Files:**
- Modify: `linux/export.c`, `linux/youcam.h` (export section), `linux/test_media.c`

**Interfaces:**
- Produces:
  ```c
  int export_available(void);
  int project_stills(const char *path, int width, cairo_surface_t **screen, cairo_surface_t **layer, GError **err);
  typedef struct exporter exporter;
  typedef struct {
      void (*progress)(double fraction, void *ud);    /* 0...1 */
      void (*finished)(const char *path, void *ud);   /* The movie, renamed from .part. */
      void (*failed)(const char *message, void *ud);  /* .part deleted, project untouched. */
      void *ud;
  } export_callbacks;
  exporter *export_start(const char *project, const project_info *info, double alpha, const char *out_dir,
                         export_callbacks cb, GError **err);
  void export_cancel(exporter *e);                    /* No callbacks; .part deleted. */
  ```
  `screen` is `CAIRO_FORMAT_RGB24`, `layer` `CAIRO_FORMAT_ARGB32` (premultiplied, as drawn by `camera_layer_draw`), both `width` wide, same height.
- Consumes: `video_encoder_chain()`, `SRGB_LABEL` (Task 3), `export_output_path()`, `container_for()` (Task 2).

- [ ] **Step 1: Write the failing media tests**

In `linux/test_media.c`:

Replace the whole `red_pixels_on_row` function (and its comment) with:

```c
/* Decodes frame number 'index' of a movie as packed RGB. Caller unrefs. */
static GstSample *frame_at(const char *path, int index, int *width) {
    char *desc = g_strdup_printf("filesrc location=\"%s\" ! decodebin ! videoconvert ! "
                                 "video/x-raw,format=RGB ! appsink name=sink sync=false", path);
    GstElement *p = gst_parse_launch(desc, NULL);
    g_free(desc);
    GstElement *sink = gst_bin_get_by_name(GST_BIN(p), "sink");
    gst_element_set_state(p, GST_STATE_PLAYING);
    GstSample *sample = NULL;
    for (int i = 0; i <= index; i++) {
        if (sample) gst_sample_unref(sample);
        g_signal_emit_by_name(sink, "pull-sample", &sample);
        if (!sample) break;
    }
    *width = 0;
    if (sample) gst_structure_get_int(gst_caps_get_structure(gst_sample_get_caps(sample), 0), "width", width);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(p);
    return sample;
}

#define RGB_STRIDE(w) (((w) * 3 + 3) & ~3)

/* Counts strongly red pixels on 'row' of frame 'index'. */
static int red_pixels_on_row(const char *path, int index, int row) {
    int w, count = 0;
    GstSample *sample = frame_at(path, index, &w);
    if (!sample) return 0;
    GstMapInfo map;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    gst_buffer_map(buf, &map, GST_MAP_READ);
    for (int x = 0; x < w; x++) {
        const guint8 *px = map.data + row * RGB_STRIDE(w) + x * 3;
        if (px[0] > 200 && px[1] < 60 && px[2] < 60) count++;
    }
    gst_buffer_unmap(buf, &map);
    gst_sample_unref(sample);
    return count;
}

static void pixel_at(const char *path, int index, int x, int y, guint8 rgb[3]) {
    int w;
    GstSample *sample = frame_at(path, index, &w);
    EXPECT(sample && x < w, "frame %d of %s decodes", index, path);
    GstMapInfo map;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    gst_buffer_map(buf, &map, GST_MAP_READ);
    memcpy(rgb, map.data + (size_t)y * RGB_STRIDE(w) + (size_t)x * 3, 3);
    gst_buffer_unmap(buf, &map);
    gst_sample_unref(sample);
}
```

After `check_project_tracks()` add:

```c
#define CX (SQUARE_X + SQUARE / 2)   /* Camera centre in movie pixels. */
#define CY (SQUARE_Y + SQUARE / 2)
#define MIX_TOLERANCE 30             /* Two H.264 passes. */

static guint32 surface_pixel(cairo_surface_t *s, int x, int y) {
    cairo_surface_flush(s);
    return *(guint32 *)(cairo_image_surface_get_data(s) + y * cairo_image_surface_get_stride(s) + x * 4);
}

/* The dialog's stills: the blue screen, and the layer with the opaque red
 * square on transparency. 640 wide, the movie's own width. */
static void check_stills(const char *path) {
    cairo_surface_t *screen, *layer;
    GError *err = NULL;
    EXPECT(project_stills(path, 640, &screen, &layer, &err), "stills of %s: %s", path, err ? err->message : "?");
    guint32 s = surface_pixel(screen, CX, CY), in = surface_pixel(layer, CX, CY), out = surface_pixel(layer, 10, 10);
    EXPECT((s & 0xff) > 200 && (s >> 16 & 0xff) < 60, "the screen still shows the screen (%08x)", s);
    EXPECT(in >> 24 == 255 && (in >> 16 & 0xff) > 200, "the layer still has the opaque camera (%08x)", in);
    EXPECT(out >> 24 == 0, "the layer still is transparent elsewhere (%08x)", out);
    cairo_surface_destroy(screen);
    cairo_surface_destroy(layer);
}

static char *ExportResult, *ExportError;
static void export_done(const char *path, void *ud) { (void)ud; ExportResult = g_strdup(path); g_main_loop_quit(Loop); }
static void export_fail(const char *msg, void *ud) { (void)ud; ExportError = g_strdup(msg); g_main_loop_quit(Loop); }
static void export_progress(double fraction, void *ud) { (void)fraction; (void)ud; }

/* Exports the project at final transparency t into info->container. */
static char *export_project(const project_info *info, double t, const char *dir) {
    GError *err = NULL;
    ExportResult = ExportError = NULL;
    exporter *e = export_start(ProjectPath, info, later_alpha(t, info->t_a), dir,
                               (export_callbacks){export_progress, export_done, export_fail, NULL}, &err);
    EXPECT(e, "export did not start: %s", err ? err->message : "?");
    g_main_loop_run(Loop);
    EXPECT(!ExportError, "export failed: %s", ExportError);
    char *part = g_strconcat(ExportResult, ".part", NULL);
    EXPECT(!g_file_test(part, G_FILE_TEST_EXISTS), "the temporary .part name is gone");
    g_free(part);
    return ExportResult;
}
```

In `main()`, replace

```c
    g_unlink(ProjectPath);
    g_unlink(KilledPath);
    g_rmdir(dir);
    printf("media ok: %.2fs 640x360 .mov with audio, pause removed; silent movie has no audio track; "
           "camera layer movie %.2fs with %d composited frames; mp4, mkv and avi ok; "
           "project %.2fs with screen, FFV1 layer and audio, settings read back after a finish and a kill\n",
           duration, layered, composited, project_s);
```
with
```c
    EXPECT(export_available(), "FFV1 and the H.264 decoder are installed (gst-libav)");
    check_stills(ProjectPath);
    check_stills(KilledPath);   /* Its duration is a guess: the first frame is used. */

    /* t = 0: the opaque camera; t = T_A: the screen alone; halfway: an even mix. */
    guint8 rgb[3];
    char *movie = export_project(&project, 0, dir);
    pixel_at(movie, 45, CX, CY, rgb);
    EXPECT(rgb[0] > 200 && rgb[1] < 60 && rgb[2] < 60, "t = 0 shows the opaque camera (%u %u %u)", rgb[0], rgb[1], rgb[2]);
    g_unlink(movie);
    g_free(movie);
    movie = export_project(&project, project.t_a, dir);
    pixel_at(movie, 45, CX, CY, rgb);
    EXPECT(rgb[0] < 60 && rgb[1] < 60 && rgb[2] > 200, "t = T_A shows only the screen (%u %u %u)", rgb[0], rgb[1], rgb[2]);
    g_unlink(movie);
    g_free(movie);
    movie = export_project(&project, project.t_a / 2, dir);
    pixel_at(movie, 45, CX, CY, rgb);
    EXPECT(abs(rgb[0] - 128) < MIX_TOLERANCE && abs(rgb[2] - 128) < MIX_TOLERANCE && rgb[1] < 60,
           "t = T_A / 2 mixes camera and screen evenly (%u %u %u)", rgb[0], rgb[1], rgb[2]);
    g_unlink(movie);
    g_free(movie);

    /* Every container, the audio copied, the length kept. */
    for (int i = 0; i < CONTAINER_COUNT; i++) {
        project_info as = project;
        g_strlcpy(as.container, Containers[i].ext, sizeof(as.container));
        movie = export_project(&as, project.t_a / 2, dir);
        EXPECT(g_str_has_suffix(movie, Containers[i].ext), "%s export has its extension", Containers[i].label);
        info = discover(movie);
        double d = (double)gst_discoverer_info_get_duration(info) / GST_SECOND;
        videos = gst_discoverer_info_get_video_streams(info);
        audios = gst_discoverer_info_get_audio_streams(info);
        EXPECT(videos && audios, "%s export holds video and the copied audio", Containers[i].label);
        EXPECT(fabs(d - project_s) < 0.5, "%s export lasts as long as the project (%.2fs, %.2fs)",
               Containers[i].label, d, project_s);
        gst_discoverer_stream_info_list_free(videos);
        gst_discoverer_stream_info_list_free(audios);
        gst_discoverer_info_unref(info);
        g_unlink(movie);
        g_free(movie);
    }

    /* A failed export keeps the project. */
    GError *bad_err = NULL;
    ExportResult = ExportError = NULL;
    exporter *bad = export_start(ProjectPath, &project, 1, "/nonexistent-youcam-dir",
                                 (export_callbacks){export_progress, export_done, export_fail, NULL}, &bad_err);
    if (bad) g_main_loop_run(Loop);
    EXPECT(!bad || ExportError, "an unwritable folder fails the export");
    EXPECT(g_file_test(ProjectPath, G_FILE_TEST_EXISTS), "a failed export keeps the project");
    g_clear_error(&bad_err);

    g_unlink(ProjectPath);
    g_unlink(KilledPath);
    g_rmdir(dir);
    printf("media ok: %.2fs 640x360 .mov with audio, pause removed; silent movie has no audio track; "
           "camera layer movie %.2fs with %d composited frames; mp4, mkv and avi ok; "
           "project %.2fs with screen, FFV1 layer and audio, settings read back after a finish and a kill; "
           "stills ok; exports at t = 0, T_A/2 and T_A ok, to MOV, MP4, MKV and AVI with audio\n",
           duration, layered, composited, project_s);
```

- [ ] **Step 2: Run it to see it fail**

```bash
make test-media 2>&1 | grep -m3 error
```
Expected: `error: unknown type name 'exporter'` and implicit declarations of `project_stills`, `export_start`.

- [ ] **Step 3: Declarations in `youcam.h`**

At the end of the export section add:

```c
int export_available(void);   /* avenc_ffv1, avdec_ffv1 and avdec_h264 are installed (gst-libav). */
/* Two stills from the middle of the project (the first frame of a short or
 * killed one): the screen (RGB24) and the camera layer (ARGB32), 'width' wide. */
int project_stills(const char *path, int width, cairo_surface_t **screen, cairo_surface_t **layer, GError **err);

typedef struct exporter exporter;
typedef struct {
    void (*progress)(double fraction, void *ud);     /* 0...1, polled every 200 ms. */
    void (*finished)(const char *path, void *ud);    /* The movie, renamed from .part. */
    void (*failed)(const char *message, void *ud);   /* .part deleted, the project untouched. */
    void *ud;
} export_callbacks;
exporter *export_start(const char *project, const project_info *info, double alpha, const char *out_dir,
                       export_callbacks cb, GError **err);
void export_cancel(exporter *e);   /* No callback follows; .part deleted. */
```

- [ ] **Step 4: Pipelines in `export.c`**

After the `#define EXT_CHARS …` line add:

```c
#define PROGRESS_MS 200
#define STILL_TIMEOUT_S 5
/* The screen and the layer wait for each other at the compositor: queues
 * bounded by time only, since an FFV1 frame can be megabytes. */
#define TRACK_QUEUE "queue max-size-buffers=0 max-size-bytes=0 max-size-time=2000000000"
/* output-corrupt=false: a damaged GOP is skipped instead of shown gray. */
#define SCREEN_DECODE "h264parse ! avdec_h264 output-corrupt=false ! videoconvert"
#define SCREEN_STILL "d.video_0 ! queue ! " SCREEN_DECODE " ! video/x-raw,format=BGRx"
#define LAYER_STILL "d.video_1 ! queue ! avdec_ffv1 ! videoconvert ! video/x-raw,format=BGRA"
```

Append to the end of `export.c`:

```c
static int has_factory(const char *name) {
    GstElementFactory *f = gst_element_factory_find(name);
    int found = f != NULL;
    if (f) gst_object_unref(f);
    return found;
}

/* The layer codec and the screen decoder come with gst-libav. Checked once. */
int export_available(void) {
    static int known, available;
    if (!known) {
        available = has_factory("avenc_ffv1") && has_factory("avdec_ffv1") && has_factory("avdec_h264");
        known = 1;
    }
    return available;
}

/* Prerolls one track of a project at 'at' (at < 0: the middle) with an
 * accurate seek. One pipeline per track, the others left unlinked: with
 * both video tracks linked the demuxer seeks by the FFV1 index and starts
 * the H.264 mid-GOP (a gray picture). */
static GstSample *still_at(const char *path, const char *track, gint64 at) {
    char *desc = g_strdup_printf("filesrc name=src ! matroskademux name=d %s ! appsink name=sink", track);
    GstElement *p = gst_parse_launch(desc, NULL);
    g_free(desc);
    if (!p) return NULL;
    GstElement *src = gst_bin_get_by_name(GST_BIN(p), "src"), *sink = gst_bin_get_by_name(GST_BIN(p), "sink");
    g_object_set(src, "location", path, NULL);
    GstSample *sample = NULL;
    gint64 duration = 0;
    if (gst_element_set_state(p, GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE &&
        gst_element_get_state(p, NULL, NULL, STILL_TIMEOUT_S * GST_SECOND) == GST_STATE_CHANGE_SUCCESS) {
        if (at < 0) at = gst_element_query_duration(p, GST_FORMAT_TIME, &duration) && duration > 0 ? duration / 2 : 0;
        if (gst_element_seek_simple(p, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, at) &&
            gst_element_get_state(p, NULL, NULL, STILL_TIMEOUT_S * GST_SECOND) == GST_STATE_CHANGE_SUCCESS)
            g_signal_emit_by_name(sink, "try-pull-preroll", (GstClockTime)(STILL_TIMEOUT_S * GST_SECOND), &sample);
    }
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(src);
    gst_object_unref(sink);
    gst_object_unref(p);
    return sample;
}

/* A decoded BGRx/BGRA frame (rows packed, 4 bytes per pixel: cairo's own
 * layout on little-endian) as a cairo surface scaled to 'width'. */
static cairo_surface_t *sample_surface(GstSample *sample, cairo_format_t format, int width) {
    int w = 0, h = 0;
    GstStructure *s = gst_caps_get_structure(gst_sample_get_caps(sample), 0);
    gst_structure_get_int(s, "width", &w);
    gst_structure_get_int(s, "height", &h);
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (w <= 0 || h <= 0 || !gst_buffer_map(buf, &map, GST_MAP_READ)) return NULL;
    if (map.size < (gsize)w * h * 4) {
        gst_buffer_unmap(buf, &map);
        return NULL;
    }
    cairo_surface_t *full = cairo_image_surface_create(format, w, h);
    int stride = cairo_image_surface_get_stride(full);
    guint8 *dst = cairo_image_surface_get_data(full);
    for (int row = 0; row < h; row++)
        memcpy(dst + (size_t)row * stride, map.data + (size_t)row * w * 4, (size_t)w * 4);
    gst_buffer_unmap(buf, &map);
    cairo_surface_mark_dirty(full);
    int height = MAX(1, (int)lround((double)h * width / w));
    cairo_surface_t *out = cairo_image_surface_create(format, width, height);
    cairo_t *cr = cairo_create(out);
    cairo_scale(cr, (double)width / w, (double)height / h);
    cairo_set_source_surface(cr, full, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(full);
    return out;
}

/* The screen at the middle, then the layer at that very frame's time. */
int project_stills(const char *path, int width, cairo_surface_t **screen, cairo_surface_t **layer, GError **err) {
    *screen = *layer = NULL;
    GstSample *a = still_at(path, SCREEN_STILL, -1);
    if (!a) a = still_at(path, SCREEN_STILL, 0);   /* Short, or killed: its duration is only a guess. */
    GstClockTime at = a ? GST_BUFFER_PTS(gst_sample_get_buffer(a)) : GST_CLOCK_TIME_NONE;
    GstSample *l = a ? still_at(path, LAYER_STILL, GST_CLOCK_TIME_IS_VALID(at) ? (gint64)at : 0) : NULL;
    if (a && l) {
        *screen = sample_surface(a, CAIRO_FORMAT_RGB24, width);
        *layer = sample_surface(l, CAIRO_FORMAT_ARGB32, width);
    }
    if (a) gst_sample_unref(a);
    if (l) gst_sample_unref(l);
    if (*screen && *layer) return 1;
    if (*screen) cairo_surface_destroy(*screen);
    if (*layer) cairo_surface_destroy(*layer);
    *screen = *layer = NULL;
    g_set_error(err, GST_STREAM_ERROR, GST_STREAM_ERROR_DECODE, "The recording cannot be read.");
    return 0;
}

struct exporter {
    GstElement *pipeline, *comp, *demux;
    char *part, *path;
    export_callbacks cb;
    guint poll;
    gint64 started;
};

static void export_free(exporter *e) {
    if (e->poll) g_source_remove(e->poll);
    GstBus *bus = gst_element_get_bus(e->pipeline);
    gst_bus_remove_signal_watch(bus);
    gst_object_unref(bus);
    gst_element_set_state(e->pipeline, GST_STATE_NULL);   /* File sources only: safe on the main loop. */
    gst_object_unref(e->comp);
    gst_object_unref(e->demux);
    gst_object_unref(e->pipeline);
    g_free(e->part);
    g_free(e->path);
    g_free(e);
}

/* Position of the compositor's output over the project's duration. A
 * killed project's duration is a guess, so the bar may lag there. */
static gboolean export_poll(gpointer data) {
    exporter *e = data;
    gint64 pos = 0, dur = 0;
    if (gst_element_query_position(e->comp, GST_FORMAT_TIME, &pos) &&
        gst_element_query_duration(e->demux, GST_FORMAT_TIME, &dur) && dur > 0)
        e->cb.progress(fmin(1, fmax(0, (double)pos / dur)), e->cb.ud);
    return G_SOURCE_CONTINUE;
}

static void export_eos(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus; (void)msg;
    exporter *e = data;
    char *path = g_strdup(e->path), *part = g_strdup(e->part);
    double seconds = (g_get_monotonic_time() - e->started) / 1e6;
    export_callbacks cb = e->cb;
    export_free(e);
    if (g_rename(part, path) == 0) {
        g_print("youcam: exported %s in %.1fs\n", path, seconds);
        cb.finished(path, cb.ud);
    } else {
        g_unlink(part);
        cb.failed("The movie could not be given its final name.", cb.ud);
    }
    g_free(path);
    g_free(part);
}

static void export_error(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus;
    exporter *e = data;
    GError *err = NULL;
    char *debug = NULL;
    gst_message_parse_error(msg, &err, &debug);
    g_printerr("youcam: export error from %s: %s\n%s\n", GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)),
               err ? err->message : "?", debug ? debug : "");
    char *text = g_strdup(err ? err->message : "The movie could not be written.");
    g_clear_error(&err);
    g_free(debug);
    char *part = g_strdup(e->part);
    export_callbacks cb = e->cb;
    export_free(e);
    g_unlink(part);
    cb.failed(text, cb.ud);
    g_free(text);
    g_free(part);
}

/* As fast as decoding and encoding allow (no live source, no clock). */
exporter *export_start(const char *project, const project_info *info, double alpha, const char *out_dir,
                       export_callbacks cb, GError **err) {
    char *desc = g_strdup_printf(
        "filesrc name=src ! matroskademux name=d "
        "d.video_0 ! " TRACK_QUEUE " ! " SCREEN_DECODE " ! video/x-raw,format=BGRA ! " SRGB_LABEL " ! comp.sink_0 "
        "d.video_1 ! " TRACK_QUEUE " ! avdec_ffv1 ! videoconvert ! video/x-raw,format=BGRA ! " SRGB_LABEL " ! comp.sink_1 "
        "compositor name=comp background=black ! video/x-raw,format=BGRA,framerate=30/1 ! "
        "%s ! h264parse ! queue ! mux. %s%s name=mux ! filesink name=file",
        video_encoder_chain(), info->has_audio ? "d.audio_0 ! " TRACK_QUEUE " ! aacparse ! queue ! mux. " : "",
        container_for(info->container)->muxer);
    GstElement *p = gst_parse_launch(desc, err);
    g_free(desc);
    if (!p) return NULL;
    exporter *e = g_new0(exporter, 1);
    e->pipeline = p;
    e->cb = cb;
    e->path = export_output_path(out_dir, project, info->container);
    e->part = g_strconcat(e->path, ".part", NULL);
    GstElement *src = gst_bin_get_by_name(GST_BIN(p), "src"), *file = gst_bin_get_by_name(GST_BIN(p), "file");
    g_object_set(src, "location", project, NULL);
    g_object_set(file, "location", e->part, NULL);
    gst_object_unref(src);
    gst_object_unref(file);
    e->comp = gst_bin_get_by_name(GST_BIN(p), "comp");
    e->demux = gst_bin_get_by_name(GST_BIN(p), "d");
    GstPad *layer = gst_element_get_static_pad(e->comp, "sink_1");
    g_object_set(layer, "alpha", alpha, "zorder", 1, NULL);   /* Set here: a %f in the description would follow the locale. */
    gst_object_unref(layer);
    GstBus *bus = gst_element_get_bus(p);
    gst_bus_add_signal_watch(bus);
    g_signal_connect(bus, "message::eos", G_CALLBACK(export_eos), e);
    g_signal_connect(bus, "message::error", G_CALLBACK(export_error), e);
    gst_object_unref(bus);
    e->started = g_get_monotonic_time();
    if (gst_element_set_state(p, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        g_set_error(err, GST_CORE_ERROR, GST_CORE_ERROR_STATE_CHANGE, "The export could not start.");
        export_cancel(e);
        return NULL;
    }
    e->poll = g_timeout_add(PROGRESS_MS, export_poll, e);
    return e;
}

void export_cancel(exporter *e) {
    char *part = g_strdup(e->part);
    export_free(e);
    g_unlink(part);
    g_free(part);
}
```

Also extend the file comment of `export.c` with one paragraph at its end:

```c
 *
 * The dialog's preview comes from two stills of the same frame, decoded by
 * project_stills(); the export decodes both tracks, composites them, and
 * encodes with the recorder's own encoder chain (video_encoder_chain()) into
 * the container chosen when the recording started; the AAC is copied. */
```
(replace the closing ` * COMMENT tag for players and ffprobe. */` line by ` * COMMENT tag for players and ffprobe.` followed by the paragraph above.)

- [ ] **Step 5: Run the media test**

```bash
make test-media && ./test-media
```
Expected: one line starting `media ok:` and ending `stills ok; exports at t = 0, T_A/2 and T_A ok, to MOV, MP4, MKV and AVI with audio`. With the unwritable-folder case GStreamer prints a `youcam: export error from file: …` line on stderr; that is expected.

- [ ] **Step 6: Full build without warnings, all tests**

```bash
make clean >/dev/null && make youcam tests test-media test-segment 2>&1 | grep -E "warning|error" || echo "no warnings"
make test 2>&1 | grep -E "^Passed|^media ok"
```
Expected: `no warnings`; `Passed 293 logic checks.`, `Passed 53 segmentation checks.`, the `media ok:` line of Step 5.

- [ ] **Step 7: Commit**

```bash
git add linux/export.c linux/youcam.h linux/test_media.c
git commit -m "feat: export a project at a chosen camera transparency

Stills for the preview, compositor at 1 - t / T_A, same encoder and muxers, AAC copied.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: The stop dialog

**Files:**
- Create: `linux/later_dialog.c`
- Modify: `linux/youcam.h` (recording and new later_dialog sections), `linux/recording.c` (two small public functions), `linux/app.c` (quit, snapshot), `linux/Makefile`

**Interfaces:**
- Produces:
  ```c
  void later_dialog_open(const char *project);   /* One window; presents it if already open. */
  GtkWidget *later_dialog_window(void);          /* NULL when closed (--snapshot). */
  void later_dialog_shutdown(void);              /* Quit: cancels a running export. */
  const char *recording_output_dir(void);        /* The Desktop (was static recordings_dir). */
  void recording_set_last_path(const char *path);  /* "Saved to Desktop" + Show Last Recording. */
  ```
  Developer check: `YOUCAM_SNAPSHOT_PROJECT=<project> ./youcam --snapshot DIR` also writes `DIR/transparency.png`.
- Consumes: `project_read_info`, `project_stills`, `later_alpha`, `later_max`, `export_start`, `export_cancel` (Tasks 2, 4).

- [ ] **Step 1: Make a synthetic project for the checks (no real screen, no face)**

```bash
CHECK=/home/patane/.cache/claude-tmp/claude-1000/-home-patane-Works-YouCam-A-youcam/46c95e59-cadb-4b73-9cd8-9e427ef68d24/scratchpad/tl-check
mkdir -p "$CHECK"
gst-launch-1.0 -q videotestsrc num-buffers=150 ! video/x-raw,width=1280,height=720,framerate=30/1 ! videoconvert ! \
  video/x-raw,format=I420 ! x264enc tune=zerolatency key-int-max=60 ! h264parse ! queue ! mux. \
  videotestsrc num-buffers=150 pattern=ball ! video/x-raw,format=BGRA,width=200,height=200,framerate=30/1 ! \
  videobox border-alpha=0 left=-1000 top=-440 right=-80 bottom=-80 ! video/x-raw,width=1280,height=720 ! \
  avenc_ffv1 ! queue ! mux. audiotestsrc num-buffers=230 ! avenc_aac ! queue ! mux. \
  matroskamux name=mux writing-app="YouCam project;YOUCAM_T_A=0.400;YOUCAM_PREVIEW_TRANSPARENCY=0.000;YOUCAM_CONTAINER=mp4;YOUCAM_AUDIO=1;" ! \
  filesink location="$CHECK/synthetic.youcam.mkv"
ffprobe -v error -show_entries stream=codec_name -of csv=p=0 "$CHECK/synthetic.youcam.mkv" | tr '\n' ' '; echo
```
Expected: `h264 ffv1 aac`.

- [ ] **Step 2: Public helpers in `recording.c` and `youcam.h`**

In `recording.c` replace

```c
static const char *recordings_dir(void) {
```
with
```c
const char *recording_output_dir(void) {
```
and the one use `recording_next_path(recordings_dir(), …)` with `recording_next_path(recording_output_dir(), …)`. After `recording_last_path` add:

```c
/* A movie finished elsewhere (the Choose After Recording export). */
void recording_set_last_path(const char *path) {
    prefs_set_string(&R.last_path, path);
    changed();
}
```

In `youcam.h`, recording section, after `const char *recording_last_path(void);` add:

```c
void recording_set_last_path(const char *path);
const char *recording_output_dir(void);   /* The Desktop, else the home folder. */
```

Before the `app.c` section add:

```c
/* ------------------------------------------------------- later_dialog.c --- */

void later_dialog_open(const char *project);
GtkWidget *later_dialog_window(void);   /* NULL when closed. */
void later_dialog_shutdown(void);
```

- [ ] **Step 3: Create `linux/later_dialog.c`**

```c
/* Choose After Recording (Linux addition): the window that finishes a
 * project. One frame from the middle of the recording is decoded once, as
 * two stills: the screen and the camera layer. Moving the slider only
 * repaints them with cairo, the layer at the opacity the export will use
 * (later_alpha), so it is instant. Save exports the movie to the Desktop
 * and deletes the project; Later and closing the window keep it for
 * "Adjust Last Recording Transparency…"; Discard deletes it after asking.
 * One window at a time. */

#include <math.h>
#include <string.h>
#include <glib/gstdio.h>
#include "youcam.h"

#define PREVIEW_WIDTH 640
#define SPACING 10
#define BORDER 16
#define NOTE_CHARS 70

static struct {
    GtkWidget *win, *slider, *value, *progress, *error, *save, *later, *discard;
    char *project;
    project_info info;
    cairo_surface_t *screen, *layer;   /* PREVIEW_WIDTH wide; NULL when unreadable. */
    exporter *job;
} D;

static double chosen(void) { return D.slider ? gtk_range_get_value(GTK_RANGE(D.slider)) / 100 : 0; }

static void close_window(void) {
    if (D.job) export_cancel(D.job);   /* Deletes the .part; the project stays. */
    if (D.screen) cairo_surface_destroy(D.screen);
    if (D.layer) cairo_surface_destroy(D.layer);
    GtkWidget *win = D.win;
    g_free(D.project);
    memset(&D, 0, sizeof(D));
    if (win) gtk_widget_destroy(win);
}

static gboolean on_delete(GtkWidget *w, GdkEvent *e, gpointer d) {
    (void)w; (void)e; (void)d;
    close_window();
    return TRUE;
}

/* The spec's still B (the layer over the screen) shown over A (the screen)
 * at opacity alpha is the layer itself painted at alpha over A. */
static gboolean draw_preview(GtkWidget *w, cairo_t *cr, gpointer d) {
    (void)w; (void)d;
    cairo_set_source_surface(cr, D.screen, 0, 0);
    cairo_paint(cr);
    cairo_set_source_surface(cr, D.layer, 0, 0);
    cairo_paint_with_alpha(cr, later_alpha(chosen(), D.info.t_a));
    return TRUE;
}

static void slider_moved(GtkRange *r, gpointer area) {
    char text[16];
    snprintf(text, sizeof(text), "%.0f%%", gtk_range_get_value(r));
    gtk_label_set_text(GTK_LABEL(D.value), text);
    gtk_widget_queue_draw(GTK_WIDGET(area));
}

static void set_busy(int busy) {
    if (D.slider) gtk_widget_set_sensitive(D.slider, !busy && later_max(D.info.t_a) > 0);
    if (D.save) gtk_widget_set_sensitive(D.save, !busy);
    if (D.later) gtk_widget_set_sensitive(D.later, !busy);
    gtk_widget_set_sensitive(D.discard, !busy);
    gtk_widget_set_visible(D.progress, busy);
    if (busy) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(D.progress), 0);
}

static void export_progress(double fraction, void *ud) {
    (void)ud;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(D.progress), fraction);
}

static void export_finished(const char *path, void *ud) {
    (void)ud;
    D.job = NULL;
    if (g_unlink(D.project) != 0) g_printerr("youcam: could not delete the project %s\n", D.project);
    recording_set_last_path(path);
    close_window();
}

static void export_failed(const char *message, void *ud) {
    (void)ud;
    D.job = NULL;
    char *text = g_strdup_printf("The movie could not be saved: %s\nThe recording is kept: you can try again.", message);
    gtk_label_set_text(GTK_LABEL(D.error), text);
    gtk_widget_show(D.error);
    g_free(text);
    set_busy(0);
}

static void on_save(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    GError *err = NULL;
    gtk_widget_hide(D.error);
    set_busy(1);
    D.job = export_start(D.project, &D.info, later_alpha(chosen(), D.info.t_a), recording_output_dir(),
                         (export_callbacks){export_progress, export_finished, export_failed, NULL}, &err);
    if (!D.job) {
        export_failed(err ? err->message : "the export could not start.", NULL);
        g_clear_error(&err);
    }
}

static void on_later(GtkButton *b, gpointer d) { (void)b; (void)d; close_window(); }

static void on_discard(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    GtkWidget *ask = gtk_message_dialog_new(GTK_WINDOW(D.win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                            GTK_BUTTONS_NONE, "Discard this recording?");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(ask), "It is deleted and cannot be recovered.");
    gtk_dialog_add_buttons(GTK_DIALOG(ask), "Cancel", GTK_RESPONSE_CANCEL, "Discard", GTK_RESPONSE_ACCEPT, NULL);
    char *asked = g_strdup(D.project);
    int answer = gtk_dialog_run(GTK_DIALOG(ask));
    gtk_widget_destroy(ask);
    /* While asking, a new recording may have replaced the window's project:
     * delete the one the user was asked about, close only if still shown. */
    if (answer == GTK_RESPONSE_ACCEPT) {
        if (g_unlink(asked) != 0) g_printerr("youcam: could not delete %s\n", asked);
        if (D.project && strcmp(D.project, asked) == 0) close_window();
    }
    g_free(asked);
}

static GtkWidget *add_button(GtkWidget *box, const char *label, GCallback cb) {
    GtkWidget *b = gtk_button_new_with_label(label);
    g_signal_connect(b, "clicked", cb, NULL);
    gtk_container_add(GTK_CONTAINER(box), b);
    return b;
}

static GtkWidget *add_label(GtkWidget *box, const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), NOTE_CHARS);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    return l;
}

static void add_preview(GtkWidget *box) {
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, cairo_image_surface_get_width(D.screen), cairo_image_surface_get_height(D.screen));
    g_signal_connect(area, "draw", G_CALLBACK(draw_preview), NULL);
    gtk_box_pack_start(GTK_BOX(box), area, FALSE, FALSE, 0);

    double max = later_max(D.info.t_a);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Camera Transparency"), FALSE, FALSE, 0);
    /* A scale needs min < max: with nothing to choose (opaque preview, no
     * extension) it sits at 0, disabled (set_busy). */
    D.slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, max > 0 ? max * 100 : 1, 1);
    gtk_scale_set_draw_value(GTK_SCALE(D.slider), FALSE);
    gtk_range_set_round_digits(GTK_RANGE(D.slider), 0);
    atk_object_set_name(gtk_widget_get_accessible(D.slider), "Camera transparency of the saved video in percent");
    gtk_box_pack_start(GTK_BOX(row), D.slider, TRUE, TRUE, 0);
    D.value = gtk_label_new("");
    gtk_label_set_width_chars(GTK_LABEL(D.value), 4);
    gtk_box_pack_start(GTK_BOX(row), D.value, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    g_signal_connect(D.slider, "value-changed", G_CALLBACK(slider_moved), area);
    gtk_range_set_value(GTK_RANGE(D.slider), round(fmin(D.info.preview, max) * 100));
    slider_moved(GTK_RANGE(D.slider), area);   /* The label, also when the value stayed 0. */
    if (D.info.t_a < 1) {
        char note[160];
        snprintf(note, sizeof(note), "Up to %.0f%%: enable the YouCam Capture Exclusion extension to choose up to 80%%.",
                 max * 100);
        add_label(box, note);
    }
}

void later_dialog_open(const char *project) {
    if (D.win && (D.job || strcmp(D.project, project) == 0)) {
        gtk_window_present(GTK_WINDOW(D.win));   /* While saving, a newer project waits for the menu. */
        return;
    }
    close_window();   /* The project it showed stays in pending/. */
    D.project = g_strdup(project);
    GError *err = NULL;
    int readable = project_read_info(project, &D.info) &&
                   project_stills(project, PREVIEW_WIDTH, &D.screen, &D.layer, &err);
    if (!readable)
        g_printerr("youcam: cannot read the project %s: %s\n", project, err ? err->message : "no YouCam settings");
    g_clear_error(&err);

    D.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWindow *w = GTK_WINDOW(D.win);
    gtk_window_set_title(w, "Camera Transparency");
    gtk_window_set_role(w, CAPTURE_HIDDEN_ROLE);   /* Out of a new recording, with the extension. */
    gtk_window_set_resizable(w, FALSE);
    gtk_window_set_position(w, GTK_WIN_POS_CENTER);
    g_signal_connect(D.win, "delete-event", G_CALLBACK(on_delete), NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING);
    gtk_container_set_border_width(GTK_CONTAINER(box), BORDER);
    gtk_container_add(GTK_CONTAINER(D.win), box);
    if (readable) add_preview(box);
    else add_label(box, "This recording cannot be read. It can only be discarded.");
    D.error = add_label(box, "");
    D.progress = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(box), D.progress, FALSE, FALSE, 0);
    GtkWidget *buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(buttons), GTK_BUTTONBOX_END);
    gtk_box_set_spacing(GTK_BOX(buttons), SPACING);
    D.discard = add_button(buttons, "Discard…", G_CALLBACK(on_discard));
    if (readable) {
        D.later = add_button(buttons, "Later", G_CALLBACK(on_later));
        D.save = add_button(buttons, "Save", G_CALLBACK(on_save));
        gtk_style_context_add_class(gtk_widget_get_style_context(D.save), GTK_STYLE_CLASS_SUGGESTED_ACTION);
    }
    gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);
    gtk_widget_show_all(D.win);
    gtk_widget_hide(D.error);
    set_busy(0);
}

GtkWidget *later_dialog_window(void) { return D.win; }

void later_dialog_shutdown(void) { close_window(); }
```

- [ ] **Step 4: Build it in, quit cleanly, developer snapshot**

`Makefile`: in `APP := …` add `later_dialog.o` after `camera.o`:

```make
APP := main.o app.o overlay.o panel.o region.o area_overlay.o statusicon.o \
       recording.o portal.o hotkeys.o camera.o later_dialog.o $(MEDIA) $(CORE)
```

`app.c`, `app_quit()`: after `region_cancel();` add

```c
    later_dialog_shutdown();   /* A running export is cancelled; the project stays. */
```

`app.c`, `take_snapshot()`: after `render(App.panel, "panel.png");` add

```c
    if (later_dialog_window()) render(later_dialog_window(), "transparency.png");
```

`app.c`, `app_snapshot()`: replace its body with

```c
    SnapshotDir = g_strdup(dir);
    app_toggle_panel();
    const char *project = g_getenv("YOUCAM_SNAPSHOT_PROJECT");   /* Developer check of the stop dialog. */
    if (project) later_dialog_open(project);
    g_timeout_add_seconds(4, take_snapshot, NULL);
```

- [ ] **Step 5: Build without warnings**

```bash
make youcam 2>&1 | grep -E "warning|error" || echo "no warnings"
```
Expected: `no warnings`.

- [ ] **Step 6: Render the dialog on the synthetic project**

`--snapshot` also starts the real camera: `camera.png` shows the user's face and is deleted unopened in the same command. Only `transparency.png` (synthetic content) may be viewed.

```bash
CHECK=/home/patane/.cache/claude-tmp/claude-1000/-home-patane-Works-YouCam-A-youcam/46c95e59-cadb-4b73-9cd8-9e427ef68d24/scratchpad/tl-check
YOUCAM_CONFIG="$CHECK/prefs.ini" YOUCAM_SNAPSHOT_PROJECT="$CHECK/synthetic.youcam.mkv" ./youcam --snapshot "$CHECK/snap" \
  | grep transparency; rm -f "$CHECK/snap/camera.png"
```
Expected: `snapshot: …/snap/transparency.png (WxH)` with W ≥ 672 (640 preview + 2 × 16 border). Open `$CHECK/snap/transparency.png`: the test bars (screen still) with the ball (layer) fully opaque at the bottom right: the synthetic project starts the slider at 0% (`YOUCAM_PREVIEW_TRANSPARENCY=0.000`), so α = 1; the value label reads `0%`; the line "Up to 40%: enable the YouCam Capture Exclusion extension to choose up to 80%."; the buttons Discard…, Later, Save. Also check `ls ~/.local/share/youcam/pending 2>/dev/null` is unchanged (the snapshot must not create projects).

- [ ] **Step 7: Unreadable project**

```bash
head -c 2000 /dev/urandom > "$CHECK/broken.youcam.mkv"
YOUCAM_CONFIG="$CHECK/prefs.ini" YOUCAM_SNAPSHOT_PROJECT="$CHECK/broken.youcam.mkv" ./youcam --snapshot "$CHECK/snap" 2>&1 \
  | grep -E "cannot read|transparency"; rm -f "$CHECK/snap/camera.png"
```
Expected: `youcam: cannot read the project …/broken.youcam.mkv: no YouCam settings` and `snapshot: …/transparency.png (…)`; the PNG shows "This recording cannot be read. It can only be discarded." and only Discard….

- [ ] **Step 8: All tests**

```bash
make test 2>&1 | grep -E "^Passed|^media ok" && make smoke-test 2>&1 | tail -2
```
Expected: `Passed 293 logic checks.`, `Passed 53 segmentation checks.`, the Task 4 `media ok:` line; smoke test prints its `Screen capture:` and `Microphone:` lines and exits 0.

- [ ] **Step 9: Commit**

```bash
git add linux/later_dialog.c linux/youcam.h linux/recording.c linux/app.c linux/Makefile
git commit -m "feat: dialog to choose the camera transparency of a recording

Preview from two stills blended with cairo; Save, Later, Discard; progress.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Recording a `later` project

**Files:**
- Modify: `linux/recording.c`, `linux/overlay.c`, `linux/app.c`, `linux/youcam.h`

**Interfaces:**
- Produces:
  ```c
  void overlay_set_capture_hidden(int hidden);   /* Title "youcam-hidden YouCam" / "YouCam". */
  void recording_set_project_handlers(void (*on_project)(const char *path), void (*on_notice)(const char *message));
  int recording_transparency_locked(void);       /* later + portal + busy: keep the preview transparency. */
  ```
  Behaviour: Saved Video = Choose After Recording (and `export_available()`) ⇒ the recording is a project in `project_dir()`; `T_A = 1` with Mutter (extension), else the preview transparency at start; the camera window title starts with `youcam-hidden` during a Mutter recording and is restored at stop; a project that cannot start falls back to an opaque movie with a notice; at stop the dialog opens (not when quitting or with `YOUCAM_NO_DIALOGS`).
- Consumes: `writer_start(…, WRITER_LAYER_TRACK, &project, …)`, `writer_push_layer`, `writer_layer_track` (Task 3); `export_available`, `project_dir`, `project_new_path`, `project_clean_parts` (Tasks 2, 4); `later_dialog_open` (Task 5).

- [ ] **Step 1: `overlay.c`: hide the camera from the screen track**

Add after `overlay_animate()`:

```c
/* Choose After Recording with the Capture Exclusion extension: the camera
 * stays on screen but out of the screen track, so its transparency can be
 * anything later. The extension hides windows whose title starts with
 * CAPTURE_HIDDEN_ROLE and follows title changes (notify::title); the role
 * cannot change once the window is mapped. */
void overlay_set_capture_hidden(int hidden) {
    if (Win) gtk_window_set_title(GTK_WINDOW(Win), hidden ? CAPTURE_HIDDEN_ROLE " YouCam" : "YouCam");
}
```

In `youcam.h`, overlay section, add `void overlay_set_capture_hidden(int hidden);`. In the recording section add:

```c
void recording_set_project_handlers(void (*on_project)(const char *path), void (*on_notice)(const char *message));
int recording_transparency_locked(void);
```

- [ ] **Step 2: `recording.c`: the project flow**

a) In the `R` struct, after `int mutter;` add:

```c
    int later;                /* A project: the camera transparency is chosen after recording. */
```
and after `void (*on_error)(const char *);` add:
```c
    void (*on_project)(const char *path);
    void (*on_notice)(const char *message);
```

b) After `#define TICK_MS 250` add:

```c
#define LATER_UNAVAILABLE "Choose After Recording needs the FFV1 codec of gst-libav. " \
                          "This recording keeps the camera opaque."
#define LATER_FAILED "The camera could not be recorded on its own track. This recording keeps the camera opaque."
```

c) After `static void changed(void) …` add:

```c
/* Not a failure: the recording goes on, the user should know why. */
static void notice(const char *message) {
    g_printerr("youcam: %s\n", message);
    if (R.on_notice) R.on_notice(message);
}

static void leave_later(const char *why) {
    if (R.mutter) overlay_set_capture_hidden(0);
    R.later = 0;
    notice(why);
}
```

d) In `complete_stop()`, before `R.phase = PHASE_IDLE;` add:

```c
    if (R.later && R.mutter) overlay_set_capture_hidden(0);
    R.later = 0;
```

e) Replace the whole `movie_finished` function with:

```c
static void movie_finished(const char *path, void *ud) {
    (void)ud;
    /* A project is not a movie yet: the dialog finishes it and sets the last path. */
    char *project = R.later ? g_strdup(path) : NULL;
    prefs_set_string(&R.last_path, project ? NULL : path);
    g_print("youcam: %s %s\n", project ? "project" : "saved", path);
    if (R.pending_error) {
        char *message = R.pending_error;
        R.pending_error = NULL;
        report(message);
        g_free(message);
    } else {
        complete_stop();
    }
    if (project && R.on_project) R.on_project(project);
    g_free(project);
}
```

f) After `recording_output_dir()` (Task 5) add:

```c
/* A project when Choose After Recording is on; if its layer track cannot
 * start, an opaque movie as before: a recording is never lost to it. */
static writer *start_movie(const char *video, GstElement *mic, GError **err) {
    writer_callbacks cb = {movie_finished, movie_failed, NULL};
    const rect *crop = R.has_crop ? &R.crop : NULL;
    if (R.later) {
        double p = transparency_normalize(Prefs.transparency);
        project_info project = {R.mutter ? 1 : p, p, "", mic != NULL};
        g_strlcpy(project.container, container_for(Prefs.container)->ext, sizeof(project.container));
        char *dir = project_dir();
        char *path = g_mkdir_with_parents(dir, 0700) == 0 ? project_new_path(dir, time(NULL)) : NULL;
        writer *w = path ? writer_start(path, video, mic, crop, WRITER_LAYER_TRACK, &project, cb, err) : NULL;
        g_free(path);
        g_free(dir);
        if (w) return w;
        g_printerr("youcam: project recording failed: %s\n", err && *err ? (*err)->message : "no pending folder");
        g_clear_error(err);
        leave_later(LATER_FAILED);
    }
    char *path = recording_next_path(recording_output_dir(), time(NULL), Prefs.container);
    writer *w = writer_start(path, video, mic, crop, WRITER_LAYER_COMPOSITE, NULL, cb, err);
    g_free(path);
    return w;
}
```

g) In `screen_ready()`, replace from `char *path = recording_next_path(` through the end of the `if (!R.movie) { … }` block with:

```c
    char *video = fd >= 0
        ? g_strdup_printf("pipewiresrc fd=%d path=%u do-timestamp=true always-copy=true keepalive-time=1000", fd, node)
        : g_strdup_printf("pipewiresrc path=%u do-timestamp=true always-copy=true keepalive-time=1000", node);
    GError *err = NULL;
    audio_monitor("recording", 1);
    /* Our own reference: a failed first attempt (start_movie) disposes its
     * pipeline, the microphone inside included, and the second needs it. */
    if (mic) gst_object_ref_sink(mic);
    R.movie = start_movie(video, mic, &err);
    if (mic) gst_object_unref(mic);
    g_free(video);
    if (!R.movie) {
        close_fd(fd);
        char *msg = g_strdup(err ? err->message : "The recording could not start.");
        g_clear_error(&err);
        report(msg);
        g_free(msg);
        return;
    }
```

(The old `if (mic) gst_object_unref(mic);` in the failure branch is gone: the reference taken above is the one released.)

h) In `begin()`, after `R.mutter = mutter_cast_available();` add:

```c
    /* With the extension the camera stays on screen but out of the screen
     * track: any transparency can be chosen later (T_A = 1). */
    if (R.later && R.mutter) overlay_set_capture_hidden(1);
```

i) In `recording_start()`, after `R.retried = 0;` add:

```c
    R.later = Prefs.saved_transparency == SAVED_LATER;
    if (R.later && !export_available()) {
        R.later = 0;
        notice(LATER_UNAVAILABLE);
    }
```

j) In `recording_stop()`, before `R.phase = PHASE_FINISHING;` add:

```c
    if (R.later && R.mutter != mutter_cast_available())
        g_printerr("youcam: the Capture Exclusion extension changed during the recording; "
                   "the project keeps the state it had at the start\n");
```

k) At the end of the file add:

```c
void recording_set_project_handlers(void (*on_project)(const char *path), void (*on_notice)(const char *message)) {
    R.on_project = on_project;
    R.on_notice = on_notice;
}

/* Without the extension the screen track holds the translucent preview:
 * the export needs it to stay at the T_A recorded at the start. */
int recording_transparency_locked(void) { return R.later && !R.mutter && PHASE_BUSY(R.phase); }
```

- [ ] **Step 3: `app.c`: the project layer, the dialog, notices, the start-up sweep**

a) In the transparency section, replace the layer-buffer growth inside `layer_tick()`

```c
    size_t size = (size_t)stride * ch;
    if (size > LayerSize) {
        LayerPixels = g_realloc(LayerPixels, size);
        LayerSize = size;
    }
```
with
```c
    ensure_layer_pixels((size_t)stride * ch);
```

and before `layer_tick()` add:

```c
static void ensure_layer_pixels(size_t size) {
    if (size <= LayerSize) return;
    LayerPixels = g_realloc(LayerPixels, size);
    LayerSize = size;
}

/* Choose After Recording: the layer is the whole movie, transparent except
 * for the opaque camera where its window is, and wholly transparent while
 * the camera is hidden or stopped. It runs for the whole recording. */
static void push_project_layer(writer *w, rect monitor, rect region) {
    int out_w, out_h;
    const camera_frame *f = camera_current_frame();
    if (!writer_output_size(w, &out_w, &out_h)) return;
    if (!f || !App.visible || camera_state() != CAMERA_RUNNING) {
        writer_push_layer(w, NULL, 0);
        return;
    }
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, out_w);
    ensure_layer_pixels((size_t)stride * out_h);
    cairo_surface_t *s = cairo_image_surface_create_for_data(LayerPixels, CAIRO_FORMAT_ARGB32, out_w, out_h, stride);
    cairo_t *cr = cairo_create(s);
    camera_layer l = camera_layer_map(window_frame(), monitor, region, out_w, out_h, Prefs.shape);
    camera_layer_draw(cr, l, 0, 0, f, Prefs.mirrored,
                      center_stage_now(l.frame.w, l.frame.h, Prefs.shape, Prefs.mirrored), background_now(f));
    cairo_destroy(cr);
    cairo_surface_flush(s);
    cairo_surface_destroy(s);
    writer_push_layer(w, LayerPixels, stride);
}
```

(`camera_layer_draw` clears the whole surface first and cairo clips what lies outside the movie.)

In `layer_tick()`, right after the `if (!w) { … }` block add:

```c
    if (writer_layer_track(w)) {
        push_project_layer(w, monitor, region);
        return G_SOURCE_CONTINUE;
    }
```

Update the comment above `layer_tick()` to:

```c
/* While recording, keep the camera copy in the movie in step with the
 * window: position, size, shape, mirroring and the latest frame. Opaque
 * camera over a translucent preview, or the project layer (Choose After
 * Recording). */
```

b) After `recording_error()` add:

```c
/* A Choose After Recording movie stopped: the dialog finishes it. When
 * quitting it waits for "Adjust Last Recording Transparency…". */
static void project_ready(const char *path) {
    if (App.quitting || App.smoke || g_getenv("YOUCAM_NO_DIALOGS")) return;   /* recording.c printed the path. */
    later_dialog_open(path);
}

/* The recording goes on, opaque: say why without blocking, and keep the
 * message out of the recording when the extension is active. */
static void recording_notice(const char *message) {
    if (App.quitting || App.smoke || g_getenv("YOUCAM_NO_DIALOGS")) return;   /* recording.c logged it. */
    GtkWidget *d = gtk_message_dialog_new(NULL, 0, GTK_MESSAGE_INFO, GTK_BUTTONS_NONE, "%s",
                                          "Recording with an opaque camera");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", message);
    gtk_dialog_add_button(GTK_DIALOG(d), "OK", GTK_RESPONSE_OK);
    gtk_window_set_role(GTK_WINDOW(d), CAPTURE_HIDDEN_ROLE);
    gtk_window_set_keep_above(GTK_WINDOW(d), TRUE);
    g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show(d);
}
```

c) In `app_startup()`, after `recording_init(update_ui, recording_error);` add:

```c
    recording_set_project_handlers(project_ready, recording_notice);
    char *pending = project_dir();   /* Stale exports' .part files, never the Desktop. */
    project_clean_parts(pending);
    g_free(pending);
```

- [ ] **Step 4: Build without warnings, all tests**

```bash
make clean >/dev/null && make youcam tests test-media test-segment 2>&1 | grep -E "warning|error" || echo "no warnings"
make test 2>&1 | grep -E "^Passed|^media ok" && make smoke-test 2>&1 | tail -2
```
Expected: `no warnings`; `Passed 293 logic checks.`, `Passed 53 segmentation checks.`, the Task 4 `media ok:` line; the smoke test's two lines.

- [ ] **Step 5: Review the failure paths by reading (no real recording here)**

Check in the diff, and fix if not so: (1) every exit of a `later` recording (finish, failure, cancelled start, fallback) restores the title when it was hidden (`complete_stop` and `leave_later`); (2) `R.last_path` is never set to a path in `pending/`; (3) the microphone reference count is balanced on all three outcomes of `start_movie` (first attempt succeeds; first fails and second succeeds; both fail); (4) `project_ready` never opens the dialog during quit. Real recordings are made only in Task 8.

- [ ] **Step 6: Commit**

```bash
git add linux/recording.c linux/overlay.c linux/app.c linux/youcam.h
git commit -m "feat: record Choose After Recording projects

Camera out of the screen track with the extension; opaque fallback; dialog at stop.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Menu, help text, README, CHANGELOG

**Files:**
- Modify: `linux/app.c`, `linux/README.md`, `linux/CHANGELOG.md`

**Interfaces:**
- Produces: Transparency submenu: slider (or, when `recording_transparency_locked()`, a disabled line with the value), separator, submenu **Saved Video** with radio items Same as Preview / Opaque / Choose After Recording (disabled with an explanation line when `!export_available()`), **Adjust Last Recording Transparency…** (enabled when `project_latest(project_dir())` is not NULL), **Reset Transparency to 0%**.
- Consumes: `saved_transparency_parse`, `SavedTransparencyNames` (Task 1), `project_dir`, `project_latest`, `export_available` (Tasks 2, 4), `later_dialog_open` (Task 5), `recording_transparency_locked` (Task 6).

- [ ] **Step 1: Replace the check item with the Saved Video group**

In `app.c` delete the function `toggle_record_transparency` and its comment. Add before `build_transparency_menu`:

```c
static void select_saved_transparency(const char *name) {
    Prefs.saved_transparency = saved_transparency_parse(name, 0, 0);
    prefs_save();
}

static void adjust_last_recording(void) {
    char *dir = project_dir(), *latest = project_latest(dir);
    if (latest) later_dialog_open(latest);
    g_free(latest);
    g_free(dir);
}

static int has_project(void) {
    char *dir = project_dir(), *latest = project_latest(dir);
    int found = latest != NULL;
    g_free(latest);
    g_free(dir);
    return found;
}

/* Applied when a recording starts, like the Recording Format. */
static void build_saved_menu(GtkWidget *menu) {
    static const char *titles[SAVED_COUNT] = {"Same as Preview", "Opaque", "Choose After Recording"};
    for (int i = 0; i < SAVED_COUNT; i++)
        add_choice(menu, titles[i], SavedTransparencyNames[i], Prefs.saved_transparency == (saved_transparency)i,
                   i != SAVED_LATER || export_available(), select_saved_transparency);
    if (!export_available()) add_item(menu, "Choose After Recording needs FFV1 (gst-libav)", NULL, 0);
}
```

Replace the whole `build_transparency_menu` with:

```c
static void build_transparency_menu(GtkWidget *menu) {
    char text[64];
    if (recording_transparency_locked()) {
        /* The export needs the T_A this recording started with. */
        snprintf(text, sizeof(text), "Transparency %.0f%% (kept while recording)", Prefs.transparency * 100);
        add_item(menu, text, NULL, 0);
    } else {
        GtkWidget *fixed = gtk_fixed_new();
        gtk_widget_set_size_request(fixed, 300, 64);
        GtkWidget *caption = gtk_label_new("Transparency");
        gtk_label_set_xalign(GTK_LABEL(caption), 0);
        gtk_widget_set_size_request(caption, 205, 16);
        gtk_fixed_put(GTK_FIXED(fixed), caption, 14, 9);
        snprintf(text, sizeof(text), "%.0f%%", Prefs.transparency * 100);
        GtkWidget *value = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(value), 1);
        gtk_widget_set_size_request(value, 60, 16);
        gtk_fixed_put(GTK_FIXED(fixed), value, 226, 9);
        GtkWidget *slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, TRANSPARENCY_MAX * 100, 1);
        gtk_scale_set_draw_value(GTK_SCALE(slider), FALSE);
        gtk_range_set_value(GTK_RANGE(slider), Prefs.transparency * 100);
        gtk_widget_set_size_request(slider, 276, 26);
        atk_object_set_name(gtk_widget_get_accessible(slider), "Camera transparency in percent");
        g_signal_connect(slider, "value-changed", G_CALLBACK(transparency_slider), value);
        gtk_fixed_put(GTK_FIXED(fixed), slider, 12, 33);
        add_slider_item(menu, fixed);
    }
    add_separator(menu);
    build_saved_menu(add_submenu(menu, "Saved Video", 1));
    add_item(menu, "Adjust Last Recording Transparency…", adjust_last_recording, has_project());
    add_item(menu, "Reset Transparency to 0%", reset_transparency, !recording_transparency_locked());
}
```

- [ ] **Step 2: Help text**

In `HelpText` replace

```c
    "camera menu; Reset Transparency to 0% turns it off. Check Apply Transparency to Saved Video to "
    "use the same transparency in your recordings, or leave it unchecked to keep the recorded camera "
    "opaque. Both choices are remembered.\n\n"
```
with
```c
    "camera menu; Reset Transparency to 0% turns it off. Saved Video chooses what recordings show: "
    "Same as Preview, Opaque, or Choose After Recording, which opens a window when you stop: pick the "
    "camera transparency with a preview, then Save the movie to your Desktop, keep it for Later, or "
    "Discard it. With the YouCam Capture Exclusion extension any value up to 80% is possible; without "
    "it, up to the preview transparency used while recording. Adjust Last Recording Transparency… "
    "reopens the last recording kept for later. Both choices are remembered.\n\n"
```

- [ ] **Step 3: README**

In `linux/README.md` replace the Transparency bullet

```markdown
- **Transparency** optionally makes the camera see-through so you can see
  what it covers. Adjust the slider from 0% to 80%, or choose **Reset
  Transparency to 0%** to turn it off. Check **Apply Transparency to Saved
  Video** to record the same transparency; leave it unchecked to keep the
  recorded camera opaque. Both choices are remembered, and changes apply
  immediately, including during recording.
```
with
```markdown
- **Transparency** optionally makes the camera see-through so you can see
  what it covers. Adjust the slider from 0% to 80%, or choose **Reset
  Transparency to 0%** to turn it off. **Saved Video** chooses what
  recordings show: **Same as Preview** (default), **Opaque**, or (Linux
  only) **Choose After Recording**: when you stop, a window shows a frame of
  the recording and a slider for the camera transparency of the saved
  movie; **Save** writes it to the Desktop in the Recording Format,
  **Later** keeps it for **Adjust Last Recording Transparency…**,
  **Discard…** deletes it. With the YouCam Capture Exclusion extension any
  value up to 80% is possible (the camera stays on screen but out of the
  screen recording); without it, up to the preview transparency used while
  recording, which then stays fixed until you stop. Unfinished recordings
  wait in `~/.local/share/youcam/pending/` (playable MKV files, also after
  a crash). It needs the FFV1 codec from gst-libav. Both choices are
  remembered, and slider changes apply immediately, including during recording.
```

In the "Run checks" paragraph, after the `--record-test` sentence add:

```markdown
`YOUCAM_SNAPSHOT_PROJECT=<project>.youcam.mkv ./youcam --snapshot DIR` also renders the
Choose After Recording window to `DIR/transparency.png`.
```

- [ ] **Step 4: CHANGELOG**

In `linux/CHANGELOG.md`, "Linux additions", after the **Background** item add:

```markdown
- **Choose After Recording** (Transparency → Saved Video): the camera layer
  is recorded on its own lossless track (FFV1) in a project MKV, and when
  the recording stops a window previews the camera transparency and saves
  the movie in the chosen format. With the Capture Exclusion extension any
  value up to 80%, without it up to the preview transparency. **Adjust Last
  Recording Transparency…** reopens a recording kept for later.
```

and change the ported item

```markdown
- Camera transparency (upstream PR #2): 0–80% slider, optionally applied to
  the saved video.
```
to
```markdown
- Camera transparency (upstream PR #2): 0–80% slider; the saved video is
  Same as Preview (now the default) or Opaque. The old "Apply Transparency
  to Saved Video" setting is migrated.
```

- [ ] **Step 5: Build, tests, menu still builds**

```bash
make clean >/dev/null && make youcam tests test-media test-segment 2>&1 | grep -E "warning|error" || echo "no warnings"
make test 2>&1 | grep -E "^Passed|^media ok"
CHECK=/home/patane/.cache/claude-tmp/claude-1000/-home-patane-Works-YouCam-A-youcam/46c95e59-cadb-4b73-9cd8-9e427ef68d24/scratchpad/tl-check
YOUCAM_CONFIG="$CHECK/prefs.ini" ./youcam --snapshot "$CHECK/snap" | grep menu; rm -f "$CHECK/snap/camera.png"
grep -c "Apply Transparency to Saved Video" app.c README.md
```
Expected: `no warnings`; the three test lines as in Task 6; `menu: N items` with the same N as before this task (the top-level menu is unchanged; run the same command on the previous commit if in doubt); `app.c:0` and `README.md:0`.

- [ ] **Step 6: Commit**

```bash
git add linux/app.c linux/README.md linux/CHANGELOG.md
git commit -m "feat: Saved Video menu with Choose After Recording

Adjust Last Recording Transparency item; help, README and changelog.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: Real-machine verification

**Files:** none changed (fixes found here go into a follow-up commit `fix: …` on the task they belong to).

Split: the **implementer** makes one real Mutter recording and reads only metadata; the **controller** (not the implementer) does the interactive and visual checks.

- [ ] **Step 1 (implementer): Back up the preferences**

```bash
REAL=/home/patane/.cache/claude-tmp/claude-1000/-home-patane-Works-YouCam-A-youcam/46c95e59-cadb-4b73-9cd8-9e427ef68d24/scratchpad/tl-real
mkdir -p "$REAL"
cp -p ~/.config/youcam/prefs.ini "$REAL/prefs.ini.bak"
ls ~/.local/share/youcam/pending 2>/dev/null > "$REAL/pending-before.txt"; echo ok
```
Expected: `ok`.

- [ ] **Step 2 (implementer): One 10-second project through Mutter, metadata only**

A copy of the real preferences (keeps camera, area and portal token) with `savedTransparency=later`; the real file is not touched.

```bash
cd /home/patane/Works/YouCam_A/youcam/linux
cp "$REAL/prefs.ini.bak" "$REAL/prefs-later.ini"
sed -i '/^savedTransparency=/d;/^recordCameraTransparency=/d;s/^\[youcam\]$/[youcam]\nsavedTransparency=later/' "$REAL/prefs-later.ini"
YOUCAM_CONFIG="$REAL/prefs-later.ini" YOUCAM_CAPTURE=mutter YOUCAM_NO_DIALOGS=1 \
  bash -c 'time ./youcam --record-test 10' 2>&1 | grep -E "youcam: (project|video encoder)|^(real|user|sys)"
P=$(ls -t ~/.local/share/youcam/pending/*.youcam.mkv | head -1)
ffprobe -v error -show_entries stream=index,codec_name,pix_fmt,width,height:format=duration,size -of compact "$P"
head -c 4096 "$P" | strings | grep "YouCam project"
rm -f "$P"
cmp ~/.config/youcam/prefs.ini "$REAL/prefs.ini.bak" && echo "prefs unchanged"
```
Expected: `youcam: video encoder va` (or `nvenc`/`x264`), `youcam: project …/pending/YouCam-Recording-….youcam.mkv`, `real`/`user`/`sys` lines (`/usr/bin/time` is not installed here, so bash's `time`; `YOUCAM_NO_DIALOGS=1` is belt and braces: `--record-test` quits through `app_quit`, which already skips the dialog); streams `0 h264 yuv420p W×H`, `1 ffv1 bgra W×H` (same size), `2 aac`; duration ≈ 10 s; `YouCam project;YOUCAM_T_A=1.000;…` (Mutter ⇒ T_A = 1); `prefs unchanged`. Write down: file size, size × 8 / duration (Mbit/s), CPU seconds (user + sys). Never open or decode frames of `$P`; it is deleted in the same step.

- [ ] **Step 3 (controller): Interactive checks with the extension active**

Real preferences change through the menu here; they are restored in Step 5.

1. `./youcam`; Transparency → Saved Video → Choose After Recording; preview transparency 30%; place the camera over a high-contrast area; record ~10 s (move the camera once); stop.
2. The window opens: preview of the middle frame; slider 0…80% starting at 30%; no extension note. Drag the slider: the camera in the preview changes instantly. Choose 50%, Save: progress bar, controls disabled, window closes; status "Saved to Desktop"; "Show Last Recording in Files" points at `YouCam-Recording-….mov` (Recording Format); `~/.local/share/youcam/pending/` no longer has that project.
3. Look only at a crop of the camera rectangle (for example `ffmpeg -v error -ss 5 -i MOVIE -vf crop=W:H:X:Y -frames:v 1 "$REAL/crop-ext.png"`, rectangle from the camera window position): camera ~50% transparent, no double camera, no ghost of the translucent preview.
4. Record again, stop, **Later**; menu → Adjust Last Recording Transparency… reopens it; **Discard…** → Cancel keeps it, Discard deletes it; the menu item is disabled once no project is left.
5. Save one export to the same name twice (record, Later, re-open after a copy of the first movie exists): the second is `… (2).mov`.

- [ ] **Step 4 (controller): Without the extension**

1. `YOUCAM_CAPTURE=portal ./youcam`, preview transparency 40%, Choose After Recording, record ~10 s; during the recording the Transparency menu shows "Transparency 40% (kept while recording)".
2. Stop: slider 0…40% starting at 40%, line "Up to 40%: enable the YouCam Capture Exclusion extension to choose up to 80%."; at 40% the preview equals the recording; at 0% the camera is opaque.
3. Save at 0%; crop check as in Step 3.3: camera opaque.
4. Change Recording Format to MKV, record, Save: `.mkv` output with audio (`ffprobe`).
5. Kill check: start a `later` recording, `kill -9` YouCam after ~5 s, start YouCam, Adjust Last Recording Transparency…: the project opens (first frame as preview) and Saves.

- [ ] **Step 5 (controller): Clean up and restore**

```bash
rm -f "$REAL"/crop-*.png
# Delete the movies made in Steps 3-4 from the Desktop (by name), and any projects created:
ls ~/.local/share/youcam/pending 2>/dev/null | diff - "$REAL/pending-before.txt" && echo "pending as before"
cp -p "$REAL/prefs.ini.bak" ~/.config/youcam/prefs.ini && cmp ~/.config/youcam/prefs.ini "$REAL/prefs.ini.bak" && echo "prefs restored"
```
Expected: `pending as before`, `prefs restored`.

- [ ] **Step 6: Record the results**

Add the measured numbers (Step 2 size, Mbit/s, CPU; export time from the `youcam: exported … in N s` log line) and the controller's verdicts to the PR description, not to the repository.

---

## Self-review

**Spec coverage**

| Spec point | Where |
|---|---|
| Radio group Saved Video: Same as Preview / Opaque / Choose After Recording, disabled with explanation without the codec | Task 7 Step 1 (`build_saved_menu`) |
| `savedTransparency` replaces `recordCameraTransparency`, migration true/false/missing/unknown | Task 1 |
| Stop dialog: middle frame ~640 px, two stills once, cairo blend, instant | Tasks 4 (`project_stills`), 5 (`draw_preview`) |
| Slider 0…max, starts at preview transparency clamped; note without extension | Task 5 `add_preview` |
| Save / Later / Discard (with confirmation); progress, controls disabled, UI responsive | Task 5; export on the main loop, polled every 200 ms (Task 4) |
| Success: "Saved…" status, project deleted | Task 5 `export_finished`; deviation note on the status line |
| Menu item Adjust Last Recording Transparency…, enabled with a project, opens the newest | Task 7, `project_latest` (Task 2) |
| New recording while the window is open | Independent paths; one-window rule in Deviations; Task 5 `later_dialog_open` |
| Projects in `~/.local/share/youcam/pending/` named `YouCam-Recording-<date>-<time>.youcam.mkv` | Task 2 `project_dir`, `project_new_path` |
| Killed project stays readable and exportable | Settings in the header (Task 3, tested on a mid-recording copy), stills fall back to the first frame (Task 4), Task 8 Step 4.5 |
| Several projects, no list | `project_latest` |
| Layer always drawn at transparency 0 for the whole recording | Task 6 `push_project_layer` |
| Layer as a separate lossless track, codec chosen by measurement | Task 3 (FFV1; QTRLE excluded by matroskamux), Task 8 Step 2 measures |
| Extension: title `youcam-hidden…`, restored at stop, T_A = 1 | Task 6 `overlay_set_capture_hidden`, `begin`, `complete_stop` |
| Portal: T_A = p | Task 6 `start_movie` |
| Project: 3 tracks, tags T_A / p / container | Task 3 (header + COMMENT tag; deviation explained) |
| α = 1 − t / T_A, range 0…min(T_A, 0.8), T_A = 0 ⇒ α = 1 | Task 2 `later_alpha`, `later_max`, tests |
| Export pipeline: demux, h264 decode, lossless decode, compositor alpha, shared encoder and muxers, AAC copied | Task 4 `export_start` |
| As fast as possible, main loop, progress pos/dur every 200 ms | Task 4 |
| Desktop `<name>.<ext>.part` renamed, " (2)" … | Tasks 2, 4 |
| Errors: codec missing; layer branch cannot start ⇒ opaque + told; export failure keeps project, deletes .part, message, retry; unreadable project ⇒ Discard only; extension toggled ⇒ logged; start-up sweep of `pending/*.part` | Tasks 7, 6 (`LATER_UNAVAILABLE`, `LATER_FAILED`), 4–5, 5 Step 7, 6 (`recording_stop`), 6 (`app_startup`) |
| Logic tests: α, slider limits, migration, unknown values, " (2)" | Tasks 1–2 |
| Media tests: 3 tracks + tags; t = 0, T_A, middle pixels; audio present, same duration; every container | Tasks 3–4 |
| Real check with and without the extension, crop only, files deleted | Task 8 |

**Placeholder scan:** no "TBD", "similar to Task N" or "add error handling" steps; every code step has the code. The one open measurement (FFV1 size/CPU on a real recording) is an explicit step with its command and what to write down.

**Type consistency** (checked across tasks): `saved_transparency` / `SAVED_*` / `SavedTransparencyNames` (Tasks 1, 6, 7); `project_info {t_a, preview, container[8], has_audio}` and `PROJECT_INFO_MAX` (Tasks 2–6); `writer_layer` / `WRITER_LAYER_*` and the 8-argument `writer_start` (Task 3; callers updated in Tasks 3 and 6, test_media in Task 3); `writer_push_layer(writer *, const guint8 *, int)` (Tasks 3, 6, test_media); `export_callbacks` field order `progress, finished, failed, ud` (Tasks 4, 5, test_media); `project_stills(path, width, &screen, &layer, err)` (Tasks 4, 5); `recording_output_dir` replaces `recordings_dir` everywhere (Tasks 5, 6); `later_dialog_open/window/shutdown` (Tasks 5, 6, 7).

Fixed during review: `project_dir()` does not create the folder (the start-up sweep and the menu would otherwise create `pending/` for everyone; `start_movie` creates it); the layer appsrc gets no caps in the description (initial 2x2 caps would reach FFV1 and Matroska cannot change caps later); stills use one pipeline per track (both linked gave a gray screen frame); the alpha is set on the pad after parsing (a `%f` in the description would print a decimal comma in an Italian locale); `gst_object_ref_sink` on the microphone so the opaque fallback can reuse it.
