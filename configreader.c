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

#include "configreader.h"
#include "httpd.h"
#include <stdio.h>
#include <string.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <libavutil/mem.h>
#include <libavutil/error.h>

const char *stream_format_names[] = { "mkv" };

static struct HTTPDConfig *parsed_configs = NULL;

void stream_free(struct StreamConfig *stream)
{
    av_freep(&stream->stream_name);
    av_freep(&stream->input_uri);
    av_freep(&stream->formats);
}

void config_free(struct HTTPDConfig *config)
{
    int i;
    av_freep(&config->server_name);
    av_freep(&config->bind_address);
    if (config->streams) {
        for (i = 0; i < config->nb_streams; i++)
            stream_free(&config->streams[i]);
        av_freep(&config->streams);
    }
}

void config_dump(struct HTTPDConfig *config, FILE *stream) {
    int i, j;
    fprintf(stream, "======\nserver name: %s\nbind_address: %s\nport: %d\nnb_streams: %d\n",
            config->server_name, config->bind_address, config->port, config->nb_streams);
    for (i = 0; i < config->nb_streams; i++) {
        fprintf(stream, "------\nstream_name: %s\ninput: %s\nformats: ",
            config->streams[i].stream_name, config->streams[i].input_uri);
        for (j = 0; j < config->streams[i].nb_formats; j++) {
            fprintf(stream, "%s ", stream_format_names[config->streams[i].formats[j]]);
        }
        fprintf(stream, "\n");
    }
}

int configs_parse(lua_State *L)
{
    int nb_configs = 0;
    int nb_streams = 0;
    int nb_formats = 0;
    int index = 0;
    const char *key;
    struct HTTPDConfig *config;
    struct StreamConfig *stream;

    luaL_checktype(L, -1, LUA_TTABLE);
    lua_pushnil(L);
    
    // iterate servers
    while (lua_next(L, -2) != 0) {
        nb_configs++;
        parsed_configs = av_realloc(parsed_configs, nb_configs * sizeof(struct HTTPDConfig));
        if (!parsed_configs) {
            return luaL_error(L, "Error could not allocate memory for configs");
        }
        luaL_checktype(L, -2, LUA_TSTRING);
        luaL_checktype(L, -1, LUA_TTABLE);
        config = &parsed_configs[nb_configs - 1];
        config->server_name = NULL;
        config->bind_address = NULL;
        config->port = -1;
        config->accept_timeout = 1000;
        config->streams = NULL;
        config->nb_streams = 0;

        config->server_name = av_strdup(lua_tostring(L, -2));
        if (!config->server_name) {
            return luaL_error(L, "Error could not allocate server name string");
        }
        lua_pushnil(L);
        // iterate server properties
        nb_streams = 0;
        while(lua_next(L, -2) != 0) {
            luaL_checktype(L, -2, LUA_TSTRING);
            key = lua_tostring(L, -2);
            if (!strncmp("bind_address", key, 12)) {
                config->bind_address = av_strdup(lua_tostring(L, -1));
                if (!config->bind_address) {
                    return luaL_error(L, "Error could not allocate bind address string");
                }
            } else if (!strncmp("port", key, 4)) {
                config->port = (int) lua_tonumber(L, -1);
            } else {
                // keys that are not "bind_address" or "port" are streams
                luaL_checktype(L, -1, LUA_TTABLE);

                nb_streams++;
                config->streams = av_realloc(config->streams, nb_streams * sizeof(struct StreamConfig));
                if (!config->streams) {
                    return luaL_error(L, "Error could not allocate memory for streams");
                }
                stream = &config->streams[nb_streams - 1];
                stream->input_uri = NULL;
                stream->formats = NULL;
                stream->nb_formats = 0;
                stream->stream_name = av_strdup(lua_tostring(L, -2));
                if (!stream->stream_name) {
                    return luaL_error(L, "Error could not allocate stream name string");
                }
                lua_pushnil(L);
                while(lua_next(L, -2) != 0) {
                    luaL_checktype(L, -2, LUA_TSTRING);
                    key = lua_tostring(L, -2);
                    if (!strncmp("input", key, 5)) {
                        stream->input_uri = av_strdup(lua_tostring(L, -1));
                        if (!stream->input_uri) {
                            return luaL_error(L, "Error could not allocate input_uri");
                        }
                    } else if (!strncmp("formats", key, 7)) {
                        index = 1;
                        nb_formats = 0;
                        lua_pushnumber(L, index);
                        while(1) {
                            lua_gettable(L, -2);
                            if (lua_isnil(L, -1))
                                break;
                            luaL_checktype(L, -1, LUA_TSTRING);
                            stream->formats = av_realloc(stream->formats,
                                                         (nb_formats + 1) * sizeof(enum StreamFormat));
                            if (!stream->formats) {
                                return luaL_error(L, "Error could not allocate stream formats");
                            }
                            key = lua_tostring(L, -1);
                            if (!strncmp("mkv", key, 3)) {
                                stream->formats[nb_formats++] = FMT_MATROSKA;
                            } else {
                                fprintf(stderr, "Warning unknown format (%s) in stream format configuration.\n",
                                                                                                           key);
                                av_realloc(stream->formats, nb_formats * sizeof(enum StreamFormat));
                                if (!stream->formats) {
                                    return luaL_error(L, "Error could not shrink stream formats");
                                }
                            }
                            stream->nb_formats = nb_formats;
                            lua_pop(L, 1);
                            lua_pushnumber(L, ++index);
                        }
                        lua_pop(L, 1);
                            
                    } else {
                        fprintf(stderr, "Warning unknown key (%s) in stream configuration.\n", key);
                    }
                    lua_pop(L, 1);
                }
                if (stream->nb_formats == 0)
                    luaL_error(L, "No format specified for stream %s on server %s", stream->stream_name, config->server_name);
            }
            lua_pop(L, 1);
        }
        if (config->port == -1)
            return luaL_error(L, "No port set for server: %s", config->server_name);
        if (!config->bind_address)
            return luaL_error(L, "No bind address set for server: %s", config->server_name);
        config->nb_streams = nb_streams;
        lua_pop(L, 1);
    }
    lua_pushnumber(L, nb_configs);
    return 1;
}


int configs_read(struct HTTPDConfig **configs, const char *filename)
{
    int ret = 0;
    int nb_configs = 0;
    lua_State *L = luaL_newstate();
    ret = luaL_loadfile(L, filename);
    if (ret != 0) {
        fprintf(stderr, "Unable to open config file: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return AVERROR_INVALIDDATA;
    }

    ret = lua_pcall(L, 0, 0, 0);

    if (ret != 0) {
        fprintf(stderr, "Unable to read config file: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return AVERROR_INVALIDDATA;
    }

    lua_pushcfunction(L, configs_parse);
    lua_getglobal(L, "settings");

    ret = lua_pcall(L, 1, 1, 0);

    if (ret != 0) {
        fprintf(stderr, "Unable to parse config file: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return AVERROR_INVALIDDATA;
    }

    nb_configs = lua_tonumber(L, -1);

    lua_close(L);
    *configs = parsed_configs;
    return nb_configs;
}

