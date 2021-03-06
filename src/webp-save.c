/**
 * gimp-webp - WebP Plugin for the GIMP
 * Copyright (C) 2015  Nathan Osman
 * Copyright (C) 2016  Ben Touchette
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <glib/gstdio.h>
#include <libgimp/gimp.h>
#include <libgimp/gimpui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <config.h>
#ifdef GIMP_2_9
#include <gegl.h>
#endif
#include <webp/encode.h>
#include <webp/mux.h>

#include "webp-save.h"

WebPPreset webp_preset_by_name(gchar *name)
{
  if(!strcmp(name, "picture"))
    {
      return WEBP_PRESET_PICTURE;
    }
  else if(!strcmp(name, "photo"))
    {
      return WEBP_PRESET_PHOTO;
    }
  else if(!strcmp(name, "drawing"))
    {
      return WEBP_PRESET_DRAWING;
    }
  else if(!strcmp(name, "icon"))
    {
      return WEBP_PRESET_ICON;
    }
  else if(!strcmp(name, "text"))
    {
      return WEBP_PRESET_TEXT;
    }
  else
    {
      return WEBP_PRESET_DEFAULT;
    }
}

int webp_anim_file_writer(FILE *outfile,
                          const uint8_t* data,
                          size_t data_size)
{
  int ok = 0;

  if (data == NULL)
    {
      return 0;
    }

  ok = (fwrite(data, data_size, 1, outfile) == 1);

  return ok;
}

int webp_file_writer(const uint8_t     *data,
                     size_t             data_size,
                     const WebPPicture *picture)
{
  FILE *outfile;

  /* Obtain the FILE* and write the data to the file */
  outfile = (FILE*)picture->custom_ptr;
  return fwrite(data, sizeof(uint8_t), data_size, outfile) == data_size;
}

int webp_file_progress(int                percent,
                       const WebPPicture *picture)
{
  return gimp_progress_update(percent / 100.0);
}

const gchar *webp_error_string(WebPEncodingError error_code)
{
  switch(error_code)
    {
    case VP8_ENC_ERROR_OUT_OF_MEMORY:
      return "out of memory";
    case VP8_ENC_ERROR_BITSTREAM_OUT_OF_MEMORY:
      return "not enough memory to flush bits";
    case VP8_ENC_ERROR_NULL_PARAMETER:
      return "NULL parameter";
    case VP8_ENC_ERROR_INVALID_CONFIGURATION:
      return "invalid configuration";
    case VP8_ENC_ERROR_BAD_DIMENSION:
      return "bad image dimensions";
    case VP8_ENC_ERROR_PARTITION0_OVERFLOW:
      return "partition is bigger than 512K";
    case VP8_ENC_ERROR_PARTITION_OVERFLOW:
      return "partition is bigger than 16M";
    case VP8_ENC_ERROR_BAD_WRITE:
      return "unable to flush bytes";
    case VP8_ENC_ERROR_FILE_TOO_BIG:
      return "file is larger than 4GiB";
    case VP8_ENC_ERROR_USER_ABORT:
      return "user aborted encoding";
    case VP8_ENC_ERROR_LAST:
      return "list terminator";
    default:
      return "unknown error";
    }
}

