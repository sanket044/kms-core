/*
 * (C) Copyright 2013 Kurento (http://kurento.org/)
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
#  include <config.h>
#endif

#include "kmsagnosticbin.h"
#include "kmsagnosticcaps.h"
#include "kmsutils.h"
#include "kmsloop.h"
#include "kmsparsetreebin.h"
#include "kmsdectreebin.h"

#define PLUGIN_NAME "agnosticbin"

#define LINKING_DATA "linking-data"
#define UNLINKING_DATA "unlinking-data"

#define DEFAULT_QUEUE_SIZE 60

static GstStaticCaps static_audio_caps =
GST_STATIC_CAPS (KMS_AGNOSTIC_AUDIO_CAPS);
static GstStaticCaps static_video_caps =
GST_STATIC_CAPS (KMS_AGNOSTIC_VIDEO_CAPS);
static GstStaticCaps static_raw_audio_caps =
GST_STATIC_CAPS (KMS_AGNOSTIC_RAW_AUDIO_CAPS);
static GstStaticCaps static_raw_video_caps =
GST_STATIC_CAPS (KMS_AGNOSTIC_RAW_VIDEO_CAPS);

#define GST_CAT_DEFAULT kms_agnostic_bin2_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define kms_agnostic_bin2_parent_class parent_class
G_DEFINE_TYPE (KmsAgnosticBin2, kms_agnostic_bin2, GST_TYPE_BIN);

#define KMS_AGNOSTIC_BIN2_GET_PRIVATE(obj) ( \
  G_TYPE_INSTANCE_GET_PRIVATE (              \
    (obj),                                   \
    KMS_TYPE_AGNOSTIC_BIN2,                  \
    KmsAgnosticBin2Private                   \
  )                                          \
)

#define KMS_AGNOSTIC_BIN2_GET_COND(obj) (       \
  &KMS_AGNOSTIC_BIN2 (obj)->priv->thread_cond   \
)

#define KMS_AGNOSTIC_BIN2_LOCK(obj) (                           \
  g_mutex_lock (&KMS_AGNOSTIC_BIN2 (obj)->priv->thread_mutex)   \
)

#define KMS_AGNOSTIC_BIN2_UNLOCK(obj) (                         \
  g_mutex_unlock (&KMS_AGNOSTIC_BIN2 (obj)->priv->thread_mutex) \
)

#define OLD_CHAIN_KEY "kms-old-chain-key"
#define CONFIGURED_KEY "kms-configured-key"

#define CUSTOM_BIN_INPUT_QUEUE_NAME "input_queue"
#define CUSTOM_BIN_OUTPUT_TEE_NAME "output_tee"
#define ENC_BIN_NAME_PREFIX "enc_bin"

struct _KmsAgnosticBin2Private
{
  GHashTable *bins;
  GQueue *pads_to_link;

  GMutex thread_mutex;

  GstElement *input_tee;
  GstCaps *input_caps;
  GstBin *input_bin;
  GstCaps *input_bin_src_caps;

  guint enc_bin_n;

  GstPad *sink;
  guint pad_count;
  gboolean started;

  KmsLoop *loop;
};

/* the capabilities of the inputs and outputs. */
static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (KMS_AGNOSTIC_CAPS_CAPS)
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src_%u",
    GST_PAD_SRC,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS (KMS_AGNOSTIC_CAPS_CAPS)
    );

typedef void (*KmsPadIterationAction) (KmsAgnosticBin2 * self, GstPad * pad);

static void
kms_agnostic_bin2_insert_bin (KmsAgnosticBin2 * self, GstBin * bin)
{
  g_hash_table_insert (self->priv->bins, GST_OBJECT_NAME (bin),
      g_object_ref (bin));
}

/*
 * This function sends a dummy event to force blocked probe to be called
 */
static void
send_dummy_event (GstPad * pad)
{
  GstElement *parent = gst_pad_get_parent_element (pad);

  if (parent == NULL) {
    return;
  }

  if (GST_PAD_IS_SINK (pad)) {
    gst_pad_send_event (pad,
        gst_event_new_custom (GST_EVENT_TYPE_DOWNSTREAM |
            GST_EVENT_TYPE_SERIALIZED,
            gst_structure_new_from_string ("dummy")));
  } else {
    gst_pad_send_event (pad,
        gst_event_new_custom (GST_EVENT_TYPE_UPSTREAM |
            GST_EVENT_TYPE_SERIALIZED,
            gst_structure_new_from_string ("dummy")));
  }

  g_object_unref (parent);
}

static gboolean
is_raw_caps (GstCaps * caps)
{
  gboolean ret;
  GstCaps *raw_caps = gst_caps_from_string (KMS_AGNOSTIC_RAW_CAPS);

  ret = gst_caps_is_always_compatible (caps, raw_caps);

  gst_caps_unref (raw_caps);
  return ret;
}

static GstPadProbeReturn
tee_src_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_EVENT_UPSTREAM) {
    GstEvent *event = gst_pad_probe_info_get_event (info);

    if (GST_EVENT_TYPE (event) == GST_EVENT_RECONFIGURE) {
      // Request key frame to upstream elements
      kms_utils_drop_until_keyframe (pad, TRUE);
      return GST_PAD_PROBE_DROP;
    }
  }

  return GST_PAD_PROBE_OK;
}

