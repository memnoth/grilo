/*
 * Copyright (C) 2010 Igalia S.L.
 *
 * Contact: Iago Toral Quiroga <itoral@igalia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include "ms-media-source.h"
#include "ms-metadata-source-priv.h"
#include "content/ms-content-media.h"
#include "content/ms-content-box.h"

#include <string.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "ms-media-source"

#define MS_MEDIA_SOURCE_GET_PRIVATE(object)				\
  (G_TYPE_INSTANCE_GET_PRIVATE((object), MS_TYPE_MEDIA_SOURCE, MsMediaSourcePrivate))

struct _MsMediaSourcePrivate {
  GHashTable *pending_operations;
};

struct FullResolutionCtlCb {
  MsMediaSourceResultCb user_callback;
  gpointer user_data;
  GList *source_map_list;
  guint flags;
};

struct FullResolutionDoneCb {
  MsMediaSourceResultCb user_callback;
  gpointer user_data;
  guint pending_callbacks;
  MsMediaSource *source;
  guint browse_id;
  guint remaining;
};

struct BrowseRelayCb {
  MsMediaSourceResultCb user_callback;
  gpointer user_data;
  gboolean use_idle;
  MsMediaSourceBrowseSpec *bspec;
  MsMediaSourceSearchSpec *sspec;
  MsMediaSourceQuerySpec *qspec;
};

struct BrowseRelayIdle {
  MsMediaSourceResultCb user_callback;
  gpointer user_data;
  MsMediaSource *source;
  guint browse_id;
  MsContentMedia *media;
  guint remaining;
  GError *error;
};

struct MetadataFullResolutionCtlCb {
  MsMediaSourceMetadataCb user_callback;
  gpointer user_data;
  GList *source_map_list;
  guint flags;
};

struct MetadataFullResolutionDoneCb {
  MsMediaSourceMetadataCb user_callback;
  gpointer user_data;
  guint pending_callbacks;
  MsMediaSource *source;
  struct MetadataFullResolutionCtlCb *ctl_info;;
};

struct MetadataRelayCb {
  MsMediaSourceMetadataCb user_callback;
  gpointer user_data;
  MsMediaSourceMetadataSpec *spec;
};

static guint ms_media_source_gen_browse_id (MsMediaSource *source);
static MsSupportedOps ms_media_source_supported_operations (MsMetadataSource *metadata_source);

/* ================ MsMediaSource GObject ================ */

G_DEFINE_ABSTRACT_TYPE (MsMediaSource, ms_media_source, MS_TYPE_METADATA_SOURCE);

static void
ms_media_source_class_init (MsMediaSourceClass *media_source_class)
{
  GObjectClass *gobject_class;
  MsMetadataSourceClass *metadata_source_class;

  gobject_class = G_OBJECT_CLASS (media_source_class);
  metadata_source_class = MS_METADATA_SOURCE_CLASS (media_source_class);

  metadata_source_class->supported_operations =
    ms_media_source_supported_operations;

  g_type_class_add_private (media_source_class, sizeof (MsMediaSourcePrivate));

  media_source_class->browse_id = 1;
}

static void
ms_media_source_init (MsMediaSource *source)
{
  source->priv = MS_MEDIA_SOURCE_GET_PRIVATE (source);
  memset (source->priv, 0, sizeof (MsMediaSourcePrivate));
  source->priv->pending_operations =
    g_hash_table_new (g_direct_hash, g_direct_equal);
}

/* ================ Utitilies ================ */

static void
set_operation_finished (MsMediaSource *source, guint operation_id)
{
  g_debug ("set_operation_finished (%d)", operation_id);
  g_hash_table_remove (source->priv->pending_operations,
		       GINT_TO_POINTER (operation_id));
}

static void
set_operation_ongoing (MsMediaSource *source, guint operation_id)
{
  g_debug ("set_operation_ongoing (%d)", operation_id);  
  g_hash_table_insert (source->priv->pending_operations,
		       GINT_TO_POINTER (operation_id), NULL);
}