gboolean save_layer(const gchar    *filename,
                    gint32          nLayers,
                    gint32          image_ID,
                    gint32          drawable_ID,
                    WebPSaveParams *params,
                    GError        **error)
{
  gboolean          status   = FALSE;
  FILE             *outfile  = NULL;
  WebPConfig        config   = {0};
  WebPPicture       picture  = {0};
  guchar           *buffer   = NULL;
  gint              w, h;
  gint              bpp;
#ifdef GIMP_2_9
  GimpColorProfile *profile;
  GimpImageType     drawable_type;
  GeglBuffer       *geglbuffer;
  GeglRectangle     extent;
#else
  GimpDrawable     *drawable = NULL;
  GimpPixelRgn      region;
  GimpImageType     drawable_type;
#endif
  gchar            *indata;
  gsize             indatalen;
  struct            stat stsz;
  int               fd_outfile;

  /* The do...while() loop is a neat little trick that makes it easier to
   * jump to error handling code while still ensuring proper cleanup */

  do
    {
      /* Begin displaying export progress */
      gimp_progress_init_printf("Saving '%s'",
                                gimp_filename_to_utf8(filename));

      /* Attempt to open the output file */
      if((outfile = g_fopen(filename, "wb+")) == NULL)
        {
          g_set_error(error,
                      G_FILE_ERROR,
                      g_file_error_from_errno(errno),
                      "Unable to open '%s' for writing",
                      gimp_filename_to_utf8(filename));
          break;
        }

      /* Obtain the drawable type */
      drawable_type = gimp_drawable_type(drawable_ID);

      /* Retrieve the buffer for the layer */
#ifdef GIMP_2_9
      geglbuffer = gimp_drawable_get_buffer(drawable_ID);
      extent = *gegl_buffer_get_extent(geglbuffer);
      bpp = gimp_drawable_bpp(drawable_ID);
      w = extent.width;
      h = extent.height;
#else
      bpp = gimp_drawable_bpp(drawable_ID);
      w = gimp_drawable_width(drawable_ID);
      h = gimp_drawable_height(drawable_ID);
      drawable_type = gimp_drawable_type(drawable_ID);
#endif

      /* Initialize the WebP configuration with a preset and fill in the
       * remaining values */
      WebPConfigPreset(&config,
                       webp_preset_by_name(params->preset),
                       params->quality);

      config.lossless      = params->lossless;
      config.method        = 6;  /* better quality */
      config.alpha_quality = params->alpha_quality;

      /* Prepare the WebP structure */
      WebPPictureInit(&picture);
      picture.use_argb      = 1;
      picture.width         = w;
      picture.height        = h;
      picture.writer        = webp_file_writer;
      picture.custom_ptr    = outfile;
      picture.progress_hook = webp_file_progress;

      /* Attempt to allocate a buffer of the appropriate size */
      buffer = (guchar *)g_malloc(w * h * bpp);
      if(!buffer)
        {
          break;
        }

#ifdef GIMP_2_9
      /* Read the region into the buffer */
      gegl_buffer_get (geglbuffer, &extent, 1.0, NULL, buffer,
                       GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_NONE);
#else
      /* Get the drawable */
      drawable = gimp_drawable_get(drawable_ID);

      /* Obtain the pixel region for the drawable */
      gimp_pixel_rgn_init(&region,
                          drawable,
                          0, 0,
                          w, h,
                          FALSE, FALSE);

      /* Read the region into the buffer */
      gimp_pixel_rgn_get_rect(&region,
                              buffer,
                              0, 0,
                              w, h);

      gimp_drawable_detach(drawable);
#endif

      /* Use the appropriate function to import the data from the buffer */
      if(drawable_type == GIMP_RGB_IMAGE)
        {
          WebPPictureImportRGB(&picture, buffer, w * bpp);
        }
      else
        {
          WebPPictureImportRGBA(&picture, buffer, w * bpp);
        }

      /* Perform the actual encode */
      if(!WebPEncode(&config, &picture))
        {
          g_print("WebP error: '%s'",
                  webp_error_string(picture.error_code));
          g_set_error(error,
                      G_FILE_ERROR,
                      picture.error_code,
                      "WebP error: '%s'",
                      webp_error_string(picture.error_code));
          break;
        }

      /* The cleanup stuff still needs to run but indicate that everything
       * completed successfully */
      status = TRUE;

    }
  while(0);

#ifdef GIMP_2_9
  /* Flush the drawable and detach */
  gegl_buffer_flush (geglbuffer);
  g_object_unref(geglbuffer);
#endif

#ifdef GIMP_2_9
  fflush(outfile);
  fd_outfile = fileno(outfile);
  fstat(fd_outfile, &stsz);
  indatalen = stsz.st_size;
  if (indatalen > 0)
    {
      indata = (gchar*)g_malloc(indatalen);
      rewind(outfile);
      int res = fread(indata, 1, indatalen, outfile);
      if (res > 0)
        {
          WebPMux *mux;
          WebPData wp_data;
          wp_data.bytes = (uint8_t*)indata;
          wp_data.size = indatalen;
          mux = WebPMuxCreate(&wp_data, 1);

          if (mux != NULL)
            {
              gboolean saved = FALSE;
              /* Save ICC data */
              profile = gimp_image_get_color_profile (image_ID);
              if (profile)
                {
                  saved = TRUE;
                  WebPData      chunk;
                  const guint8 *icc_data;
                  gsize         icc_data_size;
                  icc_data = gimp_color_profile_get_icc_profile (profile, &icc_data_size);
                  chunk.bytes = icc_data;
                  chunk.size = icc_data_size;
                  WebPMuxSetChunk(mux, "ICCP", &chunk, 1);
                  g_object_unref (profile);
                }

              if (saved == TRUE)
                {
                  WebPMuxAssemble(mux, &wp_data);
                  rewind(outfile);
                  webp_anim_file_writer(outfile, wp_data.bytes, wp_data.size);
                }
            }
          else
            {
              g_printerr("ERROR: Cannot create mux. Can't save features update.\n");
            }

          WebPDataClear(&wp_data);
        }
      else
        {
          g_printerr("ERROR: No data read for features. Can't save features update.\n");
        }
    }
  else
    {
      g_printerr("ERROR: No data for features. Can't save features update.\n");
    }
#endif

  /* Free any resources */
  if(outfile)
    {
      fclose(outfile);
    }

  if(buffer)
    {
      free(buffer);
    }

  WebPPictureFree(&picture);

  return status;
}