static gboolean
remove_on_unlinked_async (gpointer data)
{
  GstElement *elem = GST_ELEMENT_CAST (data);
  GstObject *parent = gst_object_get_parent (GST_OBJECT (elem));

  gst_element_set_locked_state (elem, TRUE);
  if (g_strcmp0 (GST_OBJECT_NAME (gst_element_get_factory (elem)),
          "queue") == 0) {
    g_object_set (G_OBJECT (elem), "flush-on-eos", TRUE, NULL);
    gst_element_send_event (elem, gst_event_new_eos ());
  }
  gst_element_set_state (elem, GST_STATE_NULL);
  if (parent != NULL) {
    gst_bin_remove (GST_BIN (parent), elem);
    g_object_unref (parent);
  }

  return G_SOURCE_REMOVE;
}

static GstPadProbeReturn
remove_on_unlinked_blocked (GstPad * pad, GstPadProbeInfo * info, gpointer elem)
{
  KmsAgnosticBin2 *self;
  GstPad *sink;

  if (elem == NULL) {
    return GST_PAD_PROBE_REMOVE;
  }

  GST_DEBUG_OBJECT (pad, "Unlinking pad");

  GST_OBJECT_LOCK (pad);
  if (g_object_get_data (G_OBJECT (pad), UNLINKING_DATA)) {
    GST_DEBUG ("Already unlinking");
    GST_OBJECT_UNLOCK (pad);
    return GST_PAD_PROBE_DROP;
  }

  g_object_set_data (G_OBJECT (pad), UNLINKING_DATA, GINT_TO_POINTER (TRUE));

  GST_OBJECT_UNLOCK (pad);

  sink = gst_pad_get_peer (pad);
  if (sink != NULL) {
    gst_pad_unlink (pad, sink);
    g_object_unref (sink);
  }

  self = KMS_AGNOSTIC_BIN2 (GST_OBJECT_PARENT (elem));

  kms_loop_idle_add_full (self->priv->loop, G_PRIORITY_DEFAULT,
      remove_on_unlinked_async, g_object_ref (elem), g_object_unref);

  return GST_PAD_PROBE_PASS;
}

static void
remove_on_unlinked_cb (GstPad * pad, GstPad * peer, gpointer user_data)
{
  GstElement *elem = gst_pad_get_parent_element (pad);
  KmsAgnosticBin2 *self;

  if (elem == NULL) {
    return;
  }

  self = KMS_AGNOSTIC_BIN2 (GST_OBJECT_PARENT (elem));

  if (self != NULL) {
    GstPad *sink = gst_element_get_static_pad (elem, (gchar *) user_data);

    if (sink != NULL) {
      GstPad *peer = gst_pad_get_peer (sink);

      g_object_unref (sink);

      if (peer != NULL) {
        gst_pad_add_probe (peer, GST_PAD_PROBE_TYPE_BLOCK,
            remove_on_unlinked_blocked, g_object_ref (elem), g_object_unref);
        send_dummy_event (peer);
        gst_object_unref (peer);
        goto end;
      }
    }

    kms_loop_idle_add_full (self->priv->loop, G_PRIORITY_DEFAULT,
        remove_on_unlinked_async, g_object_ref (elem), g_object_unref);
  }

end:
  g_object_unref (elem);
}

/* Sink name should be static memory */
static void
remove_element_on_unlinked (GstElement * element, const gchar * pad_name,
    gchar * sink_name)
{
  GstPad *pad = gst_element_get_static_pad (element, pad_name);

  if (pad == NULL) {
    return;
  }

  g_signal_connect (pad, "unlinked", G_CALLBACK (remove_on_unlinked_cb),
      sink_name);

  g_object_unref (pad);
}

static void
remove_tee_pad_on_unlink (GstPad * pad, GstPad * peer, gpointer user_data)
{
  GstElement *tee = GST_ELEMENT (GST_OBJECT_PARENT (pad));

  if (tee == NULL) {
    return;
  }

  gst_element_release_request_pad (tee, pad);
}

static GstFlowReturn
queue_chain (GstPad * pad, GstObject * parent, GstBuffer * buffer)
{
  GstPadChainFunction old_func =
      g_object_get_data (G_OBJECT (pad), OLD_CHAIN_KEY);

  old_func (pad, parent, buffer);

  return GST_FLOW_OK;
}

static void
link_queue_to_tee_locked (GstElement * tee, GstElement * queue)
{
  GstPad *tee_src = gst_element_get_request_pad (tee, "src_%u");
  GstPad *queue_sink = gst_element_get_static_pad (queue, "sink");
  GstPadLinkReturn ret;
  GstPadChainFunction old_func;

  /*
   * HACK Add a custom chain function that does not return error, this way
   * we avoid race conditions produced by reconnect events not using the stream
   * lock
   */
  old_func = GST_PAD_CHAINFUNC (queue_sink);

  if (old_func != NULL) {
    if (old_func != queue_chain) {
      g_object_set_data (G_OBJECT (queue_sink), OLD_CHAIN_KEY, old_func);
    }
    gst_pad_set_chain_function (queue_sink, queue_chain);
  }

  remove_element_on_unlinked (queue, "src", "sink");
  g_signal_connect (tee_src, "unlinked", G_CALLBACK (remove_tee_pad_on_unlink),
      NULL);

  gst_pad_add_probe (tee_src, GST_PAD_PROBE_TYPE_EVENT_UPSTREAM, tee_src_probe,
      NULL, NULL);

  ret = gst_pad_link_full (tee_src, queue_sink, GST_PAD_LINK_CHECK_NOTHING);

  if (G_UNLIKELY (GST_PAD_LINK_FAILED (ret)))
    GST_ERROR ("Linking %" GST_PTR_FORMAT " with %" GST_PTR_FORMAT " result %d",
        tee_src, queue_sink, ret);

  g_object_unref (queue_sink);
  g_object_unref (tee_src);
}

