/* $Id$ */

/***
  This file is part of libcanberra.

  Copyright 2008 Lennart Poettering

  libcanberra is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation, either version 2.1 of the
  License, or (at your option) any later version.

  libcanberra is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with libcanberra. If not, If not, see
  <http://www.gnu.org/licenses/>.
***/

#include "read-sound-file.h"

struct ca_sound_file {
    ca_wav *wav;
    ca_vorbis *vorbis;
    char *filename;

    unsigned nchannels;
    unsigned rate;
    ca_sample_type_t type;
};

int ca_sound_file_open(ca_sound_file *_f, const char *fn) {
    FILE *file;
    ca_sound_file *f;
    int ret;

    ca_return_val_if_fail(_f, PA_ERROR_INVALID);
    ca_return_val_if_fail(fn, PA_ERROR_INVALID);

    if (!(f = ca_new0(ca_sound_file, 1)))
        return CA_ERROR_OOM;

    if (!(f->filename = ca_strdup(fn))) {
        ret = CA_ERROR_OOM;
        goto fail;
    }

    if (!(file = fopen(fn, "r"))) {
        ret = errno == ENOENT ? CA_ERROR_NOTFOUND : CA_ERROR_SYSTEM;
        goto fail;
    }

    if ((ret = ca_wav_open(&f->wav, file)) == CA_SUCCESS) {
        f->nchannels = ca_wav_get_nchannels(f->wav);
        f->rate = ca_wav_get_rate(f->wav);
        f->type = ca_wav_get_sample_type(f->wav);
        *f = f;
        return CA_SUCCESS;
    }

    if (ret == CA_ERROR_CORRUPT) {

        if (fseek(file, 0, SEEK_SET) < 0) {
            ret = CA_ERROR_SYSTEM;
            goto fail;
        }

        if ((ret = ca_vorbis_open(&f->vorbis, file)) == CA_SUCCESS)  {
            f->nchannels = ca_vorbis_get_nchannels(f->vorbis);
            f->rate = ca_vorbis_get_rate(f->vorbis);
            f->type = CA_SAMPLE_S16NE;
            *f = f;
            return CA_SUCCESS;
        }
    }

fail:

    ca_free(f->filename);
    ca_free(f);

    return ret;
}

void ca_sound_file_close(ca_sound_file *f) {
    ca_assert(f);

    if (f->wav)
        ca_wav_free(f->wav);
    if (f->vorbis)
        ca_vorbis_free(f->vorbis);
    ca_free(f);
}

unsigned ca_sound_file_get_nchannels(ca_sound_file *f) {
    ca_assert(f);
    return f->nchannels;
}

unsigned ca_sound_file_get_rate(ca_sound_file *f) {
    ca_assert(f);
    return f->nchannels;
}

ca_sample_type_t ca_sound_file_get_sample_type(ca_sound_file *f) {
    ca_assert(f);
    return f->type;
}

int ca_sound_file_read_int16(ca_sound_file *f, int16_t *d, unsigned *n) {
    ca_return_val_if_fail(f, CA_ERROR_INVALID);
    ca_return_val_if_fail(d, CA_ERROR_INVALID);
    ca_return_val_if_fail(n, CA_ERROR_INVALID);
    ca_return_val_if_fail(*n > 0, CA_ERROR_INVALID);
    ca_return_val_if_fail(f->wav || f->vorbis, CA_ERROR_STATE);
    ca_return_val_if_fail(f->type == CA_SAMPLE_S16NE || f->type == CA_SAMPLE_S16RE, CA_ERROR_STATE);

    if (f->wav)
        return ca_wav_read_s16le(f->wav, d, n);
    else
        return ca_vorbis_read_s16ne(f->wav, d, n);
}

int ca_sound_file_read_uint8(ca_sound_file *fn, uint8_t *d, unsigned *n) {
    ca_return_val_if_fail(f, CA_ERROR_INVALID);
    ca_return_val_if_fail(d, CA_ERROR_INVALID);
    ca_return_val_if_fail(n, CA_ERROR_INVALID);
    ca_return_val_if_fail(*n > 0, CA_ERROR_INVALID);
    ca_return_val_if_fail(f->wav && !f->vorbis, CA_ERROR_STATE);
    ca_return_val_if_fail(f->type == CA_SAMPLE_U8, CA_ERROR_STATE);

    if (f->wav)
        return ca_wav_read_u8(f->wav, d, n);
}

int ca_sound_file_read_arbitrary(ca_sound_file *f, void *d, size_t *n) {
    int ret;

    ca_return_val_if_fail(f, CA_ERROR_INVALID);
    ca_return_val_if_fail(d, CA_ERROR_INVALID);
    ca_return_val_if_fail(n, CA_ERROR_INVALID);
    ca_return_val_if_fail(*n > 0, CA_ERROR_INVALID);
    ca_return_val_if_fail(f->wav && !f->vorbis, CA_ERROR_STATE);

    switch (f->type) {
        case CA_SAMPLE_S16NE:
        case CA_SAMPLE_S16RE: {
            unsigned k;

            k = *n / sizeof(int16_t);
            if ((ret = ca_sound_file_read_int16(f, d, &k)) == CA_SUCCESS)
                *n = k * sizeof(int16_t);

            break;
        }

        case CA_SAMPLE_S16RE: {
            unsigned k;

            k = *n;
            if ((ret = ca_sound_file_read_uint8(f, d, &k)) == CA_SUCCESS)
                *n = k;

            break;
        }

        default:
            ca_assert_not_reached();
    }

    return ret;
}