gboolean save_animation(const gchar    *filename,
                        gint32          nLayers,
                        gint32         *allLayers,
                        gint32          image_ID,
                        gint32          drawable_ID,
                        WebPSaveParams *params,
                        GError        **error)
{
  gboolean          status   = FALSE;
  FILE             *outfile  = NULL;
  WebPConfig        config   = {0};
  guchar           *buffer   = NULL;
  guchar            r, g, b, a;
  gint              w, h, bpp;
  GimpImageType     drawable_type;
  GimpRGB           bgcolor;
#ifdef GIMP_2_9
  GimpColorProfile *profile;
#else
  GimpDrawable     *drawable = NULL;
  GimpPixelRgn      region;
#endif
  WebPAnimEncoderOptions enc_options;
  WebPData          webp_data;
  int               frame_timestamp = 0;
  WebPAnimEncoder  *enc;

  g_print("attempting to save (all) %d layer(s)\n", nLayers);

  if (nLayers < 1)
    return FALSE;

  do
    {
      /* Begin displaying export progress */
      gimp_progress_init_printf("Saving '%s'",
                                gimp_filename_to_utf8(filename));

      /* Attempt to open the output file */
      if((outfile = g_fopen(filename, "wb")) == NULL)
        {
          g_set_error(error,
                      G_FILE_ERROR,
                      g_file_error_from_errno(errno),
                      "Unable to open '%s' for writing",
                      gimp_filename_to_utf8(filename));
          break;
        }

      WebPDataInit(&webp_data);
      if (!WebPAnimEncoderOptionsInit(&enc_options))
        {
          g_print("[WebPAnimEncoderOptionsInit]ERROR: verion mismatch\n");
          break;
        }

      for (int loop = 0; loop < nLayers; loop++)
        {
          /* Obtain the drawable type */
          drawable_type = gimp_drawable_type(allLayers[loop]);

          /* Retrieve the buffer for the layer */
#ifdef GIMP_2_9
          GeglBuffer       *geglbuffer;
          GeglRectangle     extent;
          geglbuffer = gimp_drawable_get_buffer (allLayers[loop]);
          extent = *gegl_buffer_get_extent (geglbuffer);
          bpp = gimp_drawable_bpp (allLayers[loop]);
          w = extent.width;
          h = extent.height;
#else
          /* Retrieve the image data */
          bpp = gimp_drawable_bpp(allLayers[loop]);
          w = gimp_drawable_width(allLayers[loop]);
          h = gimp_drawable_height(allLayers[loop]);
#endif

          if (loop == 0)
            {
              enc = WebPAnimEncoderNew(w, h, &enc_options);
              if (enc == NULL)
                {
                  g_print("[WebPAnimEncoderNew]ERROR: enc == null\n");
                  break;
                }
            }

          /* Attempt to allocate a buffer of the appropriate size */
          buffer = (guchar *)g_malloc(w * h * bpp);
          if(!buffer)
            {
              g_print("Buffer error: 'buffer null'\n");
              status = FALSE;
              break;
            }

          WebPConfig config;
          WebPConfigInit(&config);
          WebPConfigPreset(&config,
                           webp_preset_by_name(params->preset),
                           params->quality);

          config.lossless      = params->lossless;
          config.method        = 6;  /* better quality */
          config.alpha_quality = params->alpha_quality;
          config.exact         = 1;

          WebPMemoryWriter mw = { 0 };
          WebPMemoryWriterInit(&mw);

          /* Prepare the WebP structure */
          WebPPicture picture;
          WebPPictureInit(&picture);
          picture.use_argb      = 1;
          picture.argb_stride   = w * bpp;
          picture.width         = w;
          picture.height        = h;
          picture.custom_ptr    = &mw;
          picture.writer        = WebPMemoryWrite;
          picture.progress_hook = webp_file_progress;

#ifdef GIMP_2_9
          /* Read the region into the buffer */
          gegl_buffer_get (geglbuffer, &extent, 1.0, NULL, buffer,
                           GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_NONE);
#else
          /* Get the drawable */
          drawable = gimp_drawable_get(allLayers[loop]);

          /* Obtain the pixel region for the drawable */
          gimp_pixel_rgn_init(&region,
                              drawable,
                              0, 0,
                              w, h,
                              FALSE, FALSE);

          /* Read the region into the buffer */
          gimp_pixel_rgn_get_rect(&region,
                                  buffer,
                                  0, 0,
                                  w, h);

          gimp_drawable_detach(drawable);
#endif

          /* Use the appropriate function to import the data from the buffer */
          if(drawable_type == GIMP_RGB_IMAGE)
            {
              WebPPictureImportRGB(&picture, buffer, w * bpp);
            }
          else
            {
              WebPPictureImportRGBA(&picture, buffer, w * bpp);
            }

          /* Perform the actual encode */
          if (!WebPAnimEncoderAdd (enc, &picture, frame_timestamp, &config))
            {
              g_print("[WebPAnimEncoderAdd]ERROR[%d]: %s\n",
                      picture.error_code,
                      webp_error_string(picture.error_code));
            }

          WebPMemoryWriterClear(&mw);
          WebPPictureFree(&picture);

          if(buffer)
            {
              free(buffer);
            }

#ifdef GIMP_2_9
          /* Flush the drawable and detach */
          gegl_buffer_flush (geglbuffer);
          g_object_unref(geglbuffer);
#endif
        }

      WebPAnimEncoderAdd(enc, NULL, frame_timestamp, NULL);

      if (!WebPAnimEncoderAssemble(enc, &webp_data))
        {
          g_print("[WebPAnimEncoderAssemble]ERROR: %s\n",
                  WebPAnimEncoderGetError(enc));
        }

      /* Set animations parameters */
      WebPMux           *mux;
      WebPMuxAnimParams  anim_params = {0};

      mux = WebPMuxCreate(&webp_data, 1);

      /* Anim parameters */
#ifdef __BACKGROUND_COLOR__
      gimp_context_get_background (&bgcolor);
      gimp_rgb_get_uchar (&bgcolor, &r, &g, &b);
      anim_params.bgcolor = (b << 24) + (g << 16) + (r << 8) + 0xFF;
#endif
      anim_params.loop_count = 0;
      if (params->loop == FALSE)
        {
          anim_params.loop_count = 1;
        }

      WebPMuxSetAnimationParams(mux, &anim_params);

#ifdef GIMP_2_9
      /* Save ICC data */
      profile = gimp_image_get_color_profile (image_ID);
      if (profile)
        {
          WebPData      chunk;
          const guint8 *icc_data;
          gsize         icc_data_size;
          icc_data = gimp_color_profile_get_icc_profile (profile, &icc_data_size);
          chunk.bytes = icc_data;
          chunk.size = icc_data_size;
          WebPMuxSetChunk(mux, "ICCP", &chunk, 1);
          g_object_unref (profile);
        }
#endif

      WebPMuxAssemble(mux, &webp_data);

      webp_anim_file_writer(outfile, webp_data.bytes, webp_data.size);

      WebPDataClear(&webp_data);
      WebPAnimEncoderDelete(enc);

      status = TRUE;
    }
  while(0);

  /* Free any resources */
  if(outfile)
    {
      fclose(outfile);
    }

  return status;
}


