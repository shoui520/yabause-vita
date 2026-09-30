/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Game list and in-game menu on libpaf, the system software's UI library.
 *
 * Game list: PAF owns the display (Framework::Mode_Normal), as in
 * system applications. Choosing a game restarts the application with the
 * game's path, so the emulator starts with all of its memory.
 *
 * In-game menu: PAF runs in the mode common dialogs use inside games
 * (Framework::Mode_CommonDialog): it draws into the game's display buffer
 * when the game calls sceCommonDialogUpdate, from vitaGL's swap. The
 * linker routes that call here (--wrap), where the frame shown when the
 * menu opened is copied, dimmed, under the menu.
 *
 * PAF frees objects it is given with its own allocator, so every object
 * PAF may delete is allocated with sce_paf_malloc, never global new. */
#include <limits>   /* paf/std/vector uses std::numeric_limits */
#include <paf.h>
#include <app_settings.h>
#include <psp2/appmgr.h>
#include <psp2/common_dialog.h>
#include <psp2/gxm.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <psp2/sysmodule.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "paf_ui.h"
#include "../game_list.h"

extern "C" {
void YuiMsg(const char *format, ...);
/* Pinned vitaGL (build/deps/vitagl-serial-gc, source/gxm.c). */
extern void *gxm_color_surfaces_addr[];
extern unsigned int gxm_front_buffer_index;
extern SceGxmContext *gxm_context;
void vglSwapBuffers(unsigned char has_commondialog);
int __real_sceCommonDialogUpdate(const SceCommonDialogUpdateParam *param);
int __wrap_sceCommonDialogUpdate(const SceCommonDialogUpdateParam *param);
}

namespace {

/* The ScePafInit argument of the PAF sysmodule (libpaf module_start):
 * cdlg_mode 1 takes PAF's memory from the common dialog budget. */
struct PafInit {
  SceSize global_heap_size;
  int a2, a3, cdlg_mode, heap_opt_param1, heap_opt_param2;
};

enum { MODE_NONE, MODE_LAUNCHER, MODE_MENU };

int g_mode = MODE_NONE;
paf::Framework *g_framework;
paf::Plugin *g_plugin;
paf::ui::Scene *g_scene;
VitaConfig *g_config;
const char *g_config_path;
VitaGameList g_games, g_bios;
char g_title[160];   /* the disc name: the menu title and the state files' name */
/* The game chosen in the launcher; started once PAF has shut down. */
char g_launch_path[512];

/* Menu state shared with the emulation and render threads. */
int g_menu_ready, g_menu_open, g_menu_opened, g_action, g_slot, g_config_changed;
int g_status_dirty, g_settings_shown;
char g_status[128];
uint32_t *g_snapshot;
int g_snapshot_valid;

template <typename T> T atomic_load(T *value) { return __atomic_load_n(value, __ATOMIC_ACQUIRE); }
template <typename T> void atomic_store(T *value, T v) { __atomic_store_n(value, v, __ATOMIC_RELEASE); }

int LoadPaf(SceSize heap, int cdlg_mode) {
  PafInit init = {heap, 0xEA60, 0x40000, cdlg_mode, 0, 0};
  int result = 0;
  SceSysmoduleOpt opt;
  memset(&opt, 0, sizeof(opt));
  opt.result = &result;
  const int rc = sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF,
                                                       sizeof(init), &init, &opt);
  YuiMsg("paf_load heap=%u cdlg=%d rc=%08x result=%08x", (unsigned)heap, cdlg_mode,
         (unsigned)rc, (unsigned)result);
  return rc < 0 || result < 0 ? -1 : 0;
}

/* UTF-8 to the UTF-16 PAF draws. */
void Widen(const char *text, wchar_t *out, unsigned size) {
  const unsigned char *p = (const unsigned char *)text;
  unsigned n = 0;
  while (*p && n + 2 < size) {
    unsigned c = *p++;
    const int more = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
    if (more < 0) c = 0xfffd;
    else if (more) {
      c &= (1u << (6 - more)) - 1;
      for (int i = 0; i < more; ++i) {
        if ((*p & 0xc0) != 0x80) { c = 0xfffd; break; }
        c = (c << 6) | (*p++ & 0x3f);
      }
    }
    if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) c = 0xfffd;
    if (c > 0xffff) {
      c -= 0x10000;
      out[n++] = (wchar_t)(0xd800 + (c >> 10));
      out[n++] = (wchar_t)(0xdc00 + (c & 0x3ff));
    } else out[n++] = (wchar_t)c;
  }
  out[n] = 0;
}

void SetText(paf::ui::Widget *widget, const char *text) {
  if (!widget) return;
  wchar_t wide[256];
  Widen(text, wide, sizeof(wide) / sizeof(wide[0]));
  widget->SetString(wide);
}

/* ------------------------------------------------------------ settings */