static gboolean
operation_is_ongoing (MsMediaSource *source, guint operation_id)
{
  return g_hash_table_lookup_extended (source->priv->pending_operations,
				       GINT_TO_POINTER (operation_id),
				       NULL, NULL);
}

static void
free_browse_operation_spec (MsMediaSourceBrowseSpec *spec)
{
  g_object_unref (spec->source);
  g_object_unref (spec->container);
  g_list_free (spec->keys);
  g_free (spec);
}

static void
free_search_operation_spec (MsMediaSourceSearchSpec *spec)
{
  g_object_unref (spec->source);
  g_free (spec->text);
  g_list_free (spec->keys);
  g_free (spec);
}

static void
free_query_operation_spec (MsMediaSourceQuerySpec *spec)
{
  g_object_unref (spec->source);
  g_free (spec->query);
  g_list_free (spec->keys);
  g_free (spec);
}

static void
free_source_map_list (GList *source_map_list)
{
  GList *iter;
  iter = source_map_list;
  while (iter) {
    struct SourceKeyMap *map = (struct SourceKeyMap *) iter->data;
    g_object_unref (map->source);
    g_list_free (map->keys);
    iter = g_list_next (iter);
  }
  g_list_free (source_map_list);
}

static gboolean
browse_result_relay_idle (gpointer user_data)
{
  g_debug ("browse_result_relay_idle");

  struct BrowseRelayIdle *bri = (struct BrowseRelayIdle *) user_data;

  /* Check if operation was cancelled (could be cancelled between the relay
     callback and this idle loop iteration). Remember that we do
     emit the last result (remaining == 0) wich comes with a NULL media  */
  if (operation_is_ongoing (bri->source, bri->browse_id) ||
      bri->remaining == 0) {
    bri->user_callback (bri->source,
			bri->browse_id,
			bri->media,
			bri->remaining,
			bri->user_data,
			bri->error);
  } else {
    g_debug ("operation was cancelled, skipping idle result!");
  }

  /* We copy the error if we do idle relay, we have to free it here */
  if (bri->error) {
    g_error_free (bri->error);
  }

  g_free (bri);

  return FALSE;
}

static void
browse_result_relay_cb (MsMediaSource *source,
			guint browse_id,
			MsContentMedia *media,
			guint remaining,
			gpointer user_data,
			const GError *error)
{
  struct BrowseRelayCb *brc;
  gchar *source_id;

  g_debug ("browse_result_relay_cb");

  /* Check if operation was cancelled, if so, do not emit the result
     but make sure to free the operation data when remaining is 0 */
  if (!operation_is_ongoing (source, browse_id)) {
    g_debug ("operation was cancelled, skipping result!");
    if (media) {
      g_object_unref (media);
      media = NULL;
    }
    if (remaining > 0) {
      /* We freed the media and silently ignore the result, we do not do
       anything else until we get the one with remaining == 0*/
      return;
    }
    /* If remaining == 0, we have to let it through the chain of callbacks,
       because it is the signal that indicates the operation is finished
       so they can free their respective user_datas */
  }

  brc = (struct BrowseRelayCb *) user_data;

  if (media) {
    source_id = ms_metadata_source_get_id (MS_METADATA_SOURCE (source));  
    ms_content_media_set_source (media, source_id);
    g_free (source_id);
  }

  if (brc->use_idle) {
    struct BrowseRelayIdle *bri = g_new (struct BrowseRelayIdle, 1);
    bri->source = source;
    bri->browse_id = browse_id;
    bri->media = media;
    bri->remaining = remaining;
    bri->error = (GError *) (error ? g_error_copy (error) : NULL);
    bri->user_callback = brc->user_callback;
    bri->user_data = brc->user_data;
    g_idle_add (browse_result_relay_idle, bri);
    if (remaining == 0) {
      /* We can free the operation data, but we must nor mark the
	 operation as finished yet, since we still have to emit this
	 last result in the idle loop */
      goto free_operation_data;
    }
  } else {
    brc->user_callback (source,
			browse_id,
			media,
			remaining,
			brc->user_data,
			error);
    if (remaining == 0) {
      /* We are done with this operation, mark it as finished
	 and free associated memory */
      goto operation_finished;
    }
  }
  
  return;

 operation_finished:
  set_operation_finished (source, browse_id);

 free_operation_data:
  if (brc->bspec) {
    free_browse_operation_spec (brc->bspec);
  } else if (brc->sspec) {
    free_search_operation_spec (brc->sspec);
  } else if (brc->sspec) {
    free_query_operation_spec (brc->qspec);
  }
  g_free (brc);
}