gboolean save_image(const gchar    *filename,
                    gint32          nLayers,
                    gint32         *allLayers,
                    gint32          image_ID,
                    gint32          drawable_ID,
                    WebPSaveParams *params,
                    GError        **error)
{
#ifdef GIMP_2_9
  GimpMetadata          *metadata;
  GimpMetadataSaveFlags  metadata_flags;
#endif
  gboolean               status = FALSE;
  GFile                 *file;

  if (nLayers == 0)
    {
      return FALSE;
    }

#ifdef GIMP_2_9
  gegl_init(NULL, NULL);
#endif

  g_print("Saving WebP file %s\n", filename);

  if (nLayers == 1)
    {
      status = save_layer(filename, nLayers, image_ID, drawable_ID, params,
                          error);
    }
  else
    {
      if (params->animation == FALSE)
        {
          status = save_layer(filename, nLayers, image_ID, drawable_ID, params,
                              error);
        }
      else
        {
          status = save_animation(filename, nLayers, allLayers, image_ID, drawable_ID,
                                  params, error);
        }
    }

#ifdef GIMP_2_9
  metadata = gimp_image_metadata_save_prepare (image_ID,
             "image/webp",
             &metadata_flags);

  if (metadata)
    {
      gimp_metadata_set_bits_per_sample (metadata, 8);
      file = g_file_new_for_path (filename);
      gimp_image_metadata_save_finish (image_ID,
                                       "image/webp",
                                       metadata, metadata_flags,
                                       file, NULL);
      g_object_unref (file);
    }
#endif

  /* Return the status */
  return status;
}