/* sce::AppSettings, the engine of the system Settings application, builds
 * its pages from settings.xml in our resource. Its titles come from
 * GetString below; the values it stores are mirrored from config.ini. */
sce::AppSettings *g_appsettings;
paf::common::SharedPtr<paf::MemFile> *g_settings_xml;   /* kept for the process, never released */
int g_settings_loading;

const char *const kRegionTitles[] = {
  "Automatic (from the disc)", "Japan", "North America", "Europe", "Asia (NTSC)",
  "Asia (PAL)", "Korea", "South America (NTSC)", "South America (PAL)"
};
const char *const kSaturnTitles[VITA_SATURN_BUTTONS] = {
  "Up", "Right", "Down", "Left", "R", "C", "B", "A", "Y", "Z", "L", "Start", "X"
};
const char *const kButtonTitles[VITA_BUTTONS] = {
  "Not assigned", "Up", "Right", "Down", "Left", "Cross", "Circle", "Square",
  "Triangle", "L", "R", "Start", "R Analog L", "R Analog R"
};
const char *const kCartTitles[VITA_CARTS] = {
  "None", "4 Mbit Backup Memory", "1 MB RAM Expansion", "4 MB RAM Expansion"
};
const char *const kBackupSlotTitles[VITA_BACKUP_SLOTS] = {
  "Slot 1", "Slot 2", "Slot 3", "Slot 4", "Slot 5", "Slot 6", "Slot 7", "Slot 8", "Slot 9", "Slot 10"
};
enum { BIOS_SLOTS = 16 };

/* AppSettings keeps the returned pointers while its pages are shown. */
struct CachedString { char id[32]; wchar_t text[128]; };
CachedString *g_strings;
unsigned g_string_count;
enum { STRING_CACHE = 256 };

const char *Lookup(const char *id) {
  int n;
  if (!strcmp(id, "msg_settings")) return "Settings";
  if (!strcmp(id, "msg_system")) return "System";
  if (!strcmp(id, "msg_controls")) return "Controls";
  if (!strcmp(id, "msg_bios")) return "BIOS";
  if (!strcmp(id, "msg_region")) return "Region";
  if (!strcmp(id, "msg_cartridge")) return "Cartridges";
  if (!strcmp(id, "msg_backup_slot")) return "Backup Memory";
  if (sscanf(id, "msg_bios_%d", &n) == 1)
    return n >= 0 && (unsigned)n < g_bios.count ? g_bios.entries[n].title : "";
  if (sscanf(id, "msg_region_%d", &n) == 1)
    return n >= 0 && n < (int)(sizeof(kRegionTitles) / sizeof(kRegionTitles[0])) ? kRegionTitles[n] : "";
  if (sscanf(id, "msg_cartridge_%d", &n) == 1)
    return n >= 0 && n < VITA_CARTS ? kCartTitles[n] : "";
  if (sscanf(id, "msg_backup_slot_%d", &n) == 1)
    return n >= 1 && n <= VITA_BACKUP_SLOTS ? kBackupSlotTitles[n - 1] : "";
  if (sscanf(id, "msg_button_%d", &n) == 1)
    return n >= 0 && n < VITA_BUTTONS ? kButtonTitles[n] : "";
  if (!strncmp(id, "msg_pad_", 8))
    for (int i = 0; i < VITA_SATURN_BUTTONS; ++i)
      if (!strcmp(id + 8, VitaConfigSaturnName(i))) return kSaturnTitles[i];
  return "";
}

wchar_t *SettingsString(const char *id) {
  for (unsigned i = 0; i < g_string_count; ++i)
    if (!strcmp(g_strings[i].id, id)) return g_strings[i].text;
  static wchar_t empty[1];
  if (g_string_count == STRING_CACHE || strlen(id) >= sizeof(g_strings[0].id)) return empty;
  CachedString *entry = &g_strings[g_string_count++];
  snprintf(entry->id, sizeof(entry->id), "%s", id);
  Widen(Lookup(id), entry->text, sizeof(entry->text) / sizeof(entry->text[0]));
  return entry->text;
}

/* As the Videos application's 詳細設定 under Accessibility: the Backup
 * Memory choice exists only while that cartridge is chosen. */
int SettingsVisible(const char *id, bool *visible) {
  *visible = strcmp(id, "backup_slot") || g_config->cartridge == VITA_CART_BACKUP_4MBIT;
  return 0;
}

paf::ui::Widget *Ancestor(paf::ui::Widget *widget, const char *type) {
  while (widget && !widget->IsInherit(type)) widget = widget->GetParent();
  return widget;
}

/* Visibility is asked only when a page is built, so after the cartridge
 * changes the Backup Memory item (next to it) is added to or removed from
 * the open page's list, as Videos does for 詳細設定. */
