/* bjb-local-note.c
 * Copyright (C) Pierre-Yves LUYTEN 2013 <py@luyten.fr>
 *
 * bijiben is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * bijiben is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "biji-local-note.h"
#include "biji-tracker.h"
#include "serializer/biji-lazy-serializer.h"

struct _BijiLocalNote
{
  BijiNoteObj   parent_instance;
  BijiProvider *provider;
  GFile *location;
  char *basename;
  char *html;
  gboolean trashed;
};

G_DEFINE_TYPE (BijiLocalNote, biji_local_note, BIJI_TYPE_NOTE_OBJ)

static const char *
local_note_get_place (BijiItem *item)
{
  BijiLocalNote *self = BIJI_LOCAL_NOTE (item);
  const BijiProviderInfo *info = biji_provider_get_info (self->provider);

  return info->name;
}

static char *
local_note_get_html (BijiNoteObj *note)
{
  BijiLocalNote *self = BIJI_LOCAL_NOTE (note);

  return g_strdup (self->html);
}

static void
local_note_set_html (BijiNoteObj *note,
                     const char  *html)
{
  BijiLocalNote *self = BIJI_LOCAL_NOTE (note);

  g_free (self->html);

  if (html)
    self->html = g_strdup (html);
}

static void
local_note_save (BijiNoteObj *note)
{
  BijiItem *item = BIJI_ITEM (note);
  BijiLocalNote *self = BIJI_LOCAL_NOTE (note);
  const BijiProviderInfo *prov_info = biji_provider_get_info (self->provider);
  BijiInfoSet *info = biji_info_set_new ();

  /* File save */
  biji_lazy_serialize (note);

  /* Tracker */
  info->url = (char *) biji_item_get_uuid (item);
  info->title = (char *) biji_item_get_title (item);
  info->content = (char *) biji_note_obj_get_raw_text (note);
  info->mtime = biji_item_get_mtime (item);
  info->created = biji_note_obj_get_create_date (note);
  info->datasource_urn = g_strdup (prov_info->datasource);

  biji_tracker_ensure_resource_from_info (biji_item_get_manager (item), info);
}

static void
biji_local_note_finalize (GObject *object)
{
  BijiLocalNote *self = BIJI_LOCAL_NOTE (object);

  g_clear_object (&self->location);
  g_free (self->basename);
  g_free (self->html);

  G_OBJECT_CLASS (biji_local_note_parent_class)->finalize (object);
}

static void
biji_local_note_init (BijiLocalNote *self)
{
}

static gboolean
item_yes (BijiItem *item)
{
  return TRUE;
}

static gboolean
note_yes (BijiNoteObj *item)
{
  return TRUE;
}

static gboolean
local_note_archive (BijiNoteObj *note)
{
  BijiLocalNote *self = BIJI_LOCAL_NOTE (note);
  g_autoptr(GFile) parent = NULL;
  g_autoptr(GFile) trash = NULL;
  g_autoptr(GFile) archive = NULL;
  g_autofree char *parent_path = NULL;
  g_autofree char *trash_path = NULL;
  g_autofree char *backup_path = NULL;
  g_autoptr(GError) error = NULL;
  gboolean result = FALSE;

  /* Create the trash directory
   * No matter if already exists */
  parent = g_file_get_parent (self->location);
  parent_path = g_file_get_path (parent);
  trash_path = g_build_filename (parent_path, ".Trash", NULL);
  trash = g_file_new_for_path (trash_path);
  g_file_make_directory (trash, NULL, NULL);

  /* Move the note to trash */
  backup_path = g_build_filename (trash_path, self->basename, NULL);
  archive = g_file_new_for_path (backup_path);

  result = g_file_move (self->location,
                        archive,
                        G_FILE_COPY_OVERWRITE,
                        NULL, // cancellable
                        NULL, // progress callback
                        NULL, // progress_callback_data,
                        &error);

  if (error)
    g_message ("%s", error->message);
  else
    {
      g_set_object (&self->location, archive);
      self->trashed = TRUE;
    }

  return result;
}

