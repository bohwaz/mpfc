/******************************************************************
 * Copyright (C) 2003 - 2013 by SG Software.
 *
 * SG MPFC. Songs manipulation functions implementation.
 * $Id$
 *
 * This program is free software; you can redistribute it and/or 
 * modify it under the terms of the GNU General Public License 
 * as published by the Free Software Foundation; either version 2 
 * of the License, or (at your option) any later version.
 *  
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *  
 * You should have received a copy of the GNU General Public 
 * License along with this program; if not, write to the Free 
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, 
 * MA 02111-1307, USA.
 */

#include <stdlib.h>
#include <string.h>
#include <gst/gst.h>
#include "types.h"
#include "cfg.h"
#include "metadata_io.h"
#include "mystring.h"
#include "player.h"
#include "pmng.h"
#include "song.h"
#include "song_info.h"
#include "util.h"

static void song_set_sliced_len( song_t *song )
{
	song->m_len = (song->m_end_time > -1) ? 
		(song->m_end_time - song->m_start_time) : 
			(song->m_full_len - song->m_start_time);
}

static song_t *song_new( song_metadata_t *metadata )
{
	/* Try to allocate memory for new song */
	song_t *song = (song_t *)malloc(sizeof(song_t));
	if (song == NULL)
		return NULL;
	memset(song, 0, sizeof(*song));

	song->m_start_time = song->m_end_time = -1;
	song->m_len = song->m_full_len = metadata->m_len;
	pthread_mutex_init(&song->m_mutex, NULL);

	/* Slice song */
	if (metadata->m_start_time >= 0)
	{
		song->m_start_time = metadata->m_start_time;
		song->m_end_time = metadata->m_end_time;
		song_set_sliced_len(song);
	}

	/* Set song info */
	if (metadata->m_song_info)
	{
		song->m_info = metadata->m_song_info;
		song->m_flags |= SONG_STATIC_INFO;
	}

	return song;
}

static void song_set_title( song_t *s, song_metadata_t *metadata )
{
	const char *title = metadata->m_title;
	if (title == NULL)
		song_update_title(s);
	else
	{
		s->m_title = str_new(title);
		s->m_default_title = strdup(title);
	}
}

/* Create a new song */
song_t *song_new_from_file( const char *filename, song_metadata_t *metadata )
{
	/* Must be an absolute path */
	assert((*filename) == '/');

	/* Is this a supported format? */
	const char *ext = strrchr(filename, '.');
	if (!ext)
		ext = "";
	else
		ext++;
	if (!pmng_search_format(player_pmng, filename, ext))
		return NULL;
	
	song_t *song = song_new(metadata);

	/* Set various types of file name */
	song->m_fullname = gst_filename_to_uri(filename, NULL);
	if (!song->m_fullname)
	{
		free(song);
		return NULL;
	}
	song->m_filename = strdup(filename);

	song_set_title(song, metadata);

	return song_add_ref(song);
} /* End of 'song_new_from_file' function */

/* Create a new song from an URI */
song_t *song_new_from_uri( const char *uri, song_metadata_t *metadata )
{
	song_t *song = song_new(metadata);
	song->m_fullname = strdup(uri);

	song_set_title(song, metadata);

	return song_add_ref(song);
} /* End of 'song_new' function */

/* Add a reference to the song object */
song_t *song_add_ref( song_t *song )
{
	assert(song);
	assert(song->m_ref_count >= 0);
	song->m_ref_count ++;
	return song;
} /* End of 'song_add_ref' function */

/* Free song */
void song_free( song_t *song )
{
	assert(song);
	assert(song->m_ref_count > 0);

	/* Release reference */
	song->m_ref_count --;

	/* Free object */
	if (song->m_ref_count == 0)
	{
		str_free(song->m_title);
		si_free(song->m_info);
		if (song->m_filename)
			free(song->m_filename);
		free(song->m_fullname);
		if (song->m_default_title != NULL)
			free(song->m_default_title);
		pthread_mutex_destroy(&song->m_mutex);
		free(song);
	}
} /* End of 'song_free' function */