void RefreshBackupSlot() {
  paf::Plugin *plugin = paf::Plugin::Find("app_settings_plugin");
  if (!plugin) return;
  paf::vector<paf::string> pages;
  plugin->GetOpenedPages(&pages);
  for (size_t i = 0; i < pages.size(); ++i) {
    paf::ui::Scene *scene = plugin->PageRoot(pages[i].c_str());
    paf::ui::Widget *cartridge = scene ? scene->FindChild("cartridge") : NULL;
    paf::ui::ListItem *item = static_cast<paf::ui::ListItem *>(Ancestor(cartridge, paf::ui::ListItem::TypeName()));
    paf::ui::ListView *list = static_cast<paf::ui::ListView *>(Ancestor(item, paf::ui::ListView::TypeName()));
    if (!list) continue;
    const bool shown = scene->FindChild("backup_slot") != NULL;
    const bool wanted = g_config->cartridge == VITA_CART_BACKUP_4MBIT;
    const int segment = item->GetSegmentIndex(), cell = item->GetCellIndex();
    YuiMsg("backup_slot_refresh page=%s cell=%d shown=%d wanted=%d", pages[i].c_str(), cell, shown, wanted);
    if (shown && !wanted) list->DeleteCell(segment, cell + 1, 1);
    else if (!shown && wanted) list->InsertCell(segment, cell + 1, 1);
  }
}

int RegionIndex(int region) {
  for (unsigned i = 0; i < vita_config_region_count; ++i)
    if (vita_config_regions[i] == region) return (int)i;
  return 0;
}

/* The element is a list (its id is the key) or one of its items (key_NN). */
int SettingsPress(const char *id, const char *value) {
  char key[32];
  snprintf(key, sizeof(key), "%s", id);
  int item = -1;
  char *underscore = strrchr(key, '_');
  if (underscore && underscore[1] >= '0' && underscore[1] <= '9' && strcmp(key, "pad_") != 0) {
    item = atoi(underscore + 1);
    *underscore = 0;
  }
  const int n = value && value[0] >= '0' && value[0] <= '9' ? atoi(value) : item;
  if (n < 0) return 0;
  int changed = 0;
  if (!strcmp(key, "bios")) {
    if ((unsigned)n < g_bios.count && strlen(g_bios.entries[n].path) < sizeof(g_config->bios)) {
      snprintf(g_config->bios, sizeof(g_config->bios), "%s", g_bios.entries[n].path);
      changed = 1;
    }
  } else if (!strcmp(key, "region")) {
    if ((unsigned)n < vita_config_region_count) {
      g_config->region = vita_config_regions[n];
      changed = 1;
    }
  } else if (!strcmp(key, "cartridge")) {
    if (n < VITA_CARTS) {
      g_config->cartridge = n;
      changed = 1;
      RefreshBackupSlot();
    }
  } else if (!strcmp(key, "backup_slot")) {
    if (n >= 1 && n <= VITA_BACKUP_SLOTS) {
      g_config->backup_slot = n;
      changed = 1;
    }
  } else if (!strncmp(key, "pad_", 4)) {
    for (int i = 0; i < VITA_SATURN_BUTTONS; ++i)
      if (!strcmp(key + 4, VitaConfigSaturnName(i)) && n < VITA_BUTTONS) {
        g_config->pad[i] = (unsigned char)n;
        changed = 1;
      }
  }
  if (changed) {
    const int rc = VitaConfigSave(g_config, g_config_path);
    YuiMsg("settings_changed key=%s value=%d save=%d", key, n, rc);
    atomic_store(&g_config_changed, 1);
  }
  return 0;
}

int SettingsPressAgain(const char *, const char *) { return 0; }
void SettingsPage(const char *, int32_t) {}
int SettingsPreCreate(const char *, sce::AppSettings::Element *) { return 0; }
int SettingsPostCreate(const char *, paf::ui::Widget *) { return 0; }
int SettingsSurface(paf::graph::Surface **, const char *) { return 0; }

void SettingsClosed(int32_t result) {
  YuiMsg("settings_closed result=%08x", (unsigned)result);
  atomic_store(&g_settings_shown, 0);
  if (g_scene) g_scene->SetActivate(true);
}

void AddBios(const char *path) {
  VitaGameEntry *entry = &g_bios.entries[g_bios.count++];
  snprintf(entry->path, sizeof(entry->path), "%s", path);
  const char *name = strrchr(path, '/');
  snprintf(entry->title, sizeof(entry->title), "%s", name ? name + 1 : path);
}

/* The BIOS choices, found once as the settings pages load: Automatic
 * (named after the image it picks), the configured file when it is
 * outside the BIOS folder, then the folder's 512 KiB files. */
