Subtext
=======

Subtext is a subtitle renderer that uses libass and ffmpeg.

.. function::   TextFile(clip clip, string file[, string charset="UTF-8", float scale=1, int debuglevel=0, string fontdir="", float linespacing=0, int[] margins=[0, 0, 0, 0], float sar=0, string style="", bint blend=True, int matrix, string matrix_s, int transfer, string transfer_s, int primaries, string primaries_s])
   :module: sub

   TextFile renders text subtitles. Supported formats include ASS,
   JACOsub, MicroDVD, SAMI, SRT, WebVTT, and some other obscure ones.

   TextFile has two modes of operation. With blend=True (the default),
   it returns *clip* with the subtitles burned in. With blend=False, it
   returns an RGB24 clip containing the rendered subtitles, with a Gray8
   frame attached to each frame in the ``_Alpha`` frame property. These
   Gray8 frames can be extracted using std.PropToClip.

   Parameters:
      clip
         Input clip. It must have a constant format, constant
         dimensions, and a constant frame rate.

      file
         Subtitle file to be rendered.

      charset
         Character set of the subtitle, in iconv format.

      scale
         Font scale.

      debuglevel
         Debug level. Increase to make libass more chatty.
         See `ass_utils.h <https://github.com/libass/libass/blob/master/libass/ass_utils.h>`_
         in libass for the list of meaningful values.

      fontdir
         Directory with additional fonts.

      linespacing
         Space between lines, in pixels.

      margins
         Additional margins, in pixels. Negative values are not
         allowed. The order is top, bottom, left, right.

      sar
         Sample aspect ratio, also known as the pixel aspect ratio: the
         ratio of the width of a pixel to its height. 1 means square
         pixels.

         By default this follows the ``_SARNum`` and ``_SARDen`` frame
         properties of *clip*, individually for every frame, so
         anamorphic video gets subtitles that look right once the video
         is displayed at its intended shape. Frames without those
         properties are treated as having square pixels.

         Setting it uses the given ratio for every frame instead. That
         also means no frame of *clip* has to be looked at, which is
         worth doing when blend=False and producing the frames of *clip*
         is expensive.

         Only subtitles this plugin lays out are followed that way:
         the text given to Subtitle, and formats converted to ASS such
         as SRT and WebVTT. An ASS file is left exactly as its author
         wrote it, the way other renderers treat it, so its subtitles
         are squeezed along with the picture. Setting *sar* asks for
         the file to be adapted anyway.

         Any compensation only rescales subtitles horizontally, keeping the
         relationship between font size and height constant.

         Subtitles carrying ``LayoutResX`` and ``LayoutResY`` headers
         already express the shape they're laid out for and are rendered to it.
         In this case, neither frame properties nor this parameter are used.

      style
         Custom ASS style for subtitle formats other than ASS. If empty
         (the default), libavcodec's default style is used. This
         parameter has no effect on ASS subtitles.

      blend
         If True, the subtitles will be blended into *clip*. Otherwise,
         the bitmaps will be returned untouched.

      matrix

      matrix_s

      transfer

      transfer_s

      primaries

      primaries_s
         Overrides for the colorimetry of *clip*. Only used when
         blend=True, and only needed when *clip* is tagged wrongly or
         not at all.

         By default the subtitles are converted to match *clip*, frame
         by frame, following its ``_Matrix``, ``_Transfer``,
         ``_Primaries``, ``_ColorRange`` and ``_ChromaLocation`` frame
         properties. The subtitles are treated as BT.709 to start with,
         so a frame tagged as, say, PQ and BT.2020 gets subtitles
         converted into that colour space instead of blended in as if
         they were already in it. A frame that says nothing usable is
         handled the way it always was: the BT.709 matrix and no colour
         space conversion. The colour range is always taken from *clip*
         and can't be overridden, because the two can only be blended
         when they agree on it.

         These are worth reaching for in two cases:

         Untagged clips. Nothing can be adapted to, so the subtitles
         fall back to BT.709. Standard definition material is the usual
         victim, and ``matrix_s="470bg"`` is the usual answer.
         Tagging the clip with std.SetFrameProps instead is generally
         better, since that fixes everything downstream and not just
         the subtitles.

         Turning the colour space conversion off. Passing
         ``transfer=1, primaries=1`` converts the subtitles from BT.709
         to BT.709, which does nothing, so they end up blended in
         unconverted the way they were before this was done per frame.
         Both are needed, since either one on its own still leaves the
         other being adapted to.


