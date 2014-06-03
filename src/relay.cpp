#include <gst/gst.h>
#include <cairo.h>
#include <gst/video/video-info.h>
#include <stdio.h>
#include <jsonrpc/rpc.h>
#include <iostream>

#include "relay.hpp"

#include "colors.h"


Relay::Relay(const std::string& name, const std::string& uri)
  :m_name(name)
  ,m_uri(uri)
  ,m_rpc_server(0)
  ,pipeline(0)
  ,bus(0)
  ,demux(0)
  ,videoconvert(0)
  ,overlay(0)
  ,videoconvert2(0)
  ,h264enc(0)
  ,tsmux(0)
  ,tcpsink(0)
  ,audioconvert(0)
  ,lamemp3enc(0)
  ,valid(false)
  ,width(0)
  ,height(0)
  ,current_x_coord(0)
  ,previous_timestamp(0)
  ,blurb("Testing, one, two, three...")
{
  //TODO: Move to RAII
  m_rpc_server = new VideoOverlayRPCServer(&queue);
}

Relay::~Relay() {
  if(bus) {
    gst_object_unref(bus);
  }
  if(pipeline) {
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
  }
  if(m_rpc_server)
  {
    delete m_rpc_server;
  }

}

bool Relay::Initialize(void) {

  demux = gst_element_factory_make ("uridecodebin", "demux");
  videoconvert = gst_element_factory_make ("videoconvert", "videoconv");
  overlay = gst_element_factory_make("cairooverlay", "overlay");
  videoconvert2 = gst_element_factory_make("videoconvert", "videoconv2");
  h264enc = gst_element_factory_make ("x264enc", "h264");
  tsmux = gst_element_factory_make("mpegtsmux", "tsmux");
  tcpsink = gst_element_factory_make ("tcpserversink", "sink");
  audioconvert = gst_element_factory_make("audioconvert", "audioconvert");
  lamemp3enc = gst_element_factory_make("lamemp3enc", "lamemp3enc");
   
  /* Create the empty pipeline */
  pipeline = gst_pipeline_new ("test-pipeline");
  
  //TODO: Test all pipeline elements to ensure they're created correctly.
  if (!pipeline) {
    g_printerr ("Not all elements could be created.\n");
    return false;
  }
  
  /* Build the pipeline */
  gst_bin_add_many (GST_BIN (pipeline), demux, videoconvert, overlay, videoconvert2, h264enc,
      tsmux, tcpsink, audioconvert, lamemp3enc, NULL);
  if (gst_element_link(videoconvert, overlay) != TRUE) {
    g_printerr ("Elements could not be linked.\n");
    gst_object_unref (pipeline);
    return false;
  }
  if (gst_element_link(overlay, videoconvert2) != TRUE) {
    g_printerr ("Elements could not be linked.\n");
    gst_object_unref(pipeline);
    return false;
  }
  if (gst_element_link(videoconvert2, h264enc) != TRUE) {
    g_printerr("Elements could not be linked.\n");
    gst_object_unref (pipeline);
    return false;
  }
  if (gst_element_link(h264enc, tsmux) != TRUE) {
    g_printerr("Elements could not be linked.\n");
    gst_object_unref (pipeline);
    return false;
  }
  if (gst_element_link(tsmux, tcpsink) != TRUE) {
    g_printerr("Elements could not be linked.\n");
    gst_object_unref(pipeline);
    return false;
  }
  if (gst_element_link (audioconvert, lamemp3enc) != TRUE) {
    g_printerr ("Elements could not be linked.\n");
    gst_object_unref(pipeline);
    return false;
  }
  if (gst_element_link(lamemp3enc, tsmux) != TRUE) {
    g_printerr("Elements could not be linked.\n");
    gst_object_unref(pipeline);
    return false;
  }

  g_object_set(demux, "uri", m_uri.c_str(), NULL);
  g_object_set(tcpsink, "port", 10000, NULL);
  
  /* Connect to the pad-added signal */
  g_signal_connect(demux, "pad-added", G_CALLBACK (Relay::pad_added_handler), this);

  /* connect 'on draw' and video image size change handlers to cairo overlay*/
  //CairoOverlayState overlay_state;
  g_signal_connect(overlay,"draw", G_CALLBACK (Relay::draw_overlay), this);
  g_signal_connect(overlay, "caps-changed",G_CALLBACK (Relay::prepare_overlay), this);

  return true;

}
bool Relay::Run(void) {
  GstMessage *msg;
  GstStateChangeReturn ret;
  /* Set the URI to play */
  ret = gst_element_set_state (pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr ("Unable to set the pipeline to the playing state.\n");
    gst_object_unref (pipeline);
    return -1;
  }
  
  /* Wait until error or EOS */
  bus = gst_element_get_bus(pipeline);
  msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
  
  /* Parse message */
  if (msg != NULL) {
    GError *err;
    gchar *debug_info;
    
    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_ERROR:
        gst_message_parse_error (msg, &err, &debug_info);
        g_printerr ("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
        g_printerr ("Debugging information: %s\n", debug_info ? debug_info : "none");
        g_clear_error (&err);
        g_free (debug_info);
        break;
      case GST_MESSAGE_EOS:
        g_print ("End-Of-Stream reached.\n");
        break;
      default:
        /* We should not reach here because we only asked for ERRORs and EOS */
        g_printerr ("Unexpected message received.\n");
        break;
    }
    gst_message_unref (msg);
  }
}
bool Relay::Stop(void) {

}