void ScanBios() {
  const char *const dirs[] = {VITA_BIOS_DIR};
  VitaGameList found = {0, 0, 0};
  VitaBiosListScan(&found, dirs, 1);
  g_bios.entries = (VitaGameEntry *)calloc(BIOS_SLOTS, sizeof(VitaGameEntry));
  if (!g_bios.entries) { VitaGameListFree(&found); return; }
  g_bios.capacity = BIOS_SLOTS;
  AddBios(VITA_BIOS_AUTO);
  const int preferred = VitaBiosPreferred(&found);
  if (preferred >= 0) {
    const char *name = strrchr(found.entries[preferred].path, '/');
    snprintf(g_bios.entries[0].title, sizeof(g_bios.entries[0].title), "Automatic (%s)", name + 1);
  } else snprintf(g_bios.entries[0].title, sizeof(g_bios.entries[0].title), "Automatic");
  int listed = 0;
  for (unsigned i = 0; i < found.count; ++i) listed |= !strcmp(found.entries[i].path, g_config->bios);
  if (strcmp(g_config->bios, VITA_BIOS_AUTO) && !listed) AddBios(g_config->bios);
  for (unsigned i = 0; i < found.count && g_bios.count < BIOS_SLOTS; ++i) AddBios(found.entries[i].path);
  VitaGameListFree(&found);
}

int BiosIndex() {
  for (unsigned i = 0; i < g_bios.count; ++i)
    if (!strcmp(g_bios.entries[i].path, g_config->bios)) return (int)i;
  return 0;
}

/* settings.xml lists only Automatic (bios_00); an item per other choice
 * is added after it. The texts are kept for the process. */
paf::common::SharedPtr<paf::MemFile> SettingsXml(const char *xml, size_t size) {
  const char *const anchor = "<list_item id=\"bios_00\"";
  const char *at = strstr(xml, anchor);
  const char *end = at ? strstr(at, "/>") : NULL;
  const size_t item_size = 96;
  char *text = (char *)malloc(size + g_bios.count * item_size + 1);
  int32_t error = 0;
  if (!end || !text) {
    free(text);
    return paf::MemFile::Open(xml, size, &error);
  }
  end += 2;
  size_t length = end - xml;
  memcpy(text, xml, length);
  for (unsigned i = 1; i < g_bios.count; ++i)
    length += snprintf(text + length, item_size,
                       "\n        <list_item id=\"bios_%02u\" title=\"msg_bios_%02u\" value=\"%u\" />", i, i, i);
  memcpy(text + length, end, size - (end - xml));
  length += size - (end - xml);
  return paf::MemFile::Open(text, length, &error);
}

void ShowSettings() {
  g_string_count = 0;
  g_appsettings->SetInt("bios", BiosIndex());
  g_appsettings->SetInt("region", RegionIndex(g_config->region));
  g_appsettings->SetInt("cartridge", g_config->cartridge);
  g_appsettings->SetInt("backup_slot", g_config->backup_slot);
  char key[16];
  for (int i = 0; i < VITA_SATURN_BUTTONS; ++i) {
    snprintf(key, sizeof(key), "pad_%s", VitaConfigSaturnName(i));
    g_appsettings->SetInt(key, g_config->pad[i]);
  }
  sce::AppSettings::InterfaceCallbacks callbacks;
  callbacks.onStartPageTransitionCb = SettingsPage;
  callbacks.onPageActivateCb = SettingsPage;
  callbacks.onPageDeactivateCb = SettingsPage;
  callbacks.onCheckVisible = SettingsVisible;
  callbacks.onPreCreateCb = SettingsPreCreate;
  callbacks.onPostCreateCb = SettingsPostCreate;
  callbacks.onPressCb = SettingsPress;
  callbacks.onPressCb2 = SettingsPressAgain;
  callbacks.onTermCb = SettingsClosed;
  callbacks.onGetStringCb = SettingsString;
  callbacks.onGetSurfaceCb = SettingsSurface;
  paf::Plugin *plugin = paf::Plugin::Find("app_settings_plugin");
  const sce::AppSettings::Interface *settings = plugin ?
    static_cast<const sce::AppSettings::Interface *>(plugin->GetInterface(1)) : NULL;
  if (!settings) { YuiMsg("settings_interface_missing"); return; }
  if (g_scene) g_scene->SetActivate(false);
  atomic_store(&g_settings_shown, 1);
  const int rc = settings->Show(&callbacks);
  YuiMsg("settings_shown rc=%08x", (unsigned)rc);
}

void SettingsPluginLoaded(paf::Plugin *) {
  g_settings_loading = 0;
  size_t size = 0;
  const char *mime = NULL;
  /* Never released: its last release would free PAF's counter with our
   * allocator. */
  paf::common::SharedPtr<paf::MemFile> *file = new (sce_paf_malloc(sizeof(*file)))
    paf::common::SharedPtr<paf::MemFile>(g_plugin->GetResource()->GetFile("file_settings", &size, &mime));
  char *xml = (char *)malloc(size);
  if (!xml || (*file)->Read(xml, size) != (int32_t)size) { free(xml); YuiMsg("settings_xml_unread"); return; }
  ScanBios();
  g_settings_xml = new (sce_paf_malloc(sizeof(*g_settings_xml))) paf::common::SharedPtr<paf::MemFile>(SettingsXml(xml, size));
  sce::AppSettings::InitParam param;
  param.xml_file = *g_settings_xml;
  param.alloc_cb = sce_paf_malloc;
  param.realloc_cb = sce_paf_realloc;
  param.free_cb = sce_paf_free;
  param.safemem_offset = 0;
  param.safemem_size = 0x400;
  const int rc = sce::AppSettings::GetInstance(param, &g_appsettings);
  YuiMsg("settings_instance rc=%08x xml=%u", (unsigned)rc, (unsigned)size);
  if (rc < 0 || !g_appsettings) { g_appsettings = NULL; return; }
  ShowSettings();
}

