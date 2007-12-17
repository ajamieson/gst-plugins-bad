/* GStreamer
 * Copyright (C) 2003 Julien Moutte <julien@moutte.net>
 * Copyright (C) 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2005 Jan Schmidt <thaytan@mad.scientist.com>
 * Copyright (C) 2007 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "gstswitch.h"
#include "gstswitch-marshal.h"

GST_DEBUG_CATEGORY_STATIC (stream_selector_debug);
#define GST_CAT_DEFAULT stream_selector_debug

static const GstElementDetails gst_stream_selector_details =
GST_ELEMENT_DETAILS ("StreamSelector",
    "Generic",
    "N-to-1 input stream_selectoring",
    "Julien Moutte <julien@moutte.net>\n"
    "Ronald S. Bultje <rbultje@ronald.bitfreak.net>\n"
    "Jan Schmidt <thaytan@mad.scientist.com>\n"
    "Wim Taymans <wim.taymans@gmail.com>");

static GstStaticPadTemplate gst_stream_selector_sink_factory =
GST_STATIC_PAD_TEMPLATE ("sink%d",
    GST_PAD_SINK,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate gst_stream_selector_src_factory =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

enum
{
  PROP_ACTIVE_PAD = 1,
  PROP_LAST_STOP_TIME
};

enum
{
  /* methods */
  SIGNAL_BLOCK,
  SIGNAL_SWITCH,
  LAST_SIGNAL
};
static guint gst_stream_selector_signals[LAST_SIGNAL] = { 0 };

static gboolean gst_stream_selector_is_active_sinkpad (GstStreamSelector * sel,
    GstPad * pad);
static GstPad *gst_stream_selector_activate_sinkpad (GstStreamSelector * sel,
    GstPad * pad);
static GstPad *gst_stream_selector_get_linked_pad (GstPad * pad,
    gboolean strict);
static void gst_stream_selector_push_pending_stop (GstStreamSelector * self);

#define GST_TYPE_SELECTOR_PAD \
  (gst_selector_pad_get_type())
#define GST_SELECTOR_PAD(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_SELECTOR_PAD, GstSelectorPad))
#define GST_SELECTOR_PAD_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_SELECTOR_PAD, GstSelectorPadClass))
#define GST_IS_SELECTOR_PAD(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_SELECTOR_PAD))
#define GST_IS_SELECTOR_PAD_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_SELECTOR_PAD))
#define GST_SELECTOR_PAD_CAST(obj) \
  ((GstSelectorPad *)(obj))

typedef struct _GstSelectorPad GstSelectorPad;
typedef struct _GstSelectorPadClass GstSelectorPadClass;

struct _GstSelectorPad
{
  GstPad parent;

  gboolean active;
  gboolean eos;
  gboolean segment_pending;
  GstSegment segment;
};

struct _GstSelectorPadClass
{
  GstPadClass parent;
};

static void gst_selector_pad_class_init (GstSelectorPadClass * klass);
static void gst_selector_pad_init (GstSelectorPad * pad);
static void gst_selector_pad_finalize (GObject * object);

static GstPadClass *selector_pad_parent_class = NULL;

static void gst_selector_pad_reset (GstSelectorPad * pad);
static gboolean gst_selector_pad_event (GstPad * pad, GstEvent * event);
static GstCaps *gst_selector_pad_getcaps (GstPad * pad);
static GList *gst_selector_pad_get_linked_pads (GstPad * pad);
static GstFlowReturn gst_selector_pad_chain (GstPad * pad, GstBuffer * buf);
static GstFlowReturn gst_selector_pad_bufferalloc (GstPad * pad,
    guint64 offset, guint size, GstCaps * caps, GstBuffer ** buf);

static GType
gst_selector_pad_get_type (void)
{
  static GType selector_pad_type = 0;

  if (!selector_pad_type) {
    static const GTypeInfo selector_pad_info = {
      sizeof (GstSelectorPadClass),
      NULL,
      NULL,
      (GClassInitFunc) gst_selector_pad_class_init,
      NULL,
      NULL,
      sizeof (GstSelectorPad),
      0,
      (GInstanceInitFunc) gst_selector_pad_init,
    };

    selector_pad_type =
        g_type_register_static (GST_TYPE_PAD, "GstSwitchPad",
        &selector_pad_info, 0);
  }
  return selector_pad_type;
}

