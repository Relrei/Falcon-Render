/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Application level startup/shutdown functionality.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "DNA_windowmanager_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "IMB_cache.hh"
#include "IMB_imbuf.hh"

#include "MOV_util.hh"

#include "BKE_addon.h"
#include "BKE_appdir.hh"
#include "BKE_asset.hh"
#include "BKE_blender.hh"           /* own include */
#include "BKE_blender_user_menu.hh" /* own include */
#include "BKE_blender_version.h"    /* own include */
#include "BKE_brush.hh"
#include "BKE_callbacks.hh"
#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_screen.hh"
#include "BKE_studiolight.h"

#include "DEG_depsgraph.hh"

#include "RE_texture.h"

#include "BLF_api.hh"

#include "SEQ_utils.hh"

#include "CLG_log.h"

namespace blender {

Global G;
UserDef U;

/* -------------------------------------------------------------------- */
/** \name Blender Free on Exit
 * \{ */

void BKE_blender_free()
{
  /* samples are in a global list..., also sets G_MAIN->sound->sample nullptr */

  /* Needs to run before main free as window-manager is still referenced for icons preview jobs. */
  BKE_studiolight_free();

  BKE_blender_globals_clear();

  if (G.log.file != nullptr) {
    fclose(static_cast<FILE *>(G.log.file));
  }

  BKE_spacetypes_free(); /* after free main, it uses space callbacks */

  IMB_exit();
  DEG_free_node_types();

  BKE_brush_system_exit();
  RE_texture_rng_exit();

  BKE_callback_global_finalize();

  IMB_cache_destruct();
  seq::fontmap_clear();
  MOV_exit();

  bke::node_system_exit();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blender Version Access
 * \{ */

static char blender_version_string[96] = "";

/* Only includes patch if non-zero. */
static char blender_version_string_compact[96] = "";

static void blender_version_init()
{
  const char *version_cycle = "";
  const char *version_cycle_compact = "";
  if (STREQ(STRINGIFY(BLENDER_VERSION_CYCLE), "alpha")) {
    version_cycle = " Alpha";
    version_cycle_compact = " a";
  }
  else if (STREQ(STRINGIFY(BLENDER_VERSION_CYCLE), "beta")) {
    version_cycle = " Beta";
    version_cycle_compact = " b";
  }
  else if (STREQ(STRINGIFY(BLENDER_VERSION_CYCLE), "rc")) {
    version_cycle = " Release Candidate";
    version_cycle_compact = " RC";
  }
  else if (STREQ(STRINGIFY(BLENDER_VERSION_CYCLE), "release")) {
    version_cycle = "";
    version_cycle_compact = "";
  }
  else {
    BLI_assert_msg(0, "Invalid Blender version cycle");
  }

  const char *version_suffix = BKE_blender_version_is_lts() ? " LTS" : "";

  /* ---------------------------------------------------------------------------
   * Falcon Render の表示名 (2026-08-25 本人指示)
   *
   * ★「() を消す日」は、すぐ下の kFalconBaseSuffix を "" にするだけでよい。
   *   他の行は1行も触らなくてよい。画面に出る3箇所(スプラッシュ・タイトルバー・
   *   `--version`)と bpy.app.version_string は、すべてこの文字列を読んでいる。
   *
   *   例:  const char *kFalconBaseSuffix = "";   →  画面は「Falcon Render」
   *
   * 括弧の中身は「土台にした Blender の版」。表示から消しても、土台の版は
   * 内部データ側に残るので後から確認できる(2026-08-25 に実測で確認済み):
   *   - .blend ヘッダの整数        : "BLENDER17-01v0502" の末尾 0502 = 5.2
   *   - bpy.app.version            : (5, 2, 0)
   *   - bpy.app.version_file       : .blend に書く版(整数タプル)
   * この表示文字列自体は .blend には1バイトも入らない(strings で0件を確認済み)。
   *
   * ⚠ ここは文字列リテラルで、BLENDER_VERSION からは自動生成していない。
   *   土台の Blender を別の版へ上げた時は、この1行も合わせて書き換えること。
   * --------------------------------------------------------------------------- */
  /* ★2026-09-01 本人指示: 画面に出る名前は**2つだけ**。
   *
   *   「Falcon Render」       = リアルタイムで進む最新。**版を名乗らない**
   *   「Falcon Render v0.4」  = 切り離しなどを行って配布向けに調整したもの
   *
   * ★分ける鍵は FALCON_DIST(配布向けに切り出したか)であって、
   *   FALCON_BUILD_FLAVOR(GPU の実験機能を入れるか)ではない。
   *   2026-08-29〜08-31 は flavor が「配布用」と「GPU実験を外す」を兼ねていたため、
   *   **配布物でない正本まで v0.4 を名乗っていた**。それを直した。
   *
   * 配布の版を上げる時は、下の1行だけ書き換える。ここ以外に文言を増やさないこと。
   * 土台の Blender の版は表示に出さない(.desktop と README に書く)。 */
#ifdef FALCON_DIST_BUILD
  const char *kFalconName = "Falcon Render v0.4";
#else
  const char *kFalconName = "Falcon Render";
#endif

  SNPRINTF_UTF8(blender_version_string, "%s", kFalconName);

  SNPRINTF_UTF8(blender_version_string_compact, "%s", kFalconName);

  /* 表示から版数を外したので、これらは現在どこにも出ない。
   * 上の cycle 判定(不正な値の BLI_assert)は残したいので、変数だけ黙らせる。 */
  UNUSED_VARS(version_cycle, version_cycle_compact, version_suffix);
}

const char *BKE_blender_version_string()
{
  return blender_version_string;
}

const char *BKE_blender_version_string_compact()
{
  return blender_version_string_compact;
}

void BKE_blender_version_blendfile_string_from_values(char *str_buff,
                                                      const size_t str_buff_maxncpy,
                                                      const short file_version,
                                                      const short file_subversion)
{
  const short file_version_major = file_version / 100;
  const short file_version_minor = file_version % 100;
  if (file_subversion >= 0) {
    BLI_snprintf_utf8(str_buff,
                      str_buff_maxncpy,
                      "%d.%d (sub %d)",
                      file_version_major,
                      file_version_minor,
                      file_subversion);
  }
  else {
    BLI_snprintf_utf8(str_buff, str_buff_maxncpy, "%d.%d", file_version_major, file_version_minor);
  }
}

bool BKE_blender_version_is_alpha()
{
  bool is_alpha = STREQ(STRINGIFY(BLENDER_VERSION_CYCLE), "alpha");
  return is_alpha;
}

bool BKE_blender_version_is_lts()
{
  return STREQ(STRINGIFY(BLENDER_VERSION_SUFFIX), "LTS");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blender #Global Initialize/Clear
 * \{ */

void BKE_blender_globals_init()
{
  blender_version_init();

  memset(&G, 0, sizeof(Global));

  U.savetime = 1;

  BKE_blender_globals_main_replace(BKE_main_new());

  STRNCPY(G.filepath_last_image, "//");
  G.filepath_last_blend[0] = '\0';

#ifndef WITH_PYTHON_SECURITY /* default */
  G.f |= G_FLAG_SCRIPT_AUTOEXEC;
#else
  G.f &= ~G_FLAG_SCRIPT_AUTOEXEC;
#endif

  G.log.level = CLG_LEVEL_WARN;

  G.profile_gpu = false;
}

void BKE_blender_globals_clear()
{
  if (G_MAIN == nullptr) {
    return;
  }
  BLI_assert(G_MAIN->is_global_main);
  BKE_main_free(G_MAIN); /* free all lib data */

  G_MAIN = nullptr;
}

void BKE_blender_globals_main_replace(Main *bmain)
{
  BLI_assert(!bmain->is_global_main);
  BKE_blender_globals_clear();
  bmain->is_global_main = true;
  G_MAIN = bmain;
}

Main *BKE_blender_globals_main_swap(Main *new_gmain)
{
  Main *old_gmain = G_MAIN;
  BLI_assert(old_gmain->is_global_main);
  BLI_assert(!new_gmain->is_global_main);
  new_gmain->is_global_main = true;
  G_MAIN = new_gmain;
  old_gmain->is_global_main = false;
  return old_gmain;
}

void BKE_blender_globals_crash_path_get(char filepath[FILE_MAX])
{
  /* Might be called after WM/Main exit, so needs to be careful about nullptr-checking before
   * de-referencing. */

  if (!(G_MAIN && G_MAIN->filepath[0])) {
    BLI_path_join(filepath, FILE_MAX, BKE_tempdir_base(), "blender.crash.txt");
  }
  else {
    BLI_path_join(filepath, FILE_MAX, BKE_tempdir_base(), BLI_path_basename(G_MAIN->filepath));
    BLI_path_extension_replace(filepath, FILE_MAX, ".crash.txt");
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blender Preferences
 * \{ */

static void keymap_item_free(wmKeyMapItem *kmi)
{
  if (kmi->properties) {
    IDP_FreeProperty(kmi->properties);
  }
  if (kmi->ptr) {
    MEM_delete(kmi->ptr);
  }
}

void BKE_blender_userdef_data_swap(UserDef *userdef_a, UserDef *userdef_b)
{
  dna::shallow_swap(*userdef_a, *userdef_b);
}

void BKE_blender_userdef_data_set(UserDef *userdef)
{
  BKE_blender_userdef_data_swap(&U, userdef);
  BKE_blender_userdef_data_free(userdef, true);
}

void BKE_blender_userdef_data_set_and_free(UserDef *userdef)
{
  BKE_blender_userdef_data_set(userdef);
  MEM_delete(userdef);
}

static void userdef_free_keymaps(UserDef *userdef)
{
  for (wmKeyMap *km = static_cast<wmKeyMap *>(userdef->user_keymaps.first), *km_next; km;
       km = km_next)
  {
    km_next = km->next;
    for (wmKeyMapDiffItem &kmdi : km->diff_items) {
      if (kmdi.add_item) {
        keymap_item_free(kmdi.add_item);
        MEM_delete(kmdi.add_item);
      }
      if (kmdi.remove_item) {
        keymap_item_free(kmdi.remove_item);
        MEM_delete(kmdi.remove_item);
      }
    }

    for (wmKeyMapItem &kmi : km->items) {
      keymap_item_free(&kmi);
    }

    km->diff_items.free_no_destruct();
    km->items.free_no_destruct();

    MEM_delete(km);
  }
  userdef->user_keymaps.clear_no_delete();
}

static void userdef_free_keyconfig_prefs(UserDef *userdef)
{
  for (wmKeyConfigPref *kpt = static_cast<wmKeyConfigPref *>(userdef->user_keyconfig_prefs.first),
                       *kpt_next;
       kpt;
       kpt = kpt_next)
  {
    kpt_next = kpt->next;
    IDP_FreeProperty(kpt->prop);
    MEM_delete(kpt);
  }
  userdef->user_keyconfig_prefs.clear_no_delete();
}

static void userdef_free_user_menus(UserDef *userdef)
{
  for (bUserMenu *um = static_cast<bUserMenu *>(userdef->user_menus.first), *um_next; um;
       um = um_next)
  {
    um_next = um->next;
    BKE_blender_user_menu_item_free_list(&um->items);
    MEM_delete(um);
  }
}

static void userdef_free_addons(UserDef *userdef)
{
  for (bAddon *addon = static_cast<bAddon *>(userdef->addons.first), *addon_next; addon;
       addon = addon_next)
  {
    addon_next = addon->next;
    BKE_addon_free(addon);
  }
  userdef->addons.clear_no_delete();
}

void BKE_blender_userdef_data_free(UserDef *userdef, bool clear_fonts)
{
#define U BLI_STATIC_ASSERT(false, "Global 'U' not allowed, only use arguments passed in!")
#ifdef U
  /* Quiet warning. */
#endif

  userdef_free_keymaps(userdef);
  userdef_free_keyconfig_prefs(userdef);
  userdef_free_user_menus(userdef);
  userdef_free_addons(userdef);

  if (clear_fonts) {
    for (uiFont &font : userdef->uifonts) {
      BLF_unload_id(font.blf_id);
    }
    BLF_default_set(-1);
  }

  userdef->autoexec_paths.free_no_destruct();
  userdef->script_directories.free_no_destruct();
  userdef->asset_libraries.free_no_destruct();

  for (bUserExtensionRepo &repo_ref : userdef->extension_repos.items_mutable()) {
    MEM_SAFE_DELETE(repo_ref.access_token);
    MEM_delete(&repo_ref);
  }
  userdef->extension_repos.clear_no_delete();

  for (bUserAssetShelfSettings &settings : userdef->asset_shelves_settings.items_mutable()) {
    BKE_asset_catalog_path_list_free(settings.enabled_catalog_paths);
    MEM_delete(&settings);
  }
  userdef->asset_shelves_settings.clear_no_delete();

  userdef->uistyles.free_no_destruct();
  userdef->uifonts.free_no_destruct();
  userdef->themes.free_no_destruct();

#undef U
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blender Preferences (Application Templates)
 * \{ */

void BKE_blender_userdef_app_template_data_swap(UserDef *userdef_a, UserDef *userdef_b)
{
  /* TODO:
   * - various minor settings (add as needed).
   */

#define VALUE_SWAP(id) \
  { \
    std::swap(userdef_a->id, userdef_b->id); \
  }

#define DATA_SWAP(id) \
  { \
    UserDef userdef_tmp; \
    memcpy(&(userdef_tmp.id), &(userdef_a->id), sizeof(userdef_tmp.id)); \
    memcpy(&(userdef_a->id), &(userdef_b->id), sizeof(userdef_tmp.id)); \
    memcpy(&(userdef_b->id), &(userdef_tmp.id), sizeof(userdef_tmp.id)); \
  } \
  ((void)0)

#define FLAG_SWAP(id, ty, flags) \
  { \
    CHECK_TYPE(&(userdef_a->id), ty *); \
    const ty f = flags; \
    const ty a = userdef_a->id; \
    const ty b = userdef_b->id; \
    userdef_a->id = (userdef_a->id & ~f) | (b & f); \
    userdef_b->id = (userdef_b->id & ~f) | (a & f); \
  } \
  ((void)0)

  VALUE_SWAP(uistyles);
  VALUE_SWAP(uifonts);
  VALUE_SWAP(themes);
  VALUE_SWAP(addons);
  VALUE_SWAP(user_keymaps);
  VALUE_SWAP(user_keyconfig_prefs);

  DATA_SWAP(font_path_ui);
  DATA_SWAP(font_path_ui_mono);
  DATA_SWAP(keyconfigstr);

  DATA_SWAP(gizmo_flag);
  DATA_SWAP(app_flag);

  /* We could add others. */
  FLAG_SWAP(uiflag,
            eUserpref_UI_Flag,
            USER_SAVE_PROMPT | USER_SPLASH_DISABLE | USER_SHOW_GIZMO_NAVIGATE);

  DATA_SWAP(ui_scale);

#undef VALUE_SWAP
#undef DATA_SWAP
#undef FLAG_SWAP
}

void BKE_blender_userdef_app_template_data_set(UserDef *userdef)
{
  BKE_blender_userdef_app_template_data_swap(&U, userdef);
  BKE_blender_userdef_data_free(userdef, true);
}

void BKE_blender_userdef_app_template_data_set_and_free(UserDef *userdef)
{
  BKE_blender_userdef_app_template_data_set(userdef);
  MEM_delete(userdef);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blender's AtExit
 *
 * \note Don't use MEM_new_uninitialized so functions can be registered at any time.
 * \{ */

static struct AtExitData {
  AtExitData *next;

  void (*func)(void *user_data);
  void *user_data;
} *g_atexit = nullptr;

void BKE_blender_atexit_register(void (*func)(void *user_data), void *user_data)
{
  AtExitData *ae = static_cast<AtExitData *>(malloc(sizeof(*ae)));
  ae->next = g_atexit;
  ae->func = func;
  ae->user_data = user_data;
  g_atexit = ae;
}

void BKE_blender_atexit_unregister(void (*func)(void *user_data), const void *user_data)
{
  AtExitData *ae = g_atexit;
  AtExitData **ae_p = &g_atexit;

  while (ae) {
    if ((ae->func == func) && (ae->user_data == user_data)) {
      *ae_p = ae->next;
      free(ae);
      return;
    }
    ae_p = &ae->next;
    ae = ae->next;
  }
}

void BKE_blender_atexit()
{
  AtExitData *ae = g_atexit, *ae_next;
  while (ae) {
    ae_next = ae->next;

    ae->func(ae->user_data);

    free(ae);
    ae = ae_next;
  }
  g_atexit = nullptr;
}

/** \} */

}  // namespace blender