static void
metadata_result_relay_cb (MsMediaSource *source,
			  MsContentMedia *media,
			  gpointer user_data,
			  const GError *error)
{
  g_debug ("metadata_result_relay_cb");

  struct MetadataRelayCb *mrc;
  gchar *source_id;

  mrc = (struct MetadataRelayCb *) user_data;
  if (media) {
    source_id = ms_metadata_source_get_id (MS_METADATA_SOURCE (source));  
    ms_content_media_set_source (media, source_id);
    g_free (source_id);
  }

  mrc->user_callback (source, media, mrc->user_data, error);

  g_object_unref (mrc->spec->source);
  if (mrc->spec->media) {
    /* Can be NULL if getting metadata for root category */
    g_object_unref (mrc->spec->media);
  }
  g_list_free (mrc->spec->keys);
  g_free (mrc->spec);
  g_free (mrc);
}

static gboolean
browse_idle (gpointer user_data)
{
  g_debug ("browse_idle");
  MsMediaSourceBrowseSpec *bs = (MsMediaSourceBrowseSpec *) user_data;
  MS_MEDIA_SOURCE_GET_CLASS (bs->source)->browse (bs->source, bs);
  return FALSE;
}

static gboolean
search_idle (gpointer user_data)
{
  g_debug ("search_idle");
  MsMediaSourceSearchSpec *ss = (MsMediaSourceSearchSpec *) user_data;
  MS_MEDIA_SOURCE_GET_CLASS (ss->source)->search (ss->source, ss);
  return FALSE;
}

static gboolean
query_idle (gpointer user_data)
{
  g_debug ("query_idle");
  MsMediaSourceQuerySpec *qs = (MsMediaSourceQuerySpec *) user_data;
  MS_MEDIA_SOURCE_GET_CLASS (qs->source)->query (qs->source, qs);
  return FALSE;
}

static gboolean
metadata_idle (gpointer user_data)
{
  g_debug ("metadata_idle");
  MsMediaSourceMetadataSpec *ms = (MsMediaSourceMetadataSpec *) user_data;
  MS_MEDIA_SOURCE_GET_CLASS (ms->source)->metadata (ms->source, ms);
  return FALSE;
}

static void
full_resolution_done_cb (MsMetadataSource *source,
			 MsContentMedia *media,
			 gpointer user_data,
			 const GError *error)
{
  g_debug ("full_resolution_done_cb");

  gboolean cancelled = FALSE;
  struct FullResolutionDoneCb *cb_info = 
    (struct FullResolutionDoneCb *) user_data;

  cb_info->pending_callbacks--;
  
  if (error) {
    g_warning ("Failed to fully resolve some metadata: %s", error->message);
  }

  /* If we are done with this result, invoke the user's callback */
  if (cb_info->pending_callbacks == 0) {
    /* But check if operation was cancelled before emitting 
       (we execute in the idle loop) */
    if (!operation_is_ongoing (cb_info->source, cb_info->browse_id)) {
      g_debug ("operation was cancelled, skipping full resolution done result!");
      if (media) {
	g_object_unref (media);
	media = NULL;
      }
      cancelled = TRUE;
    } 

    if (!cancelled || cb_info->remaining == 0) {
      cb_info->user_callback (cb_info->source, 
			      cb_info->browse_id, 
			      media,
			      cb_info->remaining, 
			      cb_info->user_data,
			      NULL);
      /* Notice we pass NULL as error on purpose
	 since the result is valid even if the full-resolution failed */
    }

    g_free (cb_info);
  }
}