static void
gst_selector_pad_class_init (GstSelectorPadClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  selector_pad_parent_class = g_type_class_peek_parent (klass);

  gobject_class->finalize = gst_selector_pad_finalize;
}

static void
gst_selector_pad_init (GstSelectorPad * pad)
{
}

static void
gst_selector_pad_finalize (GObject * object)
{
  GstSelectorPad *pad;

  pad = GST_SELECTOR_PAD_CAST (object);

  G_OBJECT_CLASS (selector_pad_parent_class)->finalize (object);
}

static void
gst_selector_pad_reset (GstSelectorPad * pad)
{
  pad->active = FALSE;
  pad->eos = FALSE;
  gst_segment_init (&pad->segment, GST_FORMAT_UNDEFINED);
}

/* strictly get the linked pad from the sinkpad. If the pad is active we return
 * the srcpad else we return NULL */
static GList *
gst_selector_pad_get_linked_pads (GstPad * pad)
{
  GstPad *otherpad;

  otherpad = gst_stream_selector_get_linked_pad (pad, TRUE);
  if (!otherpad)
    return NULL;

  /* need to drop the ref, internal linked pads is not MT safe */
  gst_object_unref (otherpad);

  return g_list_append (NULL, otherpad);
}

static gboolean
gst_selector_pad_event (GstPad * pad, GstEvent * event)
{
  gboolean res = TRUE;
  gboolean forward = TRUE;
  GstStreamSelector *sel;
  GstSelectorPad *selpad;

  sel = GST_STREAM_SELECTOR (gst_pad_get_parent (pad));
  selpad = GST_SELECTOR_PAD_CAST (pad);

  /* only forward if we are dealing with the active sinkpad */
  forward = gst_stream_selector_is_active_sinkpad (sel, pad);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_STOP:
      gst_selector_pad_reset (selpad);
      break;
    case GST_EVENT_NEWSEGMENT:
    {
      gboolean update;
      GstFormat format;
      gdouble rate, arate;
      gint64 start, stop, time;

      gst_event_parse_new_segment_full (event, &update, &rate, &arate, &format,
          &start, &stop, &time);

      GST_DEBUG_OBJECT (sel,
          "configured NEWSEGMENT update %d, rate %lf, applied rate %lf, "
          "format %d, "
          "%" G_GINT64_FORMAT " -- %" G_GINT64_FORMAT ", time %"
          G_GINT64_FORMAT, update, rate, arate, format, start, stop, time);

      gst_segment_set_newsegment_full (&selpad->segment, update,
          rate, arate, format, start, stop, time);
      /* if we are not going to forward the segment, mark the segment as
       * pending */
      if (!forward)
        selpad->segment_pending = TRUE;
      break;
    }
    case GST_EVENT_EOS:
      selpad->eos = TRUE;
      break;
    default:
      break;
  }
  if (forward)
    res = gst_pad_push_event (sel->srcpad, event);

  gst_object_unref (sel);

  return res;
}

static GstCaps *
gst_selector_pad_getcaps (GstPad * pad)
{
  GstStreamSelector *sel;
  GstCaps *caps;

  sel = GST_STREAM_SELECTOR (gst_pad_get_parent (pad));

  GST_DEBUG_OBJECT (sel, "Getting caps of srcpad peer");
  caps = gst_pad_peer_get_caps (sel->srcpad);
  if (caps == NULL)
    caps = gst_caps_new_any ();

  gst_object_unref (sel);

  return caps;
}