void OpenSettings() {
  if (atomic_load(&g_settings_shown) || g_settings_loading) return;
  if (g_appsettings) { ShowSettings(); return; }
  /* The modules AppSettings imports from. */
  static const SceSysmoduleInternalModuleId modules[] = {
    SCE_SYSMODULE_INTERNAL_BXCE, SCE_SYSMODULE_INTERNAL_INI_FILE_PROCESSOR,
    SCE_SYSMODULE_INTERNAL_COMMON_GUI_DIALOG
  };
  for (unsigned i = 0; i < sizeof(modules) / sizeof(modules[0]); ++i) {
    const int rc = sceSysmoduleLoadModuleInternal(modules[i]);
    if (rc < 0) { YuiMsg("settings_module_failed id=%08x rc=%08x", (unsigned)modules[i], (unsigned)rc); return; }
  }
  if (!g_strings) g_strings = (CachedString *)calloc(STRING_CACHE, sizeof(CachedString));
  if (!g_strings) return;
  paf::Plugin::InitParam param;
  param.name = "app_settings_plugin";
  param.caller_name = "__main__";
  param.resource_file = "vs0:vsh/common/app_settings_plugin.rco";
  param.module_file = "vs0:vsh/common/app_settings.suprx";
  param.set_param_func = sce::AppSettings::PluginSetParamCB;
  param.init_func = sce::AppSettings::PluginInitCB;
  param.start_func = sce::AppSettings::PluginStartCB;
  param.stop_func = sce::AppSettings::PluginStopCB;
  param.exit_func = sce::AppSettings::PluginExitCB;
  param.draw_priority = 0x96;
  g_settings_loading = 1;
  paf::Plugin::LoadAsync(param, SettingsPluginLoaded);
}

void SettingsPressed(int32_t, paf::ui::Handler *, paf::ui::Event *, void *) { OpenSettings(); }

/* --------------------------------------------------------------- lists */

typedef void (*RowFill)(paf::ui::Widget *button, int index);

class RowFactory : public paf::ui::listview::ItemFactory {
public:
  RowFactory(const char *row_template, const char *button_id, RowFill fill)
    : row_template_(row_template), button_id_(button_id), fill_(fill) {}
  static void *operator new(size_t size, void *place) { (void)size; return place; }
  static void operator delete(void *pointer) { sce_paf_free(pointer); }

  paf::ui::ListItem *Create(CreateParam &param) {
    paf::Plugin::TemplateOpenParam options;
    if (g_plugin->TemplateOpen(param.parent, row_template_, options) < 0) return NULL;
    paf::ui::ListItem *item = static_cast<paf::ui::ListItem *>(
      param.parent->GetChild(param.parent->GetChildrenNum() - 1));
    paf::ui::Widget *button = item ? item->FindChild(button_id_) : NULL;
    if (button) fill_(button, param.cell_index);
    return item;
  }
  void Start(StartParam &) {}
  void Stop(StopParam &) {}
  void Dispose(DisposeParam &param) { if (param.list_item) param.list_item->DestroyWidget(); }

private:
  const char *row_template_, *button_id_;
  RowFill fill_;
};

RowFactory *NewFactory(const char *row_template, const char *button_id, RowFill fill) {
  void *memory = sce_paf_malloc(sizeof(RowFactory));
  return memory ? new (memory) RowFactory(row_template, button_id, fill) : NULL;
}

void FillList(paf::ui::ListView *list, RowFactory *factory, unsigned count, float width, float height) {
  if (!list || !factory) return;
  list->SetItemFactory(factory);
  list->SetScrollType(paf::ui::ListView::SCROLL_TYPE_VERTICAL);
  list->InsertSegment(0, 1);
  list->SetSegmentLayoutType(0, paf::ui::ListView::LAYOUT_TYPE_LIST);
  list->SetCellSizeDefault(0, paf::math::v4(width, height, 0, 0));
  if (count) list->InsertCell(0, 0, count);
}

/* ------------------------------------------------------------ launcher */