static void
full_resolution_ctl_cb (MsMediaSource *source,
			guint browse_id,
			MsContentMedia *media,
			guint remaining,
			gpointer user_data,
			const GError *error)
{
  GList *iter;
  gboolean cancelled = FALSE;
  struct FullResolutionCtlCb *ctl_info =
    (struct FullResolutionCtlCb *) user_data;
  
  g_debug ("full_resolution_ctl_cb");

  /* Check if operation was cancelled */
  if (!operation_is_ongoing (source, browse_id)) {
    g_debug ("operation cancelled, skipping full resolution ctl result!");
    cancelled = TRUE;
    if (media) {
      g_object_unref (media);
      media = NULL;
    }
    if (remaining != 0) {
      /* We only signal to the UI once with remaining 0, we just skip 
	 the rest */
      return;
    } else {
      /* If op is cancelled we do not want to signal the error, just
	 the cancellation */
      error = NULL;
    }
  }

  /* We only get here if the op is not cancelled or it is cancelled 
     but remaining == 0 */
  
  if (cancelled || error || !media) {
    /* No need to start full resolution */
    ctl_info->user_callback (source,
			     browse_id,
			     media,
			     remaining,
			     ctl_info->user_data,
			     error);
  } else {
    /* Start full-resolution: save all the data we need to emit the result 
       when fully resolved */
    struct FullResolutionDoneCb *done_info =
      g_new (struct FullResolutionDoneCb, 1);
    done_info->user_callback = ctl_info->user_callback;
    done_info->user_data = ctl_info->user_data;
    done_info->pending_callbacks = g_list_length (ctl_info->source_map_list);
    done_info->source = source;
    done_info->browse_id = browse_id;
    done_info->remaining = remaining;
    
    /* Use sources in the map to fill in missing metadata, the "done"
       callback will be used to emit the resulting object when 
       all metadata has been gathered */
    iter = ctl_info->source_map_list;
    while (iter) {
      gchar *name;
      struct SourceKeyMap *map = (struct SourceKeyMap *) iter->data;
      g_object_get (map->source, "source-name", &name, NULL);
      g_debug ("Using '%s' to resolve extra metadata now", name);
      g_free (name);
      
      ms_metadata_source_resolve (map->source, 
				  map->keys, 
				  media, 
				  ctl_info->flags,
				  full_resolution_done_cb,
				  done_info);
      
      iter = g_list_next (iter);
    }
  }

  /* When we are done with the last result, free our data */
  if (remaining == 0) {
    free_source_map_list (ctl_info->source_map_list);
    g_free (ctl_info);
  }
}

static void
metadata_full_resolution_done_cb (MsMetadataSource *source,
				  MsContentMedia *media,
				  gpointer user_data,
				  const GError *error)
{
  g_debug ("metadata_full_resolution_done_cb");

  struct MetadataFullResolutionDoneCb *cb_info = 
    (struct MetadataFullResolutionDoneCb *) user_data;

  cb_info->pending_callbacks--;

  if (error) {
    g_warning ("Failed to fully resolve some metadata: %s", error->message);
  }

  if (cb_info->pending_callbacks == 0) {
    cb_info->user_callback (cb_info->source, 
			    media,
			    cb_info->user_data,
			    NULL);
    
    free_source_map_list (cb_info->ctl_info->source_map_list);
    g_free (cb_info->ctl_info);
    g_free (cb_info);
  }
}