static GstPadProbeReturn
tee_sink_blocked (GstPad * tee_sink, GstPadProbeInfo * info, gpointer queue)
{
  GstElement *tee = gst_pad_get_parent_element (tee_sink);

  if (tee == NULL) {
    return GST_PAD_PROBE_REMOVE;
  }

  /* HACK: Ignore caps event and stream start event that causes negotiation
   * failures.This is a workaround that should be removed
   */
  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);

    if (GST_EVENT_TYPE (event) == GST_EVENT_STREAM_START
        || GST_EVENT_TYPE (event) == GST_EVENT_CAPS) {
      return GST_PAD_PROBE_PASS;
    }
  }

  /* HACK: Ignore query accept caps that causes negotiation errors.
   * This is a workaround that should be removed
   */
  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM) {
    GstQuery *query = GST_PAD_PROBE_INFO_QUERY (info);

    if (GST_QUERY_TYPE (query) == GST_QUERY_ACCEPT_CAPS) {
      return GST_PAD_PROBE_PASS;
    }
  }

  GST_OBJECT_LOCK (tee_sink);
  if (g_object_get_data (queue, LINKING_DATA)) {
    GST_OBJECT_UNLOCK (tee_sink);
    g_object_unref (tee);
    return GST_PAD_PROBE_PASS;
  }

  g_object_set_data (queue, LINKING_DATA, GINT_TO_POINTER (TRUE));
  GST_OBJECT_UNLOCK (tee_sink);

  link_queue_to_tee_locked (tee, GST_ELEMENT (queue));
  g_object_unref (tee);

  return GST_PAD_PROBE_REMOVE;
}

static void
link_queue_to_tee (GstElement * tee, GstElement * queue)
{
  GstPad *sink = gst_element_get_static_pad (tee, "sink");

  if (sink != NULL) {
    gst_pad_add_probe (sink, GST_PAD_PROBE_TYPE_BLOCK, tee_sink_blocked,
        g_object_ref (queue), g_object_unref);
    send_dummy_event (sink);
    g_object_unref (sink);
  }
}

static GstElement *
create_convert_for_caps (GstCaps * caps)
{
  GstCaps *audio_caps = gst_static_caps_get (&static_audio_caps);
  GstElement *convert;

  if (gst_caps_can_intersect (caps, audio_caps))
    convert = gst_element_factory_make ("audioconvert", NULL);
  else
    convert = gst_element_factory_make ("videoconvert", NULL);

  gst_caps_unref (audio_caps);

  return convert;
}

static GstElement *
create_mediator_element (GstCaps * caps)
{
  GstCaps *audio_caps = gst_static_caps_get (&static_audio_caps);
  GstElement *element = NULL;

  if (gst_caps_can_intersect (caps, audio_caps)) {
    element = gst_element_factory_make ("audioresample", NULL);
  } else {
    element = gst_element_factory_make ("videoscale", NULL);
  }

  gst_caps_unref (audio_caps);

  return element;
}

static GstElement *
create_rate_for_caps (GstCaps * caps)
{
  GstCaps *audio_caps = gst_static_caps_get (&static_audio_caps);
  GstElement *rate;

  if (gst_caps_can_intersect (caps, audio_caps)) {
    rate = gst_element_factory_make ("audiorate", NULL);
    g_object_set (G_OBJECT (rate), "tolerance", GST_MSECOND * 100,
        "skip-to-first", TRUE, NULL);
  } else {
    rate = gst_element_factory_make ("videorate", NULL);
    g_object_set (G_OBJECT (rate), "average-period", GST_MSECOND * 200,
        "skip-to-first", TRUE, NULL);
  }

  gst_caps_unref (audio_caps);

  return rate;
}

static GstPadProbeReturn
remove_target_pad_block (GstPad * pad, GstPadProbeInfo * info, gpointer gp)
{
  GST_DEBUG_OBJECT (pad, "Drop");
  return GST_PAD_PROBE_DROP;
}

static void
remove_target_pad (GstPad * pad)
{
  GstPad *target = gst_ghost_pad_get_target (GST_GHOST_PAD (pad));

  GST_DEBUG_OBJECT (pad, "Removing target pad");

  if (target == NULL) {
    return;
  }

  gst_pad_add_probe (target, GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM,
      remove_target_pad_block, NULL, NULL);
  gst_ghost_pad_set_target (GST_GHOST_PAD (pad), NULL);

  g_object_unref (target);
}

static void
kms_agnostic_bin2_link_to_tee (KmsAgnosticBin2 * self, GstPad * pad,
    GstElement * tee, GstCaps * caps)
{
  GstElement *queue = gst_element_factory_make ("queue", NULL);
  GstPad *target;

  g_object_set (queue, "leaky", 2 /* downstream */ ,
      "max-size-buffers", DEFAULT_QUEUE_SIZE, NULL);

  gst_bin_add (GST_BIN (self), queue);
  gst_element_sync_state_with_parent (queue);

  if (!gst_caps_is_any (caps) && is_raw_caps (caps)) {
    GstElement *convert = create_convert_for_caps (caps);
    GstElement *rate = create_rate_for_caps (caps);
    GstElement *mediator = create_mediator_element (caps);

    remove_element_on_unlinked (convert, "src", "sink");
    remove_element_on_unlinked (rate, "src", "sink");
    remove_element_on_unlinked (mediator, "src", "sink");

    gst_bin_add_many (GST_BIN (self), convert, rate, mediator, NULL);

    gst_element_sync_state_with_parent (convert);
    gst_element_sync_state_with_parent (rate);
    gst_element_sync_state_with_parent (mediator);

    gst_element_link_many (queue, rate, convert, mediator, NULL);
    target = gst_element_get_static_pad (mediator, "src");
  } else {
    target = gst_element_get_static_pad (queue, "src");
  }

  gst_ghost_pad_set_target (GST_GHOST_PAD (pad), target);
  g_object_unref (target);
  link_queue_to_tee (tee, queue);
}