void GameChosen(int32_t, paf::ui::Handler *, paf::ui::Event *, void *data) {
  const unsigned index = (unsigned)(uintptr_t)data;
  if (index >= g_games.count) return;
  const char *path = g_games.entries[index].path;
  snprintf(g_config->last_game, sizeof(g_config->last_game), "%s", path);
  VitaConfigSave(g_config, g_config_path);
  if (g_launch_path[0]) return;
  snprintf(g_launch_path, sizeof(g_launch_path), "%s", path);
  YuiMsg("launch game=%s", path);
  /* sceAppMgrLoadExec from inside PAF's loop hangs the whole system;
   * VitaUiRunLauncher calls it once Run() has returned. */
  g_framework->RequestShutdown();
}

void FillGame(paf::ui::Widget *button, int index) {
  SetText(button, g_games.entries[index].title);
  button->SetEventCallback(paf::ui::ButtonBase::CB_BTN_DECIDE, GameChosen, (void *)(uintptr_t)index);
}

void LauncherStarted(paf::Plugin *plugin) {
  g_plugin = plugin;
  paf::Plugin::PageOpenParam open;
  g_scene = plugin->PageOpen("page_games", open);
  if (!g_scene) { YuiMsg("launcher_page_failed"); return; }
  SetText(g_scene->FindChild("title_text"), "Games");
  paf::ui::Widget *empty = g_scene->FindChild("games_empty");
  if (g_games.count) { if (empty) empty->Hide(); }
  else {
    char text[384];
    snprintf(text, sizeof(text), "No games found.\n\nCopy .cue, .chd or .iso images to\n%s", g_config->games_dir);
    SetText(empty, text);
  }
  FillList(static_cast<paf::ui::ListView *>(g_scene->FindChild("games_list")),
           NewFactory("template_game_row", "game_button", FillGame), g_games.count, 960, 80);
  paf::ui::Widget *settings = g_scene->FindChild("games_settings");
  if (settings) settings->SetEventCallback(paf::ui::ButtonBase::CB_BTN_DECIDE, SettingsPressed, NULL);
  YuiMsg("launcher_ready games=%u", g_games.count);
}

/* --------------------------------------------------------- state slots */

paf::ui::Scene *g_slots_scene;   /* page_slots, over the hidden page_menu */
int g_slots_action;              /* VITA_UI_SAVE_STATE or VITA_UI_LOAD_STATE */

void SlotLabel(int slot, char *out, size_t size) {
  char path[512];
  VitaConfigStatePath(path, sizeof(path), g_title, slot);
  SceIoStat stat;
  if (sceIoGetstat(path, &stat) < 0) { snprintf(out, size, "Slot %d  \u2014  Empty", slot); return; }
  SceRtcTick utc, local;
  SceDateTime time = stat.st_mtime;
  if (sceRtcGetTick(&stat.st_mtime, &utc) == 0 && sceRtcConvertUtcToLocalTime(&utc, &local) == 0)
    sceRtcSetTick(&time, &local);
  snprintf(out, size, "Slot %d  \u2014  %04u-%02u-%02u %02u:%02u", slot, time.year, time.month,
           time.day, time.hour, time.minute);
}

void CloseSlots() {
  if (!g_slots_scene) return;
  g_slots_scene = NULL;
  paf::Plugin::PageCloseParam close;
  g_plugin->PageClose("page_slots", close);
  g_scene->Show();
  g_scene->SetActivate(true);
}

void SlotChosen(int32_t, paf::ui::Handler *, paf::ui::Event *, void *data) {
  const int action = g_slots_action;
  atomic_store(&g_slot, (int)(uintptr_t)data + 1);
  CloseSlots();
  atomic_store(&g_action, action);
}

void SlotsBack(int32_t, paf::ui::Handler *, paf::ui::Event *, void *) { CloseSlots(); }

void FillSlot(paf::ui::Widget *button, int index) {
  char label[64];
  SlotLabel(index + 1, label, sizeof(label));
  SetText(button, label);
  button->SetEventCallback(paf::ui::ButtonBase::CB_BTN_DECIDE, SlotChosen, (void *)(uintptr_t)index);
}

void OpenSlots(int action) {
  if (g_slots_scene) return;
  paf::Plugin::PageOpenParam open;
  g_slots_scene = g_plugin->PageOpen("page_slots", open);
  if (!g_slots_scene) { YuiMsg("slots_page_failed"); return; }
  g_slots_action = action;
  g_scene->SetActivate(false);
  g_scene->Hide();
  SetText(g_slots_scene->FindChild("title_text"),
          action == VITA_UI_SAVE_STATE ? "Save State" : "Load State");
  FillList(static_cast<paf::ui::ListView *>(g_slots_scene->FindChild("slots_list")),
           NewFactory("template_slot_row", "slot_button", FillSlot), VITA_STATE_SLOTS, 600, 70);
  paf::ui::Widget *back = g_slots_scene->FindChild("slots_back");
  if (back) back->SetEventCallback(paf::ui::ButtonBase::CB_BTN_DECIDE, SlotsBack, NULL);
}

/* ---------------------------------------------------------------- menu */