static void
metadata_full_resolution_ctl_cb (MsMediaSource *source,
				 MsContentMedia *media,
				 gpointer user_data,
				 const GError *error)
{
  GList *iter;

  struct MetadataFullResolutionCtlCb *ctl_info =
    (struct MetadataFullResolutionCtlCb *) user_data;

  g_debug ("metadata_full_resolution_ctl_cb");

  /* If we got an error, invoke the user callback right away and bail out */
  if (error) {
    g_warning ("Operation failed: %s", error->message);
    ctl_info->user_callback (source,
			     media,
			     ctl_info->user_data,
			     error);
    return;
  }

  /* Save all the data we need to emit the result */
  struct MetadataFullResolutionDoneCb *done_info =
    g_new (struct MetadataFullResolutionDoneCb, 1);
  done_info->user_callback = ctl_info->user_callback;
  done_info->user_data = ctl_info->user_data;
  done_info->pending_callbacks = g_list_length (ctl_info->source_map_list);
  done_info->source = source;
  done_info->ctl_info = ctl_info;

  /* Use sources in the map to fill in missing metadata, the "done"
     callback will be used to emit the resulting object when 
     all metadata has been gathered */
  iter = ctl_info->source_map_list;
  while (iter) {
    gchar *name;
    struct SourceKeyMap *map = (struct SourceKeyMap *) iter->data;
    g_object_get (map->source, "source-name", &name, NULL);
    g_debug ("Using '%s' to resolve extra metadata now", name);
    g_free (name);

    ms_metadata_source_resolve (map->source, 
				map->keys, 
				media, 
				ctl_info->flags,
				metadata_full_resolution_done_cb,
				done_info);
    
    iter = g_list_next (iter);
  }
}

static guint
ms_media_source_gen_browse_id (MsMediaSource *source)
{
  MsMediaSourceClass *klass;
  klass = MS_MEDIA_SOURCE_GET_CLASS (source);
  return klass->browse_id++;
}

/* ================ API ================ */

guint
ms_media_source_browse (MsMediaSource *source, 
			MsContentMedia *container,
			const GList *keys,
			guint skip,
			guint count,
			MsMetadataResolutionFlags flags,
			MsMediaSourceResultCb callback,
			gpointer user_data)
{
  MsMediaSourceResultCb _callback;
  gpointer _user_data ;
  GList *_keys;
  struct SourceKeyMapList key_mapping;
  MsMediaSourceBrowseSpec *bs;
  guint browse_id;
  struct BrowseRelayCb *brc;
  
  g_return_val_if_fail (MS_IS_MEDIA_SOURCE (source), 0);
  g_return_val_if_fail (callback != NULL, 0);
  g_return_val_if_fail (count > 0, 0);
  g_return_val_if_fail (ms_metadata_source_supported_operations (MS_METADATA_SOURCE (source)) &
			MS_OP_BROWSE, 0);

  /* By default assume we will use the parameters specified by the user */
  _keys = g_list_copy ((GList *) keys);
  _callback = callback;
  _user_data = user_data;

  if (flags & MS_RESOLVE_FAST_ONLY) {
    g_debug ("requested fast keys only");
    ms_metadata_source_filter_slow (MS_METADATA_SOURCE (source), &_keys, FALSE);
  }

  if (flags & MS_RESOLVE_FULL) {
    g_debug ("requested full resolution");
    ms_metadata_source_setup_full_resolution_mode (MS_METADATA_SOURCE (source),
						   _keys, &key_mapping);
    
    /* If we do not have a source map for the unsupported keys then
       we cannot resolve any of them */
    if (key_mapping.source_maps != NULL) {
      struct FullResolutionCtlCb *c = g_new0 (struct FullResolutionCtlCb, 1);
      c->user_callback = callback;
      c->user_data = user_data;
      c->source_map_list = key_mapping.source_maps;
      c->flags = flags;
      
      _callback = full_resolution_ctl_cb;
      _user_data = c;
      g_list_free (_keys);
      _keys = key_mapping.operation_keys;
    }    
  }

  browse_id = ms_media_source_gen_browse_id (source);

  /* Always hook an own relay callback so we can do some
     post-processing before handing out the results
     to the user */
  brc = g_new0 (struct BrowseRelayCb, 1);
  brc->user_callback = _callback;
  brc->user_data = _user_data;
  brc->use_idle = flags & MS_RESOLVE_IDLE_RELAY;
  _callback = browse_result_relay_cb;
  _user_data = brc;


  bs = g_new0 (MsMediaSourceBrowseSpec, 1);
  bs->source = g_object_ref (source);
  bs->browse_id = browse_id;
  bs->keys = _keys;
  bs->skip = skip;
  bs->count = count;
  bs->flags = flags;
  bs->callback = _callback;
  bs->user_data = _user_data;
  if (!container) {
    /* Special case: NULL container ==> NULL id */
    bs->container = ms_content_box_new ();
    ms_content_media_set_id (bs->container, NULL);
  } else {
    bs->container = g_object_ref (container);
  }

  /* Save a reference to the operaton spec in the relay-cb's 
     user_data so that we can free the spec there when we get
     the last result */
  brc->bspec = bs;

  set_operation_ongoing (source, browse_id);
  g_idle_add (browse_idle, bs);
  
  return browse_id;
}