/* Handler for the pad-added signal */
void Relay::pad_added_handler (GstElement *src, GstPad *new_pad, Relay *data) {
  GstPad *sink_pad = NULL;
  GstPadLinkReturn ret;
  GstCaps *new_pad_caps = NULL;
  GstStructure *new_pad_struct = NULL;
  const gchar *new_pad_type = NULL;
  
  g_print (YELLOW "[MSG]" RESET "\tReceived new pad '%s' from '%s':\n", GST_PAD_NAME (new_pad), GST_ELEMENT_NAME (src));

  /* Check the new pad's type */
  new_pad_caps = gst_pad_query_caps(new_pad, 0);
  new_pad_struct = gst_caps_get_structure (new_pad_caps, 0);
  new_pad_type = gst_structure_get_name (new_pad_struct);

  if(g_str_has_prefix(new_pad_type, "video/x-raw")) {
     sink_pad = gst_element_get_static_pad (data->videoconvert, "sink");
  }else if(g_str_has_prefix(new_pad_type, "audio/x-raw")) {
    sink_pad = gst_element_get_static_pad (data->audioconvert, "sink");
  }else{
    goto exit;
  }
  
  /* If our converter is already linked, we have nothing to do here */
  if (gst_pad_is_linked (sink_pad)) {
    g_print ("  We are already linked. Ignoring.\n");
    goto exit;
  }
  
  /* Attempt the link */
  ret = gst_pad_link (new_pad, sink_pad);
  if (GST_PAD_LINK_FAILED (ret)) {
    g_print ("  Type is '%s' but link failed.\n", new_pad_type);
  } else {
    g_print ( GREEN "[OK]" RESET "\tPipeline of type '%s' is now online.\n", new_pad_type);
  }
  
exit:
  /* Unreference the new pad's caps, if we got them */
  if (new_pad_caps != NULL)
    gst_caps_unref (new_pad_caps);
  
  /* Unreference the sink pad */
  gst_object_unref (sink_pad);
}


void Relay::prepare_overlay (GstElement * overlay, GstCaps * caps, gpointer user_data)
{
  Relay *s = static_cast<Relay*>(user_data);

   /*gst_video_format_parse_caps (caps, NULL, &state->width, &state->height);*/
  GstVideoInfo info;
  gst_video_info_from_caps(&info, caps);
  s->width = info.width;
  s->height = info.height;
  s->current_x_coord = info.width;
  //TODO: Set this to actual current timestamp value(if possible)
  s->previous_timestamp = -1;
  s->valid = TRUE;
 }


void Relay::draw_overlay (GstElement * overlay, cairo_t * cr, guint64 timestamp, 
   guint64 duration, gpointer user_data)
 {
   Relay *s = static_cast<Relay*>(user_data);
   double scale;

   if (!s->valid)
    return;

  //TODO:In current initial state, the previous_timestamp value is invalid.
  if(s->previous_timestamp<0) {
    s->previous_timestamp = timestamp;
  }

  //Are there commands to take out of our RPC queue?
  if(!s->queue.empty())
  {
    s->blurb = s->queue.front();
    s->queue.pop();
  }
  
  cairo_text_extents_t te;
  cairo_set_source_rgb (cr, 1.0, 1.0, 0.0);
  cairo_select_font_face (cr, "Georgia", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size (cr, 35.0);
  cairo_text_extents (cr, s->blurb.c_str(), &te);
  double dt = ((timestamp-s->previous_timestamp)/(double)1e9);//time format is in nanoseconds, so we convert to seconds
  //double dt = ((duration)/(double)1e9);//time format is in nanoseconds, so we convert to seconds
  s->previous_timestamp = timestamp;
  //printf("elapsed time %f", elapsed_time);
  s->current_x_coord -= ((s->width+te.width)/12.0)*dt;//full scroll in 12 seconds.
  if(s->current_x_coord<(-1.0*te.width))//wraparound.
  {
    s->current_x_coord = s->width;
  }
  //cairo_move_to (cr, 0.5 - te.width / 2 - te.x_bearing, 0.5 - te.height / 2 - te.y_bearing);
  cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
  cairo_move_to(cr, s->current_x_coord+3, (2*s->height/3)+3);
  cairo_show_text (cr, s->blurb.c_str());
  cairo_set_source_rgb (cr, 1.0, 1.0, 0.0);
  cairo_move_to(cr, s->current_x_coord, 2*s->height/3);
  cairo_show_text (cr, s->blurb.c_str());
}

/******************************************************************************
function: main()
currently takes one argument: url of stream to relay
Can connect to this realy via TCP on port 1000
(e.g) tcp://127.0.0.1:10000
******************************************************************************/
int main(int argc, char *argv[]) {
  //GstElement *pipeline, *source, *sink;
  //GstBus *bus;
  //GstMessage *msg;
  //GstStateChangeReturn ret;

  /* Initialize GStreamer */
  gst_init (&argc, &argv);

  //MyStubServer s(&queue);

  //draw url to relay out of our command line args
  if(argc<2) {
    g_print ("********relay-stream-with-overlay********"
    BOLDWHITE "\nusage: relay-stream-with-overlay [URL of http stream to relay]" RESET
    "\nIf successful, program should report correct stream online status."
    "\nClients (i.e. vlc) can then connect to the relay via TCP on port 10000"
    BOLDRED "\nPort value 10000 " RESET "is currently hard coded."
    "\nThe URL i'm using for VLC is " BOLDWHITE "\"tcp://127.0.0.1:10000\"" RESET
    "\nfor a vlc instance on the same host."
    "\nSome work needs to be done to make a more scalable solution.");
    return -1;
  }

  const std::string uri = argv[1];

  try {
    Relay relay(std::string("my_relay"), uri);

    relay.Initialize();
    relay.Run();

  }catch(...){
    g_printerr("Exception thrown. exiting.");
  }

  return 0;
}