static GstFlowReturn
gst_selector_pad_bufferalloc (GstPad * pad, guint64 offset,
    guint size, GstCaps * caps, GstBuffer ** buf)
{
  GstStreamSelector *sel;
  GstFlowReturn result;
  GstPad *active_sinkpad;

  sel = GST_STREAM_SELECTOR (gst_pad_get_parent (pad));

  active_sinkpad = gst_stream_selector_activate_sinkpad (sel, pad);

  /* Fallback allocation for buffers from pads except the selected one */
  if (pad != active_sinkpad) {
    GST_DEBUG_OBJECT (sel,
        "Pad %s:%s is not selected. Performing fallback allocation",
        GST_DEBUG_PAD_NAME (pad));

    *buf = NULL;
    result = GST_FLOW_OK;
  } else {
    result = gst_pad_alloc_buffer (sel->srcpad, offset, size, caps, buf);

    /* FIXME: HACK. If buffer alloc returns not-linked, perform a fallback
     * allocation.  This should NOT be necessary, because playbin should
     * properly block the source pad from running until it's finished hooking 
     * everything up, but playbin needs refactoring first. */
    if (result == GST_FLOW_NOT_LINKED) {
      GST_DEBUG_OBJECT (sel,
          "No peer pad yet - performing fallback allocation for pad %s:%s",
          GST_DEBUG_PAD_NAME (pad));

      *buf = NULL;
      result = GST_FLOW_OK;
    }
  }

  gst_object_unref (sel);

  return result;
}

static gboolean
gst_stream_selector_wait (GstStreamSelector * self, GstPad * pad)
{
  gboolean flushing;

  GST_OBJECT_LOCK (self);

  while (self->blocked)
    g_cond_wait (self->blocked_cond, GST_OBJECT_GET_LOCK (self));

  GST_OBJECT_UNLOCK (self);

  GST_OBJECT_LOCK (pad);
  flushing = GST_PAD_IS_FLUSHING (pad);
  GST_OBJECT_UNLOCK (pad);

  return flushing;
}

static GstFlowReturn
gst_selector_pad_chain (GstPad * pad, GstBuffer * buf)
{
  GstStreamSelector *sel;
  GstFlowReturn res;
  GstPad *active_sinkpad;
  GstSelectorPad *selpad;
  GstClockTime timestamp;
  GstSegment *seg;

  sel = GST_STREAM_SELECTOR (gst_pad_get_parent (pad));
  selpad = GST_SELECTOR_PAD_CAST (pad);
  seg = &selpad->segment;

  if (gst_stream_selector_wait (sel, pad))
    goto ignore;

  active_sinkpad = gst_stream_selector_activate_sinkpad (sel, pad);

  timestamp = GST_BUFFER_TIMESTAMP (buf);
  if (GST_CLOCK_TIME_IS_VALID (timestamp)) {
    GST_DEBUG_OBJECT (sel, "received timestamp %" GST_TIME_FORMAT,
        GST_TIME_ARGS (timestamp));
    gst_segment_set_last_stop (seg, seg->format, timestamp);
  }

  /* Ignore buffers from pads except the selected one */
  if (pad != active_sinkpad)
    goto ignore;

  gst_stream_selector_push_pending_stop (sel);

  /* if we have a pending segment, push it out now */
  if (selpad->segment_pending) {
    gst_pad_push_event (sel->srcpad, gst_event_new_new_segment_full (FALSE,
            seg->rate, seg->applied_rate, seg->format, seg->start, seg->stop,
            seg->time));

    selpad->segment_pending = FALSE;
  }

  /* forward */
  GST_DEBUG_OBJECT (sel, "Forwarding buffer %p from pad %s:%s", buf,
      GST_DEBUG_PAD_NAME (pad));
  res = gst_pad_push (sel->srcpad, buf);
done:
  gst_object_unref (sel);
  return res;
  /* dropped buffers */
ignore:
  {
    GST_DEBUG_OBJECT (sel, "Ignoring buffer %p from pad %s:%s",
        buf, GST_DEBUG_PAD_NAME (pad));
    gst_buffer_unref (buf);
    res = GST_FLOW_NOT_LINKED;
    goto done;
  }
}

static void gst_stream_selector_dispose (GObject * object);
static void gst_stream_selector_init (GstStreamSelector * sel);
static void gst_stream_selector_base_init (GstStreamSelectorClass * klass);
static void gst_stream_selector_class_init (GstStreamSelectorClass * klass);
static void gst_stream_selector_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_stream_selector_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);
static GstPad *gst_stream_selector_request_new_pad (GstElement * element,
    GstPadTemplate * templ, const gchar * unused);
