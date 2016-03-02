/*
 * (C) Copyright 2015 Kurento (http://kurento.org/)
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "kmssdpgroupmanager.h"
#include "kmssdpmidext.h"
#include "kmsutils.h"

#define OBJECT_NAME "sdpgroupmanager"

GST_DEBUG_CATEGORY_STATIC (kms_sdp_group_manager_debug_category);
#define GST_CAT_DEFAULT kms_sdp_group_manager_debug_category

#define parent_class kms_sdp_group_manager_parent_class

G_DEFINE_TYPE_WITH_CODE (KmsSdpGroupManager, kms_sdp_group_manager,
    G_TYPE_OBJECT,
    GST_DEBUG_CATEGORY_INIT (kms_sdp_group_manager_debug_category, OBJECT_NAME,
        0, "debug category for sdp group_manager"));

#define KMS_SDP_GROUP_MANAGER_GET_PRIVATE(obj) (  \
  G_TYPE_INSTANCE_GET_PRIVATE (                   \
    (obj),                                        \
    KMS_TYPE_SDP_GROUP_MANAGER,                   \
    KmsSdpGroupManagerPrivate                     \
  )                                               \
)

struct _KmsSdpGroupManagerPrivate
{
  gint gid;
  GHashTable *groups;
  GHashTable *handlers;
  GHashTable *connected_signals;
  GHashTable *mids;
  GSList *used_mids;
};

typedef struct _MidExtData
{
  KmsRefStruct ref;
  gchar *mid;
  KmsSdpHandler *handler;
  KmsSdpGroupManager *gmanager;
} MidExtData;

typedef struct _SignalData
{
  gulong on_offer_id;
  gulong on_answer_id;
  GObject *obj;
} SignalData;

static void
mid_ext_data_destroy (MidExtData * data)
{
  kms_sdp_agent_common_unref_sdp_handler (data->handler);

  g_free (data->mid);

  g_slice_free (MidExtData, data);
}

static MidExtData *
mid_ext_data_new (KmsSdpHandler * handler, KmsSdpGroupManager * gmanager)
{
  MidExtData *data;

  data = g_slice_new0 (MidExtData);

  kms_ref_struct_init (KMS_REF_STRUCT_CAST (data),
      (GDestroyNotify) mid_ext_data_destroy);

  data->handler = kms_sdp_agent_common_ref_sdp_handler (handler);
  data->gmanager = gmanager;

  return data;
}

static void
signal_data_destroy (SignalData * data)
{
  g_clear_object (&data->obj);

  g_slice_free (SignalData, data);
}

static SignalData *
signal_data_new (gulong id1, gulong id2, GObject * obj)
{
  SignalData *data;

  data = g_slice_new0 (SignalData);

  data->on_offer_id = id1;
  data->on_answer_id = id2;
  data->obj = g_object_ref (obj);

  return data;
}

static void
disconnect_signals (SignalData * data)
{
  if (data->on_offer_id != 0) {
    g_signal_handler_disconnect (data->obj, data->on_offer_id);
    data->on_offer_id = 0UL;
  }

  if (data->on_answer_id != 0) {
    g_signal_handler_disconnect (data->obj, data->on_answer_id);
    data->on_answer_id = 0UL;
  }
}

static void
disconnect_pending_signals (gpointer key, gpointer value, gpointer user_data)
{
  disconnect_signals (value);
}

static void
kms_sdp_group_manager_finalize (GObject * object)
{
  KmsSdpGroupManager *self = KMS_SDP_GROUP_MANAGER (object);

  GST_DEBUG_OBJECT (self, "finalize");

  g_hash_table_foreach (self->priv->connected_signals,
      disconnect_pending_signals, NULL);
  g_hash_table_unref (self->priv->connected_signals);
  g_hash_table_unref (self->priv->mids);
  g_hash_table_unref (self->priv->groups);
  g_hash_table_unref (self->priv->handlers);
  g_slist_free_full (self->priv->used_mids, g_free);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gint
kms_sdp_group_manager_add_group_impl (KmsSdpGroupManager * self,
    KmsSdpBaseGroup * group)
{
  guint gid;

  gid = self->priv->gid++;

  g_hash_table_insert (self->priv->groups, GUINT_TO_POINTER (gid), group);

  return gid;
}

static gboolean
kms_sdp_group_manager_is_mid_used (KmsSdpGroupManager * self, const gchar * mid)
{
  GSList *l;

  for (l = self->priv->used_mids; l != NULL; l = g_slist_next (l)) {
    gchar *id = l->data;

    if (g_strcmp0 (id, mid) == 0) {
      return TRUE;
    }
  }

  return FALSE;
}

static gchar *
kms_sdp_group_manager_assign_mid (KmsSdpGroupManager * self,
    KmsSdpHandler * handler)
{
  guint *counter;
  gchar *mid = NULL;

  counter = g_hash_table_lookup (self->priv->mids, handler->media);

  if (counter == NULL) {
    /* No stored medias of this type yet */
    counter = g_slice_new0 (guint);
    g_hash_table_insert (self->priv->mids, g_strdup (handler->media), counter);
  }

  do {
    g_free (mid);
    mid = g_strdup_printf ("%s%u", handler->media, (*counter)++);
  } while (kms_sdp_group_manager_is_mid_used (self, mid));

  return mid;
}