.. function::   Subtitle(clip clip, string text[, int start=0, int end=clip.numFrames, int debuglevel=0, string fontdir="", float linespacing=0, int[] margins=[0, 0, 0, 0], float sar=0, string style="sans-serif,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,7,10,10,10,1", bint blend=True, int matrix, string matrix_s, int transfer, string transfer_s, int primaries, string primaries_s])
   :module: sub

   Instead of rendering a subtitle file, Subtitle renders the string *text*.
   Otherwise it works the same as TextFile.

   Parameters:
      text
         String to be rendered. This can include ASS tags to enable rich text and animation.

      style
         Custom ASS style to be used.
      
      start, end
         Subtitle will be shown from *start* up until *end*. By default this will be for all frames in *clip*.

   The other parameters have the same meanings as with TextFile.


.. function::   ImageFile(clip clip, string file[, int id=-1, int[] palette, bint gray=False, bint info=False, bint flatten=False, bint blend=True, int matrix, string matrix_s, int transfer, string transfer_s, int primaries, string primaries_s])
   :module: sub

   ImageFile renders image-based subtitles such as VOBSUB and PGS.

   Parameters:
      *clip*
         If *blend* is True, the subtitles will be burned into this
         clip, Otherwise, only the frame rate and number of frames
         will be obtained from this clip.

         It must have a constant format, constant dimensions, and a
         constant frame rate, even when *blend* is False.

      *file*
         Name of the subtitle file. For VOBSUB, it must the name of the
         idx file. The corresponding sub file must be in the same
         folder, and it must have the same name.

      *id*
         Id of the subtitle track to render. There may be several
         subtitle tracks in the same file. If this is -1, the first
         supported subtitle track will be rendered. Use info=True to
         see a list of all subtitle tracks, including their ids.

         Default: -1.

      *palette*
         Custom palette. This is an array of at most 256 integers. Each
         element's least significant four bytes must contain the values
         for alpha, red, green, and blue, in that order, from most
         significant to least.

         Additionally, the special value 2**42 means that the
         corresponding element of the original palette is used. This
         way it is possible to override only the third element, without
         overriding the first and second ones, for example.

         An alpha value of 255 means the colour will be completely
         opaque, and a value of 0 means the colour will be completely
         transparent.

      *gray*
         If True, the subtitles will be turned gray.

         Default: False.

      *info*
         If this is True, a list of all supported subtitle tracks found
         in the file will be printed on each frame of the output. The
         information printed about each track includes the id, the
         language (if known), the resolution, and the format.

         Default: False.

      *flatten*
         If this is True, ImageFile will output a clip with exactly as
         many frames as there are pictures in the subtitle file.

         If this is True, *blend* has no effect (no automatic blending).

         Default: False.

   The other parameters have the same meanings as with TextFile.


Example with manual blending::

   sub = core.sub.TextFile(clip=YUV420P10_video, file="asdf.ass", blend=False)
   mask = core.std.PropToClip(clip=sub, prop='_Alpha')

   gray10 = core.query_video_format(
      mask.format.color_family,
      YUV420P10_video.format.sample_type,
      YUV420P10_video.format.bits_per_sample,
      mask.format.subsampling_w,
      mask.format.subsampling_h
   )

   sub = core.resize.Bicubic(clip=sub, format=YUV420P10_video.format.id, matrix_s="470bg")
   mask = core.resize.Bicubic(clip=mask, format=gray10.id)

   hardsubbed_video = core.std.MaskedMerge(clipa=YUV420P10_video, clipb=sub, mask=mask)

Example with automatic blending (will use BT709 matrix)::

   hardsubbed_video = core.sub.TextFile(clip=YUV420P10_video, file="asdf.ass")

Example with a custom palette and automatic blending::

   def rgba(r, g, b, a=255):
       if r < 0 or r > 255 or g < 0 or g > 255 or b < 0 or b > 255 or a < 0 or a > 255:
           raise vs.Error("Colours must be in the range [0, 255].")

       return (a << 24) + (r << 16) + (g << 8) + b
   
   unused = 1 << 42

   src = core.ffms2.Source("video.mp4")

   # Override only the third element of the palette. Set it to some kind of green.
   ret = core.sub.ImageFile(src, "subtitles.sup", palette=[unused, unused, rgba(0, 192, 128)])
