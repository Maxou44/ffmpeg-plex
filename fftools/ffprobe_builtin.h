/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef FFTOOLS_FFPROBE_BUILTIN_H
#define FFTOOLS_FFPROBE_BUILTIN_H

/* ffprobe's main, built into ffmpeg (ffprobe_builtin.c). */
int ffprobe_main(int argc, char **argv);
void ffprobe_show_help_default(const char *opt, const char *arg);

/* Set while ffmpeg runs as ffprobe, so that -h shows ffprobe's help. */
extern int ffprobe_builtin_running;

#endif /* FFTOOLS_FFPROBE_BUILTIN_H */