guint
ms_media_source_search (MsMediaSource *source,
                        const gchar *text,
                        const GList *keys,
                        guint skip,
                        guint count,
                        MsMetadataResolutionFlags flags,
                        MsMediaSourceResultCb callback,
                        gpointer user_data)
{
  MsMediaSourceResultCb _callback;
  gpointer _user_data ;
  GList *_keys;
  struct SourceKeyMapList key_mapping;
  MsMediaSourceSearchSpec *ss;
  guint search_id;
  struct BrowseRelayCb *brc;

  g_return_val_if_fail (MS_IS_MEDIA_SOURCE (source), 0);
  g_return_val_if_fail (text != NULL, 0);
  g_return_val_if_fail (callback != NULL, 0);
  g_return_val_if_fail (count > 0, 0);
  g_return_val_if_fail (ms_metadata_source_supported_operations (MS_METADATA_SOURCE (source)) &
			MS_OP_SEARCH, 0);

  /* By default assume we will use the parameters specified by the user */
  _callback = callback;
  _user_data = user_data;
  _keys = g_list_copy ((GList *) keys);

  if (flags & MS_RESOLVE_FAST_ONLY) {
    g_debug ("requested fast keys only");
    ms_metadata_source_filter_slow (MS_METADATA_SOURCE (source), &_keys, FALSE);
  }

  if (flags & MS_RESOLVE_FULL) {
    g_debug ("requested full search");
    ms_metadata_source_setup_full_resolution_mode (MS_METADATA_SOURCE (source),
						   _keys, &key_mapping);
    
    /* If we do not have a source map for the unsupported keys then
       we cannot resolve any of them */
    if (key_mapping.source_maps != NULL) {
      struct FullResolutionCtlCb *c = g_new0 (struct FullResolutionCtlCb, 1);
      c->user_callback = callback;
      c->user_data = user_data;
      c->source_map_list = key_mapping.source_maps;
      c->flags = flags;
      
      _callback = full_resolution_ctl_cb;
      _user_data = c;
      g_list_free (_keys);
      _keys = key_mapping.operation_keys;
    }    
  }

  search_id = ms_media_source_gen_browse_id (source);

  brc = g_new0 (struct BrowseRelayCb, 1);
  brc->user_callback = _callback;
  brc->user_data = _user_data;
  brc->use_idle = flags & MS_RESOLVE_IDLE_RELAY;
  _callback = browse_result_relay_cb;
  _user_data = brc;

  ss = g_new0 (MsMediaSourceSearchSpec, 1);
  ss->source = g_object_ref (source);
  ss->search_id = search_id;
  ss->text = g_strdup (text);
  ss->keys = _keys;
  ss->skip = skip;
  ss->count = count;
  ss->flags = flags;
  ss->callback = _callback;
  ss->user_data = _user_data;

  /* Save a reference to the operaton spec in the relay-cb's 
     user_data so that we can free the spec there when we get
     the last result */
  brc->sspec = ss;  

  set_operation_ongoing (source, search_id);
  g_idle_add (search_idle, ss);

  return search_id;
}