static void
kms_sdp_group_manager_update_mid (KmsSdpGroupManager * self, MidExtData * data,
    gchar * mid)
{
  GSList *l;

  l = g_slist_find_custom (self->priv->used_mids, data->mid,
      (GCompareFunc) g_strcmp0);
  if (l != NULL) {
    g_free (l->data);
    self->priv->used_mids = g_slist_delete_link (self->priv->used_mids, l);
  }

  g_free (data->mid);
  data->mid = mid;
  self->priv->used_mids =
      g_slist_append (self->priv->used_mids, g_strdup (data->mid));
}

static gchar *
on_offer_mid_cb (KmsISdpMediaExtension * ext, MidExtData * data)
{
  if (data->mid != NULL) {
    GST_DEBUG_OBJECT (data->gmanager,
        "Provide already assigned mid %s for handler %u", data->mid,
        data->handler->id);
  } else {
    gchar *mid;

    mid = kms_sdp_group_manager_assign_mid (data->gmanager, data->handler);
    kms_sdp_group_manager_update_mid (data->gmanager, data, mid);
  }

  return g_strdup (data->mid);
}

static gboolean
on_answer_mid_cb (KmsISdpMediaExtension * ext, gchar * mid, MidExtData * data)
{
  if (!data->handler->negotiated) {
    kms_sdp_group_manager_update_mid (data->gmanager, data, g_strdup (mid));
    return TRUE;
  }

  if (g_strcmp0 (data->mid, mid) == 0) {
    return TRUE;
  }

  GST_ERROR_OBJECT (data->gmanager, "Mid negotiated does not match %s != %s",
      data->mid, mid);

  return FALSE;
}

static void
kms_sdp_group_manager_add_handler_impl (KmsSdpGroupManager * self,
    KmsSdpHandler * handler)
{
  KmsISdpMediaExtension *mid_ext;

  /* Add mid extension */
  mid_ext = KMS_I_SDP_MEDIA_EXTENSION (kms_sdp_mid_ext_new ());
  if (!kms_sdp_media_handler_add_media_extension (handler->handler, mid_ext)) {
    GST_WARNING_OBJECT (self, "Can not configure media stream ids");
    g_object_unref (mid_ext);
  } else {
    MidExtData *ext_data;
    SignalData *s_data;
    gulong id1, id2 = 0UL;

    g_hash_table_insert (self->priv->handlers,
        GUINT_TO_POINTER (handler->id), handler);
    ext_data = mid_ext_data_new (handler, self);

    id1 = g_signal_connect_data (mid_ext, "on-offer-mid",
        G_CALLBACK (on_offer_mid_cb), ext_data,
        (GClosureNotify) kms_ref_struct_unref, 0);

    id2 = g_signal_connect_data (mid_ext, "on-answer-mid",
        G_CALLBACK (on_answer_mid_cb),
        kms_ref_struct_ref (KMS_REF_STRUCT_CAST (ext_data)),
        (GClosureNotify) kms_ref_struct_unref, 0);

    s_data = signal_data_new (id1, id2, G_OBJECT (mid_ext));

    g_hash_table_insert (self->priv->connected_signals,
        GUINT_TO_POINTER (handler->id), s_data);
  }
}

static gboolean
kms_sdp_group_manager_remove_handler_impl (KmsSdpGroupManager * self,
    KmsSdpHandler * handler)
{
  SignalData *data;

  data = g_hash_table_lookup (self->priv->connected_signals,
      GUINT_TO_POINTER (handler->id));

  if (data == NULL) {
    return FALSE;
  }

  disconnect_signals (data);

  if (!g_hash_table_remove (self->priv->connected_signals,
          GUINT_TO_POINTER (handler->id))) {
    GST_WARNING_OBJECT (self, "No extension signals connected");
  }

  if (g_slist_length (handler->groups) > 0) {
    /* TODO: Remove this handler from groups in new API */
    GST_WARNING_OBJECT (self, "TODO: Remove handler %u from groups",
        handler->id);
  }

  return g_hash_table_remove (self->priv->handlers,
      GUINT_TO_POINTER (handler->id));
}