const char *const kMenuTitles[] = {
  "Resume", "Save State", "Load State", "Settings", "Reset", "Quit to Game List"
};
const int kMenuActions[] = {
  VITA_UI_RESUME, VITA_UI_SAVE_STATE, VITA_UI_LOAD_STATE, VITA_UI_NONE, VITA_UI_RESET, VITA_UI_QUIT
};
enum { MENU_SETTINGS = 3 };

void MenuChosen(int32_t, paf::ui::Handler *, paf::ui::Event *, void *data) {
  const int index = (int)(uintptr_t)data;
  if (index == MENU_SETTINGS) { OpenSettings(); return; }
  if (kMenuActions[index] == VITA_UI_SAVE_STATE || kMenuActions[index] == VITA_UI_LOAD_STATE) {
    OpenSlots(kMenuActions[index]);
    return;
  }
  atomic_store(&g_action, kMenuActions[index]);
}

void MenuBack(int32_t, paf::ui::Handler *, paf::ui::Event *, void *) {
  atomic_store(&g_action, (int)VITA_UI_RESUME);
}

void FillMenu(paf::ui::Widget *button, int index) {
  SetText(button, kMenuTitles[index]);
  button->SetEventCallback(paf::ui::ButtonBase::CB_BTN_DECIDE, MenuChosen, (void *)(uintptr_t)index);
}

/* Runs on the PAF thread once per menu frame. */
void MenuPoll(void *) {
  if (!g_scene) return;
  if (atomic_load(&g_menu_opened)) {
    atomic_store(&g_menu_opened, 0);
    CloseSlots();
    SetText(g_scene->FindChild("menu_status"), "");
  }
  if (atomic_load(&g_status_dirty)) {
    atomic_store(&g_status_dirty, 0);
    SetText(g_scene->FindChild("menu_status"), g_status);
  }
}

void MenuStarted(paf::Plugin *plugin) {
  g_plugin = plugin;
  paf::Plugin::PageOpenParam open;
  g_scene = plugin->PageOpen("page_menu", open);
  if (!g_scene) { YuiMsg("menu_page_failed"); return; }
  SetText(g_scene->FindChild("title_text"), g_title);
  FillList(static_cast<paf::ui::ListView *>(g_scene->FindChild("menu_list")),
           NewFactory("template_menu_row", "menu_button", FillMenu),
           sizeof(kMenuTitles) / sizeof(kMenuTitles[0]), 600, 70);
  paf::ui::Widget *back = g_scene->FindChild("menu_back");
  if (back) back->SetEventCallback(paf::ui::ButtonBase::CB_BTN_DECIDE, MenuBack, NULL);
  paf::common::MainThreadCallList::Register(MenuPoll, NULL);
  YuiMsg("menu_page_ready");
}

void LoadPlugin(paf::Plugin::StartFunction started, bool sync) {
  paf::Plugin::InitParam param;
  param.name = "yabause_ui";
  param.caller_name = "__main__";
  param.resource_file = "app0:yabause_ui.rco";
  param.start_func = started;
  if (sync) paf::Plugin::LoadSync(param);
  else paf::Plugin::LoadAsync(param);
}

void MenuCommonResourceLoaded() { LoadPlugin(MenuStarted, false); }

int MenuThread(SceSize, void *) {
  paf::Framework::InitParam param;
  paf::Framework::SampleInit(&param);
  /* The configuration libcdlg_main gives the common dialogs. */
  param.mode = paf::Framework::Mode_CommonDialog;
  param.display_list_size = 0x80000;
  param.surface_pool_size = 0x100000;
  param.graph_heap_size_on_main_memory = 0x80000;
  param.text_surface_pool_size = 0x80000;
  param.pvf_heap_size = 0x100000;
  param.screen_width = 960;
  param.screen_height = 544;
  param.allow_button_control = true;
  void *memory = sce_paf_malloc(sizeof(paf::Framework));
  if (!memory) { YuiMsg("menu_framework_alloc_failed"); return 0; }
  g_framework = new (memory) paf::Framework(param);
  /* Loading continues in the frames the first menu opening presents. */
  g_framework->LoadCommonResourceAsync(MenuCommonResourceLoaded);
  atomic_store(&g_menu_ready, 1);
  YuiMsg("menu_framework_running");
  g_framework->Run();
  YuiMsg("menu_framework_exit");
  return 0;
}

} // namespace

