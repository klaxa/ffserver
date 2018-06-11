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

#ifndef CONFIGREADER_H
#define CONFIGREADER_H

#include "httpd.h"
#include <stdio.h>

/**
 * Read configurations from a file using the lua format. The configurations
 * are allocated as an array at *configs. This has to be freed by the user.
 *
 * @param configs pointer to a pointer where configurations will be allocated.
 * @param filename filename of the configuration to use.
 * @return number of configurations read, a negative AVERROR code on error.
 */
int configs_read(struct HTTPDConfig **configs, const char *filename);

/**
 * Dump a configuration to a stream specified by the user.
 * @param config pointer to a configuration
 * @param stream output stream to be used to dump the config
 */
void config_dump(struct HTTPDConfig *config, FILE *stream);

/**
 * Free a configuration.
 * @param config pointer to a configuration
 */
void config_free(struct HTTPDConfig *config);

#endif