static GstBin *
kms_agnostic_bin2_find_bin_for_caps (KmsAgnosticBin2 * self, GstCaps * caps)
{
  GList *bins, *l;
  GstBin *bin = NULL;

  if (gst_caps_is_any (caps)) {
    return self->priv->input_bin;
  }

  bins = g_hash_table_get_values (self->priv->bins);
  for (l = bins; l != NULL && bin == NULL; l = l->next) {
    GstElement *output_tee = gst_bin_get_by_name (GST_BIN (l->data),
        CUSTOM_BIN_OUTPUT_TEE_NAME);
    GstPad *tee_sink = gst_element_get_static_pad (output_tee, "sink");
    GstCaps *current_caps = gst_pad_get_current_caps (tee_sink);

    if (current_caps == NULL) {
      current_caps = gst_pad_get_allowed_caps (tee_sink);
      GST_TRACE_OBJECT (l->data, "Allowed caps are: %" GST_PTR_FORMAT,
          current_caps);
    } else {
      GST_TRACE_OBJECT (l->data, "Current caps are: %" GST_PTR_FORMAT,
          current_caps);
    }

    if (current_caps != NULL) {
      if (gst_caps_can_intersect (caps, current_caps))
        bin = l->data;
      gst_caps_unref (current_caps);
    }

    g_object_unref (tee_sink);
    g_object_unref (output_tee);
  }
  g_list_free (bins);

  return bin;
}

static GstCaps *
kms_agnostic_bin2_get_raw_caps (GstCaps * caps)
{
  GstCaps *audio_caps, *video_caps, *raw_caps = NULL;

  audio_caps = gst_static_caps_get (&static_audio_caps);
  video_caps = gst_static_caps_get (&static_video_caps);

  if (gst_caps_can_intersect (caps, audio_caps))
    raw_caps = gst_static_caps_get (&static_raw_audio_caps);
  else if (gst_caps_can_intersect (caps, video_caps))
    raw_caps = gst_static_caps_get (&static_raw_video_caps);

  gst_caps_unref (audio_caps);
  gst_caps_unref (video_caps);

  return raw_caps;
}

static GstBin *
kms_agnostic_bin2_create_dec_bin (KmsAgnosticBin2 * self,
    const GstCaps * raw_caps)
{
  KmsDecTreeBin *dec_bin;
  GstElement *output_tee, *input_queue;
  GstCaps *caps = self->priv->input_bin_src_caps;

  if (caps == NULL || raw_caps == NULL) {
    return NULL;
  }

  dec_bin = kms_dec_tree_bin_new (caps, raw_caps);
  if (dec_bin == NULL) {
    return NULL;
  }

  gst_bin_add (GST_BIN (self), GST_ELEMENT (dec_bin));
  gst_element_sync_state_with_parent (GST_ELEMENT (dec_bin));

  output_tee =
      gst_bin_get_by_name (self->priv->input_bin, CUSTOM_BIN_OUTPUT_TEE_NAME);
  input_queue = kms_tree_bin_get_input_queue (KMS_TREE_BIN (dec_bin));
  link_queue_to_tee (output_tee, input_queue);
  g_object_unref (output_tee);

  return GST_BIN (dec_bin);
}

static GstBin *
kms_agnostic_bin2_get_or_create_dec_bin (KmsAgnosticBin2 * self, GstCaps * caps)
{
  GstCaps *raw_caps = kms_agnostic_bin2_get_raw_caps (caps);

  if (raw_caps != NULL) {
    GstBin *dec_bin;

    GST_DEBUG ("Raw caps: %" GST_PTR_FORMAT, raw_caps);
    dec_bin = kms_agnostic_bin2_find_bin_for_caps (self, raw_caps);

    if (dec_bin == NULL) {
      dec_bin = kms_agnostic_bin2_create_dec_bin (self, raw_caps);
    }

    if (dec_bin != NULL) {
      kms_agnostic_bin2_insert_bin (self, dec_bin);
    }

    gst_caps_unref (raw_caps);

    return dec_bin;
  } else {
    GST_ELEMENT_WARNING (self, CORE, NEGOTIATION,
        ("Formats are not compatible"), ("Formats are not compatible"));
    return NULL;
  }
}

static void
configure_encoder (GstElement * encoder, const gchar * factory_name)
{
  GST_DEBUG ("Configure encoder: %s", factory_name);
  if (g_strcmp0 ("vp8enc", factory_name) == 0) {
    g_object_set (G_OBJECT (encoder), "deadline", G_GINT64_CONSTANT (200000),
        "threads", 1, "cpu-used", 16, "resize-allowed", TRUE,
        "target-bitrate", 300000, "end-usage", /* cbr */ 1, NULL);
  } else if (g_strcmp0 ("x264enc", factory_name) == 0) {
    g_object_set (G_OBJECT (encoder), "speed-preset", 1 /* ultrafast */ ,
        "tune", 4 /* zerolatency */ , "threads", (guint) 1,
        NULL);
  }
}

static GstElement *
create_encoder_for_caps (GstCaps * caps)
{
  GList *encoder_list, *filtered_list, *l;
  GstElementFactory *encoder_factory = NULL;
  GstElement *encoder = NULL;

  encoder_list =
      gst_element_factory_list_get_elements (GST_ELEMENT_FACTORY_TYPE_ENCODER,
      GST_RANK_NONE);
  filtered_list =
      gst_element_factory_list_filter (encoder_list, caps, GST_PAD_SRC, FALSE);

  for (l = filtered_list; l != NULL && encoder_factory == NULL; l = l->next) {
    encoder_factory = GST_ELEMENT_FACTORY (l->data);
    if (gst_element_factory_get_num_pad_templates (encoder_factory) != 2)
      encoder_factory = NULL;
  }

  if (encoder_factory != NULL) {
    encoder = gst_element_factory_create (encoder_factory, NULL);
    configure_encoder (encoder, GST_OBJECT_NAME (encoder_factory));
  }

  gst_plugin_feature_list_free (filtered_list);
  gst_plugin_feature_list_free (encoder_list);

  return encoder;
}