extern "C" int VitaUiRunLauncher(VitaConfig *config, const char *config_path) {
  g_config = config;
  g_config_path = config_path;
  VitaGameListScan(&g_games, config->games_dir);
  YuiMsg("launcher games=%u dir=%s", g_games.count, config->games_dir);
  if (LoadPaf(0x800000, 0) != 0) return -1;
  g_mode = MODE_LAUNCHER;
  paf::Framework::InitParam param;
  param.screen_width = 960;
  param.screen_height = 544;
  param.surface_pool_size = 0x500000;
  param.text_surface_pool_size = 0x80000;
  param.mode = paf::Framework::Mode_Normal;
  param.allow_button_control = true;
  param.graphics_option = 7;
  void *memory = sce_paf_malloc(sizeof(paf::Framework));
  if (!memory) return -1;
  g_framework = new (memory) paf::Framework(param);
  g_framework->LoadCommonResourceSync();
  LoadPlugin(LauncherStarted, true);
  g_framework->Run();
  YuiMsg("launcher_exit shutdown_done=%d", g_framework->IsShutdownDone() ? 1 : 0);
  if (!g_launch_path[0]) return -1;
  char *const argv[] = {(char *)"--game", g_launch_path, NULL};
  const int rc = sceAppMgrLoadExec("app0:eboot.bin", argv, NULL);
  YuiMsg("launch_failed rc=%08x", (unsigned)rc);
  return -1;
}

extern "C" int VitaUiMenuInit(VitaConfig *config, const char *config_path, const char *title) {
  g_config = config;
  g_config_path = config_path;
  snprintf(g_title, sizeof(g_title), "%s", title);
  g_snapshot = (uint32_t *)malloc(960 * 544 * 4);
  if (!g_snapshot) return -1;
  if (LoadPaf(0x400000, 1) != 0 && LoadPaf(0x400000, 0) != 0) return -1;
  g_mode = MODE_MENU;
  const SceUID thread = sceKernelCreateThread("YabauseMenu", MenuThread, 0x10000100, 0x40000, 0,
                                              SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
  if (thread < 0 || sceKernelStartThread(thread, 0, NULL) < 0) {
    YuiMsg("menu_thread_failed=%08x", (unsigned)thread);
    return -1;
  }
  return 0;
}

extern "C" int VitaUiMenuAvailable(void) {
  return g_mode == MODE_MENU && atomic_load(&g_menu_ready);
}

extern "C" void VitaUiMenuOpen(void) {
  g_status[0] = 0;
  atomic_store(&g_action, (int)VITA_UI_NONE);
  atomic_store(&g_menu_opened, 1);
  g_snapshot_valid = 0;
  atomic_store(&g_menu_open, 1);
}

extern "C" void VitaUiMenuFrame(void) { vglSwapBuffers(1); }

extern "C" VitaUiAction VitaUiMenuPoll(void) {
  return (VitaUiAction)__atomic_exchange_n(&g_action, (int)VITA_UI_NONE, __ATOMIC_ACQ_REL);
}

extern "C" int VitaUiMenuSlot(void) { return atomic_load(&g_slot); }

extern "C" void VitaUiMenuClose(void) {
  atomic_store(&g_menu_open, 0);
}

extern "C" void VitaUiMenuSetStatus(const char *text) {
  snprintf(g_status, sizeof(g_status), "%s", text);
  atomic_store(&g_status_dirty, 1);
}

extern "C" int VitaUiMenuTakeConfigChange(void) {
  return __atomic_exchange_n(&g_config_changed, 0, __ATOMIC_ACQ_REL);
}

extern "C" int VitaUiMenuSettingsShown(void) { return atomic_load(&g_settings_shown); }

/* vitaGL's swap with has_commondialog set: its back buffer, after its scene
 * and before it is queued for display. */
extern "C" int __wrap_sceCommonDialogUpdate(const SceCommonDialogUpdateParam *param) {
  if (!atomic_load(&g_menu_open) || !g_framework)
    return __real_sceCommonDialogUpdate(param);
  const SceCommonDialogRenderTargetInfo *target = &param->renderTarget;
  const unsigned width = target->width, height = target->height, stride = target->strideInPixels;
  if (width != 960 || height != 544 || !target->colorSurfaceData) return -1;
  /* vitaGL's GPU work and pending flips are done: no buffer is being written
   * or scanned out except the one on screen. */
  sceGxmFinish(gxm_context);
  sceGxmDisplayQueueFinish();
  if (!g_snapshot_valid) {
    /* The game frame on screen, at half brightness. */
    const uint32_t *front = (const uint32_t *)gxm_color_surfaces_addr[gxm_front_buffer_index];
    for (unsigned y = 0; y < height; ++y)
      for (unsigned x = 0; x < width; ++x)
        g_snapshot[y * width + x] = ((front[y * stride + x] >> 1) & 0x007f7f7fu) | 0xff000000u;
    g_snapshot_valid = 1;
  }
  uint32_t *back = (uint32_t *)target->colorSurfaceData;
  for (unsigned y = 0; y < height; ++y)
    memcpy(back + y * stride, g_snapshot + y * width, width * 4);
  /* Framework::Update's target, as libcdlg_main builds it. */
  uint32_t given[8] = {
    stride, width, height, (uint32_t)target->colorFormat, (uint32_t)target->surfaceType,
    (uint32_t)(uintptr_t)target->colorSurfaceData, 0, (uint32_t)(uintptr_t)param->displaySyncObject
  };
  g_framework->Update(given);
  return 0;
}
