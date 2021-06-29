# ts-snip
Lossless cutting of an MPEG2 (ISO/IEC 13818-1) transport stream.

Just open a transport stream, create slices, i.e., sections to be cut out of the stream, and save it.
Cutting is done at I frames (ICB frames). Incomplete packets are ignored in the final output.

## DEPENDS
 * glib/gtk+
 * https://github.com/lahol/libtsanalyze

## Known issues
 * Reopening and saving loses the last I frame. This is due to the recognition of those frames, which
   is only done by detecting the next pes unit.
 * Currently only color preview for yuv420p images, otherwise gray scale (more due to laziness)