/* Set current song info */
void song_set_info( song_t *song, song_info_t *si )
{
	if (song == NULL || (song->m_flags & SONG_INFO_WRITE))
	{
		return;
	}

	song_lock(song);

	if (song->m_info)
		si_free(song->m_info);
	song->m_info = si;

	song_update_title(song);

	song_unlock(song);
}

/* Update song information */
void song_update_info( song_t *song )
{
	if (song == NULL || (song->m_flags & SONG_INFO_WRITE))
	{
		return;
	}

	song_lock(song);

	song_info_t *new_info = md_get_info(song->m_filename,
			song->m_fullname, &song->m_full_len);
	song->m_len = song->m_full_len;
	if (!(song->m_flags & SONG_STATIC_INFO))
	{
		si_free(song->m_info);
		song->m_info = new_info;
	}
	else if (new_info)
		si_free(new_info);

	if (song->m_start_time > -1)
	{
		song_set_sliced_len(song);
	}

	song_update_title(song);
	song->m_flags &= (~SONG_INFO_READ);
	song_unlock(song);
} /* End of 'song_update_info' function */

/* Get short filename but only if it is not uri-based */
const char* song_get_short_name( song_t *s )
{
	return (s->m_filename ? util_short_name(s->m_filename) : s->m_fullname);
}

static str_t *song_default_title( song_t *s )
{
	str_t *title = str_new(song_get_short_name(s));
	if (cfg_get_var_int(cfg_list, "convert-underscores2spaces"))
		str_replace_char(title, '_', ' ');
	return title;
}

/* Fill song title from data from song info and other parameters */
void song_update_title( song_t *song )
{
	char *fmt;
	str_t *str;
	bool_t finish = FALSE;
	song_info_t *info;

	if (song == NULL || song->m_default_title != NULL)
		return;

	/* Free current title */
	str_free(song->m_title);
	
	/* Case that we have no info */
	info = song->m_info;
	if (info == NULL || !(info->m_flags & SI_INITIALIZED) ||
			(info->m_flags & SI_ONLY_OWN))
	{
		song->m_title = song_default_title(song);
		return;
	}

	/* Use specified title format */
	fmt = cfg_get_var(cfg_list, "title-format");
	str = song->m_title = str_new("");
	const char *filename = song_get_name(song);
	bool_t empty_title = TRUE;
	if (fmt != NULL && (*fmt != 0))
	{
		for ( ; *fmt && !finish; fmt ++ )
		{
			char *g;
			
			if (*fmt == '%')
			{
				fmt ++;
				const char *item = NULL;
				switch (*fmt)
				{
				case 'p':
					item = info->m_artist;
					break;
				case 'a':
					item = info->m_album;
					break;
				case 'f':
					item = util_short_name(filename);
					break;
				case 'F':
					item = filename;
					break;
				case 'e':
					item = util_extension(filename);
					break;
				case 't':
					item = info->m_name;
					break;
				case 'n':
					item = info->m_track;
					break;
				case 'y':
					item = info->m_year;
					break;
				case 'g':
					item = info->m_genre;
					break;
				case 'c':
					item = info->m_comments;
					break;
				case 0:
					finish = TRUE;
					break;
				}

				if (item && (*item))
				{
					str_cat_cptr(str, item);
					empty_title = FALSE;
				}
			}
			else
			{
				str_insert_char(str, *fmt, str->m_len);
			}
		}
	}
	else
	{
		str_printf(str, "%s - %s", info->m_artist, info->m_name);
		if (*(info->m_artist) || *(info->m_name))
			empty_title = FALSE;
	}

	/* If all info items participating in forming the title are empty,
	 * use the default (filename-based) one */
	if (empty_title)
	{
		str_free(song->m_title);
		song->m_title = song_default_title(song);
	}
} /* End of 'song_get_title_from_info' function */

/* Write song info */
void song_write_info( song_t *s )
{
	char *name = s->m_filename;
	bool_t is_sliced = s->m_start_time > 0 || s->m_end_time >= 0;
	if (!name || is_sliced || !md_save_info(name, s->m_info))
	{
		song_update_info(s);
		logger_error(player_log, 0, _("Failed to save info to file %s"),
				s->m_fullname);
	}
	s->m_flags &= ~(SONG_INFO_READ | SONG_INFO_WRITE);
} /* End of 'song_write_info' function */

/* End of 'song.c' file */