guint
ms_media_source_query (MsMediaSource *source,
		       const gchar *query,
		       const GList *keys,
		       guint skip,
		       guint count,
		       MsMetadataResolutionFlags flags,
		       MsMediaSourceResultCb callback,
		       gpointer user_data)
{
  MsMediaSourceResultCb _callback;
  gpointer _user_data ;
  GList *_keys;
  struct SourceKeyMapList key_mapping;
  MsMediaSourceQuerySpec *qs;
  guint query_id;
  struct BrowseRelayCb *brc;

  g_return_val_if_fail (MS_IS_MEDIA_SOURCE (source), 0);
  g_return_val_if_fail (query != NULL, 0);
  g_return_val_if_fail (callback != NULL, 0);
  g_return_val_if_fail (count > 0, 0);
  g_return_val_if_fail (ms_metadata_source_supported_operations (MS_METADATA_SOURCE (source)) &
			MS_OP_QUERY, 0);

  /* By default assume we will use the parameters specified by the user */
  _callback = callback;
  _user_data = user_data;
  _keys = g_list_copy ((GList *) keys);

  if (flags & MS_RESOLVE_FAST_ONLY) {
    g_debug ("requested fast keys only");
    ms_metadata_source_filter_slow (MS_METADATA_SOURCE (source), &_keys, FALSE);
  }

  if (flags & MS_RESOLVE_FULL) {
    g_debug ("requested full search");
    ms_metadata_source_setup_full_resolution_mode (MS_METADATA_SOURCE (source),
						   _keys, &key_mapping);
    
    /* If we do not have a source map for the unsupported keys then
       we cannot resolve any of them */
    if (key_mapping.source_maps != NULL) {
      struct FullResolutionCtlCb *c = g_new0 (struct FullResolutionCtlCb, 1);
      c->user_callback = callback;
      c->user_data = user_data;
      c->source_map_list = key_mapping.source_maps;
      c->flags = flags;
      
      _callback = full_resolution_ctl_cb;
      _user_data = c;
      g_list_free (_keys);
      _keys = key_mapping.operation_keys;
    }    
  }

  query_id = ms_media_source_gen_browse_id (source);

  brc = g_new0 (struct BrowseRelayCb, 1);
  brc->user_callback = _callback;
  brc->user_data = _user_data;
  brc->use_idle = flags & MS_RESOLVE_IDLE_RELAY;
  _callback = browse_result_relay_cb;
  _user_data = brc;

  qs = g_new0 (MsMediaSourceQuerySpec, 1);
  qs->source = g_object_ref (source);
  qs->query_id = query_id;
  qs->query = g_strdup (query);
  qs->keys = _keys;
  qs->skip = skip;
  qs->count = count;
  qs->flags = flags;
  qs->callback = _callback;
  qs->user_data = _user_data;

  /* Save a reference to the operaton spec in the relay-cb's 
     user_data so that we can free the spec there when we get
     the last result */
  brc->qspec = qs;  

  set_operation_ongoing (source, query_id);
  g_idle_add (query_idle, qs);

  return query_id;
}