static gboolean
local_note_is_trashed (BijiNoteObj *note)
{
  BijiLocalNote *self = BIJI_LOCAL_NOTE (note);
  return self->trashed;
}

static gboolean
local_note_restore (BijiItem  *item,
                    char     **old_uuid)
{
  BijiLocalNote *self = BIJI_LOCAL_NOTE (item);
  g_autofree char *root_path = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GFile) trash = NULL;
  g_autoptr(GFile) root = NULL;
  g_autoptr(GFile) target = NULL;
  gboolean retval;
  g_autoptr(GError) error = NULL;

  trash = g_file_get_parent (self->location);
  root = g_file_get_parent (trash);

  root_path = g_file_get_path (root);
  path = g_build_filename (root_path, self->basename, NULL);
  target = g_file_new_for_path (path);

  retval = g_file_move (self->location,
                        target,
                        G_FILE_COPY_NONE,
                        NULL, // cancellable
                        NULL, // progress callback
                        NULL, // progress_callback_data,
                        &error);

  if (error != NULL)
    g_warning ("Could not restore file : %s", error->message);

  *old_uuid = g_file_get_path (self->location);
  g_set_object (&self->location, target);
  self->trashed = FALSE;

  return retval;
}

static void
delete_file (GObject      *note,
             GAsyncResult *res,
             gpointer      user_data)
{
  BijiLocalNote *self = BIJI_LOCAL_NOTE (user_data);
  g_autoptr (GError) error = NULL;

  g_file_delete_finish (self->location, res, &error);

  if (error != NULL)
    g_warning ("local note delete failed, %s", error->message);
}

/* Do not check if note is already trashed
 * eg, note is empty */
static gboolean
local_note_delete (BijiItem *item)
{
  BijiLocalNote *self = BIJI_LOCAL_NOTE (item);
  g_autofree char *file_path = g_file_get_path (self->location);

  g_debug ("local note delete : %s", file_path);

  biji_note_delete_from_tracker (BIJI_NOTE_OBJ (self));
  g_file_delete_async (self->location,
                       G_PRIORITY_LOW,
                       NULL,                  /* Cancellable */
                       delete_file,
                       self);

  return TRUE;
}

static char *
local_note_get_basename (BijiNoteObj *note)
{
  BijiLocalNote *self = BIJI_LOCAL_NOTE (note);
  return self->basename;
}

static void
biji_local_note_class_init (BijiLocalNoteClass *klass)
{
  GObjectClass *g_object_class = G_OBJECT_CLASS (klass);
  BijiItemClass *item_class = BIJI_ITEM_CLASS (klass);
  BijiNoteObjClass *note_class = BIJI_NOTE_OBJ_CLASS (klass);

  g_object_class->finalize = biji_local_note_finalize;

  item_class->is_collectable = item_yes;
  item_class->has_color = item_yes;
  item_class->get_place = local_note_get_place;
  item_class->restore = local_note_restore;
  item_class->delete = local_note_delete;

  note_class->get_basename = local_note_get_basename;
  note_class->get_html = local_note_get_html;
  note_class->set_html = local_note_set_html;
  note_class->save_note = local_note_save;
  note_class->can_format = note_yes;
  note_class->archive = local_note_archive;
  note_class->is_trashed = local_note_is_trashed;
}

BijiNoteObj *
biji_local_note_new_from_info (BijiProvider *provider,
                               BijiManager  *manager,
                               BijiInfoSet  *info)
{
  BijiLocalNote *self = g_object_new (BIJI_TYPE_LOCAL_NOTE,
                                       "manager", manager,
                                       "path",    info->url,
                                       "title",   info->title,
                                       "mtime",   info->mtime,
                                       "content", info->content,
                                       NULL);

  self->location = g_file_new_for_commandline_arg (info->url);
  self->basename = g_file_get_basename (self->location);
  self->provider = provider;

  if (strstr (info->url, "Trash") != NULL)
    self->trashed = TRUE;

  return BIJI_NOTE_OBJ (self);
}