/* EncBin begin */

static GstBin *
enc_bin_new (KmsAgnosticBin2 * agnostic, GstCaps * caps)
{
  GstBin *bin;
  guint n;
  gchar *name;
  GstElement *input_queue, *rate, *convert, *mediator, *enc, *output_tee,
      *queue, *fakesink;

  enc = create_encoder_for_caps (caps);
  if (enc == NULL) {
    GST_WARNING ("Invalid encoder");
    return NULL;
  }

  n = g_atomic_int_add (&agnostic->priv->enc_bin_n, 1);
  name = g_strdup_printf ("%s_%" G_GUINT32_FORMAT, ENC_BIN_NAME_PREFIX, n);
  bin = GST_BIN (gst_bin_new (name));
  g_free (name);

  input_queue = gst_element_factory_make ("queue", CUSTOM_BIN_INPUT_QUEUE_NAME);
  rate = create_rate_for_caps (caps);
  convert = create_convert_for_caps (caps);
  mediator = create_mediator_element (caps);
  output_tee = gst_element_factory_make ("tee", CUSTOM_BIN_OUTPUT_TEE_NAME);
  queue = gst_element_factory_make ("queue", NULL);
  fakesink = gst_element_factory_make ("fakesink", NULL);

  g_object_set (queue, "max-size-buffers", DEFAULT_QUEUE_SIZE, NULL);
  g_object_set (input_queue, "max-size-buffers", DEFAULT_QUEUE_SIZE, NULL);
  g_object_set (fakesink, "async", FALSE, NULL);

  gst_bin_add_many (bin, input_queue, rate, convert, mediator, enc,
      output_tee, queue, fakesink, NULL);
  gst_element_link_many (input_queue, rate, convert, mediator, enc, output_tee,
      queue, fakesink, NULL);

  return bin;
}

/* EncBin end */

static GstBin *
kms_agnostic_bin2_create_bin_for_caps (KmsAgnosticBin2 * self, GstCaps * caps)
{
  GstBin *dec_bin, *enc_bin;
  GstElement *input_queue, *output_tee;

  dec_bin = kms_agnostic_bin2_get_or_create_dec_bin (self, caps);
  if (dec_bin == NULL) {
    return NULL;
  }

  if (is_raw_caps (caps)) {
    return dec_bin;
  }

  enc_bin = enc_bin_new (self, caps);
  if (enc_bin == NULL) {
    return NULL;
  }

  gst_bin_add (GST_BIN (self), GST_ELEMENT (enc_bin));
  gst_element_sync_state_with_parent (GST_ELEMENT (enc_bin));

  output_tee = kms_tree_bin_get_output_tee (KMS_TREE_BIN (dec_bin));
  input_queue = gst_bin_get_by_name (enc_bin, CUSTOM_BIN_INPUT_QUEUE_NAME);
  link_queue_to_tee (output_tee, input_queue);
  g_object_unref (input_queue);

  kms_agnostic_bin2_insert_bin (self, enc_bin);

  return enc_bin;
}

/**
 * Link a pad internally
 *
 * @self: The #KmsAgnosticBin2 owner of the pad
 * @pad: (transfer full): The pad to be linked
 * @peer: (transfer full): The peer pad
 */
static void
kms_agnostic_bin2_link_pad (KmsAgnosticBin2 * self, GstPad * pad, GstPad * peer)
{
  GstCaps *caps;
  GstBin *bin;

  GST_INFO_OBJECT (self, "Linking: %" GST_PTR_FORMAT, pad);

  caps = gst_pad_query_caps (peer, NULL);

  if (caps == NULL)
    goto end;

  GST_DEBUG ("Query caps are: %" GST_PTR_FORMAT, caps);
  bin = kms_agnostic_bin2_find_bin_for_caps (self, caps);

  if (bin == NULL) {
    bin = kms_agnostic_bin2_create_bin_for_caps (self, caps);
    GST_DEBUG_OBJECT (self, "Created bin: %" GST_PTR_FORMAT, bin);
  }

  if (bin != NULL) {
    GstElement *tee = gst_bin_get_by_name (bin, CUSTOM_BIN_OUTPUT_TEE_NAME);

    kms_utils_drop_until_keyframe (pad, TRUE);
    kms_agnostic_bin2_link_to_tee (self, pad, tee, caps);
    g_object_unref (tee);
  }

  gst_caps_unref (caps);

end:
  g_object_unref (pad);
  g_object_unref (peer);
}

/**
 * Unlink a pad internally
 *
 * @self: The #KmsAgnosticBin2 owner of the pad
 * @pad: (transfer full): The pad to be unlinked
 */
static void
kms_agnostic_bin2_unlink_pad (KmsAgnosticBin2 * self, GstPad * pad)
{
  GST_DEBUG_OBJECT (self, "Unlinking: %" GST_PTR_FORMAT, pad);

  g_object_unref (pad);
}

/**
 * Process a pad for connecting or disconnecting, it should be always called
 * from the loop.
 *
 * @self: The #KmsAgnosticBin2 owner of the pad
 * @pad: (transfer full): The pad to be processed
 */