void
ms_media_source_metadata (MsMediaSource *source,
			  MsContentMedia *media,
			  const GList *keys,
			  MsMetadataResolutionFlags flags,
			  MsMediaSourceMetadataCb callback,
			  gpointer user_data)
{
  MsMediaSourceMetadataCb _callback;
  gpointer _user_data ;
  GList *_keys;
  struct SourceKeyMapList key_mapping;
  MsMediaSourceMetadataSpec *ms;
  struct MetadataRelayCb *mrc;

  g_debug ("ms_media_source_metadata");

  g_return_if_fail (MS_IS_MEDIA_SOURCE (source));
  g_return_if_fail (keys != NULL);
  g_return_if_fail (callback != NULL);
  g_return_if_fail (ms_metadata_source_supported_operations (MS_METADATA_SOURCE (source)) &
		    MS_OP_METADATA);

  /* By default assume we will use the parameters specified by the user */
  _callback = callback;
  _user_data = user_data;
  _keys = g_list_copy ((GList *) keys);

  if (flags & MS_RESOLVE_FAST_ONLY) {
    ms_metadata_source_filter_slow (MS_METADATA_SOURCE (source),
				    &_keys, FALSE);
  }

  if (flags & MS_RESOLVE_FULL) {
    g_debug ("requested full metadata");
    ms_metadata_source_setup_full_resolution_mode (MS_METADATA_SOURCE (source),
						   _keys, &key_mapping);

    /* If we do not have a source map for the unsupported keys then
       we cannot resolve any of them */
    if (key_mapping.source_maps != NULL) {
      struct MetadataFullResolutionCtlCb *c =
	g_new0 (struct MetadataFullResolutionCtlCb, 1);
      c->user_callback = callback;
      c->user_data = user_data;
      c->source_map_list = key_mapping.source_maps;
      c->flags = flags;
      
      _callback = metadata_full_resolution_ctl_cb;
      _user_data = c;
      g_list_free (_keys);
      _keys = key_mapping.operation_keys;
    }    
  }

  /* Always hook an own relay callback so we can do some
     post-processing before handing out the results
     to the user */
  mrc = g_new0 (struct MetadataRelayCb, 1);
  mrc->user_callback = _callback;
  mrc->user_data = _user_data;
  _callback = metadata_result_relay_cb;
  _user_data = mrc;

  ms = g_new0 (MsMediaSourceMetadataSpec, 1);
  ms->source = g_object_ref (source);
  ms->keys = _keys; /* It is already a copy */
  ms->flags = flags;
  ms->callback = _callback;
  ms->user_data = _user_data;
  if (!media) {
    /* Special case, NULL media ==> root container */
    ms->media = ms_content_box_new ();
    ms_content_media_set_id (ms->media, NULL);
  } else {
    ms->media = g_object_ref (media);
  }

  /* Save a reference to the operaton spec in the relay-cb's 
     user_data so that we can free the spec there */
  mrc->spec = ms;

  g_idle_add (metadata_idle, ms);
}

static MsSupportedOps
ms_media_source_supported_operations (MsMetadataSource *metadata_source)
{
  MsSupportedOps caps;
  MsMediaSource *source;
  MsMediaSourceClass *media_source_class;
  MsMetadataSourceClass *metadata_source_class;

  metadata_source_class =
    MS_METADATA_SOURCE_CLASS (ms_media_source_parent_class);
  source = MS_MEDIA_SOURCE (metadata_source);
  media_source_class = MS_MEDIA_SOURCE_GET_CLASS (source);

  caps = metadata_source_class->supported_operations (metadata_source);
  if (media_source_class->browse) 
    caps |= MS_OP_BROWSE;
  if (media_source_class->search)
    caps |= MS_OP_SEARCH;
  if (media_source_class->query)
    caps |= MS_OP_QUERY;
  if (media_source_class->metadata) 
    caps |= MS_OP_METADATA;

  return caps;
}

void
ms_media_source_cancel (MsMediaSource *source, guint operation_id)
{
  g_debug ("ms_media_source_cancel");

  g_return_if_fail (MS_IS_MEDIA_SOURCE (source));

  if (!operation_is_ongoing (source, operation_id)) {
    g_debug ("Tried to cancel invalid or finished operation");
    return;
  }

  /* Mark the operation as finished, if the source does
     not implement cancelation or it did not make it in time, we will
     not emit the results for this operation in any case.
     At any rate, we will not free the operation data until we are sure
     the plugin won't need it any more, which it will tell when it emits
     remaining = 0 (which can happen because it did not cancel the op
     or because it managed to cancel it and is signaling so) */
  set_operation_finished (source, operation_id);

  /* If the source provides an implementation for operacion cancelation,
     let's use that to avoid further unnecessary processing in the plugin */
  if (MS_MEDIA_SOURCE_GET_CLASS (source)->cancel) {
    MS_MEDIA_SOURCE_GET_CLASS (source)->cancel (source, operation_id);  
  }

}