static void gst_stream_selector_release_pad (GstElement * element,
    GstPad * pad);
static GList *gst_stream_selector_get_linked_pads (GstPad * pad);
static GstCaps *gst_stream_selector_getcaps (GstPad * pad);
static void gst_stream_selector_block (GstStreamSelector * self);
static void gst_stream_selector_switch (GstStreamSelector * self,
    const gchar * pad_name, GstClockTime stop_time, GstClockTime start_time);

static GstElementClass *parent_class = NULL;

GType
gst_stream_selector_get_type (void)
{
  static GType stream_selector_type = 0;

  if (!stream_selector_type) {
    static const GTypeInfo stream_selector_info = {
      sizeof (GstStreamSelectorClass),
      (GBaseInitFunc) gst_stream_selector_base_init,
      NULL,
      (GClassInitFunc) gst_stream_selector_class_init,
      NULL,
      NULL,
      sizeof (GstStreamSelector),
      0,
      (GInstanceInitFunc) gst_stream_selector_init,
    };
    stream_selector_type =
        g_type_register_static (GST_TYPE_ELEMENT,
        "GstSwitch", &stream_selector_info, 0);
    GST_DEBUG_CATEGORY_INIT (stream_selector_debug,
        "streamselector", 0, "A stream-selector element");
  }

  return stream_selector_type;
}

static void
gst_stream_selector_base_init (GstStreamSelectorClass * klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_set_details (element_class, &gst_stream_selector_details);
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_stream_selector_sink_factory));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_stream_selector_src_factory));
}

static void
gst_stream_selector_class_init (GstStreamSelectorClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);

  parent_class = g_type_class_peek_parent (klass);
  gobject_class->set_property =
      GST_DEBUG_FUNCPTR (gst_stream_selector_set_property);
  gobject_class->get_property =
      GST_DEBUG_FUNCPTR (gst_stream_selector_get_property);
  g_object_class_install_property (gobject_class, PROP_ACTIVE_PAD,
      g_param_spec_string ("active-pad", "Active pad",
          "Name of the currently" " active sink pad", NULL, G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, PROP_LAST_STOP_TIME,
      g_param_spec_uint64 ("last-stop-time", "Last stop time",
          "Last stop time on active pad", 0, G_MAXUINT64, GST_CLOCK_TIME_NONE,
          G_PARAM_READABLE));
  gobject_class->dispose = gst_stream_selector_dispose;
  gstelement_class->request_new_pad = gst_stream_selector_request_new_pad;
  gstelement_class->release_pad = gst_stream_selector_release_pad;

  /**
   * GstStreamSelector::block:
   * @streamselector: the streamselector element to emit this signal on
   *
   * Block all sink pads in preparation for a switch.
   */
  gst_stream_selector_signals[SIGNAL_BLOCK] =
      g_signal_new ("block", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (GstStreamSelectorClass, block),
      NULL, NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);
  /**
   * GstStreamSelector::switch:
   * @streamselector: the streamselector element to emit this signal on
   * @pad:            name of pad to switch to
   * @stop_time:      time at which to close the previous segment, or
   *                  #GST_CLOCK_TIME_NONE for the last time on the previously
   *                  active pad
   * @start_time:     start time for new segment, or foo
   *
   * Switch the given open file descriptor to multifdsink to write to and
   * specify the burst parameters for the new connection.
   */
  gst_stream_selector_signals[SIGNAL_SWITCH] =
      g_signal_new ("switch", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (GstStreamSelectorClass, switch_),
      NULL, NULL, gst_switch_marshal_VOID__STRING_UINT64_UINT64,
      G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_UINT64, G_TYPE_UINT64);

  klass->block = GST_DEBUG_FUNCPTR (gst_stream_selector_block);
  klass->switch_ = GST_DEBUG_FUNCPTR (gst_stream_selector_switch);
}