static void
kms_agnostic_bin2_process_pad (KmsAgnosticBin2 * self, GstPad * pad)
{
  GstPad *peer = NULL;

  GST_DEBUG_OBJECT (self, "Processing pad: %" GST_PTR_FORMAT, pad);

  if (pad == NULL)
    return;

  /* We consider that pad is correctly configured now */
  gst_pad_check_reconfigure (pad);

  peer = gst_pad_get_peer (pad);

  if (peer == NULL)
    kms_agnostic_bin2_unlink_pad (self, pad);
  else
    kms_agnostic_bin2_link_pad (self, pad, peer);

}

static gboolean
kms_agnostic_bin2_process_pad_loop (gpointer data)
{
  KmsAgnosticBin2 *self = KMS_AGNOSTIC_BIN2 (data);

  KMS_AGNOSTIC_BIN2_LOCK (self);

  if (!self->priv->started) {
    GST_DEBUG_OBJECT (self,
        "Caps reconfiguration when reconnection is taking place");
    while (!g_queue_is_empty (self->priv->pads_to_link)) {
      gst_object_unref (GST_OBJECT (g_queue_pop_head (self->priv->
                  pads_to_link)));
    }
    goto end;
  }

  while (!g_queue_is_empty (self->priv->pads_to_link)) {
    kms_agnostic_bin2_process_pad (self,
        g_queue_pop_head (self->priv->pads_to_link));
  }

end:
  KMS_AGNOSTIC_BIN2_UNLOCK (self);

  return FALSE;
}

static void
kms_agnostic_bin2_add_pad_to_queue (KmsAgnosticBin2 * self, GstPad * pad)
{
  if (!self->priv->started)
    return;

  if (g_queue_index (self->priv->pads_to_link, pad) == -1) {
    GST_DEBUG_OBJECT (pad, "Adding pad to queue");

    remove_target_pad (pad);
    g_queue_push_tail (self->priv->pads_to_link, g_object_ref (pad));
  }
}

static void
iterate_src_pads (KmsAgnosticBin2 * self, KmsPadIterationAction action)
{
  GstIterator *it = gst_element_iterate_src_pads (GST_ELEMENT (self));
  gboolean done = FALSE;
  GstPad *pad;
  GValue item = G_VALUE_INIT;

  while (!done) {
    switch (gst_iterator_next (it, &item)) {
      case GST_ITERATOR_OK:
        pad = g_value_get_object (&item);
        action (self, pad);
        g_value_reset (&item);
        break;
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync (it);
        break;
      case GST_ITERATOR_ERROR:
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
    }
  }

  gst_iterator_free (it);
}

static void
unlink_input_queue_from_tee (GstElement * input_queue)
{
  GstPad *queue_sink, *peer, *tee_src;

  if (input_queue == NULL) {
    return;
  }

  queue_sink = gst_element_get_static_pad (input_queue, "sink");
  peer = gst_pad_get_peer (queue_sink);

  if (GST_IS_PROXY_PAD (peer)) {
    GstProxyPad *ghost;

    ghost = gst_proxy_pad_get_internal (GST_PROXY_PAD (peer));
    tee_src = gst_pad_get_peer (GST_PAD (ghost));

    g_object_unref (peer);
    g_object_unref (ghost);
  } else {
    tee_src = peer;
  }

  gst_pad_unlink (tee_src, queue_sink);
  gst_element_release_request_pad (GST_ELEMENT (GST_OBJECT_PARENT (tee_src)),
      tee_src);

  g_object_unref (tee_src);
  g_object_unref (queue_sink);
}

static void
input_bin_reset (GstBin * input_bin)
{
  GstElement *input_queue;

  if (input_bin == NULL) {
    return;
  }

  input_queue = gst_bin_get_by_name (input_bin, CUSTOM_BIN_INPUT_QUEUE_NAME);
  unlink_input_queue_from_tee (input_queue);
  g_object_unref (input_queue);
}

static void
add_linked_pads (KmsAgnosticBin2 * self, GstPad * pad)
{
  if (!gst_pad_is_linked (pad)) {
    return;
  }

  kms_agnostic_bin2_add_pad_to_queue (self, pad);
}

static GstPadProbeReturn
input_bin_src_caps_probe (GstPad * pad, GstPadProbeInfo * info, gpointer bin)
{
  KmsAgnosticBin2 *self = KMS_AGNOSTIC_BIN2 (GST_OBJECT_PARENT (bin));
  GstEvent *event = gst_pad_probe_info_get_event (info);
  GstCaps *current_caps;

  GST_TRACE_OBJECT (self, "Event in parser pad: %" GST_PTR_FORMAT, event);

  if (GST_EVENT_TYPE (event) != GST_EVENT_CAPS) {
    return GST_PAD_PROBE_OK;
  }

  KMS_AGNOSTIC_BIN2_LOCK (self);

  self->priv->started = TRUE;
  if (self->priv->input_bin_src_caps != NULL) {
    gst_caps_unref (self->priv->input_bin_src_caps);
  }

  gst_event_parse_caps (event, &current_caps);
  self->priv->input_bin_src_caps = gst_caps_copy (current_caps);
  kms_agnostic_bin2_insert_bin (self, GST_BIN (bin));

  GST_INFO_OBJECT (self, "Setting current caps to: %" GST_PTR_FORMAT,
      current_caps);

  iterate_src_pads (self, add_linked_pads);

  kms_loop_idle_add_full (self->priv->loop, G_PRIORITY_HIGH,
      kms_agnostic_bin2_process_pad_loop, g_object_ref (self), g_object_unref);

  KMS_AGNOSTIC_BIN2_UNLOCK (self);

  return GST_PAD_PROBE_REMOVE;
}