static gboolean
kms_sdp_group_manager_add_handler_to_group_impl (KmsSdpGroupManager * self,
    guint gid, guint hid)
{
  KmsSdpHandler *handler;
  KmsSdpBaseGroup *group;

  handler = g_hash_table_lookup (self->priv->handlers, GUINT_TO_POINTER (hid));
  if (handler == NULL) {
    GST_ERROR_OBJECT (self, "Handler %u is not registered", hid);
    return FALSE;
  }

  group = g_hash_table_lookup (self->priv->groups, GUINT_TO_POINTER (gid));
  if (group == NULL) {
    GST_ERROR_OBJECT (self, "Group %u is not created", gid);
    return FALSE;
  }

  return kms_sdp_base_group_add_media_handler (group, handler, NULL);
}

static gboolean
kms_sdp_group_manager_remove_handler_from_group_impl (KmsSdpGroupManager * self,
    guint gid, guint hid)
{
  KmsSdpHandler *handler;
  KmsSdpBaseGroup *group;

  handler = g_hash_table_lookup (self->priv->handlers, GUINT_TO_POINTER (hid));
  if (handler == NULL) {
    GST_ERROR_OBJECT (self, "Handler %u is not registered", hid);
    return FALSE;
  }

  group = g_hash_table_lookup (self->priv->groups, GUINT_TO_POINTER (gid));
  if (group == NULL) {
    GST_ERROR_OBJECT (self, "Group %u is not created", gid);
    return FALSE;
  }

  return kms_sdp_base_group_remove_media_handler (group, handler, NULL);
}

static void
kms_sdp_group_manager_class_init (KmsSdpGroupManagerClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = kms_sdp_group_manager_finalize;

  klass->add_group = kms_sdp_group_manager_add_group_impl;
  klass->add_handler = kms_sdp_group_manager_add_handler_impl;
  klass->remove_handler = kms_sdp_group_manager_remove_handler_impl;
  klass->add_handler_to_group = kms_sdp_group_manager_add_handler_to_group_impl;
  klass->remove_handler_from_group =
      kms_sdp_group_manager_remove_handler_from_group_impl;

  g_type_class_add_private (klass, sizeof (KmsSdpGroupManagerPrivate));
}

static void
kms_sdp_group_manager_init (KmsSdpGroupManager * self)
{
  self->priv = KMS_SDP_GROUP_MANAGER_GET_PRIVATE (self);

  self->priv->groups = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      NULL, gst_object_unref);
  self->priv->handlers = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      NULL, (GDestroyNotify) kms_ref_struct_unref);
  self->priv->connected_signals = g_hash_table_new_full (g_direct_hash,
      g_direct_equal, NULL, (GDestroyNotify) signal_data_destroy);
  self->priv->mids = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, (GDestroyNotify) kms_utils_destroy_guint);
}

KmsSdpGroupManager *
kms_sdp_group_manager_new ()
{
  KmsSdpGroupManager *obj;

  obj = KMS_SDP_GROUP_MANAGER (g_object_new (KMS_TYPE_SDP_GROUP_MANAGER, NULL));

  return obj;
}

gint
kms_sdp_group_manager_add_group (KmsSdpGroupManager * obj,
    KmsSdpBaseGroup * group)
{
  g_return_val_if_fail (KMS_IS_SDP_GROUP_MANAGER (obj), -1);

  return KMS_SDP_GROUP_MANAGER_GET_CLASS (obj)->add_group (obj, group);
}

void
kms_sdp_group_manager_add_handler (KmsSdpGroupManager * obj,
    KmsSdpHandler * handler)
{
  g_return_if_fail (KMS_IS_SDP_GROUP_MANAGER (obj));

  return KMS_SDP_GROUP_MANAGER_GET_CLASS (obj)->add_handler (obj, handler);
}

gboolean
kms_sdp_group_manager_remove_handler (KmsSdpGroupManager * obj,
    KmsSdpHandler * handler)
{
  g_return_val_if_fail (KMS_IS_SDP_GROUP_MANAGER (obj), FALSE);

  return KMS_SDP_GROUP_MANAGER_GET_CLASS (obj)->remove_handler (obj, handler);
}

gboolean
kms_sdp_group_manager_add_handler_to_group (KmsSdpGroupManager * obj, guint gid,
    guint hid)
{
  g_return_val_if_fail (KMS_IS_SDP_GROUP_MANAGER (obj), FALSE);

  return KMS_SDP_GROUP_MANAGER_GET_CLASS (obj)->add_handler_to_group (obj,
      gid, hid);
}

gboolean
kms_sdp_group_manager_remove_handler_from_group (KmsSdpGroupManager * obj,
    guint gid, guint hid)
{
  g_return_val_if_fail (KMS_IS_SDP_GROUP_MANAGER (obj), FALSE);

  return KMS_SDP_GROUP_MANAGER_GET_CLASS (obj)->remove_handler_from_group (obj,
      gid, hid);
}