static void
gst_stream_selector_init (GstStreamSelector * sel)
{
  sel->srcpad = gst_pad_new ("src", GST_PAD_SRC);
  gst_pad_set_internal_link_function (sel->srcpad,
      GST_DEBUG_FUNCPTR (gst_stream_selector_get_linked_pads));
  gst_pad_set_getcaps_function (sel->srcpad,
      GST_DEBUG_FUNCPTR (gst_stream_selector_getcaps));
  gst_element_add_pad (GST_ELEMENT (sel), sel->srcpad);
  /* sinkpad management */
  sel->active_sinkpad = NULL;
  sel->nb_sinkpads = 0;
  gst_segment_init (&sel->segment, GST_FORMAT_UNDEFINED);

  sel->blocked_cond = g_cond_new ();
  sel->blocked = FALSE;
}

static void
gst_stream_selector_dispose (GObject * object)
{
  GstStreamSelector *sel = GST_STREAM_SELECTOR (object);

  if (sel->active_sinkpad) {
    gst_object_unref (sel->active_sinkpad);
    sel->active_sinkpad = NULL;
  }

  if (sel->blocked_cond) {
    g_cond_free (sel->blocked_cond);
    sel->blocked_cond = NULL;
  }

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_stream_selector_set_active_pad (GstStreamSelector * self,
    const gchar * pad_name, GstClockTime stop_time, GstClockTime start_time)
{
  GstPad *pad;
  GstSelectorPad *old, *new;
  GstPad **active_pad_p;

  if (strcmp (pad_name, "") != 0)
    pad = gst_element_get_pad (GST_ELEMENT (self), pad_name);
  else
    pad = NULL;

  GST_OBJECT_LOCK (self);

  if (pad == self->active_sinkpad)
    goto done;

  old = GST_SELECTOR_PAD_CAST (self->active_sinkpad);
  new = GST_SELECTOR_PAD_CAST (pad);

  if (old && old->active && !self->pending_stop
      && GST_CLOCK_TIME_IS_VALID (stop_time)) {
    /* schedule a last_stop update if one isn't already scheduled, and a
       segment has been pushed before. */
    memcpy (&self->pending_stop_segment, &old->segment,
        sizeof (self->pending_stop_segment));
    gst_segment_set_last_stop (&self->pending_stop_segment,
        old->segment.format, stop_time);
    self->pending_stop = TRUE;
  }

  if (new && GST_CLOCK_TIME_IS_VALID (start_time)) {
    /* schedule a new segment push */
    new->segment.start = start_time;
    new->segment_pending = TRUE;
  }

  active_pad_p = &self->active_sinkpad;
  gst_object_replace ((GstObject **) active_pad_p, GST_OBJECT_CAST (pad));
  GST_DEBUG_OBJECT (self, "New active pad is %" GST_PTR_FORMAT,
      self->active_sinkpad);

done:
  GST_OBJECT_UNLOCK (self);

  if (pad)
    gst_object_unref (pad);
}


static void
gst_stream_selector_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstStreamSelector *sel = GST_STREAM_SELECTOR (object);

  switch (prop_id) {
    case PROP_ACTIVE_PAD:
      gst_stream_selector_set_active_pad (sel,
          g_value_get_string (value), GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_stream_selector_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstStreamSelector *sel = GST_STREAM_SELECTOR (object);

  switch (prop_id) {
    case PROP_ACTIVE_PAD:{
      GST_OBJECT_LOCK (object);
      if (sel->active_sinkpad != NULL) {
        g_value_take_string (value, gst_pad_get_name (sel->active_sinkpad));
      } else {
        g_value_set_string (value, "");
      }
      GST_OBJECT_UNLOCK (object);
      break;
    }
    case PROP_LAST_STOP_TIME:{
      GstSelectorPad *spad;

      GST_OBJECT_LOCK (object);
      spad = GST_SELECTOR_PAD_CAST (sel->active_sinkpad);
      if (spad && spad->active)
        g_value_set_uint64 (value, spad->segment.last_stop);
      else
        g_value_set_uint64 (value, GST_CLOCK_TIME_NONE);
      GST_OBJECT_UNLOCK (object);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstPad *
gst_stream_selector_get_linked_pad (GstPad * pad, gboolean strict)
{
  GstStreamSelector *sel;
  GstPad *otherpad = NULL;

  sel = GST_STREAM_SELECTOR (gst_pad_get_parent (pad));
  GST_OBJECT_LOCK (sel);
  if (pad == sel->srcpad)
    otherpad = sel->active_sinkpad;
  else if (pad == sel->active_sinkpad || !strict)
    otherpad = sel->srcpad;
  if (otherpad)
    gst_object_ref (otherpad);
  GST_OBJECT_UNLOCK (sel);
  gst_object_unref (sel);
  return otherpad;
}

static GstCaps *
gst_stream_selector_getcaps (GstPad * pad)
{
  GstPad *otherpad;
  GstObject *parent;
  GstCaps *caps;

  otherpad = gst_stream_selector_get_linked_pad (pad, FALSE);
  parent = gst_object_get_parent (GST_OBJECT (pad));
  if (!otherpad) {
    GST_DEBUG_OBJECT (parent,
        "Pad %s:%s not linked, returning ANY", GST_DEBUG_PAD_NAME (pad));
    caps = gst_caps_new_any ();
  } else {
    GST_DEBUG_OBJECT (parent,
        "Pad %s:%s is linked (to %s:%s), returning peer caps",
        GST_DEBUG_PAD_NAME (pad), GST_DEBUG_PAD_NAME (otherpad));
    /* if the peer has caps, use those. If the pad is not linked, this function
     * returns NULL and we return ANY */
    if (!(caps = gst_pad_peer_get_caps (otherpad)))
      caps = gst_caps_new_any ();
    gst_object_unref (otherpad);
  }

  gst_object_unref (parent);
  return caps;
}

/* check if the pad is the active sinkpad */
static gboolean
gst_stream_selector_is_active_sinkpad (GstStreamSelector * sel, GstPad * pad)
{
  GstSelectorPad *selpad;
  gboolean res;

  selpad = GST_SELECTOR_PAD_CAST (pad);

  GST_OBJECT_LOCK (sel);
  res = (pad == sel->active_sinkpad);
  GST_OBJECT_UNLOCK (sel);

  return res;
}

/* Get or create the active sinkpad */
static GstPad *
gst_stream_selector_activate_sinkpad (GstStreamSelector * sel, GstPad * pad)
{
  GstPad *active_sinkpad;
  GstSelectorPad *selpad;

  selpad = GST_SELECTOR_PAD_CAST (pad);

  GST_OBJECT_LOCK (sel);
  selpad->active = TRUE;
  active_sinkpad = sel->active_sinkpad;
  if (active_sinkpad == NULL) {
    /* first pad we get an alloc on becomes the activated pad by default */
    active_sinkpad = sel->active_sinkpad = gst_object_ref (pad);
    GST_DEBUG_OBJECT (sel, "Activating pad %s:%s", GST_DEBUG_PAD_NAME (pad));
  }
  GST_OBJECT_UNLOCK (sel);

  return active_sinkpad;
}

static GList *
gst_stream_selector_get_linked_pads (GstPad * pad)
{
  GstPad *otherpad;

  otherpad = gst_stream_selector_get_linked_pad (pad, TRUE);
  if (!otherpad)
    return NULL;
  /* need to drop the ref, internal linked pads is not MT safe */
  gst_object_unref (otherpad);
  return g_list_append (NULL, otherpad);
}

static GstPad *
gst_stream_selector_request_new_pad (GstElement * element,
    GstPadTemplate * templ, const gchar * unused)
{
  GstStreamSelector *sel;
  gchar *name = NULL;
  GstPad *sinkpad = NULL;

  sel = GST_STREAM_SELECTOR (element);
  g_return_val_if_fail (templ->direction == GST_PAD_SINK, NULL);
  GST_LOG_OBJECT (sel, "Creating new pad %d", sel->nb_sinkpads);
  GST_OBJECT_LOCK (sel);
  name = g_strdup_printf ("sink%d", sel->nb_sinkpads++);
  sinkpad = g_object_new (GST_TYPE_SELECTOR_PAD,
      "name", name, "direction", templ->direction, "template", templ, NULL);
  g_free (name);
  GST_OBJECT_UNLOCK (sel);

  gst_pad_set_event_function (sinkpad,
      GST_DEBUG_FUNCPTR (gst_selector_pad_event));
  gst_pad_set_getcaps_function (sinkpad,
      GST_DEBUG_FUNCPTR (gst_selector_pad_getcaps));
  gst_pad_set_chain_function (sinkpad,
      GST_DEBUG_FUNCPTR (gst_selector_pad_chain));
  gst_pad_set_internal_link_function (sinkpad,
      GST_DEBUG_FUNCPTR (gst_selector_pad_get_linked_pads));
  gst_pad_set_bufferalloc_function (sinkpad,
      GST_DEBUG_FUNCPTR (gst_selector_pad_bufferalloc));

  gst_pad_set_active (sinkpad, TRUE);
  gst_element_add_pad (GST_ELEMENT (sel), sinkpad);
  return sinkpad;
}

static void
gst_stream_selector_release_pad (GstElement * element, GstPad * pad)
{
  GstStreamSelector *sel;

  sel = GST_STREAM_SELECTOR (element);
  GST_LOG_OBJECT (sel, "Releasing pad %s:%s", GST_DEBUG_PAD_NAME (pad));

  GST_OBJECT_LOCK (sel);
  /* if the pad was the active pad, makes us select a new one */
  if (sel->active_sinkpad == pad) {
    GST_DEBUG_OBJECT (sel, "Deactivating pad %s:%s", GST_DEBUG_PAD_NAME (pad));
    sel->active_sinkpad = NULL;
  }
  GST_OBJECT_UNLOCK (sel);

  gst_pad_set_active (pad, FALSE);
  gst_element_remove_pad (GST_ELEMENT (sel), pad);
}

static void
block_func (GstPad * pad, gboolean blocked, gpointer user_data)
{
  GST_DEBUG_OBJECT (pad, "got blocked = %d", blocked ? 1 : 0);
}

static void
foreach_set_blocking (GstPad * pad, gpointer user_data)
{
  gboolean block = GPOINTER_TO_INT (user_data);

  gst_pad_set_blocked_async (pad, block, block_func, NULL);
}

static gboolean
block_all_pads (GstStreamSelector * self, gboolean block)
{
  GstIterator *iter;
  GstIteratorResult res;

  g_return_val_if_fail (self->blocked != block, FALSE);

  iter = gst_element_iterate_sink_pads (GST_ELEMENT (self));

  while (TRUE) {
    res = gst_iterator_foreach (iter, (GFunc) foreach_set_blocking,
        GINT_TO_POINTER (block));
    switch (res) {
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync (iter);
        break;
      case GST_ITERATOR_DONE:
        goto done;
      default:
        goto error;
    }
  }

done:
  GST_DEBUG_OBJECT (self, "block_all_pads(%d) succeeded", block);
  gst_iterator_free (iter);
  self->blocked = block;
  return TRUE;

error:
  GST_WARNING_OBJECT (self, "block(%d) signal error: %d", block, res);
  gst_iterator_free (iter);
  return FALSE;
}

/* FIXME: blocked flag not mt-safe */

static void
gst_stream_selector_block (GstStreamSelector * self)
{
  block_all_pads (self, TRUE);
}

static void
gst_stream_selector_push_pending_stop (GstStreamSelector * self)
{
  GstEvent *event = NULL;

  GST_OBJECT_LOCK (self);

  if (G_UNLIKELY (self->pending_stop)) {
    GstSegment *seg = &self->pending_stop_segment;

    event = gst_event_new_new_segment_full (TRUE, seg->rate,
        seg->applied_rate, seg->format, seg->start, seg->last_stop, seg->time);

    self->pending_stop = FALSE;
  }

  GST_OBJECT_UNLOCK (self);

  if (event)
    gst_pad_push_event (self->srcpad, event);
}

static void
gst_stream_selector_switch (GstStreamSelector * self, const gchar * pad_name,
    GstClockTime stop_time, GstClockTime start_time)
{
  g_return_if_fail (self->blocked == TRUE);

  gst_stream_selector_set_active_pad (self, pad_name, stop_time, start_time);

  block_all_pads (self, FALSE);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "switch", GST_RANK_NONE,
      GST_TYPE_STREAM_SELECTOR);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "switch",
    "N-to-1 input switching",
    plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