static void
kms_agnostic_bin2_configure_input (KmsAgnosticBin2 * self, const GstCaps * caps)
{
  KmsParseTreeBin *parse_bin;
  GstElement *parser;
  GstPad *parser_src;
  GstElement *input_queue;

  KMS_AGNOSTIC_BIN2_LOCK (self);

  input_bin_reset (self->priv->input_bin);
  if (self->priv->input_bin != NULL) {
    gst_element_set_state (GST_ELEMENT (self->priv->input_bin), GST_STATE_NULL);
    gst_bin_remove (GST_BIN (self), GST_ELEMENT (self->priv->input_bin));
  }

  parse_bin = kms_parse_tree_bin_new (caps);
  self->priv->input_bin = GST_BIN (parse_bin);

  parser = kms_parse_tree_bin_get_parser (KMS_PARSE_TREE_BIN (parse_bin));
  parser_src = gst_element_get_static_pad (parser, "src");
  gst_pad_add_probe (parser_src, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      input_bin_src_caps_probe, g_object_ref (parse_bin), g_object_unref);
  g_object_unref (parser_src);

  gst_bin_add (GST_BIN (self), GST_ELEMENT (parse_bin));
  gst_element_sync_state_with_parent (GST_ELEMENT (parse_bin));

  input_queue = kms_tree_bin_get_input_queue (KMS_TREE_BIN (parse_bin));
  gst_element_link (self->priv->input_tee, input_queue);

  self->priv->started = FALSE;
  g_hash_table_remove_all (self->priv->bins);
  while (!g_queue_is_empty (self->priv->pads_to_link)) {
    gst_object_unref (GST_OBJECT (g_queue_pop_head (self->priv->pads_to_link)));
  }

  KMS_AGNOSTIC_BIN2_UNLOCK (self);
}

static GstPadProbeReturn
kms_agnostic_bin2_sink_caps_probe (GstPad * pad, GstPadProbeInfo * info,
    gpointer user_data)
{
  KmsAgnosticBin2 *self;
  GstCaps *current_caps;
  GstCaps *new_caps = NULL;
  GstEvent *event = gst_pad_probe_info_get_event (info);

  if (GST_EVENT_TYPE (event) != GST_EVENT_CAPS) {
    return GST_PAD_PROBE_OK;
  }

  GST_TRACE_OBJECT (pad, "Event: %" GST_PTR_FORMAT, event);

  self = KMS_AGNOSTIC_BIN2 (user_data);

  gst_event_parse_caps (event, &new_caps);

  if (new_caps == NULL) {
    GST_ERROR_OBJECT (self, "Unexpected NULL caps");
    return GST_PAD_PROBE_OK;
  }

  KMS_AGNOSTIC_BIN2_LOCK (self);
  current_caps = self->priv->input_caps;
  self->priv->input_caps = gst_caps_copy (new_caps);
  KMS_AGNOSTIC_BIN2_UNLOCK (self);

  GST_TRACE_OBJECT (user_data, "New caps event: %" GST_PTR_FORMAT, event);

  if (current_caps != NULL) {
    GST_TRACE_OBJECT (user_data, "Current caps: %" GST_PTR_FORMAT,
        current_caps);

    if (!gst_caps_can_intersect (new_caps, current_caps) &&
        !is_raw_caps (current_caps) && !is_raw_caps (new_caps)) {
      GST_DEBUG_OBJECT (user_data, "Caps differ caps: %" GST_PTR_FORMAT,
          new_caps);
      kms_agnostic_bin2_configure_input (self, new_caps);
    }

    gst_caps_unref (current_caps);
  } else {
    GST_DEBUG_OBJECT (user_data, "No previous caps, starting");
    kms_agnostic_bin2_configure_input (self, new_caps);
  }

  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
kms_agnostic_bin2_src_reconfigure_probe (GstPad * pad, GstPadProbeInfo * info,
    gpointer user_data)
{
  KmsAgnosticBin2 *self = KMS_AGNOSTIC_BIN2 (gst_pad_get_parent_element (pad));
  GstPadProbeReturn ret = GST_PAD_PROBE_OK;
  GstEvent *event;

  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_EVENT_BOTH) {
    event = gst_pad_probe_info_get_event (info);

    if (GST_EVENT_TYPE (event) == GST_EVENT_RECONFIGURE) {
      KmsAgnosticBin2 *self = user_data;

      GST_DEBUG_OBJECT (pad, "Received reconfigure event");

      KMS_AGNOSTIC_BIN2_LOCK (self);
      kms_agnostic_bin2_add_pad_to_queue (self, pad);
      kms_loop_idle_add_full (self->priv->loop, G_PRIORITY_HIGH,
          kms_agnostic_bin2_process_pad_loop, g_object_ref (self),
          g_object_unref);
      KMS_AGNOSTIC_BIN2_UNLOCK (self);

      ret = GST_PAD_PROBE_DROP;
      goto end;
    }
  }
  // If the pads needs to be reconfigured, we should drop events
  if (gst_pad_needs_reconfigure (pad)) {
    ret = GST_PAD_PROBE_DROP;
  } else {
    ret = GST_PAD_PROBE_OK;
  }

end:
  g_object_unref (self);

  return ret;
}

static void
kms_agnostic_bin2_src_unlinked (GstPad * pad, GstPad * peer,
    KmsAgnosticBin2 * self)
{
  GST_DEBUG_OBJECT (pad, "Unlinked");
  remove_target_pad (pad);
}

