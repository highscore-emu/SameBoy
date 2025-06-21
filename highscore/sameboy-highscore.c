#include "sameboy-highscore.h"

#include <gb.h>

#define SAMPLE_RATE 96000
#define AUDIO_BUF_SIZE 8000

struct _SameBoyCore
{
  HsCore parent_instance;

  HsSoftwareContext *context;
  GB_gameboy_t *gameboy;

  GB_model_t model;
  GB_model_t pending_model;

  char *save_location;
  int colorburst_phase;

  gint16 audio_buf[AUDIO_BUF_SIZE];
  guint32 *frame_buffer;
  guint32 *repeat_buffer;
  int audio_len;

  gboolean frame_updated;
};

static void sameboy_game_boy_core_init (HsGameBoyCoreInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (SameBoyCore, sameboy_core, HS_TYPE_CORE,
                               G_IMPLEMENT_INTERFACE (HS_TYPE_GAME_BOY_CORE, sameboy_game_boy_core_init));

static void
log_cb (GB_gameboy_t *gb, const char *string, GB_log_attributes_t attributes)
{
  SameBoyCore *self = GB_get_user_data (gb);

  hs_core_log_literal (HS_CORE (self), HS_LOG_DEBUG, string);
}

static void
boot_rom_load_cb (GB_gameboy_t *gb, GB_boot_rom_t type)
{
  const char *rom_name = (char *[]) {
    [GB_BOOT_ROM_DMG_0] = "dmg_boot.bin",
    [GB_BOOT_ROM_DMG] = "dmg_boot.bin",
    [GB_BOOT_ROM_MGB] = "mgb_boot.bin",
    [GB_BOOT_ROM_SGB] = "sgb_boot.bin",
    [GB_BOOT_ROM_SGB2] = "sgb2_boot.bin",
    [GB_BOOT_ROM_CGB_0] = "cgb0_boot.bin",
    [GB_BOOT_ROM_CGB] = "cgb_boot.bin",
    [GB_BOOT_ROM_CGB_E] = "cgb_boot.bin",
    [GB_BOOT_ROM_AGB_0] = "agb_boot.bin",
    [GB_BOOT_ROM_AGB] = "agb_boot.bin",
  }[type];

  g_autofree char *path = g_build_filename (CORE_DIR, rom_name, NULL);

  if (GB_load_boot_rom (gb, path)) {
    SameBoyCore *self = GB_get_user_data (gb);

    hs_core_log (HS_CORE (self), HS_LOG_CRITICAL, "Failed to load boot ROM: %s", rom_name);
  }
}

static void
rumble_cb (GB_gameboy_t *gb, double amplitude)
{
  SameBoyCore *self = GB_get_user_data (gb);

  hs_core_rumble (HS_CORE (self), 0, amplitude, amplitude, HS_MAX_RUMBLE_DURATION);
}

static uint32_t
rgb_encode_cb (GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b)
{
  return r << 16 | g << 8 | b;
}

static void
audio_sample_cb (GB_gameboy_t *gb, GB_sample_t *sample)
{
  SameBoyCore *self = GB_get_user_data (gb);

  g_assert (self->audio_len < AUDIO_BUF_SIZE - 2);

  self->audio_buf[self->audio_len++] = sample->left;
  self->audio_buf[self->audio_len++] = sample->right;
}

static void
vblank_cb (GB_gameboy_t *gb, GB_vblank_type_t type)
{
  SameBoyCore *self = GB_get_user_data (gb);

  if (type == GB_VBLANK_TYPE_REPEAT) {
    memcpy (self->frame_buffer,
            self->repeat_buffer,
            GB_get_screen_width (gb) * GB_get_screen_height (gb) * sizeof (guint32));
  }

  self->frame_updated = TRUE;
}

static void
lcd_status_cb (GB_gameboy_t *gb, bool on)
{
  if (on)
    return;

  SameBoyCore *self = GB_get_user_data (gb);

  memcpy (self->repeat_buffer,
          self->frame_buffer,
          GB_get_screen_width (gb) * GB_get_screen_height (gb) * sizeof (guint32));
}

static void
update_framebuffer (SameBoyCore *self)
{
  int w = GB_get_screen_width (self->gameboy);
  int h = GB_get_screen_height (self->gameboy);

  hs_software_context_set_area (self->context, &HS_RECTANGLE_INIT (0, 0, w, h));
  hs_software_context_set_row_stride (self->context, w * 4);
}

static gboolean
try_migrate_libretro_save (SameBoyCore   *self,
                           const char  *save_path,
                           GError     **error)
{
  g_autoptr (GFile) save_file = g_file_new_for_path (save_path);

  if (!g_file_query_exists (save_file, NULL))
    return TRUE;

  if (g_file_query_file_type (save_file, G_FILE_QUERY_INFO_NONE, NULL) == G_FILE_TYPE_DIRECTORY)
    return TRUE;

  // Make a temporary file
  g_autofree char *cache_path = hs_core_get_cache_path (HS_CORE (self));
  g_autoptr (GFile) cache_dir = g_file_new_for_path (cache_path);
  if (!g_file_query_exists (cache_dir, NULL) &&
      !g_file_make_directory_with_parents (cache_dir, NULL, error)) {
    return FALSE;
  }

  g_autofree char *tmp_path = g_build_filename (cache_path, "sameboy-save-XXXXXX", NULL);
  tmp_path = g_mkdtemp (tmp_path);
  g_autoptr (GFile) tmp_file = g_file_new_for_path (tmp_path);

  // Move the old save, replace it with a directory
  g_autoptr (GFile) tmp_save_file = g_file_get_child (tmp_file, "save");
  if (!g_file_move (save_file, tmp_save_file, G_FILE_COPY_BACKUP, NULL, NULL, NULL, error))
    return FALSE;

  if (!g_file_make_directory_with_parents (save_file, NULL, error))
    return FALSE;

  g_autoptr (GFile) dest_file = g_file_get_child (save_file, "save.sav");
  if (!g_file_move (tmp_save_file, dest_file, G_FILE_COPY_BACKUP, NULL, NULL, NULL, error))
    return FALSE;

  if (!g_file_delete (tmp_file, NULL, error))
    return FALSE;

  hs_core_log (HS_CORE (self), HS_LOG_MESSAGE, "Libretro save file migrated successfully");

  return TRUE;
}

static gboolean
load_save (SameBoyCore *self, GError **error)
{
  if (!try_migrate_libretro_save (self, self->save_location, error))
    return FALSE;

  g_autoptr (GFile) save_dir = g_file_new_for_path (self->save_location);
  if (!g_file_query_exists (save_dir, NULL) && g_file_make_directory_with_parents (save_dir, NULL, error))
    return FALSE;

  g_autoptr (GFile) save_file = g_file_get_child (save_dir, "save.sav");
  GB_load_battery (self->gameboy, g_file_peek_path (save_file));
  return true;
}

static gboolean
sameboy_core_load_rom (HsCore      *core,
                       const char **rom_paths,
                       int          n_rom_paths,
                       const char  *save_path,
                       GError     **error)

{
  SameBoyCore *self = SAMEBOY_CORE (core);

  g_assert (n_rom_paths == 1);

  g_set_str (&self->save_location, save_path);

  self->context = hs_core_create_software_context (core, 256, 224, HS_PIXEL_FORMAT_B8G8R8X8);
  self->frame_buffer = g_new0 (guint32, 256 * 224);
  self->repeat_buffer = g_new0 (guint32, 256 * 224);

  self->gameboy = GB_init (GB_alloc (), self->pending_model);
  self->model = self->pending_model;

  GB_set_user_data (self->gameboy, self);

  GB_set_log_callback (self->gameboy, log_cb);
  GB_set_boot_rom_load_callback (self->gameboy, boot_rom_load_cb);
  GB_set_rumble_callback (self->gameboy, rumble_cb);
  GB_set_rgb_encode_callback (self->gameboy, rgb_encode_cb);
  GB_apu_set_sample_callback (self->gameboy, audio_sample_cb);
  GB_set_vblank_callback (self->gameboy, vblank_cb);
  GB_set_lcd_status_callback (self->gameboy, lcd_status_cb);

  GB_set_pixels_output (self->gameboy, self->frame_buffer);
  GB_set_sample_rate (self->gameboy, SAMPLE_RATE);
  GB_set_highpass_filter_mode (self->gameboy, GB_HIGHPASS_ACCURATE);

  update_framebuffer (self);

  if (GB_load_rom (self->gameboy, rom_paths[0])) {
    g_set_error (error, HS_CORE_ERROR, HS_CORE_ERROR_INTERNAL, "Failed to load ROM");
    return FALSE;
  }

  if (!load_save (self, error))
    return FALSE;

  return TRUE;
}

const GB_key_mask_t KEY_MAPPING[] = {
  GB_KEY_UP_MASK,     GB_KEY_DOWN_MASK,
  GB_KEY_LEFT_MASK,   GB_KEY_RIGHT_MASK,
  GB_KEY_A_MASK,      GB_KEY_B_MASK,
  GB_KEY_SELECT_MASK, GB_KEY_START_MASK,
};

static void
sameboy_core_poll_input (HsCore *core, HsInputState *input_state)
{
  SameBoyCore *self = SAMEBOY_CORE (core);
  GB_key_mask_t mask = 0;

  for (int btn = 0; btn < HS_GAME_BOY_N_BUTTONS; btn++) {
    if (input_state->game_boy.buttons & 1 << btn)
      mask |= KEY_MAPPING[btn];
  }

  GB_set_key_mask (self->gameboy, mask);
}

static void
sameboy_core_run_frame (HsCore *core)
{
  SameBoyCore *self = SAMEBOY_CORE (core);

  GB_run_frame (self->gameboy);

  if (self->frame_updated) {
    int buf_size = GB_get_screen_width (self->gameboy) * GB_get_screen_height (self->gameboy) * sizeof (guint32);
    memcpy (hs_software_context_get_framebuffer (self->context), self->frame_buffer, buf_size);
    hs_software_context_set_colorburst_phase (self->context, self->colorburst_phase);

    if (hs_core_get_region (core) == HS_REGION_NTSC)
      self->colorburst_phase ^= 1;
    else
      self->colorburst_phase = 0;

    self->frame_updated = FALSE;
  }

  if (self->audio_len > 0) {
    hs_core_play_samples (core, self->audio_buf, self->audio_len);
    self->audio_len = 0;
  }
}

static void
sameboy_core_reset (HsCore *core, gboolean hard)
{
  SameBoyCore *self = SAMEBOY_CORE (core);

  if (hard) {
    if (self->model != self->pending_model) {
      self->model = self->pending_model;
      GB_switch_model_and_reset (self->gameboy, self->model);
      update_framebuffer (self);
      return;
    }

    GB_reset (self->gameboy);
    return;
  }

  GB_quick_reset (self->gameboy);
}

static void
sameboy_core_stop (HsCore *core)
{
  SameBoyCore *self = SAMEBOY_CORE (core);

  GB_free (self->gameboy);
  GB_dealloc (self->gameboy);
  self->gameboy = NULL;

  g_clear_object (&self->context);
  g_clear_pointer (&self->save_location, g_free);
}

static gboolean
sameboy_core_reload_save (HsCore      *core,
                          const char  *save_path,
                          GError     **error)
{
  SameBoyCore *self = SAMEBOY_CORE (core);

  g_set_str (&self->save_location, save_path);

  if (!load_save (self, error))
    return FALSE;

  return TRUE;
}

static gboolean
sameboy_core_sync_save (HsCore  *core,
                        GError **error)
{
  SameBoyCore *self = SAMEBOY_CORE (core);
  g_autoptr (GFile) save_dir = g_file_new_for_path (self->save_location);

  GB_save_battery (self->gameboy, g_file_peek_path (save_dir));

  return TRUE;
}

static void
sameboy_core_load_state (HsCore          *core,
                         const char      *path,
                         HsStateCallback  callback)
{
  SameBoyCore *self = SAMEBOY_CORE (core);
  GError *error = NULL;

  if (self->pending_model != self->model)
    hs_core_reset (core, TRUE);

  if (GB_load_state (self->gameboy, path)) {
    g_set_error (&error, HS_CORE_ERROR, HS_CORE_ERROR_INTERNAL, "Failed to load state");
    callback (core, &error);
    return;
  }

  callback (core, NULL);
}

static void
sameboy_core_save_state (HsCore          *core,
                         const char      *path,
                         HsStateCallback  callback)
{
  SameBoyCore *self = SAMEBOY_CORE (core);
  GError *error = NULL;

  if (GB_save_state (self->gameboy, path)) {
    g_set_error (&error, HS_CORE_ERROR, HS_CORE_ERROR_INTERNAL, "Failed to save state");
    callback (core, &error);
    return;
  }

  callback (core, NULL);
}

static double
sameboy_core_get_frame_rate (HsCore *core)
{
  SameBoyCore *self = SAMEBOY_CORE (core);

  return GB_get_usual_frame_rate (self->gameboy);
}

static double
sameboy_core_get_aspect_ratio (HsCore *core)
{
  SameBoyCore *self = SAMEBOY_CORE (core);
  int w = GB_get_screen_width (self->gameboy);
  int h = GB_get_screen_height (self->gameboy);
  double par;

  switch (hs_core_get_region (core)) {
  case HS_REGION_UNKNOWN:
    par = 1.0;
    break;
  case HS_REGION_NTSC:
    par = 8.0 / 7.0;
    break;
  case HS_REGION_PAL:
    par = 2950000.0 / 2128137.0;
    break;
  default:
    g_assert_not_reached ();
  }

  return (double) w / (double) h * par;
}

static double
sameboy_core_get_sample_rate (HsCore *core)
{
  SameBoyCore *self = SAMEBOY_CORE (core);

  return GB_get_sample_rate (self->gameboy);
}

static int
sameboy_core_get_channels (HsCore *core)
{
  return 2;
}

static HsRegion
sameboy_core_get_region (HsCore *core)
{
  SameBoyCore *self = SAMEBOY_CORE (core);

  if (GB_is_sgb (self->gameboy)) {
    if (self->model & GB_MODEL_PAL_BIT)
      return HS_REGION_PAL;
    else
      return HS_REGION_NTSC;
  }

  return HS_REGION_UNKNOWN;
}

static void
sameboy_core_class_init (SameBoyCoreClass *klass)
{
  HsCoreClass *core_class = HS_CORE_CLASS (klass);

  core_class->load_rom = sameboy_core_load_rom;
  core_class->poll_input = sameboy_core_poll_input;
  core_class->run_frame = sameboy_core_run_frame;
  core_class->reset = sameboy_core_reset;
  core_class->stop = sameboy_core_stop;

  core_class->reload_save = sameboy_core_reload_save;
  core_class->sync_save = sameboy_core_sync_save;

  core_class->load_state = sameboy_core_load_state;
  core_class->save_state = sameboy_core_save_state;

  core_class->get_frame_rate = sameboy_core_get_frame_rate;
  core_class->get_aspect_ratio = sameboy_core_get_aspect_ratio;

  core_class->get_sample_rate = sameboy_core_get_sample_rate;
  core_class->get_channels = sameboy_core_get_channels;

  core_class->get_region = sameboy_core_get_region;
}

static void
sameboy_core_init (SameBoyCore *self)
{
  self->pending_model = GB_MODEL_DMG_B;
}

static void
sameboy_game_boy_core_set_model (HsGameBoyCore *core, HsGameBoyModel model)
{
  SameBoyCore *self = SAMEBOY_CORE (core);

  switch (model) {
  case HS_GAME_BOY_MODEL_GAME_BOY:
    self->pending_model = GB_MODEL_DMG_B;
    break;
  case HS_GAME_BOY_MODEL_GAME_BOY_POCKET:
    self->pending_model = GB_MODEL_MGB;
    break;
  case HS_GAME_BOY_MODEL_GAME_BOY_COLOR:
    self->pending_model = GB_MODEL_CGB_D;
    break;
  case HS_GAME_BOY_MODEL_GAME_BOY_ADVANCE:
    self->pending_model = GB_MODEL_AGB;
    break;
  case HS_GAME_BOY_MODEL_SUPER_GAME_BOY:
    self->pending_model = GB_MODEL_SGB;
    break;
  case HS_GAME_BOY_MODEL_SUPER_GAME_BOY_2:
    self->pending_model = GB_MODEL_SGB2;
    break;
  default:
    g_assert_not_reached ();
  }
}

static void
sameboy_game_boy_core_set_palette (HsGameBoyCore *core, int *colors, int n_colors)
{
}

static void
sameboy_game_boy_core_init (HsGameBoyCoreInterface *iface)
{
  iface->set_model = sameboy_game_boy_core_set_model;
  iface->set_palette = sameboy_game_boy_core_set_palette;
}

GType
hs_get_core_type (void)
{
  return SAMEBOY_TYPE_CORE;
}