static GstPad *
kms_agnostic_bin2_request_new_pad (GstElement * element,
    GstPadTemplate * templ, const gchar * name, const GstCaps * caps)
{
  GstPad *pad;
  gchar *pad_name;
  KmsAgnosticBin2 *self = KMS_AGNOSTIC_BIN2 (element);

  GST_OBJECT_LOCK (self);
  pad_name = g_strdup_printf ("src_%d", self->priv->pad_count++);
  GST_OBJECT_UNLOCK (self);

  pad = gst_ghost_pad_new_no_target_from_template (pad_name, templ);
  g_free (pad_name);

  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_EVENT_UPSTREAM,
      kms_agnostic_bin2_src_reconfigure_probe, element, NULL);

  g_signal_connect (pad, "unlinked",
      G_CALLBACK (kms_agnostic_bin2_src_unlinked), self);

  gst_pad_set_active (pad, TRUE);

  if (gst_element_add_pad (element, pad))
    return pad;

  g_object_unref (pad);

  return NULL;
}

static void
kms_agnostic_bin2_release_pad (GstElement * element, GstPad * pad)
{
  gst_element_remove_pad (element, pad);
}

static void
kms_agnostic_bin2_dispose (GObject * object)
{
  KmsAgnosticBin2 *self = KMS_AGNOSTIC_BIN2 (object);

  KMS_AGNOSTIC_BIN2_LOCK (self);
  g_clear_object (&self->priv->loop);

  if (self->priv->input_bin_src_caps) {
    gst_caps_unref (self->priv->input_bin_src_caps);
    self->priv->input_bin_src_caps = NULL;
  }

  if (self->priv->input_caps) {
    gst_caps_unref (self->priv->input_caps);
    self->priv->input_caps = NULL;
  }

  KMS_AGNOSTIC_BIN2_UNLOCK (self);

  /* chain up */
  G_OBJECT_CLASS (kms_agnostic_bin2_parent_class)->dispose (object);
}

static void
kms_agnostic_bin2_finalize (GObject * object)
{
  KmsAgnosticBin2 *self = KMS_AGNOSTIC_BIN2 (object);

  g_mutex_clear (&self->priv->thread_mutex);

  g_queue_free_full (self->priv->pads_to_link, g_object_unref);
  g_hash_table_unref (self->priv->bins);

  /* chain up */
  G_OBJECT_CLASS (kms_agnostic_bin2_parent_class)->finalize (object);
}

static void
kms_agnostic_bin2_class_init (KmsAgnosticBin2Class * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gstelement_class = GST_ELEMENT_CLASS (klass);

  gobject_class->dispose = kms_agnostic_bin2_dispose;
  gobject_class->finalize = kms_agnostic_bin2_finalize;

  gst_element_class_set_details_simple (gstelement_class,
      "Agnostic connector 2nd version",
      "Generic/Bin/Connector",
      "Automatically encodes/decodes media to match sink and source pads caps",
      "José Antonio Santos Cadenas <santoscadenas@kurento.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_factory));

  gstelement_class->request_new_pad =
      GST_DEBUG_FUNCPTR (kms_agnostic_bin2_request_new_pad);
  gstelement_class->release_pad =
      GST_DEBUG_FUNCPTR (kms_agnostic_bin2_release_pad);

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, PLUGIN_NAME, 0, PLUGIN_NAME);

  g_type_class_add_private (klass, sizeof (KmsAgnosticBin2Private));
}

static gboolean
kms_agnostic_bin2_sink_query (GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  gboolean ret;

  ret = gst_pad_query_default (pad, parent, query);

  if (ret && GST_QUERY_TYPE (query) == GST_QUERY_LATENCY) {
    GstClockTime min_latency;
    GstClockTime max_latency;

    gst_query_parse_latency (query, NULL, &min_latency, &max_latency);

    gst_query_set_latency (query, TRUE, min_latency, max_latency);
  }

  return ret;
}

static void
kms_agnostic_bin2_init (KmsAgnosticBin2 * self)
{
  GstPadTemplate *templ;
  GstElement *tee, *queue, *fakesink;
  GstPad *target, *sink;

  self->priv = KMS_AGNOSTIC_BIN2_GET_PRIVATE (self);

  tee = gst_element_factory_make ("tee", NULL);
  self->priv->input_tee = tee;
  queue = gst_element_factory_make ("queue", NULL);
  fakesink = gst_element_factory_make ("fakesink", NULL);

  g_object_set (fakesink, "async", FALSE, NULL);
  g_object_set (queue, "max-size-buffers", DEFAULT_QUEUE_SIZE, NULL);

  gst_bin_add_many (GST_BIN (self), tee, queue, fakesink, NULL);
  gst_element_link_many (tee, queue, fakesink, NULL);

  target = gst_element_get_static_pad (tee, "sink");
  templ = gst_static_pad_template_get (&sink_factory);
  self->priv->sink = gst_ghost_pad_new_from_template ("sink", target, templ);
  gst_pad_set_query_function (self->priv->sink, kms_agnostic_bin2_sink_query);
  kms_utils_manage_gaps (self->priv->sink);
  g_object_unref (templ);
  g_object_unref (target);

  sink = gst_element_get_static_pad (fakesink, "sink");
  gst_pad_add_probe (sink, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      kms_agnostic_bin2_sink_caps_probe, self, NULL);
  g_object_unref (sink);

  gst_element_add_pad (GST_ELEMENT (self), self->priv->sink);

  g_object_set (G_OBJECT (self), "async-handling", TRUE, NULL);

  self->priv->started = FALSE;
  self->priv->loop = kms_loop_new ();
  self->priv->bins =
      g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);
  self->priv->pads_to_link = g_queue_new ();
  g_mutex_init (&self->priv->thread_mutex);
}

gboolean
kms_agnostic_bin2_plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, PLUGIN_NAME, GST_RANK_NONE,
      KMS_TYPE_AGNOSTIC_BIN2);
